#ifdef TARGET_WII_U

#include "gx2_shader_gen.h"
#include "gx2_shader_inl.h"

#include <malloc.h>
#include <gx2/mem.h>

#define ROUNDUP(x, align) (((x) + ((align) -1)) & ~((align) -1))

static const uint8_t reg_map[] = {
    ALU_SRC_0, // SHADER_0
    _R3, // SHADER_INPUT_1
    _R4, // SHADER_INPUT_2
    _R5, // SHADER_INPUT_3
    _R6, // SHADER_INPUT_4
    _R7, // SHADER_INPUT_5
    _R8, // SHADER_INPUT_6
    _R9, // SHADER_INPUT_7
    _R10, // SHADER_INPUT_8
    _R11, // SHADER_TEXEL0
    _R11, // SHADER_TEXEL0A
    _R12, // SHADER_TEXEL1
    _R12, // SHADER_TEXEL1A
    ALU_SRC_1, // SHADER_1
    _R1, // SHADER_COMBINED
    _R1, // SHADER_COMBINEDA
    _R127, // SHADER_NOISE
};

static uint32_t gx2_count_ps_inputs(const struct CCFeatures *cc_features) {
    if (cc_features == NULL) {
        return 2;
    }
    return 2u + (uint32_t)cc_features->num_inputs;
}

static uint8_t gx2_ps_reg_for_input(const struct CCFeatures *cc_features, uint8_t shader_input) {
    (void)cc_features;
    return reg_map[shader_input];
}

static uint8_t gx2_map_shader_src_with_features(const struct CCFeatures *cc_features, uint8_t src, bool *src_alpha) {
    (void)cc_features;
    if (src_alpha != NULL) {
        *src_alpha = false;
    }

    switch (src) {
        case SHADER_0:        return ALU_SRC_0;
        case SHADER_1:        return ALU_SRC_1;
        case SHADER_INPUT_1:
        case SHADER_INPUT_2:
        case SHADER_INPUT_3:
        case SHADER_INPUT_4:
        case SHADER_INPUT_5:
        case SHADER_INPUT_6:
        case SHADER_INPUT_7:
        case SHADER_INPUT_8:
            return gx2_ps_reg_for_input(cc_features, src);
        case SHADER_TEXEL0:   return reg_map[src];
        case SHADER_TEXEL0A:
            if (src_alpha != NULL) { *src_alpha = true; }
            return reg_map[src];
        case SHADER_TEXEL1:   return reg_map[src];
        case SHADER_TEXEL1A:
            if (src_alpha != NULL) { *src_alpha = true; }
            return reg_map[src];
        case SHADER_COMBINED: return reg_map[src];
        case SHADER_COMBINEDA:
            if (src_alpha != NULL) { *src_alpha = true; }
            return reg_map[src];
        case SHADER_NOISE:    return reg_map[src];
        default:              return ALU_SRC_0;
    }
}

static inline uint8_t gx2_alpha_component_for_src(uint8_t src, bool src_alpha) {
    (void)src;
    (void)src_alpha;
    return _w;
}

#define ADD_INSTR(...) \
    uint64_t tmp[] = {__VA_ARGS__}; \
    memcpy(*alu_ptr, tmp, sizeof(tmp)); \
    *alu_ptr += sizeof(tmp) / sizeof(uint64_t)

static inline void add_mov(uint64_t **alu_ptr, const struct CCFeatures *cc_features, uint8_t src, bool single) {
    bool src_alpha = false;
    const uint8_t original_src = src;
    src = gx2_map_shader_src_with_features(cc_features, src, &src_alpha);

    /* texel = src */
    if (single) {
        ADD_INSTR(
            ALU_MOV(_R1, _w, src, gx2_alpha_component_for_src(original_src, src_alpha))
            ALU_LAST,
        );
    } else {
        ADD_INSTR(
            ALU_MOV(_R1, _x, src, src_alpha ? _w :_x),
            ALU_MOV(_R1, _y, src, src_alpha ? _w :_y),
            ALU_MOV(_R1, _z, src, src_alpha ? _w :_z)
            ALU_LAST,
        );
    }
}

