#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "sm64.h"

#include "pc/lua/smlua.h"
#include "pc/lua/utils/smlua_text_utils.h"
#include "game/memory.h"
#include "audio/data.h"
#include "audio/external.h"

#include "network/network.h"
#include "lua/smlua.h"

#include "audio/audio_api.h"
#include "audio/audio_sdl.h"
#include "audio/audio_null.h"

#include "rom_assets.h"
#include "rom_checker.h"
#include "pc_main.h"
#include "loading.h"
#include "cliopts.h"
#include "configfile.h"
#include "thread.h"
#include "controller/controller_api.h"
#include "controller/controller_keyboard.h"
#include "controller/controller_mouse.h"
#include "fs/fs.h"

#include "game/display.h" // for gGlobalTimer
#include "game/game_init.h"
#include "game/main.h"
#include "game/rumble_init.h"

#include "pc/lua/utils/smlua_audio_utils.h"

#include "pc/network/version.h"
#include "pc/network/socket/socket.h"
#include "pc/network/network_player.h"
#include "pc/update_checker.h"
#include "pc/djui/djui.h"
#include "pc/djui/djui_unicode.h"
#include "pc/djui/djui_panel.h"
#include "pc/djui/djui_panel_modlist.h"
#include "pc/djui/djui_ctx_display.h"
#include "pc/djui/djui_fps_display.h"
#include "pc/djui/djui_lua_profiler.h"
#include "pc/debuglog.h"
#include "pc/utils/misc.h"
#include "pc/mods/mods.h"
#include "pc/wiiu_network.h"

#include "debug_context.h"
#include "menu/intro_geo.h"

#include "gfx_dimensions.h"
#include "game/segment2.h"

#include "engine/math_util.h"

#ifdef DISCORD_SDK
#include "pc/discord/discord.h"
#endif

#ifndef TARGET_WII_U
#include "pc/mumble/mumble.h"
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

#ifdef HAVE_SDL2
#include <SDL2/SDL.h>
#endif

#ifdef TARGET_WII_U
#include <coreinit/debug.h>
#include <whb/sdcard.h>
#include <whb/log.h>
#endif

extern Vp gViewportFullscreen;

OSMesg D_80339BEC;
OSMesgQueue gSIEventMesgQueue;

s8 gResetTimer;
s8 D_8032C648;
s8 gDebugLevelSelect;
s8 gShowProfiler;
s8 gShowDebugText;

s32 gRumblePakPfs;
u32 gNumVblanks = 0;

u8 gRenderingInterpolated = 0;
f32 gRenderingDelta = 0;
f32 gFramePercentage = 0.f;

#define FRAMERATE 30
static const f64 sFrameTime = (1.0 / ((double)FRAMERATE));
static f64 sFpsTimeLast = 0;
static f64 sFrameTimeStart = 0;
static u32 sDrawnFrames = 0;

bool gGameInited = false;
bool gGfxInited = false;

f32 gMasterVolume;

u8 gLuaVolumeMaster = 127;
u8 gLuaVolumeLevel = 127;
u8 gLuaVolumeSfx = 127;
u8 gLuaVolumeEnv = 127;

static struct AudioAPI *audio_api;
struct GfxWindowManagerAPI *wm_api = &WAPI;

extern void gfx_run(Gfx *commands);
extern void thread5_game_loop(void *arg);
extern void create_next_audio_buffer(s16 *samples, u32 num_samples);
void game_loop_one_iteration(void);

#ifdef TARGET_WII_U
static bool wiiu_mkdirs(const char *path) {
    char buf[SYS_MAX_PATH];
    size_t len = 0;

    if (path == NULL || path[0] == '\0') { return false; }
    if (fs_sys_dir_exists(path)) { return true; }

    strncpy(buf, path, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';
    len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/') {
        buf[len - 1] = '\0';
        len--;
    }

    for (size_t i = 1; i < len; i++) {
        if (buf[i] != '/') { continue; }
        buf[i] = '\0';
        if (!fs_sys_dir_exists(buf)) {
            fs_sys_mkdir(buf);
        }
        buf[i] = '/';
    }

    if (!fs_sys_dir_exists(buf)) {
        fs_sys_mkdir(buf);
    }
    return fs_sys_dir_exists(buf);
}

