/*
 * Copyright (C) 2021 Niklas Haas
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libplacebo/renderer.h>
#include <libplacebo/shaders/lut.h>
#include <libplacebo/utils/libav.h>
#include <libplacebo/utils/frame_queue.h>

#ifdef PL_HAVE_LCMS
#include <libplacebo/shaders/icc.h>
#endif

#include "config.h"
#include "common/common.h"
#include "options/m_config.h"
#include "options/path.h"
#include "osdep/io.h"
#include "stream/stream.h"
#include "video/mp_image.h"
#include "video/fmt-conversion.h"
#include "placebo/utils.h"
#include "gpu/context.h"
#include "gpu/video.h"
#include "gpu/video_shaders.h"
#include "sub/osd.h"

#if HAVE_VULKAN
#include "vulkan/context.h"
#endif

struct osd_entry {
    pl_tex tex;
    struct pl_overlay_part *parts;
    int num_parts;
};

struct osd_state {
    struct osd_entry entries[MAX_OSD_PARTS];
    struct pl_overlay overlays[MAX_OSD_PARTS];
};

struct scaler_params {
    struct pl_filter_config config;
    struct pl_filter_function kernel;
    struct pl_filter_function window;
};

struct user_hook {
    char *path;
    const struct pl_hook *hook;
};

struct user_lut {
    char *opt;
    char *path;
    int type;
    struct pl_custom_lut *lut;
};

struct priv {
    struct mp_log *log;
    struct mpv_global *global;
    struct ra_ctx *ra_ctx;

    pl_log pllog;
    pl_gpu gpu;
    pl_renderer rr;
    pl_queue queue;
    pl_swapchain sw;
    pl_fmt osd_fmt[SUBBITMAP_COUNT];
    pl_tex *sub_tex;
    int num_sub_tex;

    struct mp_rect src, dst;
    struct mp_osd_res osd_res;
    struct osd_state osd_state;

    uint64_t last_id;
    double last_pts;
    bool is_interpolated;
    bool want_reset;

    struct m_config_cache *opts_cache;
    struct mp_csp_equalizer_state *video_eq;
    struct pl_render_params params;
    struct pl_deband_params deband;
    struct pl_sigmoid_params sigmoid;
    struct pl_color_adjustment color_adjustment;
    struct pl_peak_detect_params peak_detect;
    struct pl_color_map_params color_map;
    struct pl_dither_params dither;
    struct scaler_params scalers[SCALER_COUNT];
    const struct pl_hook **hooks; // storage for `params.hooks`

#ifdef PL_HAVE_LCMS
    struct pl_icc_params icc;
    struct pl_icc_profile icc_profile;
    char *icc_path;
#endif

    struct user_lut image_lut;
    struct user_lut target_lut;
    struct user_lut lut;

    // Cached shaders, preserved across options updates
    struct user_hook *user_hooks;
    int num_user_hooks;

    // Performance data of last frame
    struct voctrl_performance_data perf;

    int delayed_peak;
    int builtin_scalers;
    int inter_preserve;
};

static void update_render_options(struct priv *p);
static void update_lut(struct priv *p, struct user_lut *lut);

// This struct is stored at the end of DR-allocated buffers, and serves to both
// detect such buffers and hold the reference to the actual GPU buffer.
struct dr_buf {
    uint64_t sentinel[2];
    pl_gpu gpu;
    pl_buf buf;
};

static const uint64_t dr_magic[2] = { 0xc6e9222474db53ae, 0x9d49b2de6c3b563e };
static const size_t dr_align = offsetof(struct { char c; struct dr_buf dr; }, dr);
static inline struct dr_buf *dr_header(void *ptr, size_t size)
{
    uintptr_t start = (uintptr_t) ptr + size - sizeof(struct dr_buf);
    uintptr_t aligned = MP_ALIGN_DOWN(start, dr_align);
    assert(aligned >= (uintptr_t) ptr);
    return (struct dr_buf *) aligned;
}

static pl_buf get_dr_buf(struct mp_image *mpi)
{
    if (!mpi->bufs[0] || mpi->bufs[0]->size < sizeof(struct dr_buf))
        return NULL;

    struct dr_buf *dr = dr_header(mpi->bufs[0]->data, mpi->bufs[0]->size);
    if (memcmp(dr->sentinel, dr_magic, sizeof(dr_magic)) == 0)
        return dr->buf;

    return NULL;
}

static void free_dr_buf(void *opaque, uint8_t *data)
{
    struct dr_buf *dr = opaque;
    // Can't use `&dr->buf` because it gets freed during `pl_buf_destroy`
    pl_buf_destroy(dr->gpu, &(pl_buf) { dr->buf });
}

static struct mp_image *get_image(struct vo *vo, int imgfmt, int w, int h,
                                  int stride_align)
{
    struct priv *p = vo->priv;
    pl_gpu gpu = p->gpu;
    if (!gpu->limits.thread_safe || !gpu->limits.max_mapped_size)
        return NULL;

    int size = mp_image_get_alloc_size(imgfmt, w, h, stride_align);
    if (size < 0)
        return NULL;

    pl_buf buf = pl_buf_create(gpu, &(struct pl_buf_params) {
        .size = size + stride_align + sizeof(struct dr_buf) + dr_align,
        .memory_type = PL_BUF_MEM_HOST,
        .host_mapped = true,
    });

    if (!buf)
        return NULL;

    // Store the DR header at the end of the allocation
    struct dr_buf *dr = dr_header(buf->data, buf->params.size);
    memcpy(dr->sentinel, dr_magic, sizeof(dr_magic));
    dr->gpu = gpu;
    dr->buf = buf;

    struct mp_image *mpi = mp_image_from_buffer(imgfmt, w, h, stride_align,
                                                buf->data, buf->params.size,
                                                dr, free_dr_buf);
    if (!mpi) {
        pl_buf_destroy(gpu, &buf);
        return NULL;
    }

    return mpi;
}

static void write_overlays(struct vo *vo, struct mp_osd_res res, double pts,
                           int flags, struct osd_state *state,
                           struct pl_frame *frame)
{
    struct priv *p = vo->priv;
    static const bool subfmt_all[SUBBITMAP_COUNT] = {
        [SUBBITMAP_LIBASS] = true,
        [SUBBITMAP_RGBA]   = true,
    };

    struct sub_bitmap_list *subs = osd_render(vo->osd, res, pts, flags, subfmt_all);
    frame->num_overlays = 0;
    frame->overlays = state->overlays;

    for (int n = 0; n < subs->num_items; n++) {
        const struct sub_bitmaps *item = subs->items[n];
        if (!item->num_parts || !item->packed)
            continue;
        struct osd_entry *entry = &state->entries[item->render_index];
        pl_fmt tex_fmt = p->osd_fmt[item->format];
        if (!entry->tex)
            MP_TARRAY_POP(p->sub_tex, p->num_sub_tex, &entry->tex);
        bool ok = pl_tex_recreate(p->gpu, &entry->tex, &(struct pl_tex_params) {
            .format = tex_fmt,
            .w = MPMAX(item->packed_w, entry->tex ? entry->tex->params.w : 0),
            .h = MPMAX(item->packed_h, entry->tex ? entry->tex->params.h : 0),
            .host_writable = true,
            .sampleable = true,
        });
        if (!ok) {
            MP_ERR(vo, "Failed recreating OSD texture!\n");
            break;
        }
        ok = pl_tex_upload(p->gpu, &(struct pl_tex_transfer_params) {
            .tex        = entry->tex,
            .rc         = { .x1 = item->packed_w, .y1 = item->packed_h, },
            .stride_w   = item->packed->stride[0] / tex_fmt->texel_size,
            .ptr        = item->packed->planes[0],
        });
        if (!ok) {
            MP_ERR(vo, "Failed uploading OSD texture!\n");
            break;
        }

        entry->num_parts = 0;
        for (int i = 0; i < item->num_parts; i++) {
            const struct sub_bitmap *b = &item->parts[i];
            uint32_t c = b->libass.color;
            MP_TARRAY_APPEND(p, entry->parts, entry->num_parts, (struct pl_overlay_part) {
                .src = { b->src_x, b->src_y, b->src_x + b->w, b->src_y + b->h },
                .dst = { b->x, b->y, b->x + b->dw, b->y + b->dh },
                .color = {
                    (c >> 24) / 255.0,
                    ((c >> 16) & 0xFF) / 255.0,
                    ((c >> 8) & 0xFF) / 255.0,
                    1.0 - (c & 0xFF) / 255.0,
                }
            });
        }

        struct pl_overlay *ol = &state->overlays[frame->num_overlays++];
        *ol = (struct pl_overlay) {
            .tex = entry->tex,
            .parts = entry->parts,
            .num_parts = entry->num_parts,
            .color = frame->color,
        };

        switch (item->format) {
        case SUBBITMAP_RGBA:
            ol->mode = PL_OVERLAY_NORMAL;
            ol->repr.alpha = PL_ALPHA_PREMULTIPLIED;
            break;
        case SUBBITMAP_LIBASS:
            ol->mode = PL_OVERLAY_MONOCHROME;
            ol->repr.alpha = PL_ALPHA_INDEPENDENT;
            break;
        }
    }

    talloc_free(subs);
}

struct frame_priv {
    struct vo *vo;
    struct osd_state subs;
};

static int plane_data_from_imgfmt(struct pl_plane_data out_data[4],
                                  struct pl_bit_encoding *out_bits,
                                  enum mp_imgfmt imgfmt)
{
    struct mp_imgfmt_desc desc = mp_imgfmt_get_desc(imgfmt);
    if (!desc.num_planes || !(desc.flags & MP_IMGFLAG_HAS_COMPS))
        return 0;

    if (desc.flags & MP_IMGFLAG_HWACCEL)
        return 0; // HW-accelerated frames need to be mapped differently

    if (!(desc.flags & MP_IMGFLAG_NE))
        return 0; // GPU endianness follows the host's

    if (desc.flags & MP_IMGFLAG_PAL)
        return 0; // Palette formats (currently) not supported in libplacebo

    if ((desc.flags & MP_IMGFLAG_TYPE_FLOAT) && (desc.flags & MP_IMGFLAG_YUV))
        return 0; // Floating-point YUV (currently) unsupported

    bool any_padded = false;
    for (int p = 0; p < desc.num_planes; p++) {
        struct pl_plane_data *data = &out_data[p];
        struct mp_imgfmt_comp_desc sorted[MP_NUM_COMPONENTS];
        int num_comps = 0;
        for (int c = 0; c < mp_imgfmt_desc_get_num_comps(&desc); c++) {
            if (desc.comps[c].plane != p)
                continue;

            data->component_map[num_comps] = c;
            sorted[num_comps] = desc.comps[c];
            num_comps++;

            // Sort components by offset order, while keeping track of the
            // semantic mapping in `data->component_map`
            for (int i = num_comps - 1; i > 0; i--) {
                if (sorted[i].offset >= sorted[i - 1].offset)
                    break;
                MPSWAP(struct mp_imgfmt_comp_desc, sorted[i], sorted[i - 1]);
                MPSWAP(int, data->component_map[i], data->component_map[i - 1]);
            }
        }

        uint64_t total_bits = 0;

        // Fill in the pl_plane_data fields for each component
        memset(data->component_size, 0, sizeof(data->component_size));
        for (int c = 0; c < num_comps; c++) {
            data->component_size[c] = sorted[c].size;
            data->component_pad[c] = sorted[c].offset - total_bits;
            total_bits += data->component_pad[c] + data->component_size[c];
            any_padded |= sorted[c].pad;

            // Ignore bit encoding of alpha channel
            if (!out_bits || data->component_map[c] == PL_CHANNEL_A)
                continue;

            struct pl_bit_encoding bits = {
                .sample_depth = data->component_size[c],
                .color_depth = sorted[c].size - abs(sorted[c].pad),
                .bit_shift = MPMAX(sorted[c].pad, 0),
            };

            if (p == 0 && c == 0) {
                *out_bits = bits;
            } else {
                if (!pl_bit_encoding_equal(out_bits, &bits)) {
                    // Bit encoding differs between components/planes,
                    // cannot handle this
                    *out_bits = (struct pl_bit_encoding) {0};
                    out_bits = NULL;
                }
            }
        }

        if (total_bits % 8)
            return 0; // pixel size is not byte-aligned

        data->pixel_stride = total_bits / 8;
        data->type = (desc.flags & MP_IMGFLAG_TYPE_FLOAT)
                            ? PL_FMT_FLOAT
                            : PL_FMT_UNORM;
    }

    if (any_padded && !out_bits)
        return 0; // can't handle padded components without `pl_bit_encoding`

    return desc.num_planes;
}

static bool map_frame(pl_gpu gpu, pl_tex *tex, const struct pl_source_frame *src,
                      struct pl_frame *frame)
{
    struct mp_image *mpi = src->frame_data;
    struct frame_priv *fp = mpi->priv;
    struct pl_plane_data data[4] = {0};
    struct vo *vo = fp->vo;
    struct priv *p = vo->priv;

    // TODO: implement support for hwdec wrappers

    *frame = (struct pl_frame) {
        .num_planes = mpi->num_planes,
        .color = {
            .primaries = mp_prim_to_pl(mpi->params.color.primaries),
            .transfer = mp_trc_to_pl(mpi->params.color.gamma),
            .light = mp_light_to_pl(mpi->params.color.light),
            .sig_peak = mpi->params.color.sig_peak,
        },
        .repr = {
            .sys = mp_csp_to_pl(mpi->params.color.space),
            .levels = mp_levels_to_pl(mpi->params.color.levels),
            .alpha = mp_alpha_to_pl(mpi->params.alpha),
        },
        .profile = {
            .data = mpi->icc_profile ? mpi->icc_profile->data : NULL,
            .len = mpi->icc_profile ? mpi->icc_profile->size : 0,
        },
#if PL_API_VER >= 162
        .rotation = mpi->params.rotate / 90,
#endif
    };

    enum pl_chroma_location chroma = mp_chroma_to_pl(mpi->params.chroma_location);
    int planes = plane_data_from_imgfmt(data, &frame->repr.bits, mpi->imgfmt);
    for (int n = 0; n < planes; n++) {
        data[n].width = mp_image_plane_w(mpi, n);
        data[n].height = mp_image_plane_h(mpi, n);
        data[n].row_stride = mpi->stride[n];
        data[n].pixels = mpi->planes[n];

        pl_buf buf = get_dr_buf(mpi);
        if (buf) {
            data[n].pixels = NULL;
            data[n].buf = buf;
            data[n].buf_offset = mpi->planes[n] - buf->data;
        } else if (gpu->limits.callbacks) {
            data[n].callback = talloc_free;
            data[n].priv = mp_image_new_ref(mpi);
        }

        struct pl_plane *plane = &frame->planes[n];
        if (!pl_upload_plane(gpu, plane, &tex[n], &data[n])) {
            MP_ERR(vo, "Failed uploading frame!\n");
            talloc_free(data[n].priv);
            return false;
        }

        if (mpi->fmt.xs[n] || mpi->fmt.ys[n])
            pl_chroma_location_offset(chroma, &plane->shift_x, &plane->shift_y);
    }

    // Compute a unique signature for any attached ICC profile. Wasteful in
    // theory if the ICC profile is the same for multiple frames, but in
    // practice ICC profiles are overwhelmingly going to be attached to
    // still images so it shouldn't matter.
    pl_icc_profile_compute_signature(&frame->profile);

    // Generate subtitles for this frame
    struct mp_osd_res vidres = {
        .w = mpi->w, .h = mpi->h,
        // compensate for anamorphic sources (render subtitles as normal)
        .display_par = (float) mpi->params.p_h / mpi->params.p_w,
    };
    write_overlays(vo, vidres, mpi->pts, OSD_DRAW_SUB_ONLY, &fp->subs, frame);

    // Update LUT attached to this frame
    update_lut(p, &p->image_lut);
    frame->lut = p->image_lut.lut;
    frame->lut_type = p->image_lut.type;
    return true;
}

static void unmap_frame(pl_gpu gpu, struct pl_frame *frame,
                        const struct pl_source_frame *src)
{
    struct mp_image *mpi = src->frame_data;
    struct frame_priv *fp = mpi->priv;
    struct priv *p = fp->vo->priv;
    for (int i = 0; i < MP_ARRAY_SIZE(fp->subs.entries); i++) {
        pl_tex tex = fp->subs.entries[i].tex;
        if (tex)
            MP_TARRAY_APPEND(p, p->sub_tex, p->num_sub_tex, tex);
    }
    talloc_free(mpi);
}

static void discard_frame(const struct pl_source_frame *src)
{
    struct mp_image *mpi = src->frame_data;
    talloc_free(mpi);
}

static void info_callback(void *priv, const struct pl_render_info *info)
{
    struct vo *vo = priv;
    struct priv *p = vo->priv;

    int index;
    struct mp_frame_perf *frame;
    switch (info->stage) {
    case PL_RENDER_STAGE_FRAME:
        if (info->index > VO_PASS_PERF_MAX)
            return; // silently ignore clipped passes, whatever
        frame = &p->perf.fresh;
        index = info->index;
        break;
    case PL_RENDER_STAGE_BLEND:
        frame = &p->perf.redraw;
        index = 0; // ignore blended frame count
        break;
    default: abort();
    }

    struct mp_pass_perf *perf = &frame->perf[index];
    const struct pl_dispatch_info *pass = info->pass;
    assert(VO_PERF_SAMPLE_COUNT >= MP_ARRAY_SIZE(pass->samples));
    memcpy(perf->samples, pass->samples, pass->num_samples * sizeof(pass->samples[0]));
    perf->count = pass->num_samples;
    perf->last = pass->last;
    perf->peak = pass->peak;
    perf->avg = pass->average;

    talloc_free(frame->desc[index]);
    frame->desc[index] = talloc_strdup(p, pass->shader->description);
    frame->count = index + 1;
}

static void draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;
    pl_gpu gpu = p->gpu;
    if (m_config_cache_update(p->opts_cache))
        update_render_options(p);

    p->params.info_callback = info_callback;
    p->params.info_priv = vo;

    update_lut(p, &p->lut);
    p->params.lut = p->lut.lut;
    p->params.lut_type = p->lut.type;

    // Update equalizer state
    struct mp_csp_params cparams = MP_CSP_PARAMS_DEFAULTS;
    mp_csp_equalizer_state_get(p->video_eq, &cparams);
    p->color_adjustment = pl_color_adjustment_neutral;
    p->color_adjustment.brightness = cparams.brightness;
    p->color_adjustment.contrast = cparams.contrast;
    p->color_adjustment.hue = cparams.hue;
    p->color_adjustment.saturation = cparams.saturation;
    p->color_adjustment.gamma = cparams.gamma;

    // Push all incoming frames into the frame queue
    for (int n = 0; n < frame->num_frames; n++) {
        int id = frame->frame_id + n;
        if (id <= p->last_id)
            continue; // ignore already seen frames

        if (p->want_reset) {
            pl_renderer_flush_cache(p->rr);
            pl_queue_reset(p->queue);
            p->last_pts = 0.0;
            p->want_reset = false;
        }

        struct mp_image *mpi = mp_image_new_ref(frame->frames[n]);
        struct frame_priv *fp = talloc_zero(mpi, struct frame_priv);
        mpi->priv = fp;
        fp->vo = vo;

        pl_queue_push(p->queue, &(struct pl_source_frame) {
            .pts = mpi->pts,
            .frame_data = mpi,
            .map = map_frame,
            .unmap = unmap_frame,
            .discard = discard_frame,
        });

        p->last_id = id;
    }

    struct pl_swapchain_frame swframe;
    if (!pl_swapchain_start_frame(p->sw, &swframe))
        return;

    bool valid = false;
    p->is_interpolated = false;

    // Calculate target
    struct pl_frame target;
    pl_frame_from_swapchain(&target, &swframe);
    write_overlays(vo, p->osd_res, 0, OSD_DRAW_OSD_ONLY, &p->osd_state, &target);
    target.crop = (struct pl_rect2df) { p->dst.x0, p->dst.y0, p->dst.x1, p->dst.y1 };

    update_lut(p, &p->target_lut);
    target.lut = p->target_lut.lut;
    target.lut_type = p->target_lut.type;
#ifdef PL_HAVE_LCMS
    target.profile = p->icc_profile;
#endif

    // Target colorspace overrides
    const struct gl_video_opts *opts = p->opts_cache->opts;
    if (opts->target_prim)
        target.color.primaries = mp_prim_to_pl(opts->target_prim);
    if (opts->target_trc)
        target.color.transfer = mp_trc_to_pl(opts->target_trc);
    if (opts->target_peak)
        target.color.sig_peak = opts->target_peak;

    struct pl_frame_mix mix = {0};
    if (frame->current) {
        // Update queue state
        struct pl_queue_params qparams = {
            .pts = frame->current->pts + frame->vsync_offset,
            .radius = pl_frame_mix_radius(&p->params),
            .vsync_duration = frame->vsync_interval,
            .frame_duration = frame->ideal_frame_duration,
            .interpolation_threshold = opts->interpolation_threshold,
        };

        // mpv likes to generate sporadically jumping PTS shortly after
        // initialization, but pl_queue does not like these. Hard-clamp as
        // a simple work-around.
        qparams.pts = p->last_pts = MPMAX(qparams.pts, p->last_pts);

        switch (pl_queue_update(p->queue, &mix, &qparams)) {
        case PL_QUEUE_ERR:
            MP_ERR(vo, "Failed updating frames!\n");
            goto done;
        case PL_QUEUE_EOF:
            abort(); // we never signal EOF
        case PL_QUEUE_MORE:
        case PL_QUEUE_OK:
            break;
        }

        if (frame->still && mix.num_frames) {
            double best = fabs(mix.timestamps[0]);
            // Recreate nearest neighbour semantics on this frame mix
            while (mix.num_frames > 1 && fabs(mix.timestamps[1]) < best) {
                best = fabs(mix.timestamps[1]);
                mix.frames++;
                mix.signatures++;
                mix.timestamps++;
                mix.num_frames--;
            }
            mix.num_frames = 1;
        }

        // Update source crop on all existing frames. We technically own the
        // `pl_frame` struct so this is kosher. This could be avoided by
        // instead flushing the queue on resizes, but doing it this way avoids
        // unnecessarily re-uploading frames.
        for (int i = 0; i < mix.num_frames; i++) {
            struct pl_frame *img = (struct pl_frame *) mix.frames[i];
            img->crop = (struct pl_rect2df) {
                p->src.x0, p->src.y0, p->src.x1, p->src.y1,
            };

#if PL_API_VER >= 162
            // mpv gives us transposed rects, libplacebo expects untransposed
            if (img->rotation % PL_ROTATION_180) {
                MPSWAP(float, img->crop.x0, img->crop.y0);
                MPSWAP(float, img->crop.x1, img->crop.y1);
            }
#endif
        }
    }

    p->params.preserve_mixing_cache = p->inter_preserve && !frame->still;
    p->params.disable_builtin_scalers = !p->builtin_scalers;
    p->params.allow_delayed_peak_detect = p->delayed_peak;

    // Render frame
    if (!pl_render_image_mix(p->rr, &mix, &target, &p->params)) {
        MP_ERR(vo, "Failed rendering frame!\n");
        goto done;
    }

    p->is_interpolated = mix.num_frames > 1;
    valid = true;
    // fall through

done:
    if (!valid) // clear with purple to indicate error
        pl_tex_clear(gpu, swframe.fbo, (float[4]){ 0.5, 0.0, 1.0, 1.0 });

    if (!pl_swapchain_submit_frame(p->sw))
        MP_ERR(vo, "Failed presenting frame!\n");
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    struct ra_swapchain *sw = p->ra_ctx->swapchain;
    sw->fns->swap_buffers(sw);
}

static void get_vsync(struct vo *vo, struct vo_vsync_info *info)
{
    struct priv *p = vo->priv;
    struct ra_swapchain *sw = p->ra_ctx->swapchain;
    if (sw->fns->get_vsync)
        sw->fns->get_vsync(sw, info);
}

static int query_format(struct vo *vo, int format)
{
    struct priv *p = vo->priv;
    struct pl_bit_encoding bits;
    struct pl_plane_data data[4];
    int planes = plane_data_from_imgfmt(data, &bits, format);
    if (!planes)
        return false;

    for (int i = 0; i < planes; i++) {
        if (!pl_plane_find_fmt(p->gpu, NULL, &data[i]))
            return false;
    }

    return true;
}

static void resize(struct vo *vo)
{
    struct priv *p = vo->priv;
    vo_get_src_dst_rects(vo, &p->src, &p->dst, &p->osd_res);
    vo->want_redraw = true;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *p = vo->priv;
    if (!p->ra_ctx->fns->reconfig(p->ra_ctx))
        return -1;

    resize(vo);
    return 0;
}

static bool update_auto_profile(struct priv *p, int *events)
{
#ifdef PL_HAVE_LCMS

    const struct gl_video_opts *opts = p->opts_cache->opts;
    if (!opts->icc_opts || !opts->icc_opts->profile_auto || p->icc_path)
        return false;

    MP_VERBOSE(p, "Querying ICC profile...\n");
    bstr icc = {0};
    int r = p->ra_ctx->fns->control(p->ra_ctx, events, VOCTRL_GET_ICC_PROFILE, &icc);

    if (r != VO_NOTAVAIL) {
        if (r == VO_FALSE) {
            MP_WARN(p, "Could not retrieve an ICC profile.\n");
        } else if (r == VO_NOTIMPL) {
            MP_ERR(p, "icc-profile-auto not implemented on this platform.\n");
        }

        talloc_free((void *) p->icc_profile.data);
        p->icc_profile.data = icc.start;
        p->icc_profile.len = icc.len;
        pl_icc_profile_compute_signature(&p->icc_profile);
        return true;
    }

#endif // PL_HAVE_LCMS

    return false;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;

    switch (request) {
    case VOCTRL_SET_PANSCAN:
        pl_renderer_flush_cache(p->rr); // invalidate source crop
        resize(vo);
        // fall through
    case VOCTRL_SET_EQUALIZER:
    case VOCTRL_PAUSE:
        if (p->is_interpolated)
            vo->want_redraw = true;
        return VO_TRUE;

    case VOCTRL_UPDATE_RENDER_OPTS: {
        m_config_cache_update(p->opts_cache);
        const struct gl_video_opts *opts = p->opts_cache->opts;
        p->ra_ctx->opts.want_alpha = opts->alpha_mode == ALPHA_YES;
        if (p->ra_ctx->fns->update_render_opts)
            p->ra_ctx->fns->update_render_opts(p->ra_ctx);
        update_render_options(p);
        vo->want_redraw = true;

        // Also re-query the auto profile, in case `update_render_options`
        // unloaded a manually specified icc profile in favor of
        // icc-profile-auto
        int events = 0;
        update_auto_profile(p, &events);
        vo_event(vo, events);
        return VO_TRUE;
    }

    case VOCTRL_RESET:
        // Defer until the first new frame (unique ID) actually arrives
        p->want_reset = true;
        return VO_TRUE;

    case VOCTRL_PERFORMANCE_DATA:
        *(struct voctrl_performance_data *) data = p->perf;
        return true;
    }

    int events = 0;
    int r = p->ra_ctx->fns->control(p->ra_ctx, &events, request, data);
    if (events & VO_EVENT_ICC_PROFILE_CHANGED) {
        if (update_auto_profile(p, &events))
            vo->want_redraw = true;
    }
    if (events & VO_EVENT_RESIZE)
        resize(vo);
    if (events & VO_EVENT_EXPOSE)
        vo->want_redraw = true;
    vo_event(vo, events);

    return r;
}

static void wakeup(struct vo *vo)
{
    struct priv *p = vo->priv;
    if (p->ra_ctx && p->ra_ctx->fns->wakeup)
        p->ra_ctx->fns->wakeup(p->ra_ctx);
}

static void wait_events(struct vo *vo, int64_t until_time_us)
{
    struct priv *p = vo->priv;
    if (p->ra_ctx && p->ra_ctx->fns->wait_events) {
        p->ra_ctx->fns->wait_events(p->ra_ctx, until_time_us);
    } else {
        vo_wait_default(vo, until_time_us);
    }
}

static char *get_cache_file(struct priv *p)
{
    struct gl_video_opts *opts = p->opts_cache->opts;
    if (!opts->shader_cache_dir || !opts->shader_cache_dir[0])
        return NULL;

    char *dir = mp_get_user_path(NULL, p->global, opts->shader_cache_dir);
    char *file = mp_path_join(NULL, dir, "libplacebo.cache");
    talloc_free(dir);
    return file;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;
    pl_queue_destroy(&p->queue); // destroy this first
    for (int i = 0; i < MP_ARRAY_SIZE(p->osd_state.entries); i++)
        pl_tex_destroy(p->gpu, &p->osd_state.entries[i].tex);
    for (int i = 0; i < p->num_sub_tex; i++)
        pl_tex_destroy(p->gpu, &p->sub_tex[i]);
    for (int i = 0; i < p->num_user_hooks; i++)
        pl_mpv_user_shader_destroy(&p->user_hooks[i].hook);

    char *cache_file = get_cache_file(p);
    if (cache_file) {
        FILE *cache = fopen(cache_file, "wb");
        if (cache) {
            size_t size = pl_renderer_save(p->rr, NULL);
            uint8_t *buf = talloc_size(NULL, size);
            pl_renderer_save(p->rr, buf);
            fwrite(buf, size, 1, cache);
            talloc_free(buf);
            fclose(cache);
        }
        talloc_free(cache_file);
    }

    pl_renderer_destroy(&p->rr);
    ra_ctx_destroy(&p->ra_ctx);
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    p->opts_cache = m_config_cache_alloc(p, vo->global, &gl_video_conf);
    p->video_eq = mp_csp_equalizer_create(p, vo->global);
    p->global = vo->global;
    p->log = vo->log;

    struct gl_video_opts *gl_opts = p->opts_cache->opts;
    struct ra_ctx_opts *ctx_opts = mp_get_config_group(p, vo->global, &ra_ctx_conf);
    struct ra_ctx_opts opts = *ctx_opts;
    opts.context_type = "vulkan";
    opts.context_name = NULL;
    opts.want_alpha = gl_opts->alpha_mode == ALPHA_YES;
    p->ra_ctx = ra_ctx_create(vo, opts);
    if (!p->ra_ctx)
        goto err_out;

#if HAVE_VULKAN
    struct mpvk_ctx *vkctx = ra_vk_ctx_get(p->ra_ctx);
    if (vkctx) {
        p->pllog = vkctx->ctx;
        p->gpu = vkctx->gpu;
        p->sw = vkctx->swapchain;
        goto done;
    }
#endif

    // TODO: wrap GL contexts

    goto err_out;

done:
    p->rr = pl_renderer_create(p->pllog, p->gpu);
    p->queue = pl_queue_create(p->gpu);
    p->osd_fmt[SUBBITMAP_LIBASS] = pl_find_named_fmt(p->gpu, "r8");
    p->osd_fmt[SUBBITMAP_RGBA] = pl_find_named_fmt(p->gpu, "rgba8");

    char *cache_file = get_cache_file(p);
    if (cache_file) {
        if (stat(cache_file, &(struct stat){0}) == 0) {
            bstr c = stream_read_file(cache_file, p, vo->global, 1000000000);
            pl_renderer_load(p->rr, c.start);
            talloc_free(c.start);
        }
        talloc_free(cache_file);
    }

    // Request as many frames as possible from the decoder. This is not really
    // wasteful since we pass these through libplacebo's frame queueing
    // mechanism, which only uploads frames on an as-needed basis.
    vo_set_queue_params(vo, 0, VO_MAX_REQ_FRAMES);
    update_render_options(p);
    return 0;

err_out:
    uninit(vo);
    return -1;
}

static const struct pl_filter_config *map_scaler(struct priv *p,
                                                 enum scaler_unit unit)
{
    static const struct pl_filter_preset fixed_scalers[] = {
        { "bilinear",       &pl_filter_bilinear },
        { "bicubic_fast",   &pl_filter_bicubic },
        { "nearest",        &pl_filter_nearest },
        { "oversample",     &pl_filter_oversample },
        {0},
    };

    static const struct pl_filter_preset fixed_frame_mixers[] = {
        { "linear",         &pl_filter_bilinear },
        { "oversample",     &pl_filter_oversample },
        {0},
    };

    const struct pl_filter_preset *fixed_presets =
        unit == SCALER_TSCALE ? fixed_frame_mixers : fixed_scalers;

    const struct gl_video_opts *opts = p->opts_cache->opts;
    const struct scaler_config *cfg = &opts->scaler[unit];
    if (unit == SCALER_DSCALE && !cfg->kernel.name)
        cfg = &opts->scaler[SCALER_SCALE];

    for (int i = 0; fixed_presets[i].name; i++) {
        if (strcmp(cfg->kernel.name, fixed_presets[i].name) == 0)
            return fixed_presets[i].filter;
    }

    // Attempt loading filter preset first, fall back to raw filter function
    struct scaler_params *par = &p->scalers[unit];
    const struct pl_filter_preset *preset;
    const struct pl_filter_function_preset *fpreset;
    if ((preset = pl_find_filter_preset(cfg->kernel.name))) {
        par->config = *preset->filter;
        par->kernel = *par->config.kernel;
    } else if ((fpreset = pl_find_filter_function_preset(cfg->kernel.name))) {
        par->config = (struct pl_filter_config) {0};
        par->kernel = *fpreset->function;
    } else {
        MP_ERR(p, "Failed mapping filter function '%s', no libplacebo analog?\n",
               cfg->kernel.name);
        return &pl_filter_bilinear;
    }

    par->config.kernel = &par->kernel;
    if (par->config.window) {
        par->window = *par->config.window;
        par->config.window = &par->window;
    }

    const struct pl_filter_function_preset *wpreset;
    if ((wpreset = pl_find_filter_function_preset(cfg->window.name)))
        par->window = *wpreset->function;

    for (int i = 0; i < 2; i++) {
        if (!isnan(cfg->kernel.params[i]))
            par->kernel.params[i] = cfg->kernel.params[i];
        if (!isnan(cfg->window.params[i]))
            par->window.params[i] = cfg->window.params[i];
    }

    par->config.clamp = cfg->clamp;
    par->config.blur = cfg->kernel.blur;
    par->config.taper = cfg->kernel.taper;
    if (cfg->radius > 0.0) {
        if (par->kernel.resizable) {
            par->kernel.radius = cfg->radius;
        } else {
            MP_WARN(p, "Filter radius specified but filter '%s' is not "
                    "resizable, ignoring\n", cfg->kernel.name);
        }
    }

    return &par->config;
}

static const struct pl_hook *load_hook(struct priv *p, const char *path)
{
    if (!path || !path[0])
        return NULL;

    for (int i = 0; i < p->num_user_hooks; i++) {
        if (strcmp(p->user_hooks[i].path, path) == 0)
            return p->user_hooks[i].hook;
    }

    char *fname = mp_get_user_path(NULL, p->global, path);
    bstr shader = stream_read_file(fname, p, p->global, 1000000000); // 1GB
    talloc_free(fname);

    const struct pl_hook *hook = NULL;
    if (shader.len)
        hook = pl_mpv_user_shader_parse(p->gpu, shader.start, shader.len);

    MP_TARRAY_APPEND(p, p->user_hooks, p->num_user_hooks, (struct user_hook) {
        .path = talloc_strdup(p, path),
        .hook = hook,
    });

    return hook;
}

static void update_icc_opts(struct priv *p, const struct mp_icc_opts *opts)
{
    if (!opts)
        return;

#ifdef PL_HAVE_LCMS

    if (!opts->profile_auto && !p->icc_path && p->icc_profile.len) {
        // Un-set any auto-loaded profiles if icc-profile-auto was disabled
        talloc_free((void *) p->icc_profile.data);
        p->icc_profile = (struct pl_icc_profile) {0};
    }

    int s_r = 0, s_g = 0, s_b = 0;
    gl_parse_3dlut_size(opts->size_str, &s_r, &s_g, &s_b);
    p->params.icc_params = &p->icc;
    p->icc = pl_icc_default_params;
    p->icc.intent = opts->intent;
    p->icc.size_r = s_r;
    p->icc.size_g = s_g;
    p->icc.size_b = s_b;

    if (!opts->profile || !opts->profile[0]) {
        // No profile enabled, un-load any existing profiles
        if (p->icc_path) {
            talloc_free((void *) p->icc_profile.data);
            TA_FREEP(&p->icc_path);
            p->icc_profile = (struct pl_icc_profile) {0};
        }
        return;
    }

    if (p->icc_path && strcmp(opts->profile, p->icc_path) == 0)
        return; // ICC profile hasn't changed

    char *fname = mp_get_user_path(NULL, p->global, opts->profile);
    MP_VERBOSE(p, "Opening ICC profile '%s'\n", fname);
    talloc_free((void *) p->icc_profile.data);
    struct bstr icc = stream_read_file(fname, p, p->global, 100000000); // 100 MB
    p->icc_profile.data = icc.start;
    p->icc_profile.len = icc.len;
    pl_icc_profile_compute_signature(&p->icc_profile);
    talloc_free(fname);

    // Update cached path
    talloc_free(p->icc_path);
    p->icc_path = talloc_strdup(p, opts->profile);

#endif // PL_HAVE_LCMS
}

static void update_lut(struct priv *p, struct user_lut *lut)
{
    if (!lut->opt) {
        pl_lut_free(&lut->lut);
        TA_FREEP(&lut->path);
        return;
    }

    if (lut->path && strcmp(lut->path, lut->opt) == 0)
        return; // no change

    // Update cached path
    pl_lut_free(&lut->lut);
    talloc_free(lut->path);
    lut->path = talloc_strdup(p, lut->opt);

    // Load LUT file
    char *fname = mp_get_user_path(NULL, p->global, lut->path);
    MP_VERBOSE(p, "Loading custom LUT '%s'\n", fname);
    struct bstr lutdata = stream_read_file(fname, p, p->global, 100000000); // 100 MB
    lut->lut = pl_lut_parse_cube(p->pllog, lutdata.start, lutdata.len);
    talloc_free(lutdata.start);
}

static void update_render_options(struct priv *p)
{
    const struct gl_video_opts *opts = p->opts_cache->opts;
    p->params = pl_render_default_params;
    p->params.lut_entries = 1 << opts->scaler_lut_size;
    p->params.antiringing_strength = opts->scaler[0].antiring;
    p->params.polar_cutoff = opts->scaler[0].cutoff;
    p->params.deband_params = opts->deband ? &p->deband : NULL;
    p->params.sigmoid_params = opts->sigmoid_upscaling ? &p->sigmoid : NULL;
    p->params.color_adjustment = &p->color_adjustment;
    p->params.peak_detect_params = opts->tone_map.compute_peak >= 0 ? &p->peak_detect : NULL;
    p->params.color_map_params = &p->color_map;
    p->params.background_color[0] = opts->background.r / 255.0;
    p->params.background_color[1] = opts->background.g / 255.0;
    p->params.background_color[2] = opts->background.b / 255.0;
    p->params.skip_anti_aliasing = !opts->correct_downscaling;
    p->params.disable_linear_scaling = !opts->linear_downscaling && !opts->linear_upscaling;
    p->params.disable_fbos = opts->dumb_mode == 1;
#if PL_API_VER >= 164
    p->params.blend_against_tiles = opts->alpha_mode == ALPHA_BLEND_TILES;
#endif

    // Map scaler options as best we can
    p->params.upscaler = map_scaler(p, SCALER_SCALE);
    p->params.downscaler = map_scaler(p, SCALER_DSCALE);
    p->params.frame_mixer = opts->interpolation ? map_scaler(p, SCALER_TSCALE) : NULL;

    p->deband = pl_deband_default_params;
    p->deband.iterations = opts->deband_opts->iterations;
    p->deband.radius = opts->deband_opts->range;
    p->deband.threshold = opts->deband_opts->threshold / 16.384;
    p->deband.grain = opts->deband_opts->grain / 8.192;

    p->sigmoid = pl_sigmoid_default_params;
    p->sigmoid.center = opts->sigmoid_center;
    p->sigmoid.slope = opts->sigmoid_slope;

    p->peak_detect = pl_peak_detect_default_params;
    p->peak_detect.smoothing_period = opts->tone_map.decay_rate;
    p->peak_detect.scene_threshold_low = opts->tone_map.scene_threshold_low;
    p->peak_detect.scene_threshold_high = opts->tone_map.scene_threshold_high;

    static const enum pl_tone_mapping_algorithm tone_map_algos[] = {
        [TONE_MAPPING_CLIP]     = PL_TONE_MAPPING_CLIP,
        [TONE_MAPPING_MOBIUS]   = PL_TONE_MAPPING_MOBIUS,
        [TONE_MAPPING_REINHARD] = PL_TONE_MAPPING_REINHARD,
        [TONE_MAPPING_HABLE]    = PL_TONE_MAPPING_HABLE,
        [TONE_MAPPING_GAMMA]    = PL_TONE_MAPPING_GAMMA,
        [TONE_MAPPING_LINEAR]   = PL_TONE_MAPPING_LINEAR,
        [TONE_MAPPING_BT_2390]  = PL_TONE_MAPPING_BT_2390,
    };

    p->color_map = pl_color_map_default_params;
    p->color_map.intent = opts->icc_opts->intent;
    p->color_map.tone_mapping_algo = tone_map_algos[opts->tone_map.curve];
    p->color_map.tone_mapping_param = opts->tone_map.curve_param;
    p->color_map.desaturation_strength = opts->tone_map.desat;
    p->color_map.desaturation_exponent = opts->tone_map.desat_exp;
    p->color_map.max_boost = opts->tone_map.max_boost;
    p->color_map.gamut_warning = opts->tone_map.gamut_warning;
    p->color_map.gamut_clipping = opts->tone_map.gamut_clipping;

    switch (opts->dither_algo) {
    case DITHER_ERROR_DIFFUSION:
        MP_ERR(p, "Error diffusion dithering is not implemented.\n");
        // fall through
    case DITHER_NONE:
        p->params.dither_params = NULL;
        break;
    case DITHER_ORDERED:
    case DITHER_FRUIT:
        p->params.dither_params = &p->dither;
        p->dither = pl_dither_default_params;
        p->dither.method = opts->dither_algo == DITHER_FRUIT
                                ? PL_DITHER_BLUE_NOISE
                                : PL_DITHER_ORDERED_FIXED;
        p->dither.lut_size = opts->dither_size;
        p->dither.temporal = opts->temporal_dither;
        break;
    }

    update_icc_opts(p, opts->icc_opts);

    const struct pl_hook *hook;
    for (int i = 0; opts->user_shaders && opts->user_shaders[i]; i++) {
        if ((hook = load_hook(p, opts->user_shaders[i])))
            MP_TARRAY_APPEND(p, p->hooks, p->params.num_hooks, hook);
    }

    p->params.hooks = p->hooks;
}

#define OPT_BASE_STRUCT struct priv

const struct m_opt_choice_alternatives lut_types[] = {
    {"auto",        PL_LUT_UNKNOWN},
    {"native",      PL_LUT_NATIVE},
    {"normalized",  PL_LUT_NORMALIZED},
    {"conversion",  PL_LUT_CONVERSION},
    {0}
};

const struct vo_driver video_out_gpu_next = {
    .description = "Video output based on libplacebo",
    .name = "gpu-next",
#if PL_API_VER >= 162
    .caps = VO_CAP_ROTATE90,
#endif
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .get_image_ts = get_image,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .get_vsync = get_vsync,
    .wait_events = wait_events,
    .wakeup = wakeup,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
    .priv_defaults = &(const struct priv) {
        .delayed_peak = true,
        .builtin_scalers = true,
        .inter_preserve = true,
    },

    .options = (const struct m_option[]) {
        {"allow-delayed-peak-detect", OPT_FLAG(delayed_peak)},
        {"builtin-scalers", OPT_FLAG(builtin_scalers)},
        {"interpolation-preserve", OPT_FLAG(inter_preserve)},
        {"lut", OPT_STRING(lut.opt), .flags = M_OPT_FILE},
        {"lut-type", OPT_CHOICE_C(lut.type, lut_types)},
        {"image-lut", OPT_STRING(image_lut.opt), .flags = M_OPT_FILE},
        {"image-lut-type", OPT_CHOICE_C(image_lut.type, lut_types)},
        {"target-lut", OPT_STRING(target_lut.opt), .flags = M_OPT_FILE},
        // No `target-lut-type` because we don't support non-RGB targets
        {0}
    },
};
