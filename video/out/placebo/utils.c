#include "common/common.h"
#include "utils.h"

static const int pl_log_to_msg_lev[PL_LOG_ALL+1] = {
    [PL_LOG_FATAL] = MSGL_FATAL,
    [PL_LOG_ERR]   = MSGL_ERR,
    [PL_LOG_WARN]  = MSGL_WARN,
    [PL_LOG_INFO]  = MSGL_V,
    [PL_LOG_DEBUG] = MSGL_DEBUG,
    [PL_LOG_TRACE] = MSGL_TRACE,
};

static const enum pl_log_level msg_lev_to_pl_log[MSGL_MAX+1] = {
    [MSGL_FATAL]   = PL_LOG_FATAL,
    [MSGL_ERR]     = PL_LOG_ERR,
    [MSGL_WARN]    = PL_LOG_WARN,
    [MSGL_INFO]    = PL_LOG_WARN,
    [MSGL_STATUS]  = PL_LOG_WARN,
    [MSGL_V]       = PL_LOG_INFO,
    [MSGL_DEBUG]   = PL_LOG_DEBUG,
    [MSGL_TRACE]   = PL_LOG_TRACE,
    [MSGL_MAX]     = PL_LOG_ALL,
};

// translates log levels while probing
static const enum pl_log_level probing_map(enum pl_log_level level)
{
    switch (level) {
    case PL_LOG_FATAL:
    case PL_LOG_ERR:
    case PL_LOG_WARN:
        return PL_LOG_INFO;

    default:
        return level;
    }
}

static void log_cb(void *priv, enum pl_log_level level, const char *msg)
{
    struct mp_log *log = priv;
    mp_msg(log, pl_log_to_msg_lev[level], "%s\n", msg);
}

static void log_cb_probing(void *priv, enum pl_log_level level, const char *msg)
{
    struct mp_log *log = priv;
    mp_msg(log, pl_log_to_msg_lev[probing_map(level)], "%s\n", msg);
}

void mppl_ctx_set_log(struct pl_context *ctx, struct mp_log *log, bool probing)
{
    assert(log);

    pl_context_update(ctx, &(struct pl_context_params) {
        .log_cb      = probing ? log_cb_probing : log_cb,
        .log_level   = msg_lev_to_pl_log[mp_msg_level(log)],
        .log_priv    = log,
    });
}

enum pl_color_primaries mp_prim_to_pl(enum mp_csp_prim prim)
{
    switch (prim) {
    case MP_CSP_PRIM_AUTO:          return PL_COLOR_PRIM_UNKNOWN;
    case MP_CSP_PRIM_BT_601_525:    return PL_COLOR_PRIM_BT_601_525;
    case MP_CSP_PRIM_BT_601_625:    return PL_COLOR_PRIM_BT_601_625;
    case MP_CSP_PRIM_BT_709:        return PL_COLOR_PRIM_BT_709;
    case MP_CSP_PRIM_BT_2020:       return PL_COLOR_PRIM_BT_2020;
    case MP_CSP_PRIM_BT_470M:       return PL_COLOR_PRIM_BT_470M;
    case MP_CSP_PRIM_APPLE:         return PL_COLOR_PRIM_APPLE;
    case MP_CSP_PRIM_ADOBE:         return PL_COLOR_PRIM_ADOBE;
    case MP_CSP_PRIM_PRO_PHOTO:     return PL_COLOR_PRIM_PRO_PHOTO;
    case MP_CSP_PRIM_CIE_1931:      return PL_COLOR_PRIM_CIE_1931;
    case MP_CSP_PRIM_DCI_P3:        return PL_COLOR_PRIM_DCI_P3;
    case MP_CSP_PRIM_DISPLAY_P3:    return PL_COLOR_PRIM_DISPLAY_P3;
    case MP_CSP_PRIM_V_GAMUT:       return PL_COLOR_PRIM_V_GAMUT;
    case MP_CSP_PRIM_S_GAMUT:       return PL_COLOR_PRIM_S_GAMUT;
    case MP_CSP_PRIM_COUNT:         return PL_COLOR_PRIM_COUNT;
    }

    MP_UNREACHABLE();
}