static void wiiu_prepare_sd_storage(void) {
    WHBMountSdCard();
    wiiu_mkdirs("/vol/external01/wiiu/apps/sm64coopdx");
    wiiu_mkdirs("/vol/external01/wiiu/apps/sm64coopdx/roms");
    wiiu_mkdirs("/vol/external01/wiiu/apps/sm64coopdx/mods");
}
#endif

void dispatch_audio_sptask(UNUSED struct SPTask *spTask) {}
void set_vblank_handler(UNUSED s32 index, UNUSED struct VblankHandler *handler, UNUSED OSMesgQueue *queue, UNUSED OSMesg *msg) {}

void send_display_list(struct SPTask *spTask) {
    if (!gGameInited) { return; }
    gfx_run((Gfx *)spTask->task.t.data_ptr);
}

#ifdef VERSION_EU
#define SAMPLES_HIGH 560 // gAudioBufferParameters.maxAiBufferLength
#define SAMPLES_LOW 528 // gAudioBufferParameters.minAiBufferLength
#else
#define SAMPLES_HIGH 544
#define SAMPLES_LOW 528
#endif

extern void patch_mtx_before(void);
extern void patch_screen_transition_before(void);
extern void patch_title_screen_before(void);
extern void patch_dialog_before(void);
extern void patch_hud_before(void);
extern void patch_paintings_before(void);
extern void patch_bubble_particles_before(void);
extern void patch_snow_particles_before(void);
extern void patch_djui_before(void);
extern void patch_djui_hud_before(void);
extern void patch_scroll_targets_before(void);

extern void patch_mtx_interpolated(f32 delta);
extern void patch_screen_transition_interpolated(f32 delta);
extern void patch_title_screen_interpolated(f32 delta);
extern void patch_dialog_interpolated(f32 delta);
extern void patch_hud_interpolated(f32 delta);
extern void patch_paintings_interpolated(f32 delta);
extern void patch_bubble_particles_interpolated(f32 delta);
extern void patch_snow_particles_interpolated(f32 delta);
extern void patch_djui_interpolated(f32 delta);
extern void patch_djui_hud(f32 delta);
extern void patch_scroll_targets_interpolated(f32 delta);

static void patch_interpolations_before(void) {
    patch_mtx_before();
    patch_screen_transition_before();
    patch_title_screen_before();
    patch_dialog_before();
    patch_hud_before();
    patch_paintings_before();
    patch_bubble_particles_before();
    patch_snow_particles_before();
    patch_djui_before();
    patch_djui_hud_before();
    patch_scroll_targets_before();
}

static inline void patch_interpolations(f32 delta) {
    patch_mtx_interpolated(delta);
    patch_screen_transition_interpolated(delta);
    patch_title_screen_interpolated(delta);
    patch_dialog_interpolated(delta);
    patch_hud_interpolated(delta);
    patch_paintings_interpolated(delta);
    patch_bubble_particles_interpolated(delta);
    patch_snow_particles_interpolated(delta);
    patch_djui_interpolated(delta);
    patch_djui_hud(delta);
    patch_scroll_targets_interpolated(delta);
}

static void compute_fps(f64 curTime) {
    u32 fps = round((f64) sDrawnFrames / MAX(0.001, curTime - sFpsTimeLast));
    djui_fps_display_update(fps);
    sFpsTimeLast = curTime;
    sDrawnFrames = 0;
}

static s32 get_num_frames_to_draw(f64 t, u32 frameLimit) {
    if (frameLimit % FRAMERATE == 0) {
        return frameLimit / FRAMERATE;
    }
    s64 numFramesCurr = (s64) (t * (f64) frameLimit);
    s64 numFramesNext = (s64) ((t + sFrameTime) * (f64) frameLimit);
    return (s32) MAX(1, numFramesNext - numFramesCurr);
}

