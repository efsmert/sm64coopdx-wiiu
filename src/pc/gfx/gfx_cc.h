#ifndef GFX_CC_H
#define GFX_CC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum {
    CC_0,
    CC_TEXEL0,
    CC_TEXEL1,
    CC_PRIM,
    CC_SHADE,
    CC_ENV,
    CC_TEXEL0A,
    CC_LOD,
    CC_1,
    CC_TEXEL1A,
    CC_COMBINED,
    CC_COMBINEDA,
    CC_PRIMA,
    CC_SHADEA,
    CC_ENVA,
    CC_NOISE,
    CC_ENUM_MAX,
};

enum {
    SHADER_0,
    SHADER_INPUT_1,
    SHADER_INPUT_2,
    SHADER_INPUT_3,
    SHADER_INPUT_4,
    SHADER_INPUT_5,
    SHADER_INPUT_6,
    SHADER_INPUT_7,
    SHADER_INPUT_8,
    SHADER_TEXEL0,
    SHADER_TEXEL0A,
    SHADER_TEXEL1,
    SHADER_TEXEL1A,
    SHADER_1,
    SHADER_COMBINED,
    SHADER_COMBINEDA,
    SHADER_NOISE,
};

#define SHADER_CMD_LENGTH 16
#define SHADER_OPT_ALPHA (1 << 24)
#define SHADER_OPT_FOG (1 << 25)
#define SHADER_OPT_TEXTURE_EDGE (1 << 26)
#define SHADER_OPT_NOISE (1 << 27)

#define CM_FLAG_USE_ALPHA    (1u << 0)
#define CM_FLAG_USE_FOG      (1u << 1)
#define CM_FLAG_TEXTURE_EDGE (1u << 2)
#define CM_FLAG_USE_DITHER   (1u << 3)
#define CM_FLAG_USE_2CYCLE   (1u << 4)
#define CM_FLAG_LIGHT_MAP    (1u << 5)

struct CCFeatures {
    bool used_textures[2];
    int num_inputs;
    uint8_t shader_cmds[SHADER_CMD_LENGTH];
    uint8_t c[2][4];
    bool do_single[4];
    bool do_multiply[4];
    bool do_mix[4];
    bool color_alpha_same[2];
    bool opt_alpha;
    bool opt_fog;
    bool opt_texture_edge;
    bool opt_noise;
    bool opt_light_map;
    bool opt_2cycle;
    bool do_noise;
};

#pragma pack(1)
struct CombineMode {
    union {
        struct {
            uint32_t rgb1;
            uint32_t alpha1;
            uint32_t rgb2;
            uint32_t alpha2;
        };
        uint8_t all_values[16];
    };
    uint32_t flags;
    uint64_t hash;
};
#pragma pack()

static inline bool gfx_cm_has(const struct CombineMode *cm, uint32_t mask) {
    return cm != NULL && (cm->flags & mask) != 0;
}

static inline void gfx_cm_set(struct CombineMode *cm, uint32_t mask, bool enabled) {
    if (cm == NULL) {
        return;
    }
    if (enabled) {
        cm->flags |= mask;
    } else {
        cm->flags &= ~mask;
    }
}

#define CC_MAX_SHADERS 64

struct ColorCombiner {
    struct CombineMode cm;
    struct ShaderProgram *prg;
    union {
        uint8_t shader_input_mapping[16];
        uint64_t shader_input_mapping_as_u64[8];
    };
    union {
        uint8_t shader_commands[16];
        uint64_t shader_commands_as_u64[8];
    };
    uint64_t hash;
};

#ifdef __cplusplus
extern "C" {
#endif

bool gfx_cm_uses_second_texture(struct CombineMode* cm);
void gfx_cc_get_features(struct ColorCombiner* cc, struct CCFeatures *cc_features);
void gfx_cc_print(struct ColorCombiner *cc);
void gfx_cc_precomp(void);
uint32_t color_comb_rgb(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint8_t cycle);
uint32_t color_comb_alpha(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint8_t cycle);

#ifdef __cplusplus
}
#endif

#endif
