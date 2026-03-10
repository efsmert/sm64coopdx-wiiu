#ifdef TARGET_WII_U

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include <vector>
#include <unordered_set>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/texture.h>

#include <whb/log.h>
#include <whb/gfx.h>
#include <coreinit/debug.h>

#include "gx2_shader_gen.h"
#include "gfx_gx2.h"

#include "gfx_cc.h"
#include "gfx_rendering_api.h"

extern uint8_t gVertexColor[3];
extern "C" {
extern s16 gCurrLevelNum;
}

struct ShaderProgram
{
    uint64_t shader_id;
    union
    {
        WHBGfxShaderGroup whb_group;
        struct ShaderGroup gen_group;
    };
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;
    bool used_noise;
    bool used_lightmap;
    bool is_precompiled;
    uint32_t window_params_offset;
    uint32_t lightmap_color_offset;
    uint32_t samplers_location[2];
};

typedef struct _Texture
{
    GX2Texture texture;
    GX2Sampler sampler;
    bool texture_uploaded;
    bool sampler_set;
}
Texture;

static struct ShaderProgram shader_program_pool[256];
static size_t shader_program_pool_size = 0;
static bool sShaderPoolOverflowLogged = false;
static uint32_t sInvalidSamplerStateCount = 0;
static uint32_t sNullShaderSamplerSetCount = 0;
static uint32_t sTexturePoolOverflowCount = 0;
static uint32_t sInvalidTextureUploadCount = 0;

// Texture cache entries can reuse texture IDs. Retire old images for a few frames
// before freeing so we avoid reusing memory still referenced by in-flight draws.
struct RetiredTextureImage {
    void *ptr;
    uint32_t retire_frame;
};
static std::vector<RetiredTextureImage> sRetiredTextureImages;
static uint32_t sRetiredTextureFreeLogCount = 0;


static struct ShaderProgram* current_shader_program = nullptr;
static std::vector<float*> vbo_array;
static std::vector<float*> vbo_array_prev;
static std::unordered_set<float*> sTrackedVboPtrs;
static uint32_t sSkippedUntrackedVboFreeCount = 0;

#define GX2_MAX_TEXTURES 2048
static Texture gx2_textures[GX2_MAX_TEXTURES];
static size_t gx2_texture_count = 0;
static uint8_t current_tile = 0;
static uint32_t current_texture_ids[2];
static uint32_t frame_count = 0;
static uint32_t current_height = 0;
static bool sLoggedFirstDrawCall = false;
static bool sLoggedFirstDrawSubmitBegin = false;
static bool sLoggedFirstDrawSubmitEnd = false;
static uint32_t sShaderCreateLogCount = 0;
static uint32_t sMenuShaderCreateLogCount = 0;
static uint32_t sArenaDrawLogCount = 0;
static uint32_t sArenaDrawSubmitLogCount = 0;
static uint32_t sArenaFrameLogCount = 0;
static uint32_t sArenaPrepLogCount = 0;
static uint32_t sUploadProbeLogCount = 0;
static uint32_t sShaderMetaLogCount = 0;
static uint32_t sVboDumpLogCount = 0;
static uint32_t sShaderBindLogCount = 0;
static uint32_t sDrawStateLogCount = 0;
static uint32_t sTextureBindLogCount = 0;
static uint32_t sShaderBindStepLogCount = 0;
static uint32_t sSamplerLogCount = 0;
static uint32_t sMenuDrawSubmitLogCount = 0;
static uint32_t sUploadLogCount = 0;
static uint32_t sArenaLightmapTextureLogCount = 0;
static uint32_t sArenaLightmapSamplerLogCount = 0;
static uint32_t sArenaLightmapDrawStateLogCount = 0;
static uint32_t sArenaLightmapUniformLogCount = 0;
static BOOL current_depth_test = FALSE;
static BOOL current_depth_write = FALSE;
static GX2CompareFunction current_depth_compare = GX2_COMPARE_FUNC_LEQUAL;

static bool gfx_gx2_should_log_deep(void)
{
    return gCurrLevelNum == 16;
}

static bool gfx_gx2_is_arena_lightmap_shader(const struct ShaderProgram *prg)
{
    return prg != nullptr
        && gCurrLevelNum == 55
        && prg->shader_id == 0x58fd5ea80c57a2bfULL;
}

static void gfx_gx2_log_first_vbo_words(const float *buf_vbo, size_t buf_vbo_len, uint8_t stride_floats)
{
    if (buf_vbo == nullptr || sVboDumpLogCount >= 20 || !gfx_gx2_should_log_deep()) {
        return;
    }

    const size_t limit = (buf_vbo_len < 24) ? buf_vbo_len : 24;
    char scratch[768];
    int pos = snprintf(scratch, sizeof(scratch),
                       "gfx: vbo[%u] stride=%u len=%u",
                       (unsigned)sVboDumpLogCount,
                       (unsigned)stride_floats,
                       (unsigned)buf_vbo_len);
    for (size_t i = 0; i < limit && pos > 0 && (size_t)pos < sizeof(scratch); ++i) {
        pos += snprintf(scratch + pos, sizeof(scratch) - (size_t)pos,
                        " f%u=%.3f",
                        (unsigned)i,
                        buf_vbo[i]);
    }
    WHBLogPrintf("%s", scratch);
    sVboDumpLogCount++;
}

static void gfx_gx2_log_target_menu_vertex(const struct ShaderProgram *prg, const float *buf_vbo, size_t buf_vbo_len)
{
    static bool sLoggedDcb = false;
    static bool sLogged9110 = false;
    static bool sLoggedB907 = false;
    static bool sLoggedArenaLightmap = false;

    if (prg == nullptr || buf_vbo == nullptr) {
        return;
    }

    bool *logged = nullptr;
    if (gCurrLevelNum == 16) {
        switch (prg->shader_id) {
            case 0xdcb8e5ccb156fe13ULL: logged = &sLoggedDcb; break;
            case 0x9110eeef671497b0ULL: logged = &sLogged9110; break;
            case 0xb9079915e8025b0fULL: logged = &sLoggedB907; break;
            default: return;
        }
    } else if (gCurrLevelNum == 55 && prg->shader_id == 0x58fd5ea80c57a2bfULL) {
        logged = &sLoggedArenaLightmap;
    } else {
        return;
    }

    if (*logged || buf_vbo_len < prg->num_floats) {
        return;
    }

    *logged = true;
    if (prg->shader_id == 0x58fd5ea80c57a2bfULL && prg->num_floats >= 12) {
        WHBLogPrintf(
            "gfx: arena-lightmap-vtx stride=%u pos=%f,%f,%f,%f uv=%f,%f pad=%f,%f lm=%f,%f pad=%f,%f lmOff=%u vtxColor=%u,%u,%u",
            (unsigned)prg->num_floats,
            buf_vbo[0], buf_vbo[1], buf_vbo[2], buf_vbo[3],
            buf_vbo[4], buf_vbo[5], buf_vbo[6], buf_vbo[7],
            buf_vbo[8], buf_vbo[9], buf_vbo[10], buf_vbo[11],
            (unsigned)prg->lightmap_color_offset,
            (unsigned)gVertexColor[0], (unsigned)gVertexColor[1], (unsigned)gVertexColor[2]);
        return;
    }

    if (prg->num_floats < 16) {
        return;
    }

    WHBLogPrintf(
        "gfx: target-vtx hash=0x%016llx stride=%u pos=%f,%f,%f,%f uv=%f,%f in1=%f,%f,%f,%f in2=%f,%f,%f,%f",
        (unsigned long long)prg->shader_id,
        (unsigned)prg->num_floats,
        buf_vbo[0], buf_vbo[1], buf_vbo[2], buf_vbo[3],
        buf_vbo[4], buf_vbo[5],
        buf_vbo[8], buf_vbo[9], buf_vbo[10], buf_vbo[11],
        buf_vbo[12], buf_vbo[13], buf_vbo[14], buf_vbo[15]);
}