static u32 get_display_refresh_rate() {
#ifdef HAVE_SDL2
    static u32 refreshRate = 0;
    if (!refreshRate) {
        SDL_DisplayMode mode;
        if (SDL_GetCurrentDisplayMode(0, &mode) == 0) {
            if (mode.refresh_rate > 0) { refreshRate = (u32) mode.refresh_rate; }
        } else {
            refreshRate = 60;
        }
    }
    return refreshRate;
#else
    return 60;
#endif
}

static u32 get_target_refresh_rate() {
    if (configFramerateMode == RRM_MANUAL) { return configFrameLimit; }
    if (configFramerateMode == RRM_UNLIMITED) { return 3000; } // Has no effect
    return get_display_refresh_rate();
}

void produce_interpolation_frames_and_delay(void) {
    u32 refreshRate = get_target_refresh_rate();

    gRenderingInterpolated = true;

    u32 displayRefreshRate = get_display_refresh_rate();
    bool shouldDelay = configFramerateMode != RRM_UNLIMITED;
    if (configWindow.vsync && displayRefreshRate <= refreshRate) {
        shouldDelay = false;
        refreshRate = displayRefreshRate;
    }

    f64 targetTime = sFrameTimeStart + sFrameTime;
    s32 numFramesToDraw = get_num_frames_to_draw(sFrameTimeStart, refreshRate);

    f64 curTime = clock_elapsed_f64();
    f64 loopStartTime = curTime;
    f64 expectedTime = 0;
    u16 framesDrawn = 0;
    const f64 interpFrameTime = sFrameTime / (f64) numFramesToDraw;

    // interpolate and render
    // make sure to draw at least one frame to prevent the game from freezing completely
    // (including inputs and window events) if the game update duration is greater than 33ms
    do {
        ++framesDrawn;

        // when we know how many frames to draw, use a precise delta
        f64 idealTime = shouldDelay ? (sFrameTimeStart + interpFrameTime * framesDrawn) : curTime;
        f32 delta = clamp((idealTime - sFrameTimeStart) / sFrameTime, 0.f, 1.f);
        gFramePercentage = clamp((curTime - sFrameTimeStart) / sFrameTime, 0.f, 1.f);
        gRenderingDelta = delta;

        gfx_start_frame();
        if (!gSkipInterpolationTitleScreen) { patch_interpolations(delta); }
        send_display_list(gGfxSPTask);
#ifdef TARGET_WII_U
        gfx_end_frame();
#else
        gfx_end_frame_render();
#endif

        // delay if our framerate is capped
        if (shouldDelay) {
            expectedTime += (targetTime - curTime) / (f64) numFramesToDraw;
            f64 now = clock_elapsed_f64();
            f64 elapsedTime = now - loopStartTime;
            f64 delay = (expectedTime - elapsedTime);
            if (delay > 0.0) {
                precise_delay_f64(delay);
            }
        }

        // send the frame to the screen (should be directly after the delay for good frame pacing)
#ifndef TARGET_WII_U
        gfx_display_frame();
#endif
        sDrawnFrames++;
        if (shouldDelay) { numFramesToDraw--; }
    } while ((curTime = clock_elapsed_f64()) < targetTime && numFramesToDraw > 0);

    // compute and update the frame rate every second
    if ((curTime = clock_elapsed_f64()) >= sFpsTimeLast + 1.0) {
        compute_fps(curTime);
    }

    // advance frame start time
    if (curTime > sFrameTimeStart + 2 * sFrameTime) {
        sFrameTimeStart = curTime;
    } else {
        sFrameTimeStart += sFrameTime;
    }

    gRenderingInterpolated = false;
}

// It's just better to have this off the stack, Because the size isn't small.
// It also may help static analysis and bug catching.
enum { AUDIO_BATCH_MIN = 2 };
#ifdef TARGET_WII_U
enum { AUDIO_BATCH_MAX = 4 };
#else
enum { AUDIO_BATCH_MAX = 2 };
#endif

