#ifdef TARGET_WII_U

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>

#include <vpad/input.h>
#include <coreinit/debug.h>
#define OSTime WUT_OSTime
#include <padscore/wpad.h>
#include <padscore/kpad.h>
#undef OSTime

#include "controller_api.h"
#include "../configfile.h"
#include "../pc_main.h"

#define VK_BASE_WIIU 0x2000

struct WiiUKeymap {
    uint32_t n64Button;
    uint32_t vpadButton;
    uint32_t classicButton;
    uint32_t proButton;
};

// Button shortcuts
#define VB(btn) VPAD_BUTTON_##btn
#define CB(btn) WPAD_CLASSIC_BUTTON_##btn
#define PB(btn) WPAD_PRO_BUTTON_##btn
#define PT(btn) WPAD_PRO_TRIGGER_##btn

#ifndef VPAD_STICK_L_EMULATION_UP
#define VPAD_STICK_L_EMULATION_UP 0
#define VPAD_STICK_L_EMULATION_DOWN 0
#define VPAD_STICK_L_EMULATION_LEFT 0
#define VPAD_STICK_L_EMULATION_RIGHT 0
#endif

#ifndef WPAD_CLASSIC_STICK_L_EMULATION_UP
#define WPAD_CLASSIC_STICK_L_EMULATION_UP 0
#define WPAD_CLASSIC_STICK_L_EMULATION_DOWN 0
#define WPAD_CLASSIC_STICK_L_EMULATION_LEFT 0
#define WPAD_CLASSIC_STICK_L_EMULATION_RIGHT 0
#endif

#ifndef WPAD_PRO_STICK_L_EMULATION_UP
#define WPAD_PRO_STICK_L_EMULATION_UP 0
#define WPAD_PRO_STICK_L_EMULATION_DOWN 0
#define WPAD_PRO_STICK_L_EMULATION_LEFT 0
#define WPAD_PRO_STICK_L_EMULATION_RIGHT 0
#endif

// Stick emulation
#define SE(dir) VPAD_STICK_R_EMULATION_##dir, WPAD_CLASSIC_STICK_R_EMULATION_##dir, WPAD_PRO_STICK_R_EMULATION_##dir

struct WiiUKeymap map[] = {
    { B_BUTTON, VB(B) | VB(Y), CB(B) | CB(Y), PB(B) | PB(Y) },
    { A_BUTTON, VB(A) | VB(X), CB(A) | CB(X), PB(A) | PB(X) },
    { START_BUTTON, VB(PLUS), CB(PLUS), PB(PLUS) },
    { Z_TRIG, VB(L) | VB(ZL), CB(L) | CB(ZL), PT(L) | PT(ZL) },
    { L_TRIG, VB(MINUS), CB(MINUS), PB(MINUS) },
    { R_TRIG, VB(R) | VB(ZR), CB(R) | CB(ZR), PT(R) | PT(ZR) },
    { U_CBUTTONS, SE(UP) },
    { D_CBUTTONS, SE(DOWN) },
    { L_CBUTTONS, SE(LEFT) },
    { R_CBUTTONS, SE(RIGHT) }
};

size_t num_buttons = sizeof(map) / sizeof(map[0]);
KPADStatus last_kpad = {0};
int kpad_timeout = 10;
static VPADStatus s_last_vpad = { 0 };
static int s_vpad_timeout = 10;

enum WiiUDebugSource {
    WIIU_DEBUG_SOURCE_NONE = 0,
    WIIU_DEBUG_SOURCE_VPAD = 1,
    WIIU_DEBUG_SOURCE_WPAD = 2,
};

static uint32_t s_dbg_raw_main = 0;
static uint32_t s_dbg_raw_aux = 0;
static int s_dbg_source = WIIU_DEBUG_SOURCE_NONE;
static int s_dbg_channel = -1;
static int s_dbg_extension_type = -1;