static inline void add_mul(uint64_t **alu_ptr, const struct CCFeatures *cc_features, uint8_t src0, uint8_t src1, bool single) {
    bool src0_alpha = false;
    bool src1_alpha = false;
    const uint8_t original_src0 = src0;
    const uint8_t original_src1 = src1;
    src0 = gx2_map_shader_src_with_features(cc_features, src0, &src0_alpha);
    src1 = gx2_map_shader_src_with_features(cc_features, src1, &src1_alpha);

    /* texel = src0 * src1 */
    if (single) {
        ADD_INSTR(
            ALU_MUL(_R1, _w,
                    src0, gx2_alpha_component_for_src(original_src0, src0_alpha),
                    src1, gx2_alpha_component_for_src(original_src1, src1_alpha))
            ALU_LAST,
        );
    } else {
        ADD_INSTR(
            ALU_MUL(_R1, _x, src0, src0_alpha ? _w : _x, src1, src1_alpha ? _w : _x),
            ALU_MUL(_R1, _y, src0, src0_alpha ? _w : _y, src1, src1_alpha ? _w : _y),
            ALU_MUL(_R1, _z, src0, src0_alpha ? _w : _z, src1, src1_alpha ? _w : _z)
            ALU_LAST,
        );
    }
}

static inline void add_mix(uint64_t **alu_ptr, const struct CCFeatures *cc_features, uint8_t src0, uint8_t src1, uint8_t src2, uint8_t src3, bool single) {
    bool src0_alpha = false;
    bool src1_alpha = false;
    bool src2_alpha = false;
    bool src3_alpha = false;
    const uint8_t original_src0 = src0;
    const uint8_t original_src1 = src1;
    const uint8_t original_src2 = src2;
    const uint8_t original_src3 = src3;
    src0 = gx2_map_shader_src_with_features(cc_features, src0, &src0_alpha);
    src1 = gx2_map_shader_src_with_features(cc_features, src1, &src1_alpha);
    src2 = gx2_map_shader_src_with_features(cc_features, src2, &src2_alpha);
    src3 = gx2_map_shader_src_with_features(cc_features, src3, &src3_alpha);

    /* texel = (src0 - src1) * src2 - src3 */
    if (single) {
        ADD_INSTR(
            ALU_ADD(__, _w,
                    src0, gx2_alpha_component_for_src(original_src0, src0_alpha),
                    src1 _NEG, gx2_alpha_component_for_src(original_src1, src1_alpha))
            ALU_LAST,

            ALU_MULADD(_R1, _w,
                       ALU_SRC_PV, _w,
                       src2, gx2_alpha_component_for_src(original_src2, src2_alpha),
                       src3, gx2_alpha_component_for_src(original_src3, src3_alpha))
            ALU_LAST,
        );
    } else {
        ADD_INSTR(
            ALU_ADD(__, _x, src0, src0_alpha ? _w : _x, src1 _NEG, src1_alpha ? _w : _x),
            ALU_ADD(__, _y, src0, src0_alpha ? _w : _y, src1 _NEG, src1_alpha ? _w : _y),
            ALU_ADD(__, _z, src0, src0_alpha ? _w : _z, src1 _NEG, src1_alpha ? _w : _z)
            ALU_LAST,

            ALU_MULADD(_R1, _x, ALU_SRC_PV, _x, src2, src2_alpha ? _w : _x, src3, src3_alpha ? _w : _x),
            ALU_MULADD(_R1, _y, ALU_SRC_PV, _y, src2, src2_alpha ? _w : _y, src3, src3_alpha ? _w : _y),
            ALU_MULADD(_R1, _z, ALU_SRC_PV, _z, src2, src2_alpha ? _w : _z, src3, src3_alpha ? _w : _z)
            ALU_LAST,
        );
    }
}
#undef ADD_INSTR