static void gfx_gx2_log_shader_bind(const struct ShaderProgram *prg)
{
    if (prg == nullptr || sShaderBindLogCount >= 24 || !gfx_gx2_should_log_deep()) {
        return;
    }

    const GX2FetchShader *fetch_shader = prg->is_precompiled ? &prg->whb_group.fetchShader : &prg->gen_group.fetchShader;
    const GX2VertexShader *vertex_shader = prg->is_precompiled ? prg->whb_group.vertexShader : &prg->gen_group.vertexShader;
    const GX2PixelShader *pixel_shader = prg->is_precompiled ? prg->whb_group.pixelShader : &prg->gen_group.pixelShader;

    WHBLogPrintf(
        "gfx: bind[%u] lvl=%d hash=0x%016llx pre=%u fetch=%p/%u vs=%p/%u ps=%p/%u floats=%u inputs=%u tex=%u,%u samp=%u,%u noise=%u light=%u",
        (unsigned)sShaderBindLogCount,
        (int)gCurrLevelNum,
        (unsigned long long)prg->shader_id,
        (unsigned)prg->is_precompiled,
        fetch_shader != nullptr ? fetch_shader->program : nullptr,
        fetch_shader != nullptr ? (unsigned)fetch_shader->size : 0u,
        vertex_shader != nullptr ? vertex_shader->program : nullptr,
        vertex_shader != nullptr ? (unsigned)vertex_shader->size : 0u,
        pixel_shader != nullptr ? pixel_shader->program : nullptr,
        pixel_shader != nullptr ? (unsigned)pixel_shader->size : 0u,
        (unsigned)prg->num_floats,
        (unsigned)prg->num_inputs,
        (unsigned)prg->used_textures[0],
        (unsigned)prg->used_textures[1],
        (unsigned)prg->samplers_location[0],
        (unsigned)prg->samplers_location[1],
        (unsigned)prg->used_noise,
        (unsigned)prg->used_lightmap);
    sShaderBindLogCount++;
}

static void gfx_gx2_log_draw_state(const struct ShaderProgram *prg, size_t buf_vbo_len, size_t buf_vbo_num_tris, size_t vbo_len)
{
    if (prg == nullptr || sDrawStateLogCount >= 24 || !gfx_gx2_should_log_deep()) {
        return;
    }

    const uint32_t tex0 = current_texture_ids[0];
    const uint32_t tex1 = current_texture_ids[1];
    const bool tex0_valid = tex0 < gx2_texture_count;
    const bool tex1_valid = tex1 < gx2_texture_count;
    const Texture *t0 = tex0_valid ? &gx2_textures[tex0] : nullptr;
    const Texture *t1 = tex1_valid ? &gx2_textures[tex1] : nullptr;

    WHBLogPrintf(
        "gfx: draw-state[%u] lvl=%d hash=0x%016llx tris=%u len=%u bytes=%u stride=%u depth=%u/%u/%u tile=%u tex0=%u up=%u samp=%u tex1=%u up=%u samp=%u",
        (unsigned)sDrawStateLogCount,
        (int)gCurrLevelNum,
        (unsigned long long)prg->shader_id,
        (unsigned)buf_vbo_num_tris,
        (unsigned)buf_vbo_len,
        (unsigned)vbo_len,
        (unsigned)(sizeof(float) * prg->num_floats),
        (unsigned)current_depth_test,
        (unsigned)current_depth_write,
        (unsigned)current_depth_compare,
        (unsigned)current_tile,
        (unsigned)tex0,
        (unsigned)(t0 != nullptr && t0->texture_uploaded),
        (unsigned)(t0 != nullptr && t0->sampler_set),
        (unsigned)tex1,
        (unsigned)(t1 != nullptr && t1->texture_uploaded),
        (unsigned)(t1 != nullptr && t1->sampler_set));
    sDrawStateLogCount++;
}

// Releases all CPU-side VBO allocations tracked in a frame list.
static void gfx_gx2_release_vbo_ptr(float *ptr)
{
    if (ptr == nullptr) {
        return;
    }
    auto it = sTrackedVboPtrs.find(ptr);
    if (it == sTrackedVboPtrs.end()) {
        if (sSkippedUntrackedVboFreeCount < 20) {
            WHBLogPrintf("gfx: skipping untracked vbo free ptr=%p", ptr);
            sSkippedUntrackedVboFreeCount++;
        }
        return;
    }
    sTrackedVboPtrs.erase(it);
    free(ptr);
}

static void gfx_gx2_release_vbo_list(std::vector<float*>& buffers)
{
    while (!buffers.empty()) {
        float *ptr = buffers.back();
        buffers.pop_back();
        gfx_gx2_release_vbo_ptr(ptr);
    }
}

static void gfx_gx2_release_vbo_budget(std::vector<float*>& buffers, size_t budget)
{
    while (budget > 0 && !buffers.empty()) {
        float *ptr = buffers.back();
        buffers.pop_back();
        gfx_gx2_release_vbo_ptr(ptr);
        budget--;
    }
}

// Validates tile index used by N64 combiner samplers.
static bool gfx_gx2_is_valid_tile(int tile)
{
    return tile >= 0 && tile < 2;
}

// Validates texture ids before indexing into gx2_textures.
static bool gfx_gx2_is_valid_texture_id(uint32_t texture_id)
{
    return texture_id < gx2_texture_count;
}

// Logs invalid sampler/texture state with bounded spam.
static void gfx_gx2_log_invalid_sampler_state(const char *where, int tile, uint32_t texture_id, uint32_t sampler_location)
{
    if (sInvalidSamplerStateCount < 20) {
        WHBLogPrintf("gfx: invalid sampler state at %s tile=%d tex=%u loc=%u pool=%u",
                     where,
                     tile,
                     (unsigned)texture_id,
                     (unsigned)sampler_location,
                     (unsigned)shader_program_pool_size);
        sInvalidSamplerStateCount++;
    }
}

GX2SamplerVar* GX2GetPixelSamplerVar(const GX2PixelShader* shader, const char* name)
{
    for (uint32_t i = 0; i < shader->samplerVarCount; i++)
    {
       if (strcmp(shader->samplerVars[i].name, name) == 0)
           return &(shader->samplerVars[i]);
    }

    return nullptr;
}

s32 GX2GetPixelSamplerVarLocation(const GX2PixelShader* shader, const char* name)
{
    GX2SamplerVar* sampler = GX2GetPixelSamplerVar(shader, name);
    if (!sampler)
        return -1;

    return sampler->location;
}