static bool read_vpad(OSContPad *pad);
static bool read_wpad_channel(OSContPad *pad, int channel);

static bool controller_wiiu_pad_has_input(const OSContPad *pad) {
    return (pad != NULL) && (pad->button != 0 || pad->stick_x != 0 || pad->stick_y != 0);
}

static bool controller_wiiu_try_wpad_input(OSContPad *pad, int channel) {
    OSContPad tmp = { 0 };
    if (!read_wpad_channel(&tmp, channel)) {
        return false;
    }
    if (!controller_wiiu_pad_has_input(&tmp)) {
        return false;
    }
    *pad = tmp;
    return true;
}

static bool controller_wiiu_try_vpad_input(OSContPad *pad) {
    OSContPad tmp = { 0 };
    if (!read_vpad(&tmp)) {
        return false;
    }
    if (!controller_wiiu_pad_has_input(&tmp)) {
        return false;
    }
    *pad = tmp;
    return true;
}

static void controller_wiiu_apply_menu_nav_from_stick(OSContPad *pad) {
    if (pad == NULL) {
        return;
    }

    // CoopDX menu navigation listens for JPAD edges; mirror left stick directions.
    if (pad->stick_y > 40)  { pad->button |= U_JPAD; }
    if (pad->stick_y < -40) { pad->button |= D_JPAD; }
    if (pad->stick_x < -40) { pad->button |= L_JPAD; }
    if (pad->stick_x > 40)  { pad->button |= R_JPAD; }
}

static s8 controller_wiiu_clamp_stick(s16 value) {
    if (value > 80) { value = 80; }
    if (value < -80) { value = -80; }
    return (s8)value;
}

static void controller_wiiu_apply_stick_config(OSContPad *pad) {
    s16 x;
    s16 y;
    s16 tmp;
    s32 deadzone;

    if (pad == NULL) {
        return;
    }

    x = pad->stick_x;
    y = pad->stick_y;

    if (configStick.rotateLeft) {
        tmp = x;
        x = -y;
        y = tmp;
    }
    if (configStick.rotateRight) {
        tmp = x;
        x = y;
        y = -tmp;
    }
    if (configStick.invertLeftX) { x = -x; }
    if (configStick.invertLeftY) { y = -y; }

    deadzone = (s32)((configStickDeadzone * 80U) / 100U);
    if ((x * x + y * y) < deadzone * deadzone) {
        x = 0;
        y = 0;
    }

    pad->stick_x = controller_wiiu_clamp_stick(x);
    pad->stick_y = controller_wiiu_clamp_stick(y);
}

// Applies runtime button layout changes from config to all Wii U controller types.
static void controller_wiiu_apply_config(void) {
    const bool n64_face_buttons = false; // config option not yet ported on Wii U
    map[0] = (struct WiiUKeymap) { B_BUTTON, VB(B) | VB(Y), CB(B) | CB(Y), PB(B) | PB(Y) };
    map[1] = (struct WiiUKeymap) { A_BUTTON, VB(A) | VB(X), CB(A) | CB(X), PB(A) | PB(X) };

    if (n64_face_buttons) {
        map[0] = (struct WiiUKeymap) { B_BUTTON, VB(Y) | VB(X), CB(Y) | CB(X), PB(Y) | PB(X) };
        map[1] = (struct WiiUKeymap) { A_BUTTON, VB(B) | VB(A), CB(B) | CB(A), PB(B) | PB(A) };
    }
}

static void controller_wiiu_init(void) {
    VPADInit();
    KPADInit();
    WPADEnableURCC(1);
    WPADEnableWiiRemote(1);
    controller_wiiu_apply_config();
}