static void append_formula(uint64_t **alu_ptr, const struct CCFeatures *cc_features, const uint8_t *cmd, bool do_single, bool do_multiply, bool do_mix, bool only_alpha) {
    const uint8_t *c = cmd + (only_alpha ? 4 : 0);
    if (do_single) {
        add_mov(alu_ptr, cc_features, c[3], only_alpha);
    } else if (do_multiply) {
        add_mul(alu_ptr, cc_features, c[0], c[2], only_alpha);
    } else if (do_mix) {
        add_mix(alu_ptr, cc_features, c[0], c[1], c[2], c[1], only_alpha);
    } else {
        add_mix(alu_ptr, cc_features, c[0], c[1], c[2], c[3], only_alpha);
    }
}

static const uint64_t noise_instructions[] = {
    /* R127 = floor(gl_FragCoord.xy * (240.0f / window_params.x)) */
    ALU_RECIP_IEEE(__, _x, _C(0), _x) SCL_210
    ALU_LAST,

    ALU_MUL_IEEE(__, _x, ALU_SRC_PS, _x, ALU_SRC_LITERAL, _x)
    ALU_LAST,
    ALU_LITERAL(0x43700000 /* 240.0f */),

    ALU_MUL(__, _x, _R0, _x, ALU_SRC_PV, _x),
    ALU_MUL(__, _y, _R0, _y, ALU_SRC_PV, _x)
    ALU_LAST,

    ALU_FLOOR(_R127, _x, ALU_SRC_PV, _x),
    ALU_FLOOR(_R127, _y, ALU_SRC_PV, _y)
    ALU_LAST,

    /* R127 = sin(vec3(R127.x, R127.y, window_params.y)) */
    ALU_MULADD(_R127, _x, _R127, _x, ALU_SRC_LITERAL, _x, ALU_SRC_0_5, _x),
    ALU_MULADD(_R127, _y, _R127, _y, ALU_SRC_LITERAL, _x, ALU_SRC_0_5, _x),
    ALU_MULADD(_R127, _z, _C(0), _y, ALU_SRC_LITERAL, _x, ALU_SRC_0_5, _x)
    ALU_LAST,
    ALU_LITERAL(0x3E22F983 /* 0.1591549367f (radians -> revolutions) */),

    ALU_FRACT(__, _x, _R127, _x),
    ALU_FRACT(__, _y, _R127, _y),
    ALU_FRACT(__, _z, _R127, _z)
    ALU_LAST,

    ALU_MULADD(_R127, _x, ALU_SRC_PV, _x, ALU_SRC_LITERAL, _x, ALU_SRC_LITERAL, _y),
    ALU_MULADD(_R127, _y, ALU_SRC_PV, _y, ALU_SRC_LITERAL, _x, ALU_SRC_LITERAL, _y),
    ALU_MULADD(_R127, _z, ALU_SRC_PV, _z, ALU_SRC_LITERAL, _x, ALU_SRC_LITERAL, _y)
    ALU_LAST,
    ALU_LITERAL2(0x40C90FDB /* 6.283185482f (tau) */, 0xC0490FDB /* -3.141592741f (-pi) */),

    ALU_MUL(_R127, _x, ALU_SRC_PV, _x, ALU_SRC_LITERAL, _x),
    ALU_MUL(_R127, _y, ALU_SRC_PV, _y, ALU_SRC_LITERAL, _x),
    ALU_MUL(_R127, _z, ALU_SRC_PV, _z, ALU_SRC_LITERAL, _x)
    ALU_LAST,
    ALU_LITERAL(0x3E22F983 /* 0.1591549367f (radians -> revolutions) */),

    ALU_SIN(_R127, _x, _R127, _x) SCL_210
    ALU_LAST,

    ALU_SIN(_R127, _y, _R127, _y) SCL_210
    ALU_LAST,

    ALU_SIN(_R127, _z, _R127, _z) SCL_210
    ALU_LAST,

    /* R127.x = dot(R127.xyz, vec3(12.9898, 78.233, 37.719)); */
    ALU_DOT4(_R127, _x, _R127, _x, ALU_SRC_LITERAL, _x),
    ALU_DOT4(__, _y, _R127, _y, ALU_SRC_LITERAL, _y),
    ALU_DOT4(__, _z, _R127, _z, ALU_SRC_LITERAL, _z),
    ALU_DOT4(__, _w, ALU_SRC_LITERAL, _w, ALU_SRC_0, _x)
    ALU_LAST,
    ALU_LITERAL4(0x414FD639 /* 12.9898f */, 0x429C774C /* 78.233f */, 0x4216E042 /* 37.719f */, 0x80000000 /* -0.0f */),

    /* R127.x = fract(sin(R127.x) * 143758.5453); */
    ALU_MULADD(_R127, _x, _R127, _x, ALU_SRC_LITERAL, _x, ALU_SRC_0_5, _x)
    ALU_LAST,
    ALU_LITERAL(0x3E22F983 /* 0.1591549367f (radians -> revolutions) */),

    ALU_FRACT(__, _x, _R127, _x)
    ALU_LAST,

    ALU_MULADD(_R127, _x, ALU_SRC_PV, _x, ALU_SRC_LITERAL, _x, ALU_SRC_LITERAL, _y)
    ALU_LAST,
    ALU_LITERAL2(0x40C90FDB /* 6.283185482f (tau) */, 0xC0490FDB /* -3.141592741f (-pi) */),

    ALU_SIN(_R127, _x, _R127, _x) SCL_210
    ALU_LAST,

    ALU_MUL(__, _x, _R127, _x, ALU_SRC_LITERAL, _x)
    ALU_LAST,
    ALU_LITERAL(0x480C63A3 /* 143758.5453f */),

    ALU_FRACT( _R127, _x, ALU_SRC_PV, _x)
    ALU_LAST,

    /* texel.a *= floor(R127.x + 0.5); */
    ALU_ADD(__, _x, _R127, _x, ALU_SRC_0_5, _x)
    ALU_LAST,

    ALU_FLOOR(__, _x, ALU_SRC_PV, _x)
    ALU_LAST,

    ALU_MUL(_R1, _w, _R1, _w, ALU_SRC_PV, _x)
    ALU_LAST,
};