// Looks up sampler location with defensive bounds against malformed shader metadata.
static s32 GX2GetPixelSamplerVarLocationSafe(const GX2PixelShader* shader, const char* name)
{
    if (shader == nullptr || shader->samplerVars == nullptr || name == nullptr) {
        return -1;
    }

    // Expected sampler counts are small (<=2 for this renderer); clamp hard.
    uint32_t count = shader->samplerVarCount;
    if (count > 64) {
        count = 64;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (shader->samplerVars[i].name != nullptr && strcmp(shader->samplerVars[i].name, name) == 0) {
            return shader->samplerVars[i].location;
        }
    }
    return -1;
}

// Looks up uniform offset with defensive bounds against malformed shader metadata.
static s32 GX2GetPixelUniformVarOffsetSafe(const GX2PixelShader* shader, const char* name)
{
    if (shader == nullptr || shader->uniformVars == nullptr || name == nullptr) {
        return -1;
    }

    // Expected uniform counts are small in these shaders; clamp to avoid stalls.
    uint32_t count = shader->uniformVarCount;
    if (count > 128) {
        count = 128;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (shader->uniformVars[i].name != nullptr && strcmp(shader->uniformVars[i].name, name) == 0) {
            return shader->uniformVars[i].offset;
        }
    }
    return -1;
}

static bool gfx_gx2_z_is_from_0_to_1(void)
{
    return false;
}

static void gfx_gx2_unload_shader(struct ShaderProgram* old_prg)
{
    if (current_shader_program == old_prg)
        current_shader_program = nullptr;

    else
    {
        // ??????????
    }
}

static void gfx_gx2_set_uniforms(struct ShaderProgram* prg)
{
    if (prg->used_noise && prg->window_params_offset != UINT32_MAX)
    {
        float window_params_array[4] = { (float)current_height, (float)frame_count };
        GX2SetPixelUniformReg(prg->window_params_offset, 4, window_params_array);
    }
    if (prg->used_lightmap && prg->lightmap_color_offset != UINT32_MAX)
    {
        float lightmap_color[4] = {
            gVertexColor[0] / 255.0f,
            gVertexColor[1] / 255.0f,
            gVertexColor[2] / 255.0f,
            1.0f,
        };
        if (gfx_gx2_is_arena_lightmap_shader(prg) && sArenaLightmapUniformLogCount < 8) {
            WHBLogPrintf(
                "gfx: arena-lightmap uniform[%u] off=%u color=%f,%f,%f,%f",
                (unsigned)sArenaLightmapUniformLogCount,
                (unsigned)prg->lightmap_color_offset,
                lightmap_color[0],
                lightmap_color[1],
                lightmap_color[2],
                lightmap_color[3]);
            sArenaLightmapUniformLogCount++;
        }
        GX2SetPixelUniformReg(prg->lightmap_color_offset, 4, lightmap_color);
    }
}

static void gfx_gx2_load_shader(struct ShaderProgram* new_prg)
{
    current_shader_program = new_prg;
    if (!new_prg)
        return;

    if (new_prg->is_precompiled)
    {
        GX2SetFetchShader(&new_prg->whb_group.fetchShader);
        GX2SetVertexShader(new_prg->whb_group.vertexShader);
        GX2SetPixelShader(new_prg->whb_group.pixelShader);
    }
    else
    {
        GX2SetFetchShader(&new_prg->gen_group.fetchShader);
        GX2SetVertexShader(&new_prg->gen_group.vertexShader);
        GX2SetPixelShader(&new_prg->gen_group.pixelShader);
    }
    gfx_gx2_set_uniforms(new_prg);
    gfx_gx2_log_shader_bind(new_prg);
}

static struct ShaderProgram* gfx_gx2_create_and_load_new_shader(struct ColorCombiner *cc)
{
    uint64_t shader_id = cc->hash;
    struct CCFeatures cc_features = { 0 };
    gfx_cc_get_features(cc, &cc_features);
    const uint8_t shader_num_inputs = (cc_features.num_inputs > 8) ? 8 : cc_features.num_inputs;

    if (shader_program_pool_size >= (sizeof(shader_program_pool) / sizeof(shader_program_pool[0]))) {
        if (!sShaderPoolOverflowLogged) {
            sShaderPoolOverflowLogged = true;
            WHBLogPrintf("gfx: shader pool full (%u), cannot create shader_id=0x%016llx",
                         (unsigned)shader_program_pool_size, (unsigned long long)shader_id);
        }
        return nullptr;
    }

    struct ShaderProgram* prg = &shader_program_pool[shader_program_pool_size++];
    memset(prg, 0, sizeof(*prg));
    prg->window_params_offset = UINT32_MAX;
    prg->lightmap_color_offset = UINT32_MAX;
    prg->samplers_location[0] = 0;
    prg->samplers_location[1] = 1;

    prg->is_precompiled = false;
    if (gx2GenerateShaderGroup(&prg->gen_group, &cc_features) != 0) {
        WHBLogPrintf("Failed to generate shader. shader_id: 0x%016llx", (unsigned long long)shader_id);
        shader_program_pool_size--;
        return (current_shader_program = nullptr);
    }
    prg->num_floats = prg->gen_group.numAttributes * 4;

    prg->shader_id = shader_id;
    prg->num_inputs = shader_num_inputs;
    prg->used_textures[0] = cc_features.used_textures[0];
    prg->used_textures[1] = cc_features.used_textures[1];
    prg->used_lightmap = cc_features.opt_light_map;

    if ((gCurrLevelNum == 16 || gCurrLevelNum == 55) && sShaderCreateLogCount < 24) {
        WHBLogPrintf(
            "gfx: create[%u] lvl=%d hash=0x%016llx pre=%u inputs=%u tex=%u,%u fog=%u alpha=%u lm=%u lmOff=%u map=%u,%u,%u,%u cmds=%u,%u,%u,%u,%u,%u,%u,%u",
            (unsigned)sShaderCreateLogCount,
            (int)gCurrLevelNum,
            (unsigned long long)shader_id,
            (unsigned)prg->is_precompiled,
            (unsigned)cc_features.num_inputs,
            (unsigned)cc_features.used_textures[0],
            (unsigned)cc_features.used_textures[1],
            (unsigned)cc_features.opt_fog,
            (unsigned)cc_features.opt_alpha,
            (unsigned)cc_features.opt_light_map,
            (unsigned)prg->lightmap_color_offset,
            (unsigned)cc->shader_input_mapping[0],
            (unsigned)cc->shader_input_mapping[1],
            (unsigned)cc->shader_input_mapping[2],
            (unsigned)cc->shader_input_mapping[3],
            (unsigned)cc_features.shader_cmds[0],
            (unsigned)cc_features.shader_cmds[1],
            (unsigned)cc_features.shader_cmds[2],
            (unsigned)cc_features.shader_cmds[3],
            (unsigned)cc_features.shader_cmds[4],
            (unsigned)cc_features.shader_cmds[5],
            (unsigned)cc_features.shader_cmds[6],
            (unsigned)cc_features.shader_cmds[7]);
        sShaderCreateLogCount++;
    }

