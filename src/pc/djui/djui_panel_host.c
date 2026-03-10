#include <stdio.h>
#include "djui.h"
#include "djui_panel.h"
#include "djui_panel_menu.h"
#include "djui_panel_host_mods.h"
#include "djui_panel_host_settings.h"
#include "djui_panel_host_save.h"
#include "djui_panel_host_message.h"
#include "djui_panel_rules.h"
#include "game/save_file.h"
#include "pc/network/network.h"
#include "pc/utils/misc.h"
#include "pc/configfile.h"
#include "pc/update_checker.h"
#ifdef TARGET_WII_U
#include <coreinit/debug.h>
#include <stdarg.h>
#include <string.h>
#endif

static struct DjuiRect* sRectPort = NULL;
static struct DjuiInputbox* sInputboxPort = NULL;
#ifdef COOPNET
static struct DjuiRect* sRectPassword = NULL;
static struct DjuiInputbox* sInputboxPassword = NULL;

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
    snprintf(buffer, sizeof(buffer), "menu_highlight: page=host_root focus=%s text=\"%s\"\n", focus, safeText);
    if (strcmp(buffer, sLastMenuHighlightLine) == 0) { return; }
    snprintf(sLastMenuHighlightLine, sizeof(sLastMenuHighlightLine), "%s", buffer);
    OSReport("%s", buffer);
}
#else
static void djui_menu_state_logf(UNUSED const char* fmt, ...) { }
static void djui_menu_highlight_logf(UNUSED const char* focus, UNUSED const char* text) { }
#endif

static void djui_panel_host_hover_network_system(UNUSED struct DjuiBase* base) {
    djui_menu_state_logf("menu_state: page=host_root focus=network_system\n");
    djui_menu_highlight_logf("network_system", DLANG(HOST, NETWORK_SYSTEM));
}

static void djui_panel_host_hover_save_slot(UNUSED struct DjuiBase* base) {
    djui_menu_state_logf("menu_state: page=host_root focus=save_slot\n");
    djui_menu_highlight_logf("save_slot", DLANG(HOST, SAVE_SLOT));
}

static void djui_panel_host_hover_settings(UNUSED struct DjuiBase* base) {
    djui_menu_state_logf("menu_state: page=host_root focus=settings\n");
    djui_menu_highlight_logf("settings", DLANG(HOST, SETTINGS));
}

static void djui_panel_host_hover_mods(UNUSED struct DjuiBase* base) {
    djui_menu_state_logf("menu_state: page=host_root focus=mods\n");
    djui_menu_highlight_logf("mods", DLANG(HOST, MODS));
}

static void djui_panel_host_hover_back(UNUSED struct DjuiBase* base) {
    djui_menu_state_logf("menu_state: page=host_root focus=back\n");
    djui_menu_highlight_logf("back", (gNetworkType == NT_SERVER) ? DLANG(MENU, CANCEL) : DLANG(MENU, BACK));
}

static void djui_panel_host_hover_host(UNUSED struct DjuiBase* base) {
    djui_menu_state_logf("menu_state: page=host_root focus=host\n");
    djui_menu_highlight_logf("host", (gNetworkType == NT_SERVER) ? DLANG(HOST, APPLY) : DLANG(HOST, HOST));
}

static void djui_panel_host_back(struct DjuiBase* caller) {
    djui_menu_state_logf("menu_state: page=host_root action=back\n");
    djui_panel_menu_back(caller);
}

static void djui_panel_host_network_system_change(UNUSED struct DjuiBase* base) {
    djui_menu_state_logf("menu_state: page=host_root action=network_system_change value=%d\n", configNetworkSystem);
    djui_base_set_visible(&sRectPort->base, (configNetworkSystem == NS_SOCKET));
    djui_base_set_visible(&sRectPassword->base, (configNetworkSystem == NS_COOPNET));
    djui_base_set_enabled(&sInputboxPort->base, (configNetworkSystem == NS_SOCKET));
    djui_base_set_enabled(&sInputboxPassword->base, (configNetworkSystem == NS_COOPNET));
}
#endif

static bool djui_panel_host_port_valid(void) {
    char* buffer = sInputboxPort->buffer;
    int port = 0;
    while (*buffer != '\0') {
        if (*buffer < '0' || *buffer > '9') { return false; }
        port *= 10;
        port += (*buffer - '0');
        buffer++;
    }
#if __linux__
    return port >= 1024 && port <= 65535;
#else
    return port <= 65535;
#endif
}

static void djui_panel_host_port_text_change(struct DjuiBase* caller) {
    struct DjuiInputbox* sInputboxPort = (struct DjuiInputbox*)caller;
    if (djui_panel_host_port_valid()) {
        djui_inputbox_set_text_color(sInputboxPort, 0, 0, 0, 255);
    } else {
        djui_inputbox_set_text_color(sInputboxPort, 255, 0, 0, 255);
    }
}

