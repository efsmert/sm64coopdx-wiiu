#include "pc/nametags.h"
#include "pc/network/network_player.h"
#include "game/object_helpers.h"
#include "behavior_data.h"
#include "pc/djui/djui_hud_utils.h"
#include "engine/math_util.h"
#include "game/obj_behaviors.h"
#include "game/camera.h"
#include "pc/lua/utils/smlua_misc_utils.h"
#include "pc/lua/smlua_hooks.h"
#include <string.h>

#define FADE_SCALE 4.f

struct StateExtras {
    Vec3f prevPos;
    f32 prevScale;
    char cachedRawName[MAX_CONFIG_STRING];
    char cachedName[MAX_CONFIG_STRING];
    f32 cachedMeasure;
    bool nameCached;
    bool inited;
};
static struct StateExtras sStateExtras[MAX_PLAYERS];

void name_without_hex(char* input) {
    s32 i, j;
    bool inSlash = false;
    for (i = j = 0; input[i] != '\0'; i++) {
        if (input[i] == '\\') {
            inSlash = !inSlash;
        } else if (!inSlash) {
            input[j++] = input[i]; // it just works
        }
    }

    input[j] = '\0';
}

void djui_hud_print_outlined_text_interpolated(const char* text, f32 prevX, f32 prevY, f32 prevScale, f32 x, f32 y, f32 scale, u8 r, u8 g, u8 b, u8 a, f32 outlineDarkness) {
    f32 offset = 1 * (scale * 2);
    f32 prevOffset = 1 * (prevScale * 2);

    // render outline
    djui_hud_set_color(r * outlineDarkness, g * outlineDarkness, b * outlineDarkness, a);
    djui_hud_print_text_interpolated(text, prevX - prevOffset, prevY,              prevScale, x - offset, y,          scale);
    djui_hud_print_text_interpolated(text, prevX + prevOffset, prevY,              prevScale, x + offset, y,          scale);
    djui_hud_print_text_interpolated(text, prevX,              prevY - prevOffset, prevScale, x,          y - offset, scale);
    djui_hud_print_text_interpolated(text, prevX,              prevY + prevOffset, prevScale, x,          y + offset, scale);
    // render text
    djui_hud_set_color(r, g, b, a);
    djui_hud_print_text_interpolated(text, prevX, prevY, prevScale, x, y, scale);
    djui_hud_set_color(255, 255, 255, 255);
}

void nametags_render(void) {
    if (gNetworkType == NT_NONE ||
        (!gNametagsSettings.showSelfTag && network_player_connected_count() == 1) ||
        !gNetworkPlayerLocal->currAreaSyncValid ||
        find_object_with_behavior(bhvActSelector) != NULL) {
        return;
    }

    djui_hud_set_resolution(RESOLUTION_N64);
    djui_hud_set_font(FONT_SPECIAL);
    bool hasNametagHooks = smlua_any_event_hooks(HOOK_ON_NAMETAGS_RENDER);

    // The credits viewport does not change per player, so apply it once instead
    // of bouncing the viewport and scissor state for every visible nametag.
    extern Vp *gViewportOverride;
    extern Vp *gViewportClip;
    extern Vp gViewportFullscreen;
    Vp *viewport = gViewportOverride == NULL ? gViewportClip : gViewportOverride;
    if (viewport) {
        make_viewport_clip_rect(viewport);
        gSPViewport(gDisplayListHead++, viewport);
    }

    for (u8 i = gNametagsSettings.showSelfTag ? 0 : 1; i < MAX_PLAYERS; i++) {
        struct MarioState* m = &gMarioStates[i];
        if (!is_player_active(m)) { continue; }
        struct NetworkPlayer* np = &gNetworkPlayers[i];
        if (!np->currAreaSyncValid) { continue; }

        switch (m->action) {
            case ACT_START_CROUCHING:
            case ACT_CROUCHING:
            case ACT_STOP_CROUCHING:
            case ACT_START_CRAWLING:
            case ACT_CRAWLING:
            case ACT_STOP_CRAWLING:
            case ACT_IN_CANNON:
            case ACT_DISAPPEARED:
            continue;
        }

        if (m->marioBodyState->mirrorMario || m->marioBodyState->updateHeadPosTime != gGlobalTimer) { continue; }

        Vec3f pos;
        Vec3f out;
        vec3f_copy(pos, m->marioBodyState->headPos);
        pos[1] += 100;

        extern bool gDjuiHudToWorldCalcViewport;
        gDjuiHudToWorldCalcViewport = false;
        if ((i != 0 || (i == 0 && m->action != ACT_FIRST_PERSON)) &&
            djui_hud_world_pos_to_screen_pos(pos, out)) {

            struct StateExtras* e = &sStateExtras[i];
            const char* name = NULL;
            f32 measureBase = 0.0f;

            if (hasNametagHooks) {
                const char* hookedString = NULL;
                char hookName[MAX_CONFIG_STRING];
                smlua_call_event_hooks(HOOK_ON_NAMETAGS_RENDER, i, pos, &hookedString);
                if (hookedString) {
                    snprintf(hookName, MAX_CONFIG_STRING, "%s", hookedString);
                    name = hookName;
                }
            }

            if (name == NULL) {
                if (!e->nameCached || strncmp(e->cachedRawName, np->name, MAX_CONFIG_STRING) != 0) {
                    snprintf(e->cachedRawName, MAX_CONFIG_STRING, "%s", np->name);
                    snprintf(e->cachedName, MAX_CONFIG_STRING, "%s", np->name);
                    name_without_hex(e->cachedName);
                    e->cachedMeasure = djui_hud_measure_text(e->cachedName);
                    e->nameCached = true;
                }
                name = e->cachedName;
                measureBase = e->cachedMeasure;
            } else {
                measureBase = djui_hud_measure_text(name);
            }
            u8* color = network_get_player_text_color(m->playerIndex);

            f32 scale = -300 / out[2] * djui_hud_get_fov_coeff();
            f32 measure = measureBase * scale * 0.5f;
            out[1] -= 16 * scale;

            u8 alpha = (i == 0 ? 255 : MIN(np->fadeOpacity << 3, 255)) * clamp(FADE_SCALE - scale, 0.f, 1.f);

            if (!e->inited) {
                vec3f_copy(e->prevPos, out);
                e->prevScale = scale;
                e->inited = true;
            }

            djui_hud_print_outlined_text_interpolated(name,
                e->prevPos[0] - measure, e->prevPos[1], e->prevScale,
                       out[0] - measure,        out[1],        scale,
                color[0], color[1], color[2], alpha, 0.25);

            if (i != 0 && gNametagsSettings.showHealth) {
                djui_hud_set_color(255, 255, 255, alpha);
                f32 healthScale = 90 * scale;
                f32 prevHealthScale = 90 * e->prevScale;
                hud_render_power_meter_interpolated(m->health,
                    e->prevPos[0] - (prevHealthScale * 0.5f), e->prevPos[1] - 72 * scale, prevHealthScale, prevHealthScale,
                           out[0] - (    healthScale * 0.5f),        out[1] - 72 * scale,     healthScale,     healthScale
                );
            }

            vec3f_copy(e->prevPos, out);
            e->prevScale = scale;
        }
        gDjuiHudToWorldCalcViewport = true;
    }

    if (viewport) {
        gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 0, BORDER_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT - BORDER_HEIGHT);
        gSPViewport(gDisplayListHead++, &gViewportFullscreen);
    }
}

void nametags_reset(void) {
    for (u8 i = 0; i < MAX_PLAYERS; i++) {
        sStateExtras[i].nameCached = false;
        sStateExtras[i].inited = false;
    }
}