    const GX2PixelShader *pixel_shader = &prg->gen_group.pixelShader;
    const s32 window_params_offset = GX2GetPixelUniformVarOffsetSafe(pixel_shader, "window_params");
    if (window_params_offset >= 0) {
        prg->window_params_offset = (uint32_t)window_params_offset;
    }
    const s32 lightmap_color_offset = GX2GetPixelUniformVarOffsetSafe(pixel_shader, "uLightmapColor");
    if (lightmap_color_offset >= 0) {
        prg->lightmap_color_offset = (uint32_t)lightmap_color_offset;
    }

    const s32 sampler0 = GX2GetPixelSamplerVarLocationSafe(pixel_shader, "uTex0");
    const s32 sampler1 = GX2GetPixelSamplerVarLocationSafe(pixel_shader, "uTex1");
    if (sampler0 >= 0) {
        prg->samplers_location[0] = (uint32_t)sampler0;
    }
    if (sampler1 >= 0) {
        prg->samplers_location[1] = (uint32_t)sampler1;
    }
    prg->used_noise = cc_features.opt_alpha && cc_features.opt_noise;
    gfx_gx2_load_shader(prg);

    return prg;
}

static struct ShaderProgram* gfx_gx2_lookup_shader(struct ColorCombiner *cc)
{
    uint64_t shader_id = cc->hash;
    for (size_t i = 0; i < shader_program_pool_size; i++)
        if (shader_program_pool[i].shader_id == shader_id)
            return &shader_program_pool[i];

    return nullptr;
}

static void gfx_gx2_shader_get_info(struct ShaderProgram* prg, uint8_t* num_inputs, bool used_textures[2])
{
    if (prg)
    {
        *num_inputs = prg->num_inputs;
        used_textures[0] = prg->used_textures[0];
        used_textures[1] = prg->used_textures[1];
    }
    else
    {
        *num_inputs = 0;
        used_textures[0] = false;
        used_textures[1] = false;
    }
}

static uint32_t gfx_gx2_new_texture(void)
{
    size_t texture_id = gx2_texture_count;
    if (texture_id >= GX2_MAX_TEXTURES) {
        if (sTexturePoolOverflowCount < 8) {
            WHBLogPrintf("gfx: texture pool full (%u)", (unsigned)gx2_texture_count);
            sTexturePoolOverflowCount++;
        }
        return 0;
    }
    gx2_texture_count++;

    Texture& texture = gx2_textures[texture_id];
    memset(&texture, 0, sizeof(texture));
    texture.texture_uploaded = false;
    texture.sampler_set = false;

    return (uint32_t)texture_id;
}

static void gfx_gx2_select_texture(int tile, uint32_t texture_id)
{
    if (!gfx_gx2_is_valid_tile(tile) || !gfx_gx2_is_valid_texture_id(texture_id)) {
        gfx_gx2_log_invalid_sampler_state("select_texture", tile, texture_id, 0);
        return;
    }

    current_tile = tile;
    current_texture_ids[tile] = texture_id;
    if (gfx_gx2_is_arena_lightmap_shader(current_shader_program) && sArenaLightmapTextureLogCount < 12) {
        const Texture &texture = gx2_textures[texture_id];
        WHBLogPrintf(
            "gfx: arena-lightmap select[%u] tile=%d tex=%u loc=%u uploaded=%u sampler=%u size=%ux%u",
            (unsigned)sArenaLightmapTextureLogCount,
            tile,
            (unsigned)texture_id,
            (unsigned)current_shader_program->samplers_location[tile],
            (unsigned)texture.texture_uploaded,
            (unsigned)texture.sampler_set,
            (unsigned)texture.texture.surface.width,
            (unsigned)texture.texture.surface.height);
        sArenaLightmapTextureLogCount++;
    }
    if (sTextureBindLogCount < 24 && gfx_gx2_should_log_deep()) {
        WHBLogPrintf("gfx: select-texture[%u] lvl=%d tile=%d tex=%u shader=0x%016llx",
                     (unsigned)sTextureBindLogCount,
                     (int)gCurrLevelNum,
                     tile,
                     (unsigned)texture_id,
                     (unsigned long long)(current_shader_program != nullptr ? current_shader_program->shader_id : 0ULL));
        sTextureBindLogCount++;
    }
    if (current_shader_program) {
        Texture &texture = gx2_textures[texture_id];
        const uint32_t sampler_loc = current_shader_program->samplers_location[tile];
        if (sampler_loc >= 32) {
            gfx_gx2_log_invalid_sampler_state("set_pixel_texture", tile, texture_id, sampler_loc);
            return;
        }

        if (texture.texture_uploaded) {
            GX2SetPixelTexture(&texture.texture, sampler_loc);
        }
        if (texture.sampler_set) {
            GX2SetPixelSampler(&texture.sampler, sampler_loc);
        }
    }
}