static GX2UniformVar uniformVars[] = {
    { "window_params", GX2_SHADER_VAR_TYPE_FLOAT2, 1, 0, -1, },
};

static GX2SamplerVar samplerVars[] = {
    { "uTex0", GX2_SAMPLER_VAR_TYPE_SAMPLER_2D, 0 },
    { "uTex1", GX2_SAMPLER_VAR_TYPE_SAMPLER_2D, 1 },
};

#define ADD_INSTR(...) \
    do { \
    uint64_t tmp[] = {__VA_ARGS__}; \
    memcpy(cur_buf, tmp, sizeof(tmp)); \
    cur_buf += sizeof(tmp) / sizeof(uint64_t); \
    } while (0)

static int generatePixelShader(GX2PixelShader *psh, struct CCFeatures *cc_features) {
    static const size_t max_program_buf_size = (128 + 4) * sizeof(uint64_t);
    uint64_t *program_buf = memalign(GX2_SHADER_PROGRAM_ALIGNMENT, max_program_buf_size);
    if (!program_buf) {
        return -1;
    }

    memset(program_buf, 0, max_program_buf_size);

    // alu0
    static const uint32_t alu0_offset = 32;
    uint64_t *cur_buf = program_buf + alu0_offset;
    const uint32_t num_cycles = cc_features->opt_2cycle ? 2u : 1u;
    for (uint32_t cycle = 0; cycle < num_cycles; ++cycle) {
        const uint32_t cmd_offset = cycle * 8u;
        const uint32_t formula_offset = cycle * 2u;
        append_formula(&cur_buf,
                       cc_features,
                       &cc_features->shader_cmds[cmd_offset],
                       cc_features->do_single[formula_offset + 0],
                       cc_features->do_multiply[formula_offset + 0],
                       cc_features->do_mix[formula_offset + 0],
                       false);
        if (cc_features->opt_alpha) {
            append_formula(&cur_buf,
                           cc_features,
                           &cc_features->shader_cmds[cmd_offset],
                           cc_features->do_single[formula_offset + 1],
                           cc_features->do_multiply[formula_offset + 1],
                           cc_features->do_mix[formula_offset + 1],
                           true);
        }
    }

    if (!cc_features->opt_alpha) {
        ADD_INSTR(
            ALU_MOV(_R1, _w, ALU_SRC_1, _x)
            ALU_LAST,
        );
    }

    if (cc_features->opt_texture_edge && cc_features->opt_alpha) {
        ADD_INSTR(
            /* if (texel.a > 0.3) texel.a = 1.0; else discard; */
            ALU_KILLGT(__, _x, ALU_SRC_LITERAL, _x, _R1, _w),
            ALU_MOV(_R1, _w, ALU_SRC_1, _x)
            ALU_LAST,
            ALU_LITERAL(0x3e99999a /*0.3f*/),
        );
    }

    const uint32_t alu0_size = (uintptr_t) cur_buf - ((uintptr_t) (program_buf + alu0_offset));
    const uint32_t alu0_cnt = alu0_size / sizeof(uint64_t);

    // alu1
    // place the following instructions into a new alu, in case the other alu uses KILL
    const uint32_t alu1_offset = alu0_offset + alu0_cnt;
    cur_buf = program_buf + alu1_offset;

    if (cc_features->opt_fog) {
        ADD_INSTR(
            /* texel.rgb = mix(texel.rgb, vFog.rgb, vFog.a); */
            ALU_ADD(__, _x, _R2, _x, _R1 _NEG, _x),
            ALU_ADD(__, _y, _R2, _y, _R1 _NEG, _y),
            ALU_ADD(__, _z, _R2, _z, _R1 _NEG, _z)
            ALU_LAST,

            ALU_MULADD(_R1, _x, ALU_SRC_PV, _x, _R2, _w, _R1, _x),
            ALU_MULADD(_R1, _y, ALU_SRC_PV, _y, _R2, _w, _R1, _y),
            ALU_MULADD(_R1, _z, ALU_SRC_PV, _z, _R2, _w, _R1, _z)
            ALU_LAST,
        );
    }

    if (cc_features->opt_alpha && cc_features->opt_noise) {
        memcpy(cur_buf, noise_instructions, sizeof(noise_instructions));
        cur_buf += sizeof(noise_instructions) / sizeof(uint64_t);
    }

    const uint32_t alu1_size = (uintptr_t) cur_buf - ((uintptr_t) (program_buf + alu1_offset));
    const uint32_t alu1_cnt = alu1_size / sizeof(uint64_t);

    // make sure we have enough space for textures
    assert(32 + alu0_cnt + alu1_cnt <= 128);

    // tex
    const uint32_t tex_offset = ROUNDUP(alu1_offset + alu1_cnt, 16);
    const uint32_t num_textures = cc_features->used_textures[0] + cc_features->used_textures[1];
    uint32_t cur_tex_offset = tex_offset;

    if (cc_features->used_textures[0]) {
        uint64_t tex0_buf[] = { TEX_SAMPLE(reg_map[SHADER_TEXEL0], _x, _y, _z, _w, _R1, _x, _y, _0, _x, _t0, _s0) };
        memcpy(program_buf + cur_tex_offset, tex0_buf, sizeof(tex0_buf));
        cur_tex_offset += sizeof(tex0_buf) / sizeof(uint64_t);
    }
    if (cc_features->used_textures[1]) {
        uint64_t tex1_buf[] = { TEX_SAMPLE(reg_map[SHADER_TEXEL1],_x, _y, _z, _w, _R1, _x, _y, _0, _x, _t1, _s1) };
        memcpy(program_buf + cur_tex_offset, tex1_buf, sizeof(tex1_buf));
        cur_tex_offset += sizeof(tex1_buf) / sizeof(uint64_t);
    }

    // cf
    uint32_t cur_cf_offset = 0;

    if (num_textures > 0) {
        program_buf[cur_cf_offset++] = TEX(tex_offset, num_textures) VALID_PIX;
    }

    program_buf[cur_cf_offset++] = ALU(alu0_offset, alu0_cnt);

    if (alu1_cnt > 0) {
        program_buf[cur_cf_offset++] = ALU(alu1_offset, alu1_cnt);
    }

    if (cc_features->opt_alpha) {
        program_buf[cur_cf_offset++] = EXP_DONE(PIX0, _R1, _x, _y, _z, _w) END_OF_PROGRAM;
    } else {
        program_buf[cur_cf_offset++] = EXP_DONE(PIX0, _R1, _x, _y, _z, _1) END_OF_PROGRAM;
    }

    // regs
    const uint32_t num_ps_inputs = gx2_count_ps_inputs(cc_features);

    // Reserve a fixed PS layout: texcoord in R1, fog in R2, combiner inputs in
    // R3-R10, sampled texels in R11/R12.
    psh->regs.sq_pgm_resources_ps = 14; // num_gprs
    psh->regs.sq_pgm_exports_ps = 2; // export_mode
    psh->regs.spi_ps_in_control_0 = (num_ps_inputs + 1) // num_interp
        | (1 << 8) // position_ena
        | (1 << 26) // persp_gradient_ena
        | (1 << 28); // baryc_sample_cntl
    
    psh->regs.num_spi_ps_input_cntl = num_ps_inputs + 1;

    // frag pos
    psh->regs.spi_ps_input_cntls[0] = 0 | (1 << 8);

    // inputs
    for (uint32_t i = 0; i < num_ps_inputs; i++) {
        psh->regs.spi_ps_input_cntls[i + 1] = i | (1 << 8);
    }

    psh->regs.cb_shader_mask = 0xf; // output0_enable
    psh->regs.cb_shader_control = 1; // rt0_enable
    psh->regs.db_shader_control = (1 << 4) // z_order
        | (1 << 6); // kill_enable
    
    // program
    psh->size = (tex_offset + (num_textures * 2)) * sizeof(uint64_t);
    psh->program = program_buf;

    psh->mode = GX2_SHADER_MODE_UNIFORM_REGISTER;

    // uniform vars
    psh->uniformVars = uniformVars;
    psh->uniformVarCount = sizeof(uniformVars) / sizeof(GX2UniformVar);

    // samplers
    psh->samplerVars = samplerVars;
    psh->samplerVarCount = sizeof(samplerVars) / sizeof(GX2SamplerVar);

    return 0;
}