static s16 sAudioBuffer[SAMPLES_HIGH * 2 * AUDIO_BATCH_MAX] = { 0 };

inline static void buffer_audio(void) {
    gMasterVolume = ((f32)configMasterVolume / 127.0f) * ((f32)gLuaVolumeMaster / 127.0f);
    bool shouldMute = (configMuteFocusLoss && !WAPI.has_focus()) || (gMasterVolume <= 0.0f);
    if (!shouldMute) {
        set_sequence_player_volume(SEQ_PLAYER_LEVEL, (f32)configMusicVolume / 127.0f * (f32)gLuaVolumeLevel / 127.0f);
        set_sequence_player_volume(SEQ_PLAYER_SFX,   (f32)configSfxVolume / 127.0f * (f32)gLuaVolumeSfx / 127.0f);
        set_sequence_player_volume(SEQ_PLAYER_ENV,   (f32)configEnvVolume / 127.0f * (f32)gLuaVolumeEnv / 127.0f);
    }

    int samplesLeft = audio_api->buffered();
    int desiredBuffered = audio_api->get_desired_buffered();
    u32 numAudioSamples = samplesLeft < desiredBuffered ? SAMPLES_HIGH : SAMPLES_LOW;
    u32 audioBatches = AUDIO_BATCH_MIN;
#ifdef TARGET_WII_U
    // Never advance synth state for audio that won't be queued.
    if (samplesLeft >= 5200) {
        return;
    }

    // Size production by actual queue deficit to avoid oscillation/overrun.
    int deficit = desiredBuffered - samplesLeft;
    if (deficit > 0) {
        int needed = (deficit + (int)SAMPLES_LOW - 1) / (int)SAMPLES_LOW;
        if (needed < (int)AUDIO_BATCH_MIN) { needed = AUDIO_BATCH_MIN; }
        if (needed > (int)AUDIO_BATCH_MAX) { needed = AUDIO_BATCH_MAX; }
        audioBatches = (u32)needed;
    }
#endif

    for (u32 i = 0; i < audioBatches; i++) {
        create_next_audio_buffer(sAudioBuffer + i * (numAudioSamples * 2), numAudioSamples);
    }

    if (!shouldMute) {
        // Apply master gain only to the active mixed sample span.
        const u32 mixedSamples = 2 * audioBatches * numAudioSamples;
        for (u32 i = 0; i < mixedSamples; i++) {
            sAudioBuffer[i] = (s16)((f32)sAudioBuffer[i] * gMasterVolume);
        }
        audio_api->play((u8 *)sAudioBuffer, audioBatches * numAudioSamples * 4);
    }
}

void *audio_thread(UNUSED void *arg) {
    // As long as we have an audio api and that we're threaded, Loop.
    while (audio_api) {
        // Buffer the audio.
        lock_mutex(&gAudioThread);
        buffer_audio();
        unlock_mutex(&gAudioThread);

        // Queue-driven on Wii U: poll frequently and top up only when needed.
#ifdef TARGET_WII_U
        WAPI.delay(4);
#else
        // Delay till the next frame for smooth audio at the correct speed.
        f64 curTime = clock_elapsed_f64();
        f64 targetDelta = 1.0 / (f64)FRAMERATE;
        f64 now = clock_elapsed_f64();
        f64 actualDelta = now - curTime;
        if (actualDelta < targetDelta) {
            f64 delay = ((targetDelta - actualDelta) * 1000.0);
            WAPI.delay((u32)delay);
        }
#endif
    }

    // Exit the thread if our loop breaks.
    exit_thread();

    return NULL;
}