#ifdef COOPNET
static void djui_panel_host_password_text_change(UNUSED struct DjuiBase* caller) {
    snprintf(configPassword, 64, "%s", sInputboxPassword->buffer);
    if (strlen(sInputboxPassword->buffer) >= 64) {
        djui_inputbox_set_text(sInputboxPassword, configPassword);
    }
}
#endif

extern void djui_panel_do_host(bool reconnecting, bool playSound);
static void djui_panel_host_do_host(struct DjuiBase* caller) {
    djui_menu_state_logf("menu_state: page=host_root action=host_lobby\n");
    if (!djui_panel_host_port_valid()) {
        djui_interactable_set_input_focus(&sInputboxPort->base);
        djui_inputbox_select_all(sInputboxPort);
        return;
    }

    // Doesn't let you host if the player limit is not good
    if (configAmountOfPlayers < 1 || configAmountOfPlayers > MAX_PLAYERS) {
        return;
    }

    configHostPort = atoi(sInputboxPort->buffer);

    if (gNetworkType == NT_SERVER) {
        network_rehost_begin();
    } else if (configNetworkSystem == NS_COOPNET || configAmountOfPlayers == 1) {
        network_reset_reconnect_and_rehost();
        djui_panel_do_host(false, true);
    } else {
        djui_panel_host_message_create(caller);
    }
}

