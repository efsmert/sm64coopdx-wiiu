#include "types.h"
#include "smlua_audio_utils.h"
#include <stdlib.h>
#include <string.h>

static struct ModAudio* audio_alloc_dummy(const char* filename, bool isStream) {
    struct ModAudio* audio = calloc(1, sizeof(struct ModAudio));
    if (audio == NULL) {
        return NULL;
    }
    if (filename != NULL) {
        audio->filepath = strdup(filename);
    }
    audio->isStream = isStream;
    audio->loaded = true;
    audio->baseVolume = 1.0f;
    return audio;
}

void smlua_audio_utils_reset_all(void) {
}

bool smlua_audio_utils_override(u8 sequenceId, s32* bankId, void** seqData) {
    (void)sequenceId;
    (void)bankId;
    (void)seqData;
    return false;
}

void smlua_audio_utils_replace_sequence(u8 sequenceId, u8 bankId, u8 defaultVolume, const char* m64Name) {
    (void)sequenceId;
    (void)bankId;
    (void)defaultVolume;
    (void)m64Name;
}

struct ModAudio* audio_stream_load(const char* filename) {
    return audio_alloc_dummy(filename, true);
}

void audio_stream_destroy(struct ModAudio* audio) {
    if (audio == NULL) {
        return;
    }
    if (audio->filepath != NULL) {
        free((void*)audio->filepath);
    }
    free(audio);
}

void audio_stream_play(struct ModAudio* audio, bool restart, f32 volume) {
    (void)audio;
    (void)restart;
    (void)volume;
}

void audio_stream_pause(struct ModAudio* audio) {
    (void)audio;
}

void audio_stream_stop(struct ModAudio* audio) {
    (void)audio;
}

f32 audio_stream_get_position(struct ModAudio* audio) {
    (void)audio;
    return 0.0f;
}

void audio_stream_set_position(struct ModAudio* audio, f32 pos) {
    (void)audio;
    (void)pos;
}

bool audio_stream_get_looping(struct ModAudio* audio) {
    (void)audio;
    return false;
}

void audio_stream_set_looping(struct ModAudio* audio, bool looping) {
    (void)audio;
    (void)looping;
}

void audio_stream_set_loop_points(struct ModAudio* audio, s64 loopStart, s64 loopEnd) {
    (void)audio;
    (void)loopStart;
    (void)loopEnd;
}

f32 audio_stream_get_frequency(struct ModAudio* audio) {
    (void)audio;
    return 1.0f;
}

void audio_stream_set_frequency(struct ModAudio* audio, f32 freq) {
    (void)audio;
    (void)freq;
}

f32 audio_stream_get_volume(struct ModAudio* audio) {
    if (audio == NULL) {
        return 0.0f;
    }
    return audio->baseVolume;
}

void audio_stream_set_volume(struct ModAudio* audio, f32 volume) {
    if (audio == NULL) {
        return;
    }
    audio->baseVolume = volume;
}

void audio_sample_destroy_pending_copies(void) {
}

struct ModAudio* audio_sample_load(const char* filename) {
    return audio_alloc_dummy(filename, false);
}

void audio_sample_destroy(struct ModAudio* audio) {
    audio_stream_destroy(audio);
}

void audio_sample_stop(struct ModAudio* audio) {
    (void)audio;
}

void audio_sample_play(struct ModAudio* audio, Vec3f position, f32 volume) {
    (void)audio;
    (void)position;
    (void)volume;
}

void audio_custom_update_volume(void) {
}

void audio_custom_shutdown(void) {
}

void smlua_audio_custom_deinit(void) {
}
