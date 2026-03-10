#include "djui.h"
#include "djui_panel.h"
#include "djui_panel_menu.h"
#include "djui_panel_join_lobbies.h"
#include "djui_panel_join_private.h"
#include "djui_panel_join_direct.h"
#include "pc/network/network.h"
#include "pc/utils/misc.h"
#include "pc/update_checker.h"
#ifdef TARGET_WII_U
#include <coreinit/debug.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#endif

#ifdef COOPNET
#ifdef TARGET_WII_U
#define DJUI_MENU_STATE_BUFSZ 512
static char sLastMenuStateLine[DJUI_MENU_STATE_BUFSZ] = "";
static char sLastMenuHighlightLine[DJUI_MENU_STATE_BUFSZ] = "";
static void djui_menu_state_logf(const char* fmt, ...) {
    char buffer[DJUI_MENU_STATE_BUFSZ];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (strcmp(buffer, sLastMenuStateLine) == 0) { return; }
    snprintf(sLastMenuStateLine, sizeof(sLastMenuStateLine), "%s", buffer);
    OSReport("%s", buffer);
}

static void djui_menu_state_sanitize_text(const char* text, char* out, size_t outSize) {
    if (outSize == 0) { return; }
    if (text == NULL) { out[0] = '\0'; return; }
    size_t j = 0;
    for (size_t i = 0; text[i] != '\0' && j + 1 < outSize; i++) {
        char c = text[i];
        if (c == '"' || c == '\n' || c == '\r') { c = ' '; }
        out[j++] = c;
    }
    out[j] = '\0';
}

static void djui_menu_highlight_logf(const char* focus, const char* text) {
    char buffer[DJUI_MENU_STATE_BUFSZ];
    char safeText[128];
    djui_menu_state_sanitize_text(text, safeText, sizeof(safeText));
    snprintf(buffer, sizeof(buffer), "menu_highlight: page=join_root focus=%s text=\"%s\"\n", focus, safeText);
    if (strcmp(buffer, sLastMenuHighlightLine) == 0) { return; }
    snprintf(sLastMenuHighlightLine, sizeof(sLastMenuHighlightLine), "%s", buffer);
    OSReport("%s", buffer);
}
#else
static void djui_menu_state_logf(UNUSED const char* fmt, ...) { }
static void djui_menu_highlight_logf(UNUSED const char* focus, UNUSED const char* text) { }
#endif

static void djui_panel_join_hover_public_lobbies(UNUSED struct DjuiBase* base) {
    djui_menu_state_logf("menu_state: page=join_root focus=public_lobbies\n");
    djui_menu_highlight_logf("public_lobbies", DLANG(JOIN, PUBLIC_LOBBIES));
}

static void djui_panel_join_hover_private_lobbies(UNUSED struct DjuiBase* base) {
    djui_menu_state_logf("menu_state: page=join_root focus=private_lobbies\n");
    djui_menu_highlight_logf("private_lobbies", DLANG(JOIN, PRIVATE_LOBBIES));
}

static void djui_panel_join_hover_direct(UNUSED struct DjuiBase* base) {
    djui_menu_state_logf("menu_state: page=join_root focus=direct\n");
    djui_menu_highlight_logf("direct", DLANG(JOIN, DIRECT));
}

static void djui_panel_join_hover_back(UNUSED struct DjuiBase* base) {
    djui_menu_state_logf("menu_state: page=join_root focus=back\n");
    djui_menu_highlight_logf("back", DLANG(MENU, BACK));
}

static void djui_panel_join_public_lobbies(struct DjuiBase* caller) {
    djui_menu_state_logf("menu_state: page=join_root action=open_public_lobbies\n");
    djui_panel_join_lobbies_create(caller, "");
}
#endif

void djui_panel_join_create(struct DjuiBase* caller) {
#ifndef COOPNET
    djui_panel_join_direct_create(caller);
#else
    struct DjuiThreePanel* panel = djui_panel_menu_create(DLANG(JOIN, JOIN_TITLE), false);
    struct DjuiBase* body = djui_three_panel_get_body(panel);
#ifdef TARGET_WII_U
    sLastMenuStateLine[0] = '\0';
    sLastMenuHighlightLine[0] = '\0';
#endif
    djui_menu_state_logf("menu_state: page=join_root opened\n");
    djui_menu_highlight_logf("public_lobbies", DLANG(JOIN, PUBLIC_LOBBIES));
    {
        struct DjuiButton* publicLobbies = djui_button_create(body, DLANG(JOIN, PUBLIC_LOBBIES), DJUI_BUTTON_STYLE_NORMAL, djui_panel_join_public_lobbies);
        djui_interactable_hook_hover(&publicLobbies->base, djui_panel_join_hover_public_lobbies, NULL);
        struct DjuiButton* privateLobbies = djui_button_create(body, DLANG(JOIN, PRIVATE_LOBBIES), DJUI_BUTTON_STYLE_NORMAL, djui_panel_join_private_create);
        djui_interactable_hook_hover(&privateLobbies->base, djui_panel_join_hover_private_lobbies, NULL);
        struct DjuiButton* direct = djui_button_create(body, DLANG(JOIN, DIRECT), DJUI_BUTTON_STYLE_NORMAL, djui_panel_join_direct_create);
        djui_interactable_hook_hover(&direct->base, djui_panel_join_hover_direct, NULL);
        struct DjuiButton* back = djui_button_create(body, DLANG(MENU, BACK), DJUI_BUTTON_STYLE_BACK, djui_panel_menu_back);
        djui_interactable_hook_hover(&back->base, djui_panel_join_hover_back, NULL);
    }

    if (gUpdateMessage) {
        struct DjuiText* message = djui_text_create(&panel->base, DLANG(NOTIF, UPDATE_AVAILABLE));
        djui_base_set_size_type(&message->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
        djui_base_set_size(&message->base, 1.0f, 1.0f);
        djui_base_set_color(&message->base, 255, 255, 160, 255);
        djui_text_set_alignment(message, DJUI_HALIGN_CENTER, DJUI_VALIGN_BOTTOM);
    }

    djui_panel_add(caller, panel, NULL);
#endif
}
