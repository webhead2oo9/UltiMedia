#include "audio.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dr_mp3.h"
#include "dr_wav.h"
#include "dr_flac.h"
#include "stb_vorbis_compat.h"
#include "path_io.h"

#define AUDIO_STATE_SNAPSHOT_VERSION 1u
#define SOURCE_BUFFER_FRAMES (SAMPLES_PER_FRAME * 8)

// Global audio state
AudioType current_type = AUDIO_NONE;
void *decoder = NULL;
uint32_t source_rate = 44100;
int source_channels = 2;
uint64_t total_frames = 0;
uint64_t cur_frame = 0;

// Resample state
static double resample_phase = 0.0;
static int16_t resample_in_buf[SOURCE_BUFFER_FRAMES * MAX_CHANNELS];
static int16_t resample_cache[RESAMPLE_CACHE_FRAMES * MAX_CHANNELS];
static int resample_cache_frames = 0;

static bool audio_snapshot_fields_valid(const AudioStateSnapshot *state) {
    if (!state) return false;
    if (state->current_type > AUDIO_FLAC) return false;
    if (state->source_channels < 1 || state->source_channels > MAX_CHANNELS) return false;
    if (state->resample_cache_frames < 0 || state->resample_cache_frames > RESAMPLE_CACHE_FRAMES)
        return false;
    if (!isfinite(state->resample_phase) || state->resample_phase < 0.0 || state->resample_phase >= 1.0)
        return false;
    if ((uint64_t)state->resample_cache_frames > UINT64_MAX - state->cur_frame)
        return false;
    uint64_t decoder_frame = state->cur_frame + (uint64_t)state->resample_cache_frames;

    AudioType state_type = (AudioType)state->current_type;
    if (state_type == AUDIO_NONE) {
        return state->source_rate == 0 &&
               state->total_frames == 0 &&
               state->cur_frame == 0 &&
               state->resample_cache_frames == 0;
    }
    if (state->source_rate == 0 || state->source_rate > (uint32_t)(OUT_RATE * 8))
        return false;
    // Only FLAC and Vorbis can keep decoding past absent or under-reported
    // length metadata. Such a position is malformed for WAV/MP3 snapshots.
    if (decoder_frame > state->total_frames &&
        state_type != AUDIO_FLAC && state_type != AUDIO_OGG)
        return false;

    return true;
}

static bool audio_init_mp3_path(drmp3 *mp3, const char *path) {
    bool opened;
    PATH_OPEN_WIDE_THEN_NARROW(opened, path,
                               drmp3_init_file_w(mp3, wide, NULL) != 0,
                               drmp3_init_file(mp3, path, NULL) != 0,
                               false);
    return opened;
}

static bool audio_init_wav_path(drwav *wav, const char *path) {
    bool opened;
    PATH_OPEN_WIDE_THEN_NARROW(opened, path,
                               drwav_init_file_w(wav, wide, NULL) != 0,
                               drwav_init_file(wav, path, NULL) != 0,
                               false);
    return opened;
}

static drflac *audio_open_flac_path(const char *path) {
    drflac *flac;
    PATH_OPEN_WIDE_THEN_NARROW(flac, path,
                               drflac_open_file_w(wide, NULL),
                               drflac_open_file(path, NULL),
                               (drflac*)NULL);
    return flac;
}

static stb_vorbis *audio_open_ogg_path(const char *path, int *error) {
    FILE *file = path_fopen_read(path);
    if (!file) return NULL;
    return stb_vorbis_open_file(file, 1, error, NULL);
}

static void audio_reset_state_fields(void) {
    current_type = AUDIO_NONE;
    source_rate = 0;
    source_channels = 2;
    total_frames = 0;
    cur_frame = 0;
    resample_phase = 0.0;
    resample_cache_frames = 0;
    memset(resample_cache, 0, sizeof(resample_cache));
}

// Case-insensitive string compare
static int strcasecmp_simple(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = (*s1 >= 'A' && *s1 <= 'Z') ? *s1 + 32 : *s1;
        char c2 = (*s2 >= 'A' && *s2 <= 'Z') ? *s2 + 32 : *s2;
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return *s1 - *s2;
}

int16_t clamp_i16(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)v;
}

