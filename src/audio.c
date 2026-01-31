#include "audio.h"
#include <stdlib.h>
#include <string.h>

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#include "stb_vorbis.c"

// Global audio state
AudioType current_type = AUDIO_NONE;
void *decoder = NULL;
uint32_t source_rate = 44100;
int source_channels = 2;
uint64_t total_frames = 0;
uint64_t cur_frame = 0;

// Resample state
static double resample_phase = 0.0;
static int16_t resample_in_buf[SAMPLES_PER_FRAME * 8 * MAX_CHANNELS];
static int16_t resample_cache[RESAMPLE_CACHE_FRAMES * MAX_CHANNELS];
static int resample_cache_frames = 0;

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
    current_type = AUDIO_NONE;
    decoder = NULL;
    resample_phase = 0.0;
    resample_cache_frames = 0;
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
    current_type = AUDIO_NONE;
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
        if (decoder && drmp3_init_file((drmp3*)decoder, path, NULL)) {
            current_type = AUDIO_MP3;
            source_rate = ((drmp3*)decoder)->sampleRate;
            source_channels = ((drmp3*)decoder)->channels;
            total_frames = ((drmp3*)decoder)->totalPCMFrameCount;
            load_success = true;
        }
    } else if (ext && strcasecmp_simple(ext, ".ogg") == 0) {
        int err = 0;
        stb_vorbis* ogg = stb_vorbis_open_filename(path, &err, NULL);
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
        drflac* flac = drflac_open_file(path, NULL);
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
        if (decoder && drwav_init_file((drwav*)decoder, path, NULL)) {
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

    resample_phase = 0.0;
    resample_cache_frames = 0;
    cur_frame = 0;

    return true;
}

void audio_seek(uint64_t frame) {
    cur_frame = frame;
    if (current_type == AUDIO_MP3) drmp3_seek_to_pcm_frame((drmp3*)decoder, cur_frame);
    else if (current_type == AUDIO_WAV) drwav_seek_to_pcm_frame((drwav*)decoder, cur_frame);
    else if (current_type == AUDIO_OGG) stb_vorbis_seek((stb_vorbis*)decoder, cur_frame);
    else if (current_type == AUDIO_FLAC) drflac_seek_to_pcm_frame((drflac*)decoder, cur_frame);
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
    if (frames_to_read > SAMPLES_PER_FRAME * 8) frames_to_read = SAMPLES_PER_FRAME * 8;

    uint64_t read = 0;
    int channels = source_channels;

    uint32_t cache_frames = (resample_cache_frames > (int)frames_to_read) ? frames_to_read : (uint32_t)resample_cache_frames;
    if (cache_frames > 0) {
        memcpy(resample_in_buf, resample_cache, cache_frames * (uint32_t)channels * sizeof(int16_t));
    }
    uint32_t need_read = (frames_to_read > cache_frames) ? (frames_to_read - cache_frames) : 0;

    if (current_type == AUDIO_MP3) {
        read = drmp3_read_pcm_frames_s16((drmp3*)decoder, need_read, resample_in_buf + cache_frames * channels);
    } else if (current_type == AUDIO_WAV) {
        read = drwav_read_pcm_frames_s16((drwav*)decoder, need_read, resample_in_buf + cache_frames * channels);
    } else if (current_type == AUDIO_OGG) {
        read = stb_vorbis_get_samples_short_interleaved((stb_vorbis*)decoder, channels, resample_in_buf + cache_frames * channels, need_read * channels);
    } else if (current_type == AUDIO_FLAC) {
        read = drflac_read_pcm_frames_s16((drflac*)decoder, need_read, resample_in_buf + cache_frames * channels);
    }

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
    cur_frame += (uint64_t)advance_frames;

    int overshoot = (int)total_available - (int)advance_frames;
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