static bool read_vpad(OSContPad *pad) {
    VPADStatus status;
    VPADReadError err;
    uint32_t v;
    int32_t read = VPADRead(VPAD_CHAN_0, &status, 1, &err);

    if (read <= 0 || err != VPAD_READ_SUCCESS) {
        if ((err == VPAD_READ_NO_SAMPLES || err == VPAD_READ_BUSY) && s_vpad_timeout > 0) {
            status = s_last_vpad;
            status.trigger = 0;
            status.release = 0;
            s_vpad_timeout--;
        } else {
            return false;
        }
    } else {
        s_vpad_timeout = 10;
        s_last_vpad = status;
    }

    // Include edge bits so quick presses still register in menu/UI flows.
    v = status.hold | status.trigger;
    s_dbg_source = WIIU_DEBUG_SOURCE_VPAD;
    s_dbg_channel = 0;
    s_dbg_extension_type = -1;
    s_dbg_raw_main = v;
    s_dbg_raw_aux = 0;

    // Explicit face/shoulder mapping for reliability across SDK/header variants.
    if (v & (VPAD_BUTTON_A | VPAD_BUTTON_X)) { pad->button |= A_BUTTON; }
    if (v & (VPAD_BUTTON_B | VPAD_BUTTON_Y)) { pad->button |= B_BUTTON; }
    if (v & VPAD_BUTTON_PLUS)                { pad->button |= START_BUTTON; }
    if (v & (VPAD_BUTTON_L | VPAD_BUTTON_ZL)){ pad->button |= Z_TRIG; }
    if (v & VPAD_BUTTON_MINUS)               { pad->button |= L_TRIG; }
    if (v & (VPAD_BUTTON_R | VPAD_BUTTON_ZR)){ pad->button |= R_TRIG; }
    if (v & VPAD_STICK_R_EMULATION_UP)       { pad->button |= U_CBUTTONS; }
    if (v & VPAD_STICK_R_EMULATION_DOWN)     { pad->button |= D_CBUTTONS; }
    if (v & VPAD_STICK_R_EMULATION_LEFT)     { pad->button |= L_CBUTTONS; }
    if (v & VPAD_STICK_R_EMULATION_RIGHT)    { pad->button |= R_CBUTTONS; }

    if (v & (VPAD_BUTTON_UP | VPAD_STICK_L_EMULATION_UP))       { pad->button |= U_JPAD; }
    if (v & (VPAD_BUTTON_DOWN | VPAD_STICK_L_EMULATION_DOWN))   { pad->button |= D_JPAD; }
    if (v & (VPAD_BUTTON_LEFT | VPAD_STICK_L_EMULATION_LEFT))   { pad->button |= L_JPAD; }
    if (v & (VPAD_BUTTON_RIGHT | VPAD_STICK_L_EMULATION_RIGHT)) { pad->button |= R_JPAD; }

    if (v & (VPAD_BUTTON_LEFT | VPAD_STICK_L_EMULATION_LEFT))  { pad->stick_x = -80; }
    if (v & (VPAD_BUTTON_RIGHT | VPAD_STICK_L_EMULATION_RIGHT)){ pad->stick_x =  80; }
    if (v & (VPAD_BUTTON_DOWN | VPAD_STICK_L_EMULATION_DOWN))  { pad->stick_y = -80; }
    if (v & (VPAD_BUTTON_UP | VPAD_STICK_L_EMULATION_UP))      { pad->stick_y =  80; }

    if (status.leftStick.x != 0) {
        pad->stick_x = (s8) round(status.leftStick.x * 80);
    }
    if (status.leftStick.y != 0) {
        pad->stick_y = (s8) round(status.leftStick.y * 80);
    }
    // Fallback: some controller profiles drive menu navigation via right stick.
    if (pad->stick_x == 0 && fabsf(status.rightStick.x) > 0.10f) {
        pad->stick_x = (s8) round(status.rightStick.x * 80);
    }
    if (pad->stick_y == 0 && fabsf(status.rightStick.y) > 0.10f) {
        pad->stick_y = (s8) round(status.rightStick.y * 80);
    }

    return true;
}