static void downmix_frame_lr(const int16_t *buf, int channels, int frame, float *l, float *r, bool vorbis_order) {
    if (channels <= 1) {
        int16_t s = buf[frame * ((channels > 0) ? channels : 1)];
        *l = (float)s;
        *r = (float)s;
        return;
    }
    if (channels == 2) {
        int idx = frame * 2;
        *l = (float)buf[idx];
        *r = (float)buf[idx + 1];
        return;
    }

    int idx = frame * channels;
    if (channels == 3) {
        float fl = (float)buf[idx];
        float fr = (float)buf[idx + 1];
        float fc = (float)buf[idx + 2];
        if (vorbis_order) {
            fc = (float)buf[idx + 1];
            fr = (float)buf[idx + 2];
        }
        *l = fl + 0.707f * fc;
        *r = fr + 0.707f * fc;
        return;
    }
    if (channels == 4) {
        float fl = (float)buf[idx];
        float fr = (float)buf[idx + 1];
        float fsl = (float)buf[idx + 2];
        float fsr = (float)buf[idx + 3];
        *l = fl + 0.707f * fsl;
        *r = fr + 0.707f * fsr;
        return;
    }
    if (channels == 5) {
        float fl = (float)buf[idx];
        float fr = (float)buf[idx + 1];
        float fc = (float)buf[idx + 2];
        float fsl = (float)buf[idx + 3];
        float fsr = (float)buf[idx + 4];
        if (vorbis_order) {
            fc = (float)buf[idx + 1];
            fr = (float)buf[idx + 2];
            fsl = (float)buf[idx + 3];
            fsr = (float)buf[idx + 4];
        }
        *l = fl + 0.707f * fc + 0.707f * fsl;
        *r = fr + 0.707f * fc + 0.707f * fsr;
        return;
    }
    if (channels == 6) {
        float fl = (float)buf[idx];
        float fr = (float)buf[idx + 1];
        float fc = (float)buf[idx + 2];
        float flfe = (float)buf[idx + 3];
        float fsl = (float)buf[idx + 4];
        float fsr = (float)buf[idx + 5];
        if (vorbis_order) {
            fc = (float)buf[idx + 1];
            fr = (float)buf[idx + 2];
            fsl = (float)buf[idx + 3];
            fsr = (float)buf[idx + 4];
            flfe = (float)buf[idx + 5];
        }
        *l = fl + 0.707f * fc + 0.707f * fsl + 0.5f * flfe;
        *r = fr + 0.707f * fc + 0.707f * fsr + 0.5f * flfe;
        return;
    }

    int32_t sum = 0;
    for (int c = 0; c < channels; c++) sum += buf[idx + c];
    float mono = (float)sum / (float)channels;
    *l = mono;
    *r = mono;
}

void audio_init(void) {
    decoder = NULL;
    audio_reset_state_fields();
}

void audio_close(void) {
    if (decoder) {
        if (current_type == AUDIO_MP3) drmp3_uninit((drmp3*)decoder);
        else if (current_type == AUDIO_WAV) drwav_uninit((drwav*)decoder);
        else if (current_type == AUDIO_FLAC) drflac_close((drflac*)decoder);
        else if (current_type == AUDIO_OGG) stb_vorbis_close((stb_vorbis*)decoder);
        if (current_type != AUDIO_OGG && current_type != AUDIO_FLAC) free(decoder);
        decoder = NULL;
    }
    audio_reset_state_fields();
}

void audio_deinit(void) {
    audio_close();
}

bool audio_open_track(const char *path) {
    audio_close();

    const char *ext = strrchr(path, '.');
    bool load_success = false;

    if (ext && strcasecmp_simple(ext, ".mp3") == 0) {
        decoder = malloc(sizeof(drmp3));
        if (decoder && audio_init_mp3_path((drmp3*)decoder, path)) {
            current_type = AUDIO_MP3;
            source_rate = ((drmp3*)decoder)->sampleRate;
            source_channels = ((drmp3*)decoder)->channels;
            // The struct field is UINT64_MAX when Xing/Info length metadata
            // is absent. The public API scans seekable files in that case
            // and otherwise uses the header count without a scan.
            total_frames = drmp3_get_pcm_frame_count((drmp3*)decoder);
            load_success = true;
        }
    } else if (ext && strcasecmp_simple(ext, ".ogg") == 0) {
        int err = 0;
        stb_vorbis* ogg = audio_open_ogg_path(path, &err);
        if (ogg) {
            current_type = AUDIO_OGG;
            decoder = ogg;
            stb_vorbis_info info = stb_vorbis_get_info(ogg);
            source_rate = info.sample_rate;
            source_channels = info.channels;
            total_frames = stb_vorbis_stream_length_in_samples(ogg);
            load_success = true;
        }
    } else if (ext && strcasecmp_simple(ext, ".flac") == 0) {
        drflac* flac = audio_open_flac_path(path);
        if (flac) {
            current_type = AUDIO_FLAC;
            decoder = flac;
            source_rate = flac->sampleRate;
            source_channels = flac->channels;
            total_frames = flac->totalPCMFrameCount;
            load_success = true;
        }
    } else {
        decoder = malloc(sizeof(drwav));
        if (decoder && audio_init_wav_path((drwav*)decoder, path)) {
            current_type = AUDIO_WAV;
            source_rate = ((drwav*)decoder)->sampleRate;
            source_channels = ((drwav*)decoder)->channels;
            total_frames = ((drwav*)decoder)->totalPCMFrameCount;
            load_success = true;
        }
    }

    if (!load_success) {
        if (decoder) { free(decoder); decoder = NULL; }
        return false;
    }

    if (source_channels <= 0) source_channels = 2;
    if (source_channels > MAX_CHANNELS) {
        audio_close();
        return false;
    }
    // A zero rate stalls playback, while rates above the input-buffer ratio
    // would force the resampler to skip source frames.
    if (source_rate == 0 || source_rate > (uint32_t)(OUT_RATE * 8)) {
        audio_close();
        return false;
    }

    resample_phase = 0.0;
    resample_cache_frames = 0;
    cur_frame = 0;

    return true;
}