void produce_one_frame(void) {
#ifdef TARGET_WII_U
    static bool sLoggedFirstProduceOneFrame = false;
    const bool traceFirstFrame = !sLoggedFirstProduceOneFrame;
    if (!sLoggedFirstProduceOneFrame) {
        sLoggedFirstProduceOneFrame = true;
        OSReport("pc_main: first produce_one_frame\n");
    }
    if (traceFirstFrame) { OSReport("pc_main: frame stage network_update begin\n"); }
#endif
    CTX_EXTENT(CTX_NETWORK, network_update);
#ifdef TARGET_WII_U
    if (traceFirstFrame) { OSReport("pc_main: frame stage network_update end\n"); }
    if (traceFirstFrame) { OSReport("pc_main: frame stage patch_interpolations_before begin\n"); }
#endif

    CTX_EXTENT(CTX_INTERP, patch_interpolations_before);
#ifdef TARGET_WII_U
    if (traceFirstFrame) { OSReport("pc_main: frame stage patch_interpolations_before end\n"); }
    if (traceFirstFrame) { OSReport("pc_main: frame stage game_loop_one_iteration begin\n"); }
#endif

    CTX_EXTENT(CTX_GAME_LOOP, game_loop_one_iteration);
#ifdef TARGET_WII_U
    if (traceFirstFrame) { OSReport("pc_main: frame stage game_loop_one_iteration end\n"); }
    if (traceFirstFrame) { OSReport("pc_main: frame stage smlua_update begin\n"); }
#endif

    CTX_EXTENT(CTX_SMLUA, smlua_update);
#ifdef TARGET_WII_U
    if (traceFirstFrame) { OSReport("pc_main: frame stage smlua_update end\n"); }
#endif

    // If we aren't threaded
    if (gAudioThread.state == INVALID) {
#ifdef TARGET_WII_U
        if (traceFirstFrame) { OSReport("pc_main: frame stage buffer_audio begin\n"); }
#endif
        CTX_EXTENT(CTX_AUDIO, buffer_audio);
#ifdef TARGET_WII_U
        if (traceFirstFrame) { OSReport("pc_main: frame stage buffer_audio end\n"); }
#endif
    }

#ifdef TARGET_WII_U
    if (traceFirstFrame) { OSReport("pc_main: frame stage produce_interpolation_frames_and_delay begin\n"); }
#endif
    CTX_EXTENT(CTX_RENDER, produce_interpolation_frames_and_delay);
#ifdef TARGET_WII_U
    if (traceFirstFrame) { OSReport("pc_main: frame stage produce_interpolation_frames_and_delay end\n"); }
#endif
}

// used for rendering 2D scenes fullscreen like the loading or crash screens
void produce_one_dummy_frame(void (*callback)(), u8 clearColorR, u8 clearColorG, u8 clearColorB) {
    // measure frame start time
    f64 frameStart = clock_elapsed_f64();
    f64 targetFrameTime = 1.0 / 60.0; // update at 60fps

    // start frame
    gfx_start_frame();
    config_gfx_pool();
    init_render_image();
    create_dl_ortho_matrix();
    djui_gfx_displaylist_begin();

    // fix scaling issues
    gSPViewport(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gViewportFullscreen));
    gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 0, BORDER_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT - BORDER_HEIGHT);

    // clear screen
    create_dl_translation_matrix(MENU_MTX_PUSH, GFX_DIMENSIONS_FROM_LEFT_EDGE(0), 240.f, 0.f);
    create_dl_scale_matrix(MENU_MTX_NOPUSH, (GFX_DIMENSIONS_ASPECT_RATIO * SCREEN_HEIGHT) / 130.f, 3.f, 1.f);
    gDPSetEnvColor(gDisplayListHead++, clearColorR, clearColorG, clearColorB, 0xFF);
    gSPDisplayList(gDisplayListHead++, dl_draw_text_bg_box);
    gSPPopMatrix(gDisplayListHead++, G_MTX_MODELVIEW);

    // call the callback
    callback();

    // render frame
    djui_gfx_displaylist_end();
    end_master_display_list();
    alloc_display_list(0);
    gfx_run((Gfx*) gGfxSPTask->task.t.data_ptr); // send_display_list
    display_and_vsync();

    // delay to go easy on the cpu
    f64 frameEnd = clock_elapsed_f64();
    f64 elapsed = frameEnd - frameStart;
    f64 remaining = targetFrameTime - elapsed;
    if (remaining > 0) {
        WAPI.delay((u32)(remaining * 1000.0));
    }

    gfx_end_frame();
}