void djui_panel_host_create(struct DjuiBase* caller) {
#ifdef TARGET_WII_U
    sLastMenuStateLine[0] = '\0';
    sLastMenuHighlightLine[0] = '\0';
#endif
    djui_menu_state_logf("menu_state: page=host_root opened\n");
    djui_menu_highlight_logf("host", (gNetworkType == NT_SERVER) ? DLANG(HOST, APPLY) : DLANG(HOST, HOST));
    struct DjuiBase* defaultBase = NULL;
    struct DjuiThreePanel* panel = djui_panel_menu_create(
        (gNetworkType == NT_SERVER) ? DLANG(HOST, SERVER_TITLE) : DLANG(HOST, HOST_TITLE),
        false);
    struct DjuiBase* body = djui_three_panel_get_body(panel);
    {
        #ifdef COOPNET
        char* nChoices[] = { DLANG(HOST, DIRECT_CONNECTION), DLANG(HOST, COOPNET) };
        struct DjuiSelectionbox* selectionbox1 = djui_selectionbox_create(body, DLANG(HOST, NETWORK_SYSTEM), nChoices, 2, &configNetworkSystem, djui_panel_host_network_system_change);
        djui_interactable_hook_hover(&selectionbox1->base, djui_panel_host_hover_network_system, NULL);
        if (gNetworkType == NT_SERVER) {
            djui_base_set_enabled(&selectionbox1->base, false);
        }
        #endif

        struct DjuiRect* rect1 = djui_rect_container_create(body, 32);
        {
            sRectPort = djui_rect_container_create(&rect1->base, 32);
            djui_base_set_location(&sRectPort->base, 0, 0);
            djui_base_set_visible(&sRectPort->base, (configNetworkSystem == NS_SOCKET));
            {
                struct DjuiText* text1 = djui_text_create(&sRectPort->base, DLANG(HOST, PORT));
                djui_base_set_size_type(&text1->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
                djui_base_set_color(&text1->base, 220, 220, 220, 255);
                djui_base_set_size(&text1->base, 0.585f, 64);
                djui_base_set_alignment(&text1->base, DJUI_HALIGN_LEFT, DJUI_VALIGN_TOP);
                djui_text_set_drop_shadow(text1, 64, 64, 64, 100);
                if (gNetworkType == NT_SERVER) {
                    djui_base_set_enabled(&text1->base, false);
                }

                sInputboxPort = djui_inputbox_create(&sRectPort->base, 32);
                djui_base_set_size_type(&sInputboxPort->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
                djui_base_set_size(&sInputboxPort->base, 0.45f, 32);
                djui_base_set_alignment(&sInputboxPort->base, DJUI_HALIGN_RIGHT, DJUI_VALIGN_TOP);
                char portString[32] = { 0 };
                snprintf(portString, 32, "%d", configHostPort);
                djui_inputbox_set_text(sInputboxPort, portString);
                djui_interactable_hook_value_change(&sInputboxPort->base, djui_panel_host_port_text_change);
                if (gNetworkType == NT_SERVER) {
                    djui_base_set_enabled(&sInputboxPort->base, false);
                } else {
                    djui_base_set_enabled(&sInputboxPort->base, (configNetworkSystem == NS_SOCKET));
                }
            }
#ifdef COOPNET
            sRectPassword = djui_rect_container_create(&rect1->base, 32);
            djui_base_set_location(&sRectPassword->base, 0, 0);
            djui_base_set_visible(&sRectPassword->base, (configNetworkSystem == NS_COOPNET));
            {
                struct DjuiText* text1 = djui_text_create(&sRectPassword->base, DLANG(HOST, PASSWORD));
                djui_base_set_size_type(&text1->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
                djui_base_set_color(&text1->base, 220, 220, 220, 255);
                djui_base_set_size(&text1->base, 0.585f, 64);
                djui_base_set_alignment(&text1->base, DJUI_HALIGN_LEFT, DJUI_VALIGN_TOP);
                if (gNetworkType == NT_SERVER) {
                    djui_base_set_enabled(&text1->base, false);
                }

                sInputboxPassword = djui_inputbox_create(&sRectPassword->base, 32);
                sInputboxPassword->passwordChar[0] = '#';
                djui_base_set_size_type(&sInputboxPassword->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
                djui_base_set_size(&sInputboxPassword->base, 0.45f, 32);
                djui_base_set_alignment(&sInputboxPassword->base, DJUI_HALIGN_RIGHT, DJUI_VALIGN_TOP);
                char portPassword[64] = { 0 };
                snprintf(portPassword, 64, "%s", configPassword);
                djui_inputbox_set_text(sInputboxPassword, portPassword);
                djui_interactable_hook_value_change(&sInputboxPassword->base, djui_panel_host_password_text_change);
                if (gNetworkType == NT_SERVER) {
                    djui_base_set_enabled(&sInputboxPassword->base, false);
                } else {
                    djui_base_set_enabled(&sInputboxPassword->base, (configNetworkSystem == NS_COOPNET));
                }
            }
#endif
        }

        struct DjuiRect* rect2 = djui_rect_container_create(body, 32);
        {
            struct DjuiText* text1 = djui_text_create(&rect2->base, DLANG(HOST, SAVE_SLOT));
            djui_base_set_size_type(&text1->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
            djui_base_set_color(&text1->base, 220, 220, 220, 255);
            djui_base_set_size(&text1->base, 0.585f, 64);
            djui_base_set_alignment(&text1->base, DJUI_HALIGN_LEFT, DJUI_VALIGN_TOP);
            djui_text_set_drop_shadow(text1, 64, 64, 64, 100);

            char starString[64] = { 0 };
            snprintf(starString, 64, "%c x%d - %s", '~' + 1, save_file_get_total_star_count(configHostSaveSlot - 1, 0, 24), configSaveNames[configHostSaveSlot - 1]);
            struct DjuiButton* button1 = djui_button_create(&rect2->base, starString, DJUI_BUTTON_STYLE_NORMAL, djui_panel_host_save_create);
            djui_base_set_size(&button1->base, 0.45f, 32);
            djui_base_set_alignment(&button1->base, DJUI_HALIGN_RIGHT, DJUI_VALIGN_TOP);
            djui_interactable_hook_hover(&button1->base, djui_panel_host_hover_save_slot, NULL);
        }

        struct DjuiButton* buttonSettings = djui_button_create(body, DLANG(HOST, SETTINGS), DJUI_BUTTON_STYLE_NORMAL, djui_panel_host_settings_create);
        djui_interactable_hook_hover(&buttonSettings->base, djui_panel_host_hover_settings, NULL);
        struct DjuiButton* buttonMods = djui_button_create(body, DLANG(HOST, MODS), DJUI_BUTTON_STYLE_NORMAL, djui_panel_host_mods_create);
        djui_interactable_hook_hover(&buttonMods->base, djui_panel_host_hover_mods, NULL);

        struct DjuiRect* rect3 = djui_rect_container_create(body, 64);
        {
            struct DjuiButton* button1 = djui_button_create(&rect3->base, (gNetworkType == NT_SERVER) ? DLANG(MENU, CANCEL) : DLANG(MENU, BACK), DJUI_BUTTON_STYLE_BACK, djui_panel_host_back);
            djui_base_set_size(&button1->base, 0.485f, 64);
            djui_base_set_alignment(&button1->base, DJUI_HALIGN_LEFT, DJUI_VALIGN_TOP);
            djui_interactable_hook_hover(&button1->base, djui_panel_host_hover_back, NULL);

            struct DjuiButton* button2 = djui_button_create(&rect3->base, (gNetworkType == NT_SERVER) ? DLANG(HOST, APPLY) : DLANG(HOST, HOST), DJUI_BUTTON_STYLE_NORMAL, djui_panel_host_do_host);
            djui_base_set_size(&button2->base, 0.485f, 64);
            djui_base_set_alignment(&button2->base, DJUI_HALIGN_RIGHT, DJUI_VALIGN_TOP);
            djui_interactable_hook_hover(&button2->base, djui_panel_host_hover_host, NULL);

            defaultBase = (gNetworkType == NT_SERVER)
                        ? &button1->base
                        : &button2->base;
        }

        if (gUpdateMessage) {
            struct DjuiText* message = djui_text_create(&panel->base, DLANG(NOTIF, UPDATE_AVAILABLE));
            djui_base_set_size_type(&message->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
            djui_base_set_size(&message->base, 1.0f, 1.0f);
            djui_base_set_color(&message->base, 255, 255, 160, 255);
            djui_text_set_alignment(message, DJUI_HALIGN_CENTER, DJUI_VALIGN_BOTTOM);
        }
    }

    djui_panel_add(caller, panel, defaultBase);
}