static void gfx_gx2_upload_texture(const uint8_t* rgba32_buf, int width, int height)
{
    static const uint32_t kGx2TexMaxDim = 4096;
    static const uint64_t kGx2TexMaxBytes = 64ULL * 1024ULL * 1024ULL;

    int tile = current_tile;
    if (!gfx_gx2_is_valid_tile(tile) || !gfx_gx2_is_valid_texture_id(current_texture_ids[tile])) {
        gfx_gx2_log_invalid_sampler_state("upload_texture", tile, current_texture_ids[tile], 0);
        return;
    }
    if (rgba32_buf == nullptr || width <= 0 || height <= 0 ||
        (uint32_t)width > kGx2TexMaxDim || (uint32_t)height > kGx2TexMaxDim) {
        if (sInvalidTextureUploadCount < 32) {
            WHBLogPrintf("gfx: reject texture upload ptr=%p w=%d h=%d tile=%d tex=%u",
                         (const void *)rgba32_buf,
                         width,
                         height,
                         tile,
                         (unsigned)current_texture_ids[tile]);
            sInvalidTextureUploadCount++;
        }
        return;
    }

    uint64_t srcImageBytes = (uint64_t)width * (uint64_t)height * 4ULL;
    if (srcImageBytes == 0 || srcImageBytes > kGx2TexMaxBytes) {
        if (sInvalidTextureUploadCount < 32) {
            WHBLogPrintf("gfx: reject texture upload size=%llu w=%d h=%d tile=%d tex=%u",
                         (unsigned long long)srcImageBytes,
                         width,
                         height,
                         tile,
                         (unsigned)current_texture_ids[tile]);
            sInvalidTextureUploadCount++;
        }
        return;
    }
    // Re-uploading into an existing texture_id must release the previous image to avoid
    // unbounded leaks when the PC texture cache wraps/reuses nodes. This must be deferred
    // because the GPU may still be sampling from the old image in the current frame.
    Texture& texture_entry = gx2_textures[current_texture_ids[tile]];
    GX2Texture& texture = texture_entry.texture;
    if (sUploadLogCount < 16 && gfx_gx2_should_log_deep()) {
        WHBLogPrintf("gfx: upload[%u] begin lvl=%d tile=%d tex=%u w=%d h=%d bytes=%llu",
                     (unsigned)sUploadLogCount,
                     (int)gCurrLevelNum,
                     tile,
                     (unsigned)current_texture_ids[tile],
                     width,
                     height,
                     (unsigned long long)srcImageBytes);
    }
    if (texture.surface.image != nullptr) {
        sRetiredTextureImages.push_back({ texture.surface.image, frame_count });
        texture.surface.image = nullptr;
        texture.surface.imageSize = 0;
        texture_entry.texture_uploaded = false;
    }
    memset(&texture, 0, sizeof(texture));

    texture.surface.use       = GX2_SURFACE_USE_TEXTURE;
    texture.surface.dim       = GX2_SURFACE_DIM_TEXTURE_2D;
    texture.surface.width     = width;
    texture.surface.height    = height;
    texture.surface.depth     = 1;
    texture.surface.mipLevels = 1;
    texture.surface.format    = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    texture.surface.aa        = GX2_AA_MODE1X;
    texture.surface.tileMode  = GX2_TILE_MODE_LINEAR_ALIGNED;
    texture.viewFirstMip      = 0;
    texture.viewNumMips       = 1;
    texture.viewFirstSlice    = 0;
    texture.viewNumSlices     = 1;
    texture.surface.swizzle   = 0;
    texture.surface.alignment = 0;
    texture.surface.pitch     = 0;

    for (uint32_t i = 0; i < 13; i++)
        texture.surface.mipLevelOffset[i] = 0;

    texture.viewFirstMip   = 0;
    texture.viewNumMips    = 1;
    texture.viewFirstSlice = 0;
    texture.viewNumSlices  = 1;
    texture.compMap        = 0x00010203;

    for (uint32_t i = 0; i < 5; i++)
        texture.regs[i] = 0;

    GX2CalcSurfaceSizeAndAlignment(&texture.surface);    if (texture.surface.alignment == 0 ||
        texture.surface.imageSize == 0 ||
        (uint64_t)texture.surface.imageSize > kGx2TexMaxBytes) {
        if (sInvalidTextureUploadCount < 32) {
            WHBLogPrintf("gfx: reject gx2 surface w=%d h=%d imageSize=%u align=%u tile=%d tex=%u",
                         width,
                         height,
                         (unsigned)texture.surface.imageSize,
                         (unsigned)texture.surface.alignment,
                         tile,
                         (unsigned)current_texture_ids[tile]);
            sInvalidTextureUploadCount++;
        }
        return;
    }
    GX2InitTextureRegs(&texture);
    texture.surface.image = memalign(texture.surface.alignment, texture.surface.imageSize);
    if (texture.surface.image == nullptr) {
        WHBLogPrintf("gfx: texture allocation failed, size=%u align=%u",
                     (unsigned)texture.surface.imageSize,
                     (unsigned)texture.surface.alignment);
        return;
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, texture.surface.image, texture.surface.imageSize);
    GX2Surface surf = texture.surface;
    surf.tileMode = GX2_TILE_MODE_LINEAR_SPECIAL;
    GX2CalcSurfaceSizeAndAlignment(&surf);    if (surf.alignment == 0 ||
        surf.imageSize == 0 ||
        (uint64_t)surf.imageSize > kGx2TexMaxBytes) {
        if (sInvalidTextureUploadCount < 32) {
            WHBLogPrintf("gfx: reject gx2 src surface w=%d h=%d imageSize=%u align=%u tile=%d tex=%u",
                         width,
                         height,
                         (unsigned)surf.imageSize,
                         (unsigned)surf.alignment,
                         tile,
                         (unsigned)current_texture_ids[tile]);
            sInvalidTextureUploadCount++;
        }
        free(texture.surface.image);
        texture.surface.image = nullptr;
        texture.surface.imageSize = 0;
        return;
    }

    surf.image = (void *)rgba32_buf;
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, (void *)rgba32_buf, (uint32_t)srcImageBytes);
    GX2CopySurface(&surf, 0, 0, &texture.surface, 0, 0);
    if (sUploadLogCount < 16 && gfx_gx2_should_log_deep()) {
        WHBLogPrintf("gfx: upload[%u] copied lvl=%d tile=%d tex=%u",
                     (unsigned)sUploadLogCount,
                     (int)gCurrLevelNum,
                     tile,
                     (unsigned)current_texture_ids[tile]);
    }

    if (current_shader_program) {
        const uint32_t sampler_loc = current_shader_program->samplers_location[tile];
        if (sampler_loc < 32) {
            GX2SetPixelTexture(&texture, sampler_loc);
            if (texture_entry.sampler_set) {
                GX2SetPixelSampler(&texture_entry.sampler, sampler_loc);
            }
        } else {
            gfx_gx2_log_invalid_sampler_state("upload_set_pixel_texture", tile, current_texture_ids[tile], sampler_loc);
        }
    }

    gx2_textures[current_texture_ids[tile]].texture_uploaded = true;
    if (sUploadLogCount < 16 && gfx_gx2_should_log_deep()) {
        WHBLogPrintf("gfx: upload[%u] done lvl=%d tile=%d tex=%u",
                     (unsigned)sUploadLogCount,
                     (int)gCurrLevelNum,
                     tile,
                     (unsigned)current_texture_ids[tile]);
        sUploadLogCount++;
    }
    //WHBLogPrint("Texture set.");
}

static GX2TexClampMode gfx_cm_to_gx2(uint32_t val)
{
    if (val & G_TX_CLAMP)
        return GX2_TEX_CLAMP_MODE_CLAMP;
    else if (val & G_TX_MIRROR)
        return GX2_TEX_CLAMP_MODE_MIRROR;
    else
        return GX2_TEX_CLAMP_MODE_WRAP;
}