void audio_shutdown(void) {
    struct AudioAPI *api = audio_api;

    // Stop and join the producer thread first so the backend can't be
    // torn down while it is still queuing audio.
    if (gAudioThread.state == RUNNING) {
        audio_api = NULL;
        join_thread(&gAudioThread);
        destroy_mutex(&gAudioThread);
        gAudioThread.state = INVALID;
    }

    audio_custom_shutdown();
    if (api) {
        if (api->shutdown) { api->shutdown(); }
        audio_api = NULL;
    }
}

void game_deinit(void) {
    if (gGameInited) { configfile_save(configfile_name()); }
    controller_shutdown();
    audio_custom_shutdown();
    audio_shutdown();
    network_shutdown(true, true, false, false);
#ifdef TARGET_WII_U
    wiiu_network_shutdown();
#endif
    smlua_text_utils_shutdown();
    smlua_shutdown();
    smlua_audio_custom_deinit();
    mods_shutdown();
    djui_shutdown();
    gfx_shutdown();
    gGameInited = false;
}

void game_exit(void) {
    LOG_INFO("exiting cleanly");
    game_deinit();
    exit(0);
}

void* main_game_init(UNUSED void* dummy) {
    // load language
    if (!djui_language_init(configLanguage)) { snprintf(configLanguage, MAX_CONFIG_STRING, "%s", ""); }

    LOADING_SCREEN_MUTEX(loading_screen_set_segment_text("Loading"));
    dynos_gfx_init();
    enable_queued_dynos_packs();
    sync_objects_init_system();

    if (gCLIOpts.network != NT_SERVER && !gCLIOpts.skipUpdateCheck) {
        check_for_updates();
    }

    LOADING_SCREEN_MUTEX(loading_screen_set_segment_text("Loading ROM Assets"));
    rom_assets_load();
    smlua_text_utils_init();
    mods_init();
    enable_queued_mods();
#ifndef TARGET_WII_U
    LOADING_SCREEN_MUTEX(
        gCurrLoadingSegment.percentage = 0;
        loading_screen_set_segment_text("Starting Game");
    );
#else
    LOADING_SCREEN_MUTEX(loading_screen_set_segment_text("Preparing Runtime"));
#endif

    audio_init();
#ifdef TARGET_WII_U
    OSReport("pc_main: after audio_init\n");
#endif
    sound_init();
#ifdef TARGET_WII_U
    OSReport("pc_main: after sound_init\n");
#endif
    network_player_init();
#ifdef TARGET_WII_U
    OSReport("pc_main: after network_player_init\n");
#endif
#ifndef TARGET_WII_U
    mumble_init();
#endif

    gGameInited = true;
#ifdef TARGET_WII_U
    OSReport("pc_main: main_game_init complete\n");
#endif
    return NULL;
}