static GX2AttribVar attribVars[] = {
    { "aVtxPos",   GX2_SHADER_VAR_TYPE_FLOAT4, 0, 0},
    { "aTexCoord", GX2_SHADER_VAR_TYPE_FLOAT2, 0, 1},
    { "aFog",      GX2_SHADER_VAR_TYPE_FLOAT4, 0, 2},
    { "aInput1",   GX2_SHADER_VAR_TYPE_FLOAT4, 0, 3},
    { "aInput2",   GX2_SHADER_VAR_TYPE_FLOAT4, 0, 4},
    { "aInput3",   GX2_SHADER_VAR_TYPE_FLOAT4, 0, 5},
    { "aInput4",   GX2_SHADER_VAR_TYPE_FLOAT4, 0, 6},
    { "aInput5",   GX2_SHADER_VAR_TYPE_FLOAT4, 0, 7},
    { "aInput6",   GX2_SHADER_VAR_TYPE_FLOAT4, 0, 8},
    { "aInput7",   GX2_SHADER_VAR_TYPE_FLOAT4, 0, 9},
    { "aInput8",   GX2_SHADER_VAR_TYPE_FLOAT4, 0, 10},
};

static int generateVertexShader(GX2VertexShader *vsh, struct CCFeatures *cc_features) {
    static const size_t max_program_buf_size = 16 * sizeof(uint64_t);
    uint64_t *program_buf = memalign(GX2_SHADER_PROGRAM_ALIGNMENT, max_program_buf_size);
    if (!program_buf) {
        return -1;
    }

    const uint32_t num_ps_inputs = gx2_count_ps_inputs(cc_features);

    uint64_t *cur_buf = program_buf;

    // aVtxPos
    ADD_INSTR(
        CALL_FS NO_BARRIER,
        EXP_DONE(POS0, _R1, _x, _y, _z, _w),
    );

    // params
    for (uint32_t i = 0; i + 1 < num_ps_inputs; i++) {
        const uint32_t param_reg = i + 2;
        ADD_INSTR(
            EXP(PARAM(i), _R(param_reg), _x, _y, _z, _w) NO_BARRIER,
        );
    }

    // last param
    const uint32_t last_param = num_ps_inputs - 1;
    const uint32_t last_param_reg = num_ps_inputs + 1;
    ADD_INSTR(
        (EXP_DONE(PARAM(last_param), _R(last_param_reg), _x, _y, _z, _w) NO_BARRIER)
        END_OF_PROGRAM,
    );

    const uint32_t program_size = (uintptr_t) cur_buf - ((uintptr_t) program_buf);
    assert(program_size <= max_program_buf_size);

    // regs
    vsh->regs.sq_pgm_resources_vs = (num_ps_inputs + 2) // num_gprs
        | (1 << 8); // stack_size

    // num outputs minus 1
    vsh->regs.spi_vs_out_config = ((num_ps_inputs > 0 ? num_ps_inputs - 1 : 0) << 1);

    vsh->regs.num_spi_vs_out_id = 3;
    memset(vsh->regs.spi_vs_out_id, 0xff, sizeof(vsh->regs.spi_vs_out_id));
    vsh->regs.spi_vs_out_id[0] = (0) | (1 << 8) | (2 << 16) | (3 << 24);
    vsh->regs.spi_vs_out_id[1] = (4) | (5 << 8) | (0xff << 16) | (0xff << 24);
    vsh->regs.spi_vs_out_id[2] = (6) | (7 << 8) | (8 << 16) | (9 << 24);

    vsh->regs.sq_vtx_semantic_clear = ~((1 << 11) - 1);
    vsh->regs.num_sq_vtx_semantic = 11;
    memset(vsh->regs.sq_vtx_semantic, 0xff, sizeof(vsh->regs.sq_vtx_semantic));
    // aVtxPos
    vsh->regs.sq_vtx_semantic[0] = 0;
    // aTexCoord
    vsh->regs.sq_vtx_semantic[1] = 1;
    // aFog
    vsh->regs.sq_vtx_semantic[2] = 2;
    // aInput1
    vsh->regs.sq_vtx_semantic[3] = 3;
    // aInput2
    vsh->regs.sq_vtx_semantic[4] = 4;
    // aInput3
    vsh->regs.sq_vtx_semantic[5] = 5;
    // aInput4
    vsh->regs.sq_vtx_semantic[6] = 6;
    // aInput5
    vsh->regs.sq_vtx_semantic[7] = 7;
    // aInput6
    vsh->regs.sq_vtx_semantic[8] = 8;
    // aInput7
    vsh->regs.sq_vtx_semantic[9] = 9;
    // aInput8
    vsh->regs.sq_vtx_semantic[10] = 10;

    vsh->regs.vgt_vertex_reuse_block_cntl = 14; // vtx_reuse_depth
    vsh->regs.vgt_hos_reuse_depth = 16; // reuse_depth

    // program
    vsh->program = program_buf;
    vsh->size = program_size;

    vsh->mode = GX2_SHADER_MODE_UNIFORM_REGISTER;

    // attribs
    vsh->attribVarCount = sizeof(attribVars) / sizeof(GX2AttribVar);
    vsh->attribVars = attribVars;

    return 0;
}
#undef ADD_INSTR