static void gfx_gx2_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt)
{
    if (!gfx_gx2_is_valid_tile(tile) || !gfx_gx2_is_valid_texture_id(current_texture_ids[tile])) {
        gfx_gx2_log_invalid_sampler_state("set_sampler_parameters", tile, current_texture_ids[tile], 0);
        return;
    }

    current_tile = tile;

    uint32_t texture_id = current_texture_ids[tile];
    GX2Sampler* sampler = &gx2_textures[texture_id].sampler;
    if (gfx_gx2_is_arena_lightmap_shader(current_shader_program) && sArenaLightmapSamplerLogCount < 12) {
        WHBLogPrintf(
            "gfx: arena-lightmap sampler[%u] tile=%d tex=%u loc=%u linear=%u cms=0x%x cmt=0x%x",
            (unsigned)sArenaLightmapSamplerLogCount,
            tile,
            (unsigned)texture_id,
            (unsigned)current_shader_program->samplers_location[tile],
            (unsigned)linear_filter,
            (unsigned)cms,
            (unsigned)cmt);
        sArenaLightmapSamplerLogCount++;
    }
    if (sSamplerLogCount < 24 && gfx_gx2_should_log_deep()) {
        WHBLogPrintf("gfx: sampler[%u] begin lvl=%d tile=%d tex=%u linear=%u cms=0x%x cmt=0x%x shader=0x%016llx",
                     (unsigned)sSamplerLogCount,
                     (int)gCurrLevelNum,
                     tile,
                     (unsigned)texture_id,
                     (unsigned)linear_filter,
                     (unsigned)cms,
                     (unsigned)cmt,
                     (unsigned long long)(current_shader_program != nullptr ? current_shader_program->shader_id : 0ULL));
    }
    if (gCurrLevelNum == 16 && tile == 1) {
        WHBLogPrintf("gfx: tile1 sampler begin tex=%u linear=%u cms=0x%x cmt=0x%x shader=0x%016llx",
                     (unsigned)texture_id,
                     (unsigned)linear_filter,
                     (unsigned)cms,
                     (unsigned)cmt,
                     (unsigned long long)(current_shader_program != nullptr ? current_shader_program->shader_id : 0ULL));
    }
    if (sSamplerLogCount < 24 && gfx_gx2_should_log_deep()) {
        WHBLogPrintf("gfx: sampler[%u] state tile=%d set=%u linear=%u cms=0x%x cmt=0x%x",
                     (unsigned)sSamplerLogCount,
                     tile,
                     (unsigned)gx2_textures[texture_id].sampler_set,
                     (unsigned)linear_filter,
                     (unsigned)cms,
                     (unsigned)cmt);
    }
    if (sSamplerLogCount < 24 && gfx_gx2_should_log_deep()) {
        WHBLogPrintf("gfx: sampler[%u] init-path tile=%d tex=%u", (unsigned)sSamplerLogCount, tile, (unsigned)texture_id);
    }
    if (sSamplerLogCount < 24 && gfx_gx2_should_log_deep()) {
        WHBLogPrintf("gfx: sampler[%u] pre-tex-write tile=%d tex=%u", (unsigned)sSamplerLogCount, tile, (unsigned)texture_id);
    }
    gx2_textures[texture_id].sampler_set = true;
    if (sSamplerLogCount < 24 && gfx_gx2_should_log_deep()) {
        WHBLogPrintf("gfx: sampler[%u] pre-init tile=%d tex=%u", (unsigned)sSamplerLogCount, tile, (unsigned)texture_id);
    }
    GX2InitSampler(sampler, GX2_TEX_CLAMP_MODE_CLAMP,
                   linear_filter ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT);
    GX2InitSamplerClamping(sampler, gfx_cm_to_gx2(cms), gfx_cm_to_gx2(cmt), GX2_TEX_CLAMP_MODE_WRAP);
    if (sSamplerLogCount < 24 && gfx_gx2_should_log_deep()) {
        WHBLogPrintf("gfx: sampler[%u] post-init tile=%d tex=%u", (unsigned)sSamplerLogCount, tile, (unsigned)texture_id);
    }
    if (sSamplerLogCount < 24 && gfx_gx2_should_log_deep()) {
        WHBLogPrintf("gfx: sampler[%u] post-clamp tile=%d tex=%u", (unsigned)sSamplerLogCount, tile, (unsigned)texture_id);
    }

    if (current_shader_program) {
        const uint32_t sampler_loc = current_shader_program->samplers_location[tile];
        if (sampler_loc < 32) {
            if (sSamplerLogCount < 24 && gfx_gx2_should_log_deep()) {
                WHBLogPrintf("gfx: sampler[%u] bind lvl=%d tile=%d tex=%u loc=%u",
                             (unsigned)sSamplerLogCount,
                             (int)gCurrLevelNum,
                             tile,
                             (unsigned)texture_id,
                             (unsigned)sampler_loc);
            }
            GX2SetPixelSampler(sampler, sampler_loc);
            if (sSamplerLogCount < 24 && gfx_gx2_should_log_deep()) {
                WHBLogPrintf("gfx: sampler[%u] done lvl=%d tile=%d tex=%u loc=%u",
                             (unsigned)sSamplerLogCount,
                             (int)gCurrLevelNum,
                             tile,
                             (unsigned)texture_id,
                             (unsigned)sampler_loc);
                sSamplerLogCount++;
            }
        } else {
            gfx_gx2_log_invalid_sampler_state("set_sampler", tile, texture_id, sampler_loc);
        }
    } else if (sNullShaderSamplerSetCount < 10) {
        WHBLogPrintf("gfx: set_sampler_parameters without active shader tile=%d tex=%u", tile, texture_id);
        sNullShaderSamplerSetCount++;
    }
}

static void gfx_gx2_set_depth_test(bool depth_test)
{
    current_depth_test = depth_test;
    GX2SetDepthOnlyControl(current_depth_test, current_depth_write, current_depth_compare);
}

static void gfx_gx2_set_depth_mask(bool z_upd)
{
    current_depth_write = z_upd;
    GX2SetDepthOnlyControl(current_depth_test, current_depth_write, current_depth_compare);
}

static void gfx_gx2_set_zmode_decal(bool zmode_decal)
{
    if (zmode_decal)
    {
        GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, TRUE,
                             GX2_POLYGON_MODE_TRIANGLE, GX2_POLYGON_MODE_TRIANGLE,
                             TRUE, TRUE, FALSE);

        GX2SetPolygonOffset(-2.0f, -2.0f, -2.0f, -2.0f, 0.0f );
    }
    else
    {
        GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, FALSE,
                             GX2_POLYGON_MODE_TRIANGLE, GX2_POLYGON_MODE_TRIANGLE,
                             FALSE, FALSE, FALSE);

        GX2SetPolygonOffset( 0.0f,  0.0f,  0.0f,  0.0f, 0.0f );
    }
}

static void gfx_gx2_set_viewport(int x, int y, int width, int height)
{
    GX2SetViewport(x, g_window_height - y - height, width, height, 0.0f, 1.0f);
    current_height = height;
}

static void gfx_gx2_set_scissor(int x, int y, int width, int height)
{
    GX2SetScissor(x, g_window_height - y - height, width, height);
}

static void gfx_gx2_set_use_alpha(bool use_alpha)
{
    if (use_alpha)
    {
        GX2SetBlendControl(GX2_RENDER_TARGET_0,
                           GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA,
                           GX2_BLEND_COMBINE_MODE_ADD, FALSE,
                           GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ZERO,
                           GX2_BLEND_COMBINE_MODE_ADD);

        GX2SetColorControl(GX2_LOGIC_OP_COPY, 1, FALSE, TRUE);
    }
    else
    {
        GX2SetBlendControl(GX2_RENDER_TARGET_0,
                           GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO,
                           GX2_BLEND_COMBINE_MODE_ADD, FALSE,
                           GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ZERO,
                           GX2_BLEND_COMBINE_MODE_ADD);

        GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, FALSE, TRUE);
    }
}