static bool read_wpad_channel(OSContPad* pad, int channel) {
    WPADExtensionType ext;
    int res = WPADProbe(channel, &ext);
    if (res != 0) {
        return false;
    }

    KPADStatus status;
    int err;
    int read = KPADReadEx(channel, &status, 1, &err);
    if (read == 0) {
        kpad_timeout--;

        if (kpad_timeout == 0) {
            WPADDisconnect(channel);
            memset(&last_kpad, 0, sizeof(KPADStatus));
            return false;
        }
        status = last_kpad;
    } else {
        kpad_timeout = 10;
        last_kpad = status;
    }

    // Include edge bits so short taps don't get missed between frames.
    uint32_t wm = status.hold | status.trigger;
    s_dbg_source = WIIU_DEBUG_SOURCE_WPAD;
    s_dbg_channel = channel;
    s_dbg_extension_type = status.extensionType;
    s_dbg_raw_main = wm;
    s_dbg_raw_aux = 0;
    KPADVec2D stick = { 0 };

    bool gamepadStickNotSet = pad->stick_x == 0 && pad->stick_y == 0;

    if (status.extensionType == WPAD_EXT_CORE || status.extensionType == WPAD_EXT_MPLUS) {
        // Core Wii Remote fallback so menus remain usable without extensions.
        if (wm & WPAD_BUTTON_A)     pad->button |= A_BUTTON;
        if (wm & WPAD_BUTTON_B)     pad->button |= B_BUTTON;
        if (wm & WPAD_BUTTON_PLUS)  pad->button |= START_BUTTON;
        if (wm & WPAD_BUTTON_MINUS) pad->button |= L_TRIG;
        if (wm & WPAD_BUTTON_UP)    pad->button |= U_JPAD;
        if (wm & WPAD_BUTTON_DOWN)  pad->button |= D_JPAD;
        if (wm & WPAD_BUTTON_LEFT)  pad->button |= L_JPAD;
        if (wm & WPAD_BUTTON_RIGHT) pad->button |= R_JPAD;
    } else if (status.extensionType == WPAD_EXT_NUNCHUK || status.extensionType == WPAD_EXT_MPLUS_NUNCHUK) {
        uint32_t ext = status.nunchuk.hold | status.nunchuk.trigger;
        s_dbg_raw_aux = ext;
        stick = status.nunchuk.stick;

        if (wm & WPAD_BUTTON_A) pad->button |= A_BUTTON;
        if (wm & WPAD_BUTTON_B) pad->button |= B_BUTTON;
        if (wm & WPAD_BUTTON_PLUS) pad->button |= START_BUTTON;
        if (wm & WPAD_BUTTON_UP) pad->button |= U_JPAD;
        if (wm & WPAD_BUTTON_DOWN) pad->button |= D_JPAD;
        if (wm & WPAD_BUTTON_LEFT) pad->button |= L_JPAD;
        if (wm & WPAD_BUTTON_RIGHT) pad->button |= R_JPAD;
        if (ext & WPAD_NUNCHUK_BUTTON_C) pad->button |= R_TRIG;
        if (ext & WPAD_NUNCHUK_BUTTON_Z) pad->button |= Z_TRIG;
    } else if (status.extensionType == WPAD_EXT_CLASSIC || status.extensionType == WPAD_EXT_MPLUS_CLASSIC) {
        uint32_t ext = status.classic.hold | status.classic.trigger;
        s_dbg_raw_aux = ext;
        stick = status.classic.leftStick;
        if (fabsf(stick.x) < 0.10f && fabsf(status.classic.rightStick.x) > 0.10f) {
            stick.x = status.classic.rightStick.x;
        }
        if (fabsf(stick.y) < 0.10f && fabsf(status.classic.rightStick.y) > 0.10f) {
            stick.y = status.classic.rightStick.y;
        }
        if (ext & (WPAD_CLASSIC_BUTTON_A | WPAD_CLASSIC_BUTTON_X)) pad->button |= A_BUTTON;
        if (ext & (WPAD_CLASSIC_BUTTON_B | WPAD_CLASSIC_BUTTON_Y)) pad->button |= B_BUTTON;
        if (ext & WPAD_CLASSIC_BUTTON_PLUS)                         pad->button |= START_BUTTON;
        if (ext & (WPAD_CLASSIC_BUTTON_L | WPAD_CLASSIC_BUTTON_ZL))pad->button |= Z_TRIG;
        if (ext & WPAD_CLASSIC_BUTTON_MINUS)                        pad->button |= L_TRIG;
        if (ext & (WPAD_CLASSIC_BUTTON_R | WPAD_CLASSIC_BUTTON_ZR))pad->button |= R_TRIG;
        if (ext & WPAD_CLASSIC_STICK_R_EMULATION_UP)               pad->button |= U_CBUTTONS;
        if (ext & WPAD_CLASSIC_STICK_R_EMULATION_DOWN)             pad->button |= D_CBUTTONS;
        if (ext & WPAD_CLASSIC_STICK_R_EMULATION_LEFT)             pad->button |= L_CBUTTONS;
        if (ext & WPAD_CLASSIC_STICK_R_EMULATION_RIGHT)            pad->button |= R_CBUTTONS;
        if (ext & (WPAD_CLASSIC_BUTTON_UP | WPAD_CLASSIC_STICK_L_EMULATION_UP))       { pad->button |= U_JPAD; }
        if (ext & (WPAD_CLASSIC_BUTTON_DOWN | WPAD_CLASSIC_STICK_L_EMULATION_DOWN))   { pad->button |= D_JPAD; }
        if (ext & (WPAD_CLASSIC_BUTTON_LEFT | WPAD_CLASSIC_STICK_L_EMULATION_LEFT))   { pad->button |= L_JPAD; }
        if (ext & (WPAD_CLASSIC_BUTTON_RIGHT | WPAD_CLASSIC_STICK_L_EMULATION_RIGHT)) { pad->button |= R_JPAD; }
        if (ext & (WPAD_CLASSIC_BUTTON_LEFT | WPAD_CLASSIC_STICK_L_EMULATION_LEFT))   { pad->stick_x = -80; }
        if (ext & (WPAD_CLASSIC_BUTTON_RIGHT | WPAD_CLASSIC_STICK_L_EMULATION_RIGHT)) { pad->stick_x = 80; }
        if (ext & (WPAD_CLASSIC_BUTTON_DOWN | WPAD_CLASSIC_STICK_L_EMULATION_DOWN))   { pad->stick_y = -80; }
        if (ext & (WPAD_CLASSIC_BUTTON_UP | WPAD_CLASSIC_STICK_L_EMULATION_UP))       { pad->stick_y = 80; }
    } else if (status.extensionType == WPAD_EXT_PRO_CONTROLLER) {
        uint32_t ext = status.pro.hold | status.pro.trigger;
        s_dbg_raw_aux = ext;
        stick = status.pro.leftStick;
        if (fabsf(stick.x) < 0.10f && fabsf(status.pro.rightStick.x) > 0.10f) {
            stick.x = status.pro.rightStick.x;
        }
        if (fabsf(stick.y) < 0.10f && fabsf(status.pro.rightStick.y) > 0.10f) {
            stick.y = status.pro.rightStick.y;
        }
        if (ext & (WPAD_PRO_BUTTON_A | WPAD_PRO_BUTTON_X)) pad->button |= A_BUTTON;
        if (ext & (WPAD_PRO_BUTTON_B | WPAD_PRO_BUTTON_Y)) pad->button |= B_BUTTON;
        if (ext & WPAD_PRO_BUTTON_PLUS)                    pad->button |= START_BUTTON;
        if (ext & (WPAD_PRO_BUTTON_L | WPAD_PRO_BUTTON_ZL))pad->button |= Z_TRIG;
        if (ext & WPAD_PRO_BUTTON_MINUS)                   pad->button |= L_TRIG;
        if (ext & (WPAD_PRO_BUTTON_R | WPAD_PRO_BUTTON_ZR))pad->button |= R_TRIG;
        if (ext & WPAD_PRO_STICK_R_EMULATION_UP)           pad->button |= U_CBUTTONS;
        if (ext & WPAD_PRO_STICK_R_EMULATION_DOWN)         pad->button |= D_CBUTTONS;
        if (ext & WPAD_PRO_STICK_R_EMULATION_LEFT)         pad->button |= L_CBUTTONS;
        if (ext & WPAD_PRO_STICK_R_EMULATION_RIGHT)        pad->button |= R_CBUTTONS;
        if (ext & (WPAD_PRO_BUTTON_UP | WPAD_PRO_STICK_L_EMULATION_UP))       { pad->button |= U_JPAD; }
        if (ext & (WPAD_PRO_BUTTON_DOWN | WPAD_PRO_STICK_L_EMULATION_DOWN))   { pad->button |= D_JPAD; }
        if (ext & (WPAD_PRO_BUTTON_LEFT | WPAD_PRO_STICK_L_EMULATION_LEFT))   { pad->button |= L_JPAD; }
        if (ext & (WPAD_PRO_BUTTON_RIGHT | WPAD_PRO_STICK_L_EMULATION_RIGHT)) { pad->button |= R_JPAD; }
        if (ext & (WPAD_PRO_BUTTON_LEFT | WPAD_PRO_STICK_L_EMULATION_LEFT))   { pad->stick_x = -80; }
        if (ext & (WPAD_PRO_BUTTON_RIGHT | WPAD_PRO_STICK_L_EMULATION_RIGHT)) { pad->stick_x = 80; }
        if (ext & (WPAD_PRO_BUTTON_DOWN | WPAD_PRO_STICK_L_EMULATION_DOWN))   { pad->stick_y = -80; }
        if (ext & (WPAD_PRO_BUTTON_UP | WPAD_PRO_STICK_L_EMULATION_UP))       { pad->stick_y = 80; }
    }

    // If we didn't already get stick input from the gamepad
    if (gamepadStickNotSet) {
        if (stick.x != 0) {
            pad->stick_x = (s8) round(stick.x * 80);
        }
        if (stick.y != 0) {
            pad->stick_y = (s8) round(stick.y * 80);
        }
    }

    return true;
}