enum pl_color_transfer mp_trc_to_pl(enum mp_csp_trc trc)
{
    switch (trc) {
    case MP_CSP_TRC_AUTO:           return PL_COLOR_TRC_UNKNOWN;
    case MP_CSP_TRC_BT_1886:        return PL_COLOR_TRC_BT_1886;
    case MP_CSP_TRC_SRGB:           return PL_COLOR_TRC_SRGB;
    case MP_CSP_TRC_LINEAR:         return PL_COLOR_TRC_LINEAR;
    case MP_CSP_TRC_GAMMA18:        return PL_COLOR_TRC_GAMMA18;
    case MP_CSP_TRC_GAMMA20:        return PL_COLOR_TRC_UNKNOWN; // missing
    case MP_CSP_TRC_GAMMA22:        return PL_COLOR_TRC_GAMMA22;
    case MP_CSP_TRC_GAMMA24:        return PL_COLOR_TRC_UNKNOWN; // missing
    case MP_CSP_TRC_GAMMA26:        return PL_COLOR_TRC_UNKNOWN; // missing
    case MP_CSP_TRC_GAMMA28:        return PL_COLOR_TRC_GAMMA28;
    case MP_CSP_TRC_PRO_PHOTO:      return PL_COLOR_TRC_PRO_PHOTO;
    case MP_CSP_TRC_PQ:             return PL_COLOR_TRC_PQ;
    case MP_CSP_TRC_HLG:            return PL_COLOR_TRC_HLG;
    case MP_CSP_TRC_V_LOG:          return PL_COLOR_TRC_V_LOG;
    case MP_CSP_TRC_S_LOG1:         return PL_COLOR_TRC_S_LOG1;
    case MP_CSP_TRC_S_LOG2:         return PL_COLOR_TRC_S_LOG2;
    case MP_CSP_TRC_COUNT:          return PL_COLOR_TRC_COUNT;
    }

    MP_UNREACHABLE();
}

enum pl_color_light mp_light_to_pl(enum mp_csp_light light)
{
    switch (light) {
    case MP_CSP_LIGHT_AUTO:         return PL_COLOR_LIGHT_UNKNOWN;
    case MP_CSP_LIGHT_DISPLAY:      return PL_COLOR_LIGHT_DISPLAY;
    case MP_CSP_LIGHT_SCENE_HLG:    return PL_COLOR_LIGHT_SCENE_HLG;
    case MP_CSP_LIGHT_SCENE_709_1886: return PL_COLOR_LIGHT_SCENE_709_1886;
    case MP_CSP_LIGHT_SCENE_1_2:    return PL_COLOR_LIGHT_SCENE_1_2;
    case MP_CSP_LIGHT_COUNT:        return PL_COLOR_LIGHT_COUNT;
    }

    MP_UNREACHABLE();
}

enum pl_color_system mp_csp_to_pl(enum mp_csp csp)
{
    switch (csp) {
    case MP_CSP_AUTO:               return PL_COLOR_SYSTEM_UNKNOWN;
    case MP_CSP_BT_601:             return PL_COLOR_SYSTEM_BT_601;
    case MP_CSP_BT_709:             return PL_COLOR_SYSTEM_BT_709;
    case MP_CSP_SMPTE_240M:         return PL_COLOR_SYSTEM_SMPTE_240M;
    case MP_CSP_BT_2020_NC:         return PL_COLOR_SYSTEM_BT_2020_NC;
    case MP_CSP_BT_2020_C:          return PL_COLOR_SYSTEM_BT_2020_C;
    case MP_CSP_RGB:                return PL_COLOR_SYSTEM_RGB;
    case MP_CSP_XYZ:                return PL_COLOR_SYSTEM_XYZ;
    case MP_CSP_YCGCO:              return PL_COLOR_SYSTEM_YCGCO;
    case MP_CSP_COUNT:              return PL_COLOR_SYSTEM_COUNT;
    }

    MP_UNREACHABLE();
}

enum pl_color_levels mp_levels_to_pl(enum mp_csp_levels levels)
{
    switch (levels) {
    case MP_CSP_LEVELS_AUTO:        return PL_COLOR_LEVELS_UNKNOWN;
    case MP_CSP_LEVELS_TV:          return PL_COLOR_LEVELS_TV;
    case MP_CSP_LEVELS_PC:          return PL_COLOR_LEVELS_PC;
    case MP_CSP_LEVELS_COUNT:       return PL_COLOR_LEVELS_COUNT;
    }

    MP_UNREACHABLE();
}

enum pl_alpha_mode mp_alpha_to_pl(enum mp_alpha_type alpha)
{
    switch (alpha) {
    // Note: Older versions of libplacebo incorreclty handled PL_ALPHA_UNKNOWN
    // as premultiplied, so explicitly default this to independent instead.
    case MP_ALPHA_AUTO:             return PL_ALPHA_INDEPENDENT;
    case MP_ALPHA_STRAIGHT:         return PL_ALPHA_INDEPENDENT;
    case MP_ALPHA_PREMUL:           return PL_ALPHA_PREMULTIPLIED;
    }

    MP_UNREACHABLE();
}

enum pl_chroma_location mp_chroma_to_pl(enum mp_chroma_location chroma)
{
    switch (chroma) {
    case MP_CHROMA_AUTO:            return PL_CHROMA_UNKNOWN;
    case MP_CHROMA_TOPLEFT:         return PL_CHROMA_TOP_LEFT;
    case MP_CHROMA_LEFT:            return PL_CHROMA_LEFT;
    case MP_CHROMA_CENTER:          return PL_CHROMA_CENTER;
    case MP_CHROMA_COUNT:           return PL_CHROMA_COUNT;
    }

    MP_UNREACHABLE();
}