static void gfx_gx2_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris)
{
    if (!current_shader_program)
        return;

    if (!sLoggedFirstDrawCall) {
        sLoggedFirstDrawCall = true;
        WHBLogPrintf("gfx: first draw shader=0x%016llx tris=%u stride_f=%u pre=%u",
                     (unsigned long long)current_shader_program->shader_id,
                     (unsigned)buf_vbo_num_tris,
                     (unsigned)current_shader_program->num_floats,
                     (unsigned)current_shader_program->is_precompiled);
    }

    const bool log_arena_draw = (gCurrLevelNum == 55 && sArenaDrawLogCount < 20);
    if (log_arena_draw) {
        const float x = (buf_vbo_len >= 1) ? buf_vbo[0] : 0.0f;
        const float y = (buf_vbo_len >= 2) ? buf_vbo[1] : 0.0f;
        const float z = (buf_vbo_len >= 3) ? buf_vbo[2] : 0.0f;
        WHBLogPrintf(
            "gfx: arena draw[%u] shader=0x%016llx tris=%u stride_f=%u inputs=%u tex=%u/%u light=%u pos=%f,%f,%f",
            (unsigned)sArenaDrawLogCount,
            (unsigned long long)current_shader_program->shader_id,
            (unsigned)buf_vbo_num_tris,
            (unsigned)current_shader_program->num_floats,
            (unsigned)current_shader_program->num_inputs,
            (unsigned)current_shader_program->used_textures[0],
            (unsigned)current_shader_program->used_textures[1],
            (unsigned)current_shader_program->used_lightmap,
            x, y, z
        );
        sArenaDrawLogCount++;
    }
    if (gfx_gx2_is_arena_lightmap_shader(current_shader_program) && sArenaLightmapDrawStateLogCount < 8) {
        const uint32_t tex0 = current_texture_ids[0];
        const uint32_t tex1 = current_texture_ids[1];
        const Texture *t0 = gfx_gx2_is_valid_texture_id(tex0) ? &gx2_textures[tex0] : nullptr;
        const Texture *t1 = gfx_gx2_is_valid_texture_id(tex1) ? &gx2_textures[tex1] : nullptr;
        WHBLogPrintf(
            "gfx: arena-lightmap draw[%u] tex0=%u up=%u size=%ux%u sampLoc=%u tex1=%u up=%u size=%ux%u sampLoc=%u tris=%u stride=%u",
            (unsigned)sArenaLightmapDrawStateLogCount,
            (unsigned)tex0,
            (unsigned)(t0 != nullptr && t0->texture_uploaded),
            (unsigned)(t0 != nullptr ? t0->texture.surface.width : 0),
            (unsigned)(t0 != nullptr ? t0->texture.surface.height : 0),
            (unsigned)current_shader_program->samplers_location[0],
            (unsigned)tex1,
            (unsigned)(t1 != nullptr && t1->texture_uploaded),
            (unsigned)(t1 != nullptr ? t1->texture.surface.width : 0),
            (unsigned)(t1 != nullptr ? t1->texture.surface.height : 0),
            (unsigned)current_shader_program->samplers_location[1],
            (unsigned)buf_vbo_num_tris,
            (unsigned)current_shader_program->num_floats);
        sArenaLightmapDrawStateLogCount++;
    }
    if (gCurrLevelNum == 55 && sArenaPrepLogCount < 3) {
        WHBLogPrintf("gfx: arena step[%u] post-drawlog len=%u tris=%u",
                     (unsigned)sArenaPrepLogCount,
                     (unsigned)buf_vbo_len,
                     (unsigned)buf_vbo_num_tris);
    }

    if (buf_vbo_len == 0 || buf_vbo_len > (1u << 20)) {
    if (gCurrLevelNum == 55 && sArenaPrepLogCount < 12) {
        WHBLogPrintf("gfx: arena prep[%u] reject len=%u", (unsigned)sArenaPrepLogCount, (unsigned)buf_vbo_len);
    }
        return;
    }
    if (gCurrLevelNum == 55 && sArenaPrepLogCount < 3) {
        WHBLogPrintf("gfx: arena step[%u] post-lencheck", (unsigned)sArenaPrepLogCount);
    }

    size_t vbo_len = sizeof(float) * buf_vbo_len;
    if (gCurrLevelNum == 55 && sArenaPrepLogCount < 3) {
        WHBLogPrintf("gfx: arena step[%u] post-vbolen bytes=%u",
                     (unsigned)sArenaPrepLogCount,
                     (unsigned)vbo_len);
    }
    if (gCurrLevelNum == 55 && sArenaPrepLogCount < 12) {
        WHBLogPrintf("gfx: arena prep[%u] len=%u bytes=%u",
                     (unsigned)sArenaPrepLogCount,
                     (unsigned)buf_vbo_len,
                     (unsigned)vbo_len);
    }
    float* new_vbo = static_cast<float*>(memalign(0x40, vbo_len));
    if (new_vbo == nullptr) {
        return;
    }
    vbo_array.push_back(new_vbo);
    if (gCurrLevelNum == 55 && sArenaPrepLogCount < 12) {
        WHBLogPrintf("gfx: arena prep[%u] alloc len=%u bytes=%u ptr=%p vbo_size=%u",
                     (unsigned)sArenaPrepLogCount,
                     (unsigned)buf_vbo_len,
                     (unsigned)vbo_len,
                     new_vbo,
                     (unsigned)vbo_array.size());
    }
    sTrackedVboPtrs.insert(new_vbo);
    if (gCurrLevelNum == 55 && sArenaPrepLogCount < 12) {
        WHBLogPrintf("gfx: arena prep[%u] upload source ready", (unsigned)sArenaPrepLogCount);
    }
    memcpy(new_vbo, buf_vbo, vbo_len);
    if (gCurrLevelNum == 55 && sArenaPrepLogCount < 12) {
        WHBLogPrintf("gfx: arena prep[%u] memcpy done", (unsigned)sArenaPrepLogCount);
    }

    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, new_vbo, vbo_len);
    if (gCurrLevelNum == 55 && sArenaPrepLogCount < 12) {
        WHBLogPrintf("gfx: arena prep[%u] invalidate done", (unsigned)sArenaPrepLogCount);
    }

    if ((gCurrLevelNum == 16 || gCurrLevelNum == 55) && sVboDumpLogCount < 20) {
        gfx_gx2_log_first_vbo_words(buf_vbo, buf_vbo_len, current_shader_program->num_floats);
    }
    gfx_gx2_log_target_menu_vertex(current_shader_program, buf_vbo, buf_vbo_len);
    gfx_gx2_log_draw_state(current_shader_program, buf_vbo_len, buf_vbo_num_tris, vbo_len);

    GX2SetAttribBuffer(0, vbo_len, sizeof(float) * current_shader_program->num_floats, new_vbo);
    if (gCurrLevelNum == 55 && sArenaPrepLogCount < 12) {
        WHBLogPrintf("gfx: arena prep[%u] attrib done stride=%u",
                     (unsigned)sArenaPrepLogCount,
                     (unsigned)(sizeof(float) * current_shader_program->num_floats));
        sArenaPrepLogCount++;
    }
    if (gCurrLevelNum == 55 && sArenaDrawSubmitLogCount < 12) {
        WHBLogPrintf("gfx: arena submit-begin[%u] shader=0x%016llx tris=%u",
                     (unsigned)sArenaDrawSubmitLogCount,
                     (unsigned long long)current_shader_program->shader_id,
                     (unsigned)buf_vbo_num_tris);
    }
    if (gCurrLevelNum == 16 && sMenuDrawSubmitLogCount < 24) {
        WHBLogPrintf("gfx: menu submit-begin[%u] shader=0x%016llx tris=%u stride_f=%u tex=%u,%u",
                     (unsigned)sMenuDrawSubmitLogCount,
                     (unsigned long long)current_shader_program->shader_id,
                     (unsigned)buf_vbo_num_tris,
                     (unsigned)current_shader_program->num_floats,
                     (unsigned)current_shader_program->used_textures[0],
                     (unsigned)current_shader_program->used_textures[1]);
    }
    if (!sLoggedFirstDrawSubmitBegin) {
        sLoggedFirstDrawSubmitBegin = true;
        WHBLogPrint("gfx: first draw submit begin");
    }
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 3 * buf_vbo_num_tris, 0, 1);
    if (gCurrLevelNum == 55 && sArenaDrawSubmitLogCount < 12) {
        WHBLogPrintf("gfx: arena submit-end[%u] shader=0x%016llx",
                     (unsigned)sArenaDrawSubmitLogCount,
                     (unsigned long long)current_shader_program->shader_id);
        sArenaDrawSubmitLogCount++;
    }
    if (!sLoggedFirstDrawSubmitEnd) {
        sLoggedFirstDrawSubmitEnd = true;
        WHBLogPrint("gfx: first draw submit end");
    }
    if (gCurrLevelNum == 16 && sMenuDrawSubmitLogCount < 24) {
        WHBLogPrintf("gfx: menu submit-end[%u] shader=0x%016llx",
                     (unsigned)sMenuDrawSubmitLogCount,
                     (unsigned long long)current_shader_program->shader_id);
        sMenuDrawSubmitLogCount++;
    }
}

