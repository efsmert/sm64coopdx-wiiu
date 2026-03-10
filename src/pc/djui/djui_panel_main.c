#include "djui.h"
#include "djui_panel.h"
#include "djui_panel_host.h"
#include "djui_panel_join.h"
#include "djui_panel_options.h"
#include "djui_panel_menu.h"
#include "djui_panel_confirm.h"
#include "pc/controller/controller_sdl.h"
#include "pc/pc_main.h"
#include "pc/update_checker.h"
#ifdef TARGET_WII_U
#include <coreinit/debug.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#endif

extern ALIGNED8 u8 texture_coopdx_logo[];

bool gDjuiPanelMainCreated = false;

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
    snprintf(buffer, sizeof(buffer), "menu_highlight: page=main_menu focus=%s text=\"%s\"\n", focus, safeText);
    if (strcmp(buffer, sLastMenuHighlightLine) == 0) { return; }
    snprintf(sLastMenuHighlightLine, sizeof(sLastMenuHighlightLine), "%s", buffer);
    OSReport("%s", buffer);
}
#else
static void djui_menu_state_logf(UNUSED const char* fmt, ...) { }
static void djui_menu_highlight_logf(UNUSED const char* focus, UNUSED const char* text) { }
#endif

static void djui_panel_main_quit_yes(UNUSED struct DjuiBase* caller) {
    game_exit();
}

static void djui_panel_main_quit(struct DjuiBase* caller) {
    djui_menu_state_logf("menu_state: page=main_menu action=open_quit_confirm\n");
    djui_panel_confirm_create(caller,
                              DLANG(MAIN, QUIT_TITLE),
                              DLANG(MAIN, QUIT_CONFIRM),
                              djui_panel_main_quit_yes);
}

static void djui_panel_main_open_host(struct DjuiBase* caller) {
    djui_menu_state_logf("menu_state: page=main_menu action=open_host\n");
    djui_panel_host_create(caller);
}

static void djui_panel_main_open_join(struct DjuiBase* caller) {
    djui_menu_state_logf("menu_state: page=main_menu action=open_join\n");
    djui_panel_join_create(caller);
}

static void djui_panel_main_open_options(struct DjuiBase* caller) {
    djui_menu_state_logf("menu_state: page=main_menu action=open_options\n");
    djui_panel_options_create(caller);
}

static void djui_panel_main_hover_host(UNUSED struct DjuiBase* base) {
    djui_menu_state_logf("menu_state: page=main_menu focus=host\n");
    djui_menu_highlight_logf("host", DLANG(MAIN, HOST));
}

static void djui_panel_main_hover_join(UNUSED struct DjuiBase* base) {
    djui_menu_state_logf("menu_state: page=main_menu focus=join\n");
    djui_menu_highlight_logf("join", DLANG(MAIN, JOIN));
}

static void djui_panel_main_hover_options(UNUSED struct DjuiBase* base) {
    djui_menu_state_logf("menu_state: page=main_menu focus=options\n");
    djui_menu_highlight_logf("options", DLANG(MAIN, OPTIONS));
}

static void djui_panel_main_hover_quit(UNUSED struct DjuiBase* base) {
    djui_menu_state_logf("menu_state: page=main_menu focus=quit\n");
    djui_menu_highlight_logf("quit", DLANG(MAIN, QUIT));
}

void djui_panel_main_create(struct DjuiBase* caller) {
#ifdef TARGET_WII_U
    sLastMenuStateLine[0] = '\0';
    sLastMenuHighlightLine[0] = '\0';
#endif
    djui_menu_state_logf("menu_state: page=main_menu opened\n");
    djui_menu_highlight_logf("host", DLANG(MAIN, HOST));
    struct DjuiThreePanel* panel = djui_panel_menu_create(configExCoopTheme ? "\\#ff0800\\SM\\#1be700\\64\\#00b3ff\\EX\n\\#ffef00\\COOP" : "", false);
    {
        struct DjuiBase* body = djui_three_panel_get_body(panel);
        {
            if (!configExCoopTheme) {
                struct DjuiImage* logo = djui_image_create(body, texture_coopdx_logo, 2048, 1024, G_IM_FMT_RGBA, G_IM_SIZ_32b);
                if (configDjuiThemeCenter) {
                    djui_base_set_size(&logo->base, 550, 275);
                } else {
                    djui_base_set_size(&logo->base, 480, 240);
                }
                djui_base_set_alignment(&logo->base, DJUI_HALIGN_CENTER, DJUI_VALIGN_TOP);
                djui_base_set_location_type(&logo->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
                djui_base_set_location(&logo->base, 0, -30);
            }

            struct DjuiButton* button1 = djui_button_create(body, DLANG(MAIN, HOST), DJUI_BUTTON_STYLE_NORMAL, djui_panel_main_open_host);
            if (!configExCoopTheme) { djui_base_set_location(&button1->base, 0, -30); }
            djui_cursor_input_controlled_center(&button1->base);
            djui_interactable_hook_hover(&button1->base, djui_panel_main_hover_host, NULL);

            struct DjuiButton* button2 = djui_button_create(body, DLANG(MAIN, JOIN), DJUI_BUTTON_STYLE_NORMAL, djui_panel_main_open_join);
            if (!configExCoopTheme) { djui_base_set_location(&button2->base, 0, -30); }
            djui_interactable_hook_hover(&button2->base, djui_panel_main_hover_join, NULL);
            struct DjuiButton* button3 = djui_button_create(body, DLANG(MAIN, OPTIONS), DJUI_BUTTON_STYLE_NORMAL, djui_panel_main_open_options);
            if (!configExCoopTheme) { djui_base_set_location(&button3->base, 0, -30); }
            djui_interactable_hook_hover(&button3->base, djui_panel_main_hover_options, NULL);
            struct DjuiButton* button4 = djui_button_create(body, DLANG(MAIN, QUIT), DJUI_BUTTON_STYLE_BACK, djui_panel_main_quit);
            if (!configExCoopTheme) { djui_base_set_location(&button4->base, 0, -30); }
            djui_interactable_hook_hover(&button4->base, djui_panel_main_hover_quit, NULL);
        }

        // these two cannot co-exist for some reason
        if (gUpdateMessage) {
            struct DjuiText* message = djui_text_create(&panel->base, DLANG(NOTIF, UPDATE_AVAILABLE));
            djui_base_set_size_type(&message->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
            djui_base_set_size(&message->base, 1.0f, 1.0f);
            djui_base_set_color(&message->base, 255, 255, 160, 255);
            djui_text_set_alignment(message, DJUI_HALIGN_CENTER, DJUI_VALIGN_BOTTOM);
        } else {
          #ifdef COMPILE_TIME
            struct DjuiText* version = djui_text_create(&panel->base, get_version_with_build_date());
          #else
            struct DjuiText* version = djui_text_create(&panel->base, get_version());
          #endif
            djui_base_set_size_type(&version->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
            djui_base_set_size(&version->base, 1.0f, 1.0f);
            djui_base_set_color(&version->base, 50, 50, 50, 255);
            djui_text_set_alignment(version, DJUI_HALIGN_RIGHT, DJUI_VALIGN_BOTTOM);
        }
    }

    djui_panel_add(caller, panel, NULL);
    gInteractableOverridePad = true;
    gDjuiPanelMainCreated = true;
}