static uint64_t audio_read_source_frames(int16_t *buffer, uint32_t frame_count) {
    if (!decoder || !buffer || frame_count == 0) return 0;

    if (current_type == AUDIO_MP3)
        return drmp3_read_pcm_frames_s16((drmp3*)decoder, frame_count, buffer);
    if (current_type == AUDIO_WAV)
        return drwav_read_pcm_frames_s16((drwav*)decoder, frame_count, buffer);
    if (current_type == AUDIO_OGG) {
        int frames = stb_vorbis_get_samples_short_interleaved(
            (stb_vorbis*)decoder, source_channels, buffer,
            (int)(frame_count * (uint32_t)source_channels));
        return (frames > 0) ? (uint64_t)frames : 0;
    }
    if (current_type == AUDIO_FLAC)
        return drflac_read_pcm_frames_s16((drflac*)decoder, frame_count, buffer);
    return 0;
}

static bool audio_seek_decoder(uint64_t frame) {
    if (!decoder) return false;
    if (total_frames == 0 && frame > 0) return false;
    if (total_frames > 0 && frame > total_frames) return false;

    if (current_type == AUDIO_MP3)
        return drmp3_seek_to_pcm_frame((drmp3*)decoder, frame) != 0;
    if (current_type == AUDIO_WAV)
        return drwav_seek_to_pcm_frame((drwav*)decoder, frame) != 0;
    if (current_type == AUDIO_OGG) {
        if (frame == 0) return stb_vorbis_seek_start((stb_vorbis*)decoder) != 0;
        if (frame > UINT32_MAX) return false;
        return stb_vorbis_seek((stb_vorbis*)decoder, (unsigned int)frame) != 0;
    }
    if (current_type == AUDIO_FLAC)
        return drflac_seek_to_pcm_frame((drflac*)decoder, frame) != 0;
    return false;
}

static bool audio_discard_source_frames(uint64_t frame_count) {
    while (frame_count > 0) {
        uint32_t request = (frame_count > SOURCE_BUFFER_FRAMES)
            ? SOURCE_BUFFER_FRAMES
            : (uint32_t)frame_count;
        uint64_t read = audio_read_source_frames(resample_in_buf, request);
        if (read != request) return false;
        frame_count -= read;
    }
    return true;
}

static bool audio_position_decoder(uint64_t frame) {
    if (frame == 0) return true;
    // Unknown and under-reported lengths cannot use decoder seek tables
    // reliably. Replaying source PCM from the freshly opened decoder is
    // slower, but preserves an exact save-state position.
    if (total_frames == 0 || frame > total_frames)
        return audio_discard_source_frames(frame);
    return audio_seek_decoder(frame);
}

void audio_capture_state(AudioStateSnapshot *state) {
    if (!state) return;

    memset(state, 0, sizeof(*state));
    state->version = AUDIO_STATE_SNAPSHOT_VERSION;
    state->current_type = (uint32_t)current_type;
    state->source_rate = source_rate;
    state->source_channels = source_channels;
    state->total_frames = total_frames;
    state->cur_frame = cur_frame;
    state->resample_phase = resample_phase;
    state->resample_cache_frames = resample_cache_frames;
    memcpy(state->resample_cache, resample_cache, sizeof(resample_cache));
}

bool audio_snapshot_valid(const AudioStateSnapshot *state) {
    return state &&
           state->version == AUDIO_STATE_SNAPSHOT_VERSION &&
           audio_snapshot_fields_valid(state);
}