static void gfx_gx2_init(void)
{
    static bool sReservedContainers = false;
    if (!sReservedContainers) {
        vbo_array.reserve(4096);
        vbo_array_prev.reserve(4096);
        sTrackedVboPtrs.reserve(8192);
        sReservedContainers = true;
    }
}

static void gfx_gx2_on_resize(void)
{
}

static void gfx_gx2_start_frame(void)
{
    frame_count++;
    if (gCurrLevelNum == 55 && sArenaFrameLogCount < 12) {
        WHBLogPrintf("gfx: arena frame[%u] begin frame_count=%u vbo=%u prev_vbo=%u",
                     (unsigned)sArenaFrameLogCount,
                     (unsigned)frame_count,
                     (unsigned)vbo_array.size(),
                     (unsigned)vbo_array_prev.size());
        sArenaFrameLogCount++;
    }
    if (!sRetiredTextureImages.empty()) {
        const uint32_t retire_delay_frames = 8;
        const size_t free_budget = 64;
        size_t freed = 0;
        size_t write_idx = 0;
        const size_t count = sRetiredTextureImages.size();
        for (size_t i = 0; i < count; ++i) {
            RetiredTextureImage entry = sRetiredTextureImages[i];
            if (entry.ptr == nullptr) {
                continue;
            }
            if (freed < free_budget && (uint32_t)(frame_count - entry.retire_frame) >= retire_delay_frames) {
                free(entry.ptr);
                entry.ptr = nullptr;
                freed++;
            }
            if (entry.ptr != nullptr) {
                sRetiredTextureImages[write_idx++] = entry;
            }
        }
        if (write_idx != count) {
            sRetiredTextureImages.resize(write_idx);
        }
        if (sRetiredTextureFreeLogCount < 8 && freed > 0) {
            WHBLogPrintf("gfx: retired frees=%u remaining=%u delay=%u",
                         (unsigned)freed,
                         (unsigned)sRetiredTextureImages.size(),
                         (unsigned)retire_delay_frames);
            sRetiredTextureFreeLogCount++;
        }
        if (sRetiredTextureImages.size() > 4096) {
            // Safety valve for runaway churn: force some reclamation without stalling
            // the frame loop in GX2DrawDone().
            size_t emergency_freed = 0;
            while (!sRetiredTextureImages.empty() && emergency_freed < 256) {
                RetiredTextureImage entry = sRetiredTextureImages.back();
                sRetiredTextureImages.pop_back();
                if (entry.ptr != nullptr) {
                    free(entry.ptr);
                }
                emergency_freed++;
            }
            if (sRetiredTextureFreeLogCount < 8) {
                WHBLogPrintf("gfx: retired emergency frees=%u remaining=%u",
                             (unsigned)emergency_freed,
                             (unsigned)sRetiredTextureImages.size());
                sRetiredTextureFreeLogCount++;
            }
        }
    }
    // Keep one-frame latency before freeing transient VBO uploads.
    // Use a per-frame free budget to avoid multi-second stalls when overlays
    // temporarily generate very large VBO backlogs (e.g. stacked pause UIs).
    const size_t free_budget = 2048;
    gfx_gx2_release_vbo_budget(vbo_array_prev, free_budget);
    if (vbo_array_prev.empty()) {
        vbo_array_prev.swap(vbo_array);
    } else if (!vbo_array.empty()) {
        vbo_array_prev.insert(vbo_array_prev.end(), vbo_array.begin(), vbo_array.end());
        vbo_array.clear();
    }
}

static void gfx_gx2_end_frame(void)
{
}

static void gfx_gx2_finish_render(void)
{
    GX2Flush();
}

static void gfx_gx2_shutdown(void)
{
    gfx_gx2_free_vbo();
    gfx_gx2_free();
}

extern "C" void gfx_gx2_free_vbo(void)
{
    gfx_gx2_release_vbo_list(vbo_array);
    gfx_gx2_release_vbo_list(vbo_array_prev);
    sTrackedVboPtrs.clear();
}

extern "C" void gfx_gx2_free(void)
{
    // Free our textures and shaders
    for (uint32_t i = 0; i < gx2_texture_count; i++)
    {
        Texture& texture = gx2_textures[i];
        if (texture.texture_uploaded)
            free(texture.texture.surface.image);
    }

    for (uint32_t i = 0; i < shader_program_pool_size; i++)
    {
        if (shader_program_pool[i].is_precompiled)
            WHBGfxFreeShaderGroup(&shader_program_pool[i].whb_group);

        else
            gx2FreeShaderGroup(&shader_program_pool[i].gen_group);
    }

    gx2_texture_count = 0;
    shader_program_pool_size = 0;

    while (!sRetiredTextureImages.empty()) {
        if (sRetiredTextureImages.back().ptr != nullptr) {
            free(sRetiredTextureImages.back().ptr);
        }
        sRetiredTextureImages.pop_back();
    }
}

struct GfxRenderingAPI gfx_gx2_api = {
    gfx_gx2_z_is_from_0_to_1,
    gfx_gx2_unload_shader,
    gfx_gx2_load_shader,
    gfx_gx2_create_and_load_new_shader,
    gfx_gx2_lookup_shader,
    gfx_gx2_shader_get_info,
    gfx_gx2_new_texture,
    gfx_gx2_select_texture,
    gfx_gx2_upload_texture,
    gfx_gx2_set_sampler_parameters,
    gfx_gx2_set_depth_test,
    gfx_gx2_set_depth_mask,
    gfx_gx2_set_zmode_decal,
    gfx_gx2_set_viewport,
    gfx_gx2_set_scissor,
    gfx_gx2_set_use_alpha,
    gfx_gx2_draw_triangles,
    gfx_gx2_init,
    gfx_gx2_on_resize,
    gfx_gx2_start_frame,
    gfx_gx2_end_frame,
    gfx_gx2_finish_render,
    gfx_gx2_shutdown
};

#endif