int gx2GenerateShaderGroup(struct ShaderGroup *group, struct CCFeatures *cc_features) {
    memset(group, 0, sizeof(struct ShaderGroup));
    
    // generate the pixel shader
    if (generatePixelShader(&group->pixelShader, cc_features) != 0) {
        gx2FreeShaderGroup(group);
        return -1;
    }

    // generate the vertex shader
    if (generateVertexShader(&group->vertexShader, cc_features) != 0) {
        gx2FreeShaderGroup(group);
        return -1;
    }

    uint32_t attribOffset = 0;
    // aVtxPos
    group->attributes[group->numAttributes++] = 
        (GX2AttribStream) { 0, 0, attribOffset, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32, GX2_ATTRIB_INDEX_PER_VERTEX, 0, GX2_COMP_SEL(_x, _y, _z, _w), GX2_ENDIAN_SWAP_DEFAULT };
    attribOffset += 4 * sizeof(float);

    // aTexCoord
    if (cc_features->used_textures[0] || cc_features->used_textures[1]) {
        group->attributes[group->numAttributes++] =
            (GX2AttribStream) { 1, 0, attribOffset, GX2_ATTRIB_FORMAT_FLOAT_32_32, GX2_ATTRIB_INDEX_PER_VERTEX, 0, GX2_COMP_SEL(_x, _y, _0, _1), GX2_ENDIAN_SWAP_DEFAULT };
        attribOffset += (2 + 2) * sizeof(float);
    }

    // aFog
    if (cc_features->opt_fog) {
        group->attributes[group->numAttributes++] =
            (GX2AttribStream) { 2, 0, attribOffset, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32, GX2_ATTRIB_INDEX_PER_VERTEX, 0, GX2_COMP_SEL(_x, _y, _z, _w), GX2_ENDIAN_SWAP_DEFAULT };
        attribOffset += 4 * sizeof(float);
    }

    const uint32_t num_inputs = (cc_features->num_inputs > 8) ? 8u : (uint32_t)cc_features->num_inputs;

    // aInput
    for (uint32_t i = 0; i < num_inputs; i++) {
        group->attributes[group->numAttributes++] = 
            (GX2AttribStream) { 3 + i, 0, attribOffset, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32, GX2_ATTRIB_INDEX_PER_VERTEX, 0, GX2_COMP_SEL(_x, _y, _z, _w), GX2_ENDIAN_SWAP_DEFAULT };
        attribOffset += 4 * sizeof(float);
    }

    // init the fetch shader
    group->fetchShader.size = GX2CalcFetchShaderSizeEx(group->numAttributes, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
    group->fetchShader.program = memalign(GX2_SHADER_PROGRAM_ALIGNMENT, group->fetchShader.size);
    if (!group->fetchShader.program) {
        gx2FreeShaderGroup(group);
        return -1;
    }

    GX2InitFetchShaderEx(&group->fetchShader, group->fetchShader.program, group->numAttributes, group->attributes, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);

    // invalidate all programs
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, group->vertexShader.program, group->vertexShader.size);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, group->pixelShader.program, group->pixelShader.size);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, group->fetchShader.program, group->fetchShader.size);

    return 0;
}

void gx2FreeShaderGroup(struct ShaderGroup *group) {
    free(group->vertexShader.program);
    free(group->pixelShader.program);
    free(group->fetchShader.program);
}

#endif