bool audio_restore_state(const char *path, const AudioStateSnapshot *state) {
    if (!audio_snapshot_valid(state))
        return false;

    if ((AudioType)state->current_type == AUDIO_NONE) {
        audio_close();
        return true;
    }

    uint64_t resume_frame = state->cur_frame + (uint64_t)state->resample_cache_frames;
    if (!path || !audio_open_track(path)) return false;
    if (current_type != (AudioType)state->current_type ||
        source_rate != state->source_rate ||
        source_channels != state->source_channels ||
        total_frames != state->total_frames ||
        !audio_position_decoder(resume_frame)) {
        audio_close();
        return false;
    }

    cur_frame = state->cur_frame;
    resample_phase = state->resample_phase;
    resample_cache_frames = state->resample_cache_frames;
    memset(resample_cache, 0, sizeof(resample_cache));
    memcpy(resample_cache, state->resample_cache, sizeof(resample_cache));
    return true;
}

bool audio_seek(uint64_t frame) {
    if (!audio_seek_decoder(frame)) return false;

    cur_frame = frame;
    resample_phase = 0.0;
    resample_cache_frames = 0;
    memset(resample_cache, 0, sizeof(resample_cache));
    return true;
}

int audio_read_frame(int16_t *out_buf) {
    if (!decoder) return 0;

    double ratio = (double)source_rate / (double)OUT_RATE;
    double advance_d = resample_phase + (double)SAMPLES_PER_FRAME * ratio;
    uint32_t advance_frames = (uint32_t)advance_d;
    double new_phase = advance_d - (double)advance_frames;

    double max_src_pos = resample_phase + (double)(SAMPLES_PER_FRAME - 1) * ratio;
    uint32_t i2_max = (uint32_t)max_src_pos + 1;
    uint32_t required_frames = i2_max + 1;
    uint32_t frames_to_read = required_frames;
    if (frames_to_read < advance_frames) frames_to_read = advance_frames;
    if (frames_to_read > SOURCE_BUFFER_FRAMES) frames_to_read = SOURCE_BUFFER_FRAMES;

    int channels = source_channels;
    uint32_t cache_frames = (resample_cache_frames > (int)frames_to_read) ? frames_to_read : (uint32_t)resample_cache_frames;
    if (cache_frames > 0) {
        memcpy(resample_in_buf, resample_cache, cache_frames * (uint32_t)channels * sizeof(int16_t));
    }
    uint32_t need_read = (frames_to_read > cache_frames) ? (frames_to_read - cache_frames) : 0;
    uint64_t read = audio_read_source_frames(
        resample_in_buf + cache_frames * (uint32_t)channels, need_read);

    uint32_t total_available = cache_frames + (uint32_t)read;
    if (total_available < 2 || (read < need_read && cur_frame > 1000)) {
        return 0; // End of track
    }

    bool vorbis_order = (current_type == AUDIO_OGG);
    for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
        double src_pos = resample_phase + i * ratio;
        int i1 = (int)src_pos;
        int i2 = i1 + 1;

        // Clamp indices
        if (i1 < 0) i1 = 0;
        if (i2 < 0) i2 = 0;
        if (i1 >= (int)total_available) i1 = (int)total_available - 1;
        if (i2 >= (int)total_available) i2 = i1;

        float frac = (float)(src_pos - i1);

        float l1, r1, l2, r2;
        downmix_frame_lr(resample_in_buf, channels, i1, &l1, &r1, vorbis_order);
        downmix_frame_lr(resample_in_buf, channels, i2, &l2, &r2, vorbis_order);
        float out_l = (1.0f - frac) * l1 + frac * l2;
        float out_r = (1.0f - frac) * r1 + frac * r2;
        out_buf[i*2]   = clamp_i16(out_l);
        out_buf[i*2+1] = clamp_i16(out_r);
    }

    resample_phase = new_phase;
    uint32_t consumed_frames = (advance_frames < total_available) ? advance_frames : total_available;
    if ((uint64_t)consumed_frames > UINT64_MAX - cur_frame) return 0;
    cur_frame += (uint64_t)consumed_frames;

    int overshoot = (int)total_available - (int)consumed_frames;
    if (overshoot < 0) overshoot = 0;
    if (overshoot > RESAMPLE_CACHE_FRAMES) overshoot = RESAMPLE_CACHE_FRAMES;
    resample_cache_frames = overshoot;
    if (overshoot > 0) {
        memcpy(resample_cache,
               resample_in_buf + (total_available - (uint32_t)overshoot) * (uint32_t)channels,
               (uint32_t)overshoot * (uint32_t)channels * sizeof(int16_t));
    }

    return SAMPLES_PER_FRAME;
}
