#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <alsa/asoundlib.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#define SAMPLE_RATE 44100
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float *ring_buffer;
    int buffer_size;
    int head;
} StringState;

typedef struct {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} BPF;

typedef struct {
    float x1, y1;
} APF;

static float process_apf(APF *f, float in, float a) {
    float out = a * in + f->x1 - a * f->y1;
    f->x1 = in;
    f->y1 = out;
    return out;
}

static void update_bpf(BPF *f, float freq, float q) {
    float w0 = 2.0f * (float)M_PI * freq / SAMPLE_RATE;
    float alpha = sinf(w0) / (2.0f * q);
    float a0 = 1.0f + alpha;
    f->b0 = alpha / a0;
    f->b1 = 0.0f;
    f->b2 = -alpha / a0;
    f->a1 = (-2.0f * cosf(w0)) / a0;
    f->a2 = (1.0f - alpha) / a0;
}

static float process_bpf(BPF *f, float in) {
    float out = f->b0 * in + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = in;
    f->y2 = f->y1; f->y1 = out;
    return out;
}

static float formants[5][3] = {
    {600.0f, 1040.0f, 2250.0f}, // A
    {400.0f, 1620.0f, 2400.0f}, // E
    {250.0f, 1750.0f, 2600.0f}, // I
    {400.0f,  750.0f, 2400.0f}, // O
    {350.0f,  600.0f, 2400.0f}  // U
};

