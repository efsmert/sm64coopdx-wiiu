#ifdef AAPI_SDL2
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include <SDL2/SDL.h>

#include "audio_api.h"

#ifdef TARGET_WII_U
#include <coreinit/debug.h>
#define AUDIO_WIIU_LOG(...) OSReport(__VA_ARGS__)
#else
#define AUDIO_WIIU_LOG(...) ((void)0)
#endif

static SDL_AudioDeviceID dev;
#ifdef TARGET_WII_U
static SDL_AudioSpec g_audio_have;
static SDL_AudioStream *g_audio_stream;
static uint8_t *g_audio_stream_buf;
static size_t g_audio_stream_buf_size;
static uint8_t *g_audio_swap_buf;
static size_t g_audio_swap_buf_size;

enum {
    AUDIO_SRC_FREQ = 32000,
    AUDIO_SRC_CHANNELS = 2,
    AUDIO_SRC_FRAME_BYTES = 4, // S16 stereo
};

static int audio_sdl_have_frame_bytes(void) {
    int bytes_per_sample = SDL_AUDIO_BITSIZE(g_audio_have.format) / 8;
    if (bytes_per_sample <= 0 || g_audio_have.channels <= 0) {
        return AUDIO_SRC_FRAME_BYTES;
    }
    return bytes_per_sample * g_audio_have.channels;
}

static const uint8_t *audio_prepare_wiiu_pcm(const uint8_t *buf, size_t len) {
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    if ((len & 1u) != 0u) {
        return buf;
    }

    if (g_audio_swap_buf_size < len) {
        uint8_t *new_buf = realloc(g_audio_swap_buf, len);
        if (new_buf == NULL) {
            return buf;
        }
        g_audio_swap_buf = new_buf;
        g_audio_swap_buf_size = len;
    }

    uint16_t *dst = (uint16_t *)g_audio_swap_buf;
    const uint16_t *src = (const uint16_t *)buf;
    size_t count = len / sizeof(uint16_t);
    for (size_t i = 0; i < count; i++) {
        dst[i] = SDL_Swap16(src[i]);
    }
    return g_audio_swap_buf;
#else
    (void)len;
    return buf;
#endif
}
#endif

static bool audio_sdl_init(void) {
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL init error: %s\n", SDL_GetError());
        return false;
    }

#ifdef TARGET_WII_U
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = AUDIO_SRC_FREQ;
    // Match donor path: feed little-endian S16 samples.
    want.format = AUDIO_S16;
    want.channels = AUDIO_SRC_CHANNELS;
    want.samples = 1024;
    want.callback = NULL;
    // Prefer a strict native format first; conversion paths have proven brittle
    // on some Wii U/Cemu setups.
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev == 0) {
        dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_ANY_CHANGE);
    }
    if (dev == 0) {
        fprintf(stderr, "SDL_OpenAudio error: %s\n", SDL_GetError());
        return false;
    }

    g_audio_have = have;
    g_audio_stream = NULL;
    if (g_audio_have.format != AUDIO_S16
        || g_audio_have.channels != AUDIO_SRC_CHANNELS
        || g_audio_have.freq != AUDIO_SRC_FREQ) {
        g_audio_stream = SDL_NewAudioStream(
            AUDIO_S16, AUDIO_SRC_CHANNELS, AUDIO_SRC_FREQ,
            g_audio_have.format, g_audio_have.channels, g_audio_have.freq);
        if (g_audio_stream == NULL) {
            fprintf(stderr, "SDL_NewAudioStream error: %s\n", SDL_GetError());
            SDL_CloseAudioDevice(dev);
            dev = 0;
            return false;
        }
    }

    AUDIO_WIIU_LOG("audio_sdl2: have format=0x%04x channels=%u freq=%d samples=%u stream=%d\n",
                   (unsigned int)g_audio_have.format,
                   (unsigned int)g_audio_have.channels,
                   g_audio_have.freq,
                   (unsigned int)g_audio_have.samples,
                   g_audio_stream != NULL ? 1 : 0);
    SDL_PauseAudioDevice(dev, 0);
    return true;
#else
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 32000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = NULL;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev == 0) {
        fprintf(stderr, "SDL_OpenAudio error: %s\n", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(dev, 0);
    return true;
#endif
}

static int audio_sdl_buffered(void) {
#ifdef TARGET_WII_U
    const int have_frame_bytes = audio_sdl_have_frame_bytes();
    if (have_frame_bytes <= 0 || g_audio_have.freq <= 0) {
        return SDL_GetQueuedAudioSize(dev) / AUDIO_SRC_FRAME_BYTES;
    }

    const int queued_bytes = (int)SDL_GetQueuedAudioSize(dev);
    const double queued_frames_out = (double)queued_bytes / (double)have_frame_bytes;
    const double queued_frames_src = queued_frames_out * ((double)AUDIO_SRC_FREQ / (double)g_audio_have.freq);
    if (queued_frames_src < 0.0) {
        return 0;
    }
    return (int)queued_frames_src;
#else
    return SDL_GetQueuedAudioSize(dev) / 4;
#endif
}

static int audio_sdl_get_desired_buffered(void) {
#ifdef TARGET_WII_U
    return 1600;
#else
    return 1100;
#endif
}

static void audio_sdl_play(const uint8_t *buf, size_t len) {
    if (audio_sdl_buffered() < 6000) {
#ifdef TARGET_WII_U
        const uint8_t *queue_buf = audio_prepare_wiiu_pcm(buf, len);
        if (g_audio_stream == NULL) {
            SDL_QueueAudio(dev, queue_buf, len);
            return;
        }

        if (SDL_AudioStreamPut(g_audio_stream, queue_buf, (int)len) < 0) {
            return;
        }

        int available = SDL_AudioStreamAvailable(g_audio_stream);
        if (available <= 0) {
            return;
        }

        if (g_audio_stream_buf_size < (size_t)available) {
            uint8_t *new_buf = realloc(g_audio_stream_buf, (size_t)available);
            if (new_buf == NULL) {
                return;
            }
            g_audio_stream_buf = new_buf;
            g_audio_stream_buf_size = (size_t)available;
        }

        int got = SDL_AudioStreamGet(g_audio_stream, g_audio_stream_buf, available);
        if (got > 0) {
            SDL_QueueAudio(dev, g_audio_stream_buf, (size_t)got);
        }
#else
        SDL_QueueAudio(dev, buf, len);
#endif
    }
}

static void audio_sdl_shutdown(void) {
#ifdef TARGET_WII_U
    if (g_audio_stream != NULL) {
        SDL_FreeAudioStream(g_audio_stream);
        g_audio_stream = NULL;
    }
#endif
    if (SDL_WasInit(SDL_INIT_AUDIO)) {
        if (dev != 0) {
            SDL_CloseAudioDevice(dev);
            dev = 0;
        }
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
#ifdef TARGET_WII_U
    free(g_audio_stream_buf);
    g_audio_stream_buf = NULL;
    g_audio_stream_buf_size = 0;
    free(g_audio_swap_buf);
    g_audio_swap_buf = NULL;
    g_audio_swap_buf_size = 0;
#endif
}

struct AudioAPI audio_sdl = {
    audio_sdl_init,
    audio_sdl_buffered,
    audio_sdl_get_desired_buffered,
    audio_sdl_play,
    audio_sdl_shutdown
};

#endif