int main(int argc, char *argv[]) {
    // handle terminal arguments
    if (!parse_cli_opts(argc, argv)) { return 0; }

#if defined(RAPI_DUMMY) || defined(WAPI_DUMMY)
    gCLIOpts.headless = true;
#endif

#ifdef _WIN32
    // handle Windows console
    if (gCLIOpts.console || gCLIOpts.headless) {
        SetConsoleOutputCP(CP_UTF8);
    } else {
        FreeConsole();
        freopen("NUL", "w", stdout);
    }
#endif

#ifdef _WIN32
    if (gCLIOpts.savePath[0]) {
        char portable_path[SYS_MAX_PATH] = {};
        sys_windows_short_path_from_mbs(portable_path, SYS_MAX_PATH, gCLIOpts.savePath);
        fs_init(portable_path);
    } else {
        fs_init(sys_user_path());
    }
#else
#ifdef TARGET_WII_U
    wiiu_prepare_sd_storage();
#endif
    fs_init(gCLIOpts.savePath[0] ? gCLIOpts.savePath : sys_user_path());
#endif

#ifdef TARGET_WII_U
    wiiu_network_init();
#endif

#if !defined(RAPI_DUMMY) && !defined(WAPI_DUMMY)
    if (gCLIOpts.headless) {
        memcpy(&WAPI, &gfx_dummy_wm_api, sizeof(struct GfxWindowManagerAPI));
        memcpy(&RAPI, &gfx_dummy_renderer_api, sizeof(struct GfxRenderingAPI));
    }
#endif

    configfile_load();

    legacy_folder_handler();

    // create the window almost straight away
    if (!gGfxInited) {
        gfx_init(&WAPI, &RAPI, TITLE);
        WAPI.set_keyboard_callbacks(keyboard_on_key_down, keyboard_on_key_up, keyboard_on_all_keys_up,
            keyboard_on_text_input, keyboard_on_text_editing);
        WAPI.set_scroll_callback(mouse_on_scroll);
    }

    // render the rom setup screen
    bool rom_ready = main_rom_handler();
#ifdef TARGET_WII_U
    OSReport("pc_main: main_rom_handler=%d\n", rom_ready ? 1 : 0);
#endif
    if (!rom_ready) {
#ifdef LOADING_SCREEN_SUPPORTED
        if (!gCLIOpts.hideLoadingScreen) {
            render_rom_setup_screen(); // holds the game load until a valid rom is provided
        } else
#endif
        {
            printf("ERROR: could not find valid vanilla us sm64 rom in game's user folder\n");
            return 0;
        }
    }

    // start the thread for setting up the game
#ifdef LOADING_SCREEN_SUPPORTED
    bool threadSuccess = false;
    bool usedLoadingScreen = false;
#ifdef TARGET_WII_U
    // Keep Wii U boot on the main thread for now. The loading-screen thread
    // path is desktop-oriented and has been leaving the port presenting a
    // frame while game init silently stalls in the background.
    const bool allowAsyncLoadingThread = false;
#else
    const bool allowAsyncLoadingThread = true;
#endif
    if (!gCLIOpts.hideLoadingScreen && !gCLIOpts.headless) {
        if (allowAsyncLoadingThread && init_thread_handle(&gLoadingThread, main_game_init, NULL, NULL, 0) == 0) {
            usedLoadingScreen = true;
            render_loading_screen(); // render the loading screen while the game is setup
            threadSuccess = true;
            destroy_mutex(&gLoadingThread);
        }
    }
    if (!threadSuccess)
#endif
    {
#ifdef TARGET_WII_U
        OSReport("pc_main: entering main_game_init (single-thread fallback)\n");
#endif
        main_game_init(NULL); // failsafe incase threading doesn't work
    }

    // initialize sm64 data and controllers
#ifdef TARGET_WII_U
    OSReport("pc_main: before thread5_game_loop\n");
#endif
    thread5_game_loop(NULL);
#ifdef TARGET_WII_U
    OSReport("pc_main: after thread5_game_loop\n");
#endif

    // initialize sound outside threads
    if (gCLIOpts.headless) audio_api = &audio_null;
#if defined(AAPI_SDL1) || defined(AAPI_SDL2)
    if (!audio_api && audio_sdl.init()) audio_api = &audio_sdl;
#endif
    if (!audio_api) audio_api = &audio_null;
#ifdef TARGET_WII_U
    WHBLogPrintf("audio: backend=%s",
                 (audio_api == &audio_null) ? "null" : "sdl");
    OSReport("audio: backend=%s\n",
             (audio_api == &audio_null) ? "null" : "sdl");
#endif

    // Initialize the audio thread if possible. Falling back to frame-threaded
    // audio is functional but prone to underruns when rendering stalls.
    if (audio_api != &audio_null) {
#ifdef TARGET_WII_U
        // Keep Wii U on frame-threaded audio for now. The pthread shim/newlib
        // interaction can destabilize large Lua allocations during host/mod init.
        gAudioThread.state = INVALID;
        OSReport("audio: forcing frame-threaded fallback on Wii U\n");
#else
        if (init_thread_handle(&gAudioThread, audio_thread, NULL, NULL, 0) != 0) {
            destroy_mutex(&gAudioThread);
            gAudioThread.state = INVALID;
        }
#endif
    }

#ifdef LOADING_SCREEN_SUPPORTED
#ifdef TARGET_WII_U
    if (usedLoadingScreen) {
        loading_screen_reset();
        OSReport("pc_main: after loading_screen_reset\n");
    } else {
        OSReport("pc_main: skipping loading_screen_reset (loading screen unused)\n");
    }
#else
    loading_screen_reset();
#endif
#endif

    // initialize djui
    djui_init();
#ifdef TARGET_WII_U
    OSReport("pc_main: after djui_init\n");
#endif
    djui_unicode_init();
#ifdef TARGET_WII_U
    OSReport("pc_main: after djui_unicode_init\n");
#endif
    djui_init_late();
#ifdef TARGET_WII_U
    OSReport("pc_main: after djui_init_late\n");
#endif
    djui_console_message_dequeue();
#ifdef TARGET_WII_U
    OSReport("pc_main: after djui_console_message_dequeue\n");
#endif

    show_update_popup();
#ifdef TARGET_WII_U
    OSReport("pc_main: after show_update_popup\n");
#endif

    // initialize network
    if (gCLIOpts.network == NT_CLIENT) {
        network_set_system(NS_SOCKET);
        snprintf(gGetHostName, MAX_CONFIG_STRING, "%s", gCLIOpts.joinIp);
        snprintf(configJoinIp, MAX_CONFIG_STRING, "%s", gCLIOpts.joinIp);
        configJoinPort = gCLIOpts.networkPort;
        network_init(NT_CLIENT, false);
#ifdef TARGET_WII_U
        OSReport("pc_main: after network_init client\n");
#endif
    } else if (gCLIOpts.network == NT_SERVER || gCLIOpts.coopnet) {
        if (gCLIOpts.network == NT_SERVER) {
            configNetworkSystem = NS_SOCKET;
            configHostPort = gCLIOpts.networkPort;
        } else {
            configNetworkSystem = NS_COOPNET;
            snprintf(configPassword, MAX_CONFIG_STRING, "%s", gCLIOpts.coopnetPassword);
        }

        // horrible, hacky fix for mods that access marioObj straight away
        // best fix: host with the standard main menu method
        static struct Object sHackyObject = { 0 };
        gMarioStates[0].marioObj = &sHackyObject;

        extern void djui_panel_do_host(bool reconnecting, bool playSound);
        djui_panel_do_host(NULL, false);
#ifdef TARGET_WII_U
        OSReport("pc_main: after djui_panel_do_host\n");
#endif
    } else {
        network_init(NT_NONE, false);
#ifdef TARGET_WII_U
        OSReport("pc_main: after network_init none\n");
#endif
    }

    // main loop
    while (true) {
#ifdef TARGET_WII_U
        static bool sLoggedMainLoopEntry = false;
        if (!sLoggedMainLoopEntry) {
            sLoggedMainLoopEntry = true;
            OSReport("pc_main: entering main loop\n");
        }
#endif
        debug_context_reset();
        CTX_BEGIN(CTX_TOTAL);
        WAPI.main_loop(produce_one_frame);
#ifdef DISCORD_SDK
        discord_update();
#endif
#ifndef TARGET_WII_U
        mumble_update();
#endif
#ifdef DEBUG
        fflush(stdout);
        fflush(stderr);
#endif
        CTX_END(CTX_TOTAL);

#ifdef DEVELOPMENT
        djui_ctx_display_update();
#endif
        djui_lua_profiler_update();
    }

    return 0;
}