static void controller_wiiu_debug_log_state(const OSContPad *pad, bool hasInput, unsigned int selectedGamepad) {
    if (!configDebugPrint) {
        return;
    }

    static bool initialized = false;
    static bool lastHasInput = false;
    static unsigned int lastSelectedGamepad = 0;
    static uint16_t lastButtons = 0;
    static int8_t lastStickX = 0;
    static int8_t lastStickY = 0;
    static uint32_t lastRawMain = 0;
    static uint32_t lastRawAux = 0;
    static int lastSource = WIIU_DEBUG_SOURCE_NONE;
    static int lastChannel = -1;
    static int lastExtensionType = -1;

    if (!initialized
        || hasInput != lastHasInput
        || selectedGamepad != lastSelectedGamepad
        || (pad != NULL && (pad->button != lastButtons || pad->stick_x != lastStickX || pad->stick_y != lastStickY))
        || s_dbg_raw_main != lastRawMain
        || s_dbg_raw_aux != lastRawAux
        || s_dbg_source != lastSource
        || s_dbg_channel != lastChannel
        || s_dbg_extension_type != lastExtensionType) {
        const char *sourceName = "none";
        if (s_dbg_source == WIIU_DEBUG_SOURCE_VPAD) sourceName = "vpad";
        if (s_dbg_source == WIIU_DEBUG_SOURCE_WPAD) sourceName = "wpad";
        OSReport("[wiiu-ctrl] sel=%u has=%d src=%s ch=%d ext=%d raw=0x%08x aux=0x%08x mapped=0x%04x stick=(%d,%d)\n",
                 (unsigned int)selectedGamepad,
                 hasInput ? 1 : 0,
                 sourceName,
                 s_dbg_channel,
                 s_dbg_extension_type,
                 (unsigned int)s_dbg_raw_main,
                 (unsigned int)s_dbg_raw_aux,
                 pad != NULL ? (unsigned int)pad->button : 0U,
                 pad != NULL ? (int)pad->stick_x : 0,
                 pad != NULL ? (int)pad->stick_y : 0);

        initialized = true;
        lastHasInput = hasInput;
        lastSelectedGamepad = selectedGamepad;
        lastButtons = pad != NULL ? pad->button : 0;
        lastStickX = pad != NULL ? pad->stick_x : 0;
        lastStickY = pad != NULL ? pad->stick_y : 0;
        lastRawMain = s_dbg_raw_main;
        lastRawAux = s_dbg_raw_aux;
        lastSource = s_dbg_source;
        lastChannel = s_dbg_channel;
        lastExtensionType = s_dbg_extension_type;
    }
}