static int l_play_string(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int num_notes = 0;
    double *frequencies = NULL;

    lua_getfield(L, 1, "frequency");
    if (lua_istable(L, -1)) {
        num_notes = (int)lua_rawlen(L, -1);
        frequencies = (double *)malloc(sizeof(double) * num_notes);
        for (int i = 1; i <= num_notes; i++) {
            lua_rawgeti(L, -1, i);
            frequencies[i - 1] = luaL_checknumber(L, -1);
            lua_pop(L, 1);
        }
    } else if (lua_isnumber(L, -1)) {
        num_notes = 1;
        frequencies = (double *)malloc(sizeof(double));
        frequencies[0] = luaL_checknumber(L, -1);
    } else {
        return luaL_error(L, "Field 'frequency' (number or table) is required");
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "duration");
    double duration_seconds = luaL_checknumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "decay");
    float decay = (float)luaL_optnumber(L, -1, 0.996);
    lua_pop(L, 1);

    lua_getfield(L, 1, "brightness");
    float brightness = (float)luaL_optnumber(L, -1, 0.5);
    lua_pop(L, 1);

    lua_getfield(L, 1, "volume");
    float volume = (float)luaL_optnumber(L, -1, 0.5);
    lua_pop(L, 1);

    lua_getfield(L, 1, "vowel");
    float vowel_start = (float)luaL_optnumber(L, -1, -1.0);
    lua_pop(L, 1);

    lua_getfield(L, 1, "vowel_end");
    float vowel_end = (float)luaL_optnumber(L, -1, vowel_start);
    lua_pop(L, 1);

    lua_getfield(L, 1, "attack");
    float env_a = (float)luaL_optnumber(L, -1, 0.0);
    lua_pop(L, 1);
    lua_getfield(L, 1, "decay_env");
    float env_d = (float)luaL_optnumber(L, -1, 0.0);
    lua_pop(L, 1);
    lua_getfield(L, 1, "sustain");
    float env_s = (float)luaL_optnumber(L, -1, 1.0);
    lua_pop(L, 1);
    lua_getfield(L, 1, "release");
    float env_r = (float)luaL_optnumber(L, -1, 0.0);
    lua_pop(L, 1);

    lua_getfield(L, 1, "phaser_freq");
    float phaser_freq = (float)luaL_optnumber(L, -1, 0.0);
    lua_getfield(L, 1, "phaser_depth");
    float phaser_depth = (float)luaL_optnumber(L, -1, 0.0);
    lua_getfield(L, 1, "phaser_feedback");
    float phaser_fb = (float)luaL_optnumber(L, -1, 0.0);
    lua_pop(L, 3);

    int err;
    snd_pcm_t *handle;
    snd_pcm_sframes_t frames;

    if ((err = snd_pcm_open(&handle, "default", SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        return luaL_error(L, "Playback open error: %s", snd_strerror(err));
    }

    if ((err = snd_pcm_set_params(handle,
                                  SND_PCM_FORMAT_S16_LE,
                                  SND_PCM_ACCESS_RW_INTERLEAVED,
                                  1,
                                  SAMPLE_RATE,
                                  1,
                                  500000)) < 0) {
        snd_pcm_close(handle);
        return luaL_error(L, "Playback parameters error: %s", snd_strerror(err));
    }

    StringState *strings = (StringState *)malloc(sizeof(StringState) * num_notes);
    for (int n = 0; n < num_notes; n++) {
        strings[n].buffer_size = (int)(SAMPLE_RATE / frequencies[n]);
        strings[n].ring_buffer = (float *)malloc(sizeof(float) * strings[n].buffer_size);
        strings[n].head = 0;

        if (!strings[n].ring_buffer) {
            // Simple cleanup for brevity; in production, loop back and free previously allocated buffers
            return luaL_error(L, "Memory allocation failed for ring buffer %d", n);
        }

        for (int i = 0; i < strings[n].buffer_size; i++) {
            strings[n].ring_buffer[i] = ((float)rand() / (float)(RAND_MAX)) * 2.0f - 1.0f;
        }
    }

    BPF f1 = {0}, f2 = {0}, f3 = {0};
    APF p_stages[4] = {0};
    float p_last_out = 0.0f;

    int total_frames = (int)(SAMPLE_RATE * duration_seconds);
    int attack_frames = (int)(env_a * SAMPLE_RATE);
    int decay_frames = (int)(env_d * SAMPLE_RATE);
    int release_frames = (int)(env_r * SAMPLE_RATE);
    int release_start = total_frames - release_frames;

    // Pre-calculate constants for the inner loop
    float inv_sample_rate = 1.0f / (float)SAMPLE_RATE;
    float one_minus_brightness = 1.0f - brightness;
    float env_diff = 1.0f - env_s;
    float inv_attack = (attack_frames > 0) ? 1.0f / (float)attack_frames : 0.0f;
    float inv_decay = (decay_frames > 0) ? 1.0f / (float)decay_frames : 0.0f;
    float inv_release = (release_frames > 0) ? 1.0f / (float)release_frames : 0.0f;
    float final_vol_scale = volume * 32767.0f;
    float phaser_lfo_step = 2.0f * (float)M_PI * phaser_freq * inv_sample_rate;

    short *output_pcm = (short *)malloc(sizeof(short) * 1024);
    if (!output_pcm) {
        // Cleanup logic would go here
        snd_pcm_close(handle);
        return luaL_error(L, "Memory allocation failed for PCM buffer");
    }

    int processed_frames = 0;
    while (processed_frames < total_frames) {
        int chunk = (total_frames - processed_frames > 1024) ? 1024 : (total_frames - processed_frames);

        if (vowel_start >= 0.0f) {
            float progress = (float)processed_frames / (float)total_frames;
            float current_vowel = vowel_start + (vowel_end - vowel_start) * progress;
            if (current_vowel > 1.0f) current_vowel = 1.0f;
            if (current_vowel < 0.0f) current_vowel = 0.0f;

            float sector = current_vowel * 4.0f;
            int idx = (int)sector;
            float frac = sector - (float)idx;
            if (idx > 3) { idx = 3; frac = 1.0f; }

            update_bpf(&f1, formants[idx][0] * (1.0f - frac) + formants[idx+1][0] * frac, 10.0f);
            update_bpf(&f2, formants[idx][1] * (1.0f - frac) + formants[idx+1][1] * frac, 10.0f);
            update_bpf(&f3, formants[idx][2] * (1.0f - frac) + formants[idx+1][2] * frac, 10.0f);
        }

        for (int i = 0; i < chunk; i++) {
            float mixed_sample = 0.0f;

            int f = processed_frames + i;
            float env = 1.0f;

            // Optimized ADSR calculation using reciprocals
            if (f < attack_frames) {
                env = (float)f * inv_attack;
            } else if (f < attack_frames + decay_frames) {
                env = 1.0f - (float)(f - attack_frames) * inv_decay * env_diff;
            } else {
                env = env_s;
            }

            if (release_frames > 0 && f >= release_start) {
                env *= (1.0f - (float)(f - release_start) * inv_release);
            }

            for (int n = 0; n < num_notes; n++) {
                // Cache structure members in local variables for the hot path
                float *rb = strings[n].ring_buffer;
                int head = strings[n].head;
                int size = strings[n].buffer_size;

                float current_s = rb[head];
                int next_index = head + 1;
                if (next_index >= size) next_index = 0; // Faster than modulo
                float next_s = rb[next_index];

                // Optimized Karplus-Strong calculation
                rb[head] = (current_s * one_minus_brightness + next_s * brightness) * decay;
                strings[n].head = next_index;
                mixed_sample += current_s;
            }
            
            if (vowel_start >= 0.0f) {
                float vocoded = process_bpf(&f1, mixed_sample) + 
                                process_bpf(&f2, mixed_sample) + 
                                process_bpf(&f3, mixed_sample);
                mixed_sample = vocoded * 4.0f; // Gain compensation
            }

            if (phaser_depth > 0.0f) {
                // Use optimized LFO phase calculation
                float lfo = sinf((float)f * phaser_lfo_step);
                float a_coeff = lfo * 0.7f; // Sweep range for APF
                
                float phaser_in = mixed_sample + (p_last_out * phaser_fb);
                float stage_out = phaser_in;
                for (int s = 0; s < 4; s++) {
                    stage_out = process_apf(&p_stages[s], stage_out, a_coeff);
                }
                
                p_last_out = stage_out;
                mixed_sample = mixed_sample + (stage_out * phaser_depth);
            }

            output_pcm[i] = (short)(mixed_sample * env * final_vol_scale);
        }

        frames = snd_pcm_writei(handle, output_pcm, chunk);
        if (frames < 0) {
            frames = snd_pcm_prepare(handle);
        }
        processed_frames += chunk;
    }

    snd_pcm_drain(handle);
    snd_pcm_close(handle);
    for (int n = 0; n < num_notes; n++) {
        free(strings[n].ring_buffer);
    }
    free(strings);
    free(output_pcm);
    free(frequencies);

    return 0;
}

static const struct luaL_Reg kps_funcs[] = {
    {"play", l_play_string},
    {NULL, NULL}
};

int luaopen_kpsintent(lua_State *L) {
    srand((unsigned int)time(NULL));
    luaL_newlib(L, kps_funcs);
    return 1;
}