static void controller_wiiu_read(OSContPad* pad) {
    bool hasInput = false;
    unsigned int selectedGamepad = 0;

    pad->stick_x = 0;
    pad->stick_y = 0;
    pad->button = 0;

    if (configDisableGamepads) {
        return;
    }

    if (!configBackgroundGamepad && wm_api != NULL && wm_api->has_focus != NULL && !wm_api->has_focus()) {
        return;
    }

    selectedGamepad = configGamepadNumber;
    s_dbg_source = WIIU_DEBUG_SOURCE_NONE;
    s_dbg_channel = -1;
    s_dbg_extension_type = -1;
    s_dbg_raw_main = 0;
    s_dbg_raw_aux = 0;

    // Wii U mapping: 0 = GamePad (VPAD), 1..4 = WPAD channels 0..3.
    if (selectedGamepad == 0) {
        hasInput = controller_wiiu_try_vpad_input(pad);
        if (!hasInput) {
            for (int channel = 0; channel < 4; channel++) {
                if (controller_wiiu_try_wpad_input(pad, channel)) {
                    hasInput = true;
                    break;
                }
            }
        }
    } else {
        int wpadChannel = (int)(selectedGamepad - 1U);
        if (wpadChannel > 3) {
            wpadChannel = 0;
        }
        hasInput = controller_wiiu_try_wpad_input(pad, wpadChannel);
        if (!hasInput) {
            // Fallback so a stale gamepad selection does not silently disable controls.
            hasInput = controller_wiiu_try_vpad_input(pad);
        }
        if (!hasInput) {
            for (int channel = 0; channel < 4; channel++) {
                if (channel == wpadChannel) {
                    continue;
                }
                if (controller_wiiu_try_wpad_input(pad, channel)) {
                    hasInput = true;
                    break;
                }
            }
        }
    }

    if (hasInput) {
        controller_wiiu_apply_stick_config(pad);
        controller_wiiu_apply_menu_nav_from_stick(pad);
    }

    controller_wiiu_debug_log_state(pad, hasInput, selectedGamepad);
}

static u32 controller_wiiu_rawkey(void) {
    return VK_INVALID;
}

static void controller_wiiu_rumble_play(float str, float time) {
    (void)str;
    (void)time;
}

static void controller_wiiu_rumble_stop(void) {
}

static void controller_wiiu_shutdown(void) {
    KPADShutdown();
    VPADShutdown();
}

struct ControllerAPI controller_wiiu = {
    .vkbase = VK_BASE_WIIU,
    .init = controller_wiiu_init,
    .read = controller_wiiu_read,
    .rawkey = controller_wiiu_rawkey,
    .rumble_play = controller_wiiu_rumble_play,
    .rumble_stop = controller_wiiu_rumble_stop,
    .reconfig = controller_wiiu_apply_config,
    .shutdown = controller_wiiu_shutdown,
};

#endif
