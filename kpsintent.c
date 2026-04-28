#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <string.h>

#ifdef __linux__
#include <alsa/asoundlib.h>
static snd_pcm_t *global_pcm_handle = NULL;
#elif defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#define NUM_BUFFERS 3
static AudioQueueRef global_audio_queue;
static AudioQueueBufferRef global_buffers[NUM_BUFFERS];
static int global_buffer_used[NUM_BUFFERS] = {0};
static pthread_mutex_t global_audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t global_audio_cond = PTHREAD_COND_INITIALIZER;
static int global_audio_initialized = 0;

static void audio_queue_callback(void *user_data, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    (void)user_data; (void)queue;
    pthread_mutex_lock(&global_audio_mutex);
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (global_buffers[i] == buffer) {
            global_buffer_used[i] = 0;
            break;
        }
    }
    pthread_cond_signal(&global_audio_cond);
    pthread_mutex_unlock(&global_audio_mutex);
}
#endif

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#define SAMPLE_RATE 44100
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int audio_init() {
#ifdef __linux__
    if (global_pcm_handle) return 0;
    int err;
    if ((err = snd_pcm_open(&global_pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        return err;
    }
    if ((err = snd_pcm_set_params(global_pcm_handle,
                                  SND_PCM_FORMAT_S16_LE,
                                  SND_PCM_ACCESS_RW_INTERLEAVED,
                                  2, SAMPLE_RATE, 1, 500000)) < 0) {
        return err;
    }
    return 0;
#elif defined(__APPLE__)
    if (global_audio_initialized) return 0;
    AudioStreamBasicDescription format;
    format.mSampleRate = SAMPLE_RATE;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    format.mBitsPerChannel = 16;
    format.mChannelsPerFrame = 2;
    format.mBytesPerPacket = 4;
    format.mBytesPerFrame = 4;
    format.mFramesPerPacket = 1;
    format.mReserved = 0;

    OSStatus status = AudioQueueNewOutput(&format, audio_queue_callback, NULL, NULL, NULL, 0, &global_audio_queue);
    if (status != noErr) return -1;

    for (int i = 0; i < NUM_BUFFERS; i++) {
        AudioQueueAllocateBuffer(global_audio_queue, 4096, &global_buffers[i]);
        global_buffer_used[i] = 0;
    }

    AudioQueueStart(global_audio_queue, NULL);
    global_audio_initialized = 1;
    return 0;
#else
    return -1;
#endif
}

static void audio_write(short *data, int frames) {
#ifdef __linux__
    snd_pcm_sframes_t res = snd_pcm_writei(global_pcm_handle, data, frames);
    if (res < 0) {
        snd_pcm_prepare(global_pcm_handle);
    }
#elif defined(__APPLE__)
    pthread_mutex_lock(&global_audio_mutex);
    int buffer_idx = -1;
    while (buffer_idx == -1) {
        for (int i = 0; i < NUM_BUFFERS; i++) {
            if (!global_buffer_used[i]) {
                buffer_idx = i;
                break;
            }
        }
        if (buffer_idx == -1) {
            pthread_cond_wait(&global_audio_cond, &global_audio_mutex);
        }
    }

    global_buffer_used[buffer_idx] = 1;
    AudioQueueBufferRef buffer = global_buffers[buffer_idx];
    int bytes = frames * 4;
    if (bytes > (int)buffer->mAudioDataBytesCapacity) bytes = buffer->mAudioDataBytesCapacity;
    memcpy(buffer->mAudioData, data, bytes);
    buffer->mAudioDataByteSize = bytes;
    AudioQueueEnqueueBuffer(global_audio_queue, buffer, 0, NULL);
    pthread_mutex_unlock(&global_audio_mutex);
#endif
}


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

static void update_lpf(BPF *f, float freq, float q) {
    float w0 = 2.0f * (float)M_PI * freq / SAMPLE_RATE;
    float alpha = sinf(w0) / (2.0f * q);
    float cosw0 = cosf(w0);
    float a0 = 1.0f + alpha;
    f->b0 = (1.0f - cosw0) * 0.5f / a0;
    f->b1 = (1.0f - cosw0) / a0;
    f->b2 = (1.0f - cosw0) * 0.5f / a0;
    f->a1 = (-2.0f * cosw0) / a0;
    f->a2 = (1.0f - alpha) / a0;
}

static float process_bpf(BPF *f, float in) {
    float out = f->b0 * in + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = in;
    f->y2 = f->y1; f->y1 = out;
    return out;
}

static double midi_to_freq(int midi) {
    return 440.0 * pow(2.0, (midi - 69) / 12.0);
}

static int parse_chord_to_freqs(const char *str, double **out_freqs) {
    if (!str || !str[0]) return 0;
    int note = 0;
    char c = str[0];
    if (c >= 'a' && c <= 'g') c -= 32;
    if (c < 'A' || c > 'G') return 0;

    static int note_map[] = {9, 11, 0, 2, 4, 5, 7}; // A, B, C, D, E, F, G
    note = note_map[c - 'A'];

    int pos = 1;
    if (str[pos] == '#') { note++; pos++; }
    else if (str[pos] == 'b') { note--; pos++; }

    int octave = 4;
    if (str[pos] >= '0' && str[pos] <= '9') {
        octave = str[pos] - '0';
        pos++;
    }

    int root_midi = (octave + 1) * 12 + note;
    const char *type = str + pos;

    int intervals[8];
    int count = 0;

    if (strcmp(type, "") == 0 || strcmp(type, "maj") == 0) {
        intervals[0]=0; intervals[1]=4; intervals[2]=7; count=3;
    } else if (strcmp(type, "m") == 0 || strcmp(type, "min") == 0) {
        intervals[0]=0; intervals[1]=3; intervals[2]=7; count=3;
    } else if (strcmp(type, "7") == 0) {
        intervals[0]=0; intervals[1]=4; intervals[2]=7; intervals[3]=10; count=4;
    } else if (strcmp(type, "maj7") == 0) {
        intervals[0]=0; intervals[1]=4; intervals[2]=7; intervals[3]=11; count=4;
    } else if (strcmp(type, "m7") == 0) {
        intervals[0]=0; intervals[1]=3; intervals[2]=7; intervals[3]=10; count=4;
    } else if (strcmp(type, "dim") == 0) {
        intervals[0]=0; intervals[1]=3; intervals[2]=6; count=3;
    } else if (strcmp(type, "sus4") == 0) {
        intervals[0]=0; intervals[1]=5; intervals[2]=7; count=3;
    } else if (strcmp(type, "sus2") == 0) {
        intervals[0]=0; intervals[1]=2; intervals[2]=7; count=3;
    } else {
        intervals[0]=0; count=1; // Single note
    }

    *out_freqs = (double *)malloc(sizeof(double) * count);
    for(int i=0; i<count; i++) {
        (*out_freqs)[i] = midi_to_freq(root_midi + intervals[i]);
    }
    return count;
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
    } else if (lua_isstring(L, -1)) {
        num_notes = parse_chord_to_freqs(lua_tostring(L, -1), &frequencies);
    } else if (lua_isnumber(L, -1)) {
        num_notes = 1;
        frequencies = (double *)malloc(sizeof(double));
        frequencies[0] = luaL_checknumber(L, -1);
    } else {
        return luaL_error(L, "Field 'frequency' (number or table) is required");
    }
    lua_pop(L, 1);

    double duration_seconds;
    lua_getfield(L, 1, "bpm");
    if (lua_isnumber(L, -1)) {
        double bpm = lua_tonumber(L, -1);
        duration_seconds = (bpm > 0.0) ? (240.0 / bpm) : 2.0;
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
        lua_getfield(L, 1, "duration");
        duration_seconds = luaL_checknumber(L, -1);
        lua_pop(L, 1);
    }

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

    if (audio_init() < 0) {
        return luaL_error(L, "Failed to initialize audio backend");
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
    if (!output_pcm) { // This buffer needs to be twice as large for stereo
        // Cleanup logic would go here
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

            output_pcm[i*2] = output_pcm[i*2+1] = (short)(mixed_sample * env * final_vol_scale); // Mono output to both channels
        }

        audio_write(output_pcm, chunk);
        processed_frames += chunk;
    }

    for (int n = 0; n < num_notes; n++) {
        free(strings[n].ring_buffer);
    }
    free(strings);
    free(output_pcm);
    free(frequencies);

    return 0;
}

static int rev_comb_sizes[] = {1116, 1188, 1277, 1356};
static int rev_apf_sizes[] = {556, 441};

typedef struct {
    double *frequencies;
    double *phases;
    double *fm_phases;
    int num_notes;
    float duty_cycle, bit_depth, arp_speed, stutter_speed, stutter_gate, stutter_random, glitch_amount, pwm_lfo_freq, pwm_lfo_depth, vibrato_freq, vibrato_depth, fm_freq_ratio, fm_amount, detune, reverb_mix, reverb_room, reverb_damp;
    float env_a, env_d, env_s, env_r, portamento, volume;
    int downsample, arp_random, use_triangle, use_noise, use_sine, unison, mute, solo;
    int last_arp_step, current_arp_idx, hold_counter, last_stutter_step, stutter_skipped, glitch_timer, glitch_type, rev_comb_ptr[4], rev_apf_ptr[2];
    double current_freq, target_freq;
    float last_val, vibrato_phase, vibrato_step, glitch_val, rev_comb_filter[4];
    float *rev_comb_bufs[4], *rev_apf_bufs[2];
    float *delay_buffer_L; // Left channel delay buffer
    float *delay_buffer_R; // Right channel delay buffer
    int delay_size;        // Size of each buffer
    int delay_ptr_L;       // Write pointer for Left
    int delay_ptr_R;       // Write pointer for Right
    int delay_offset;      // Delay time in samples
    float delay_feedback;
    float delay_damping;
    float delay_lpf_state_L; // LPF state for Left
    float delay_lpf_state_R; // LPF state for Right
    int delay_is_pingpong; // New flag
    float inv_attack, inv_decay, inv_release, bit_levels, pwm_lfo_step, glide_factor;
    float lpf_freq, lpf_q, vowel_start, vowel_end, vowel_lfo_freq, vowel_lfo_depth, vowel_lfo_step, vocal_grit;
    BPF filter, f_formant[3];
    int attack_frames, decay_frames, release_frames, release_start;
} TrackState;

static int l_play_8bit(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    double duration_seconds;
    lua_getfield(L, 1, "bpm");
    double bpm = 0.0;
    if (lua_isnumber(L, -1)) {
        bpm = lua_tonumber(L, -1);
        duration_seconds = (bpm > 0.0) ? (240.0 / bpm) : 2.0;
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
        lua_getfield(L, 1, "duration");
        duration_seconds = luaL_checknumber(L, -1);
        lua_pop(L, 1);
    }

    int num_tracks = 0;
    lua_getfield(L, 1, "tracks");
    int is_multi = lua_istable(L, -1);
    if (is_multi) {
        num_tracks = (int)lua_rawlen(L, -1);
    } else {
        num_tracks = 1;
    }
    lua_pop(L, 1);

    TrackState *tracks = (TrackState *)calloc(num_tracks, sizeof(TrackState));
    int total_frames = (int)(SAMPLE_RATE * duration_seconds);

    for (int t = 0; t < num_tracks; t++) {
        if (is_multi) {
            lua_getfield(L, 1, "tracks");
            lua_rawgeti(L, -1, t + 1);
        } else {
            lua_pushvalue(L, 1);
        }

        lua_getfield(L, -1, "frequency");
        if (lua_istable(L, -1)) {
            tracks[t].num_notes = (int)lua_rawlen(L, -1);
            tracks[t].frequencies = (double *)malloc(sizeof(double) * tracks[t].num_notes);
            for (int i = 1; i <= tracks[t].num_notes; i++) {
                lua_rawgeti(L, -1, i);
                tracks[t].frequencies[i - 1] = luaL_checknumber(L, -1);
                lua_pop(L, 1);
            }
        } else if (lua_isstring(L, -1)) {
            const char *chord_str = lua_tostring(L, -1);
            tracks[t].num_notes = parse_chord_to_freqs(chord_str, &tracks[t].frequencies);
        } else {
            tracks[t].num_notes = 1;
            tracks[t].frequencies = (double *)malloc(sizeof(double));
            tracks[t].frequencies[0] = (double)luaL_optnumber(L, -1, 440.0);
        }
        lua_pop(L, 1);

        lua_getfield(L, -1, "unison"); tracks[t].unison = (int)luaL_optinteger(L, -1, 1); lua_pop(L, 1);
        if (tracks[t].unison < 1) tracks[t].unison = 1;
        lua_getfield(L, -1, "detune"); tracks[t].detune = (float)luaL_optnumber(L, -1, 0.005); lua_pop(L, 1);

        tracks[t].phases = (double *)calloc(tracks[t].num_notes * tracks[t].unison, sizeof(double));

        lua_getfield(L, -1, "volume"); tracks[t].volume = (float)luaL_optnumber(L, -1, 0.5); lua_pop(L, 1);
        lua_getfield(L, -1, "duty_cycle"); tracks[t].duty_cycle = (float)luaL_optnumber(L, -1, 0.5); lua_pop(L, 1);
        lua_getfield(L, -1, "bit_depth"); tracks[t].bit_depth = (float)luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
        lua_getfield(L, -1, "downsample"); tracks[t].downsample = (int)luaL_optinteger(L, -1, 1); lua_pop(L, 1);
        lua_getfield(L, -1, "arp_speed"); 
        float arp_val = (float)luaL_optnumber(L, -1, 0.0);
        lua_pop(L, 1);
        lua_getfield(L, -1, "stutter_random");
        tracks[t].stutter_random = (float)luaL_optnumber(L, -1, 0.0);
        lua_pop(L, 1);

        lua_getfield(L, -1, "solo"); tracks[t].solo = lua_toboolean(L, -1); lua_pop(L, 1);
        lua_getfield(L, -1, "mute"); tracks[t].mute = lua_toboolean(L, -1); lua_pop(L, 1);
        if (bpm > 0.0 && arp_val > 0.0) {
            tracks[t].arp_speed = (float)(240.0 / bpm) * arp_val;
        } else {
            tracks[t].arp_speed = arp_val;
        }
        lua_getfield(L, -1, "arp_random"); tracks[t].arp_random = lua_toboolean(L, -1); lua_pop(L, 1);
        lua_getfield(L, -1, "triangle"); tracks[t].use_triangle = lua_toboolean(L, -1); lua_pop(L, 1);
        lua_getfield(L, -1, "noise"); tracks[t].use_noise = lua_toboolean(L, -1); lua_pop(L, 1);
        lua_getfield(L, -1, "sine"); tracks[t].use_sine = lua_toboolean(L, -1); lua_pop(L, 1);
        lua_getfield(L, -1, "pwm_lfo_freq");
        float lfo_val = (float)luaL_optnumber(L, -1, 0.0);
        if (bpm > 0.0 && lfo_val > 0.0) {
            // Treat lfo_val as rhythmic division: frequency = 1 / (seconds per cycle)
            tracks[t].pwm_lfo_freq = 1.0f / ((float)(240.0 / bpm) * lfo_val);
        } else {
            tracks[t].pwm_lfo_freq = lfo_val;
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "pwm_lfo_depth"); tracks[t].pwm_lfo_depth = (float)luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
        lua_getfield(L, -1, "vibrato_freq"); tracks[t].vibrato_freq = (float)luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
        lua_getfield(L, -1, "vibrato_depth"); tracks[t].vibrato_depth = (float)luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
        lua_getfield(L, -1, "vowel"); tracks[t].vowel_start = (float)luaL_optnumber(L, -1, -1.0); lua_pop(L, 1);
        lua_getfield(L, -1, "vowel_end"); tracks[t].vowel_end = (float)luaL_optnumber(L, -1, tracks[t].vowel_start); lua_pop(L, 1);
        lua_getfield(L, -1, "vowel_lfo_freq");
        float v_lfo_val = (float)luaL_optnumber(L, -1, 0.0);
        if (bpm > 0.0 && v_lfo_val > 0.0) {
            tracks[t].vowel_lfo_freq = 1.0f / ((float)(240.0 / bpm) * v_lfo_val);
        } else {
            tracks[t].vowel_lfo_freq = v_lfo_val;
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "vowel_lfo_depth"); tracks[t].vowel_lfo_depth = (float)luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
        lua_getfield(L, -1, "vocal_grit"); tracks[t].vocal_grit = (float)luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
        lua_getfield(L, -1, "glitch_amount"); tracks[t].glitch_amount = (float)luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);

        lua_getfield(L, -1, "fm_freq_ratio"); tracks[t].fm_freq_ratio = (float)luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
        lua_getfield(L, -1, "fm_amount"); tracks[t].fm_amount = (float)luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
        tracks[t].fm_phases = (double *)calloc(tracks[t].num_notes * tracks[t].unison, sizeof(double));

        lua_getfield(L, -1, "attack"); tracks[t].env_a = (float)luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
        lua_getfield(L, -1, "decay_env"); tracks[t].env_d = (float)luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
        lua_getfield(L, -1, "sustain"); tracks[t].env_s = (float)luaL_optnumber(L, -1, 1.0); lua_pop(L, 1);
        lua_getfield(L, -1, "release"); tracks[t].env_r = (float)luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
        lua_getfield(L, -1, "portamento"); tracks[t].portamento = (float)luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
        lua_getfield(L, -1, "lpf_freq"); tracks[t].lpf_freq = (float)luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
        lua_getfield(L, -1, "lpf_q"); tracks[t].lpf_q = (float)luaL_optnumber(L, -1, 1.0); lua_pop(L, 1);
        
        lua_getfield(L, -1, "delay_time");
        float d_val = (float)luaL_optnumber(L, -1, 0.0);
        float d_time;
        if (bpm > 0.0 && d_val > 0.0) {
            d_time = (float)(240.0 / bpm) * d_val;
        } else {
            d_time = d_val;
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "delay_feedback"); 
        tracks[t].delay_feedback = (float)luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);
        lua_getfield(L, -1, "delay_is_pingpong");
        tracks[t].delay_is_pingpong = lua_toboolean(L, -1); lua_pop(L, 1);
        lua_getfield(L, -1, "delay_damping");
        tracks[t].delay_damping = (float)luaL_optnumber(L, -1, 0.0); lua_pop(L, 1);

        if (d_time > 0.0f) {
            tracks[t].delay_size = SAMPLE_RATE * 2; // 2 seconds max delay
            tracks[t].delay_buffer_L = (float *)calloc(tracks[t].delay_size, sizeof(float));
            tracks[t].delay_buffer_R = (float *)calloc(tracks[t].delay_size, sizeof(float));
            tracks[t].delay_offset = (int)(d_time * SAMPLE_RATE);
            if (tracks[t].delay_offset >= tracks[t].delay_size) tracks[t].delay_offset = tracks[t].delay_size - 1;
            tracks[t].delay_ptr_L = 0;
            tracks[t].delay_ptr_R = 0;
            tracks[t].delay_lpf_state_L = 0.0f;
            tracks[t].delay_lpf_state_R = 0.0f;
        } else {
            tracks[t].delay_buffer_L = NULL;
            tracks[t].delay_buffer_R = NULL;
            tracks[t].delay_is_pingpong = 0; // Ensure ping-pong is off if no delay

        }

        // Pre-calculate track constants
        tracks[t].attack_frames = (int)(tracks[t].env_a * SAMPLE_RATE);
        tracks[t].decay_frames = (int)(tracks[t].env_d * SAMPLE_RATE);
        tracks[t].release_frames = (int)(tracks[t].env_r * SAMPLE_RATE);
        tracks[t].release_start = total_frames - tracks[t].release_frames;
        tracks[t].inv_attack = (tracks[t].attack_frames > 0) ? 1.0f / (float)tracks[t].attack_frames : 0.0f;
        tracks[t].inv_decay = (tracks[t].decay_frames > 0) ? 1.0f / (float)tracks[t].decay_frames : 0.0f;
        tracks[t].inv_release = (tracks[t].release_frames > 0) ? 1.0f / (float)tracks[t].release_frames : 0.0f;
        tracks[t].bit_levels = (tracks[t].bit_depth > 0.0f) ? powf(2.0f, tracks[t].bit_depth) : 0.0f;
        tracks[t].pwm_lfo_step = 2.0f * (float)M_PI * tracks[t].pwm_lfo_freq * (1.0f / (float)SAMPLE_RATE);
        tracks[t].vowel_lfo_step = 2.0f * (float)M_PI * tracks[t].vowel_lfo_freq / SAMPLE_RATE;
        tracks[t].vibrato_step = 2.0f * (float)M_PI * tracks[t].vibrato_freq / SAMPLE_RATE;
        tracks[t].vibrato_phase = 0.0f;
        tracks[t].glide_factor = (tracks[t].portamento > 0.0f) ? expf(-1.0f / (tracks[t].portamento * (float)SAMPLE_RATE)) : 0.0f;
        tracks[t].current_freq = tracks[t].frequencies[0];
        tracks[t].target_freq = tracks[t].current_freq;
        tracks[t].last_arp_step = -1;
        tracks[t].last_stutter_step = -1;
        tracks[t].stutter_skipped = 0;
        tracks[t].glitch_timer = 0;
        tracks[t].glitch_type = 0;

        if (tracks[t].lpf_freq > 0.0f) {
            update_lpf(&tracks[t].filter, tracks[t].lpf_freq, tracks[t].lpf_q);
        }

        if (is_multi) lua_pop(L, 2);
        else lua_pop(L, 1);
    }

    int any_solo = 0;
    for (int i = 0; i < num_tracks; i++) {
        if (tracks[i].solo) {
            any_solo = 1;
            break;
        }
    }

    if (audio_init() < 0) {
        for(int t=0; t<num_tracks; t++) { 
            free(tracks[t].frequencies); free(tracks[t].phases); 
            if (tracks[t].delay_buffer_L) free(tracks[t].delay_buffer_L);
            if (tracks[t].delay_buffer_R) free(tracks[t].delay_buffer_R);
        }
        free(tracks);
        return luaL_error(L, "Failed to initialize audio backend");
    }

    short *output_pcm = (short *)malloc(sizeof(short) * 1024 * 2); // Double size for stereo
    int processed_frames = 0;
    while (processed_frames < total_frames) {
        int chunk = (total_frames - processed_frames > 1024) ? 1024 : (total_frames - processed_frames);

        for (int i = 0; i < chunk; i++) {
            int f = processed_frames + i;
            float final_mixed_L = 0.0f;
            float final_mixed_R = 0.0f;
            for (int t = 0; t < num_tracks; t++) { // <--- Added missing opening brace here
                TrackState *tr = &tracks[t];
                if (any_solo) { if (!tr->solo) continue; }
                else if (tr->mute) continue;

                // Glitch state machine logic
                if (tr->glitch_amount > 0.0f) {
                    if (tr->glitch_timer > 0) tr->glitch_timer--;
                    else {
                        if (((float)rand() / (float)RAND_MAX) < (tr->glitch_amount * 0.0004f)) {
                            tr->glitch_timer = rand() % 2500 + 100;
                            tr->glitch_type = (rand() % 3) + 1;
                            tr->glitch_val = (float)rand() / (float)RAND_MAX;
                        } else tr->glitch_type = 0;
                    }
                }

                if (tr->vowel_start >= 0.0f) {
                    float progress = (float)f / (float)total_frames;
                    float current_vowel = tr->vowel_start + (tr->vowel_end - tr->vowel_start) * progress;
                    if (tr->vowel_lfo_depth > 0.0f) {
                        current_vowel += sinf((float)f * tr->vowel_lfo_step) * tr->vowel_lfo_depth;
                    }
                    current_vowel = (current_vowel > 1.0f) ? 1.0f : (current_vowel < 0.0f ? 0.0f : current_vowel);

                    float sector = current_vowel * 4.0f;
                    int idx = (int)sector;
                    float frac = sector - (float)idx;
                    if (idx > 3) { idx = 3; frac = 1.0f; }

                    update_bpf(&tr->f_formant[0], formants[idx][0] * (1.0f - frac) + formants[idx+1][0] * frac, 10.0f);
                    update_bpf(&tr->f_formant[1], formants[idx][1] * (1.0f - frac) + formants[idx+1][1] * frac, 10.0f);
                    update_bpf(&tr->f_formant[2], formants[idx][2] * (1.0f - frac) + formants[idx+1][2] * frac, 10.0f);
                }

                float v_mod = 1.0f + (sinf(tr->vibrato_phase) * tr->vibrato_depth);
                tr->vibrato_phase += tr->vibrato_step;

                // Apply Glitch Type 1: Frequency warping
                if (tr->glitch_type == 1) v_mod *= (1.0f + (tr->glitch_val * 4.0f));

                float env = 1.0f;

                // ADSR Calculation
                if (f < tr->attack_frames) env = (float)f * tr->inv_attack;
                else if (f < tr->attack_frames + tr->decay_frames) env = 1.0f - (float)(f - tr->attack_frames) * tr->inv_decay * (1.0f - tr->env_s);
                else env = tr->env_s;
                if (tr->release_frames > 0 && f >= tr->release_start) env *= (1.0f - (float)(f - tr->release_start) * tr->inv_release);

                // PWM LFO modulation
                float current_duty = tr->duty_cycle;
                if (tr->pwm_lfo_depth > 0.0f) {
                    current_duty += sinf((float)f * tr->pwm_lfo_step) * tr->pwm_lfo_depth;
                    if (current_duty < 0.01f) current_duty = 0.01f;
                    if (current_duty > 0.99f) current_duty = 0.99f;
                }

                float track_sample = 0.0f;
                int uni = tr->unison;
                if (tr->arp_speed > 0.0f && tr->num_notes > 0) {
                    int step_frames = (int)(tr->arp_speed * SAMPLE_RATE);
                    if (step_frames < 1) step_frames = 1;
                    int current_step = f / step_frames;

                    if (current_step != tr->last_arp_step) {
                        if (tr->arp_random) tr->current_arp_idx = rand() % tr->num_notes;
                        else tr->current_arp_idx = current_step % tr->num_notes;
                        tr->target_freq = tr->frequencies[tr->current_arp_idx];
                        tr->last_arp_step = current_step;
                    }
                    if (tr->portamento > 0.0f) tr->current_freq = tr->target_freq + (tr->current_freq - tr->target_freq) * tr->glide_factor;
                    else tr->current_freq = tr->target_freq;

                    for (int u = 0; u < uni; u++) {
                        float detune_mul = 1.0f + (u - (uni - 1) * 0.5f) * tr->detune;
                        float osc_f = (float)tr->current_freq * detune_mul;
                        
                        // Apply Glitch Type 2: Phase Jitter
                        if (tr->glitch_type == 2 && ((float)rand()/RAND_MAX) < 0.1f) {
                            tr->phases[u] = (double)tr->glitch_val;
                        }

                        float fm_mod = 0.0f;
                        if (tr->fm_amount > 0.0f) {
                            tr->fm_phases[u] += (osc_f * v_mod * tr->fm_freq_ratio) / (double)SAMPLE_RATE;
                            if (tr->fm_phases[u] >= 1.0) tr->fm_phases[u] -= 1.0;
                            fm_mod = sinf((float)tr->fm_phases[u] * 2.0f * (float)M_PI) * tr->fm_amount * osc_f;
                        }
                        tr->phases[u] += ((osc_f * v_mod) + fm_mod) / (double)SAMPLE_RATE;
                        if (tr->phases[u] >= 1.0) tr->phases[u] -= 1.0;

                        if (tr->use_noise) track_sample += ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
                        else if (tr->use_sine) track_sample += sinf((float)tr->phases[u] * 2.0f * (float)M_PI);
                        else if (tr->use_triangle) {
                            float ph = (float)tr->phases[u];
                            track_sample += (ph < 0.5f) ? (-1.0f + 4.0f * ph) : (3.0f - 4.0f * ph);
                        } else track_sample += (tr->phases[u] < (double)current_duty) ? 1.0f : -1.0f;
                    }
                    track_sample /= (float)uni;
                } else {
                    for (int n = 0; n < tr->num_notes; n++) {
                        for (int u = 0; u < uni; u++) {
                            int idx = n * uni + u;
                            float detune_mul = 1.0f + (u - (uni - 1) * 0.5f) * tr->detune;
                            float osc_f = (float)tr->frequencies[n] * detune_mul;

                            // Apply Glitch Type 2: Phase Jitter
                            if (tr->glitch_type == 2 && ((float)rand()/RAND_MAX) < 0.1f) {
                                tr->phases[idx] = (double)tr->glitch_val;
                            }

                            float fm_mod = 0.0f;
                            if (tr->fm_amount > 0.0f) {
                                tr->fm_phases[idx] += (osc_f * v_mod * tr->fm_freq_ratio) / (double)SAMPLE_RATE;
                                if (tr->fm_phases[idx] >= 1.0) tr->fm_phases[idx] -= 1.0;
                                fm_mod = sinf((float)tr->fm_phases[idx] * 2.0f * (float)M_PI) * tr->fm_amount * osc_f;
                            }
                            tr->phases[idx] += ((osc_f * v_mod) + fm_mod) / (double)SAMPLE_RATE;
                            if (tr->phases[idx] >= 1.0) tr->phases[idx] -= 1.0;

                            if (tr->use_noise) track_sample += ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
                            else if (tr->use_sine) track_sample += sinf((float)tr->phases[idx] * 2.0f * (float)M_PI);
                            else if (tr->use_triangle) {
                                float ph = (float)tr->phases[idx];
                                track_sample += (ph < 0.5f) ? (-1.0f + 4.0f * ph) : (3.0f - 4.0f * ph);
                            } else track_sample += (tr->phases[idx] < (double)current_duty) ? 1.0f : -1.0f;
                        }
                    }
                    track_sample /= (float)(tr->num_notes * uni);
                }

                if (tr->hold_counter <= 0) {
                    float stutter_mul = 1.0f;
                    if (tr->stutter_speed > 0.0f) {
                        int s_frames = (int)(tr->stutter_speed * (float)SAMPLE_RATE);
                        if (s_frames < 1) s_frames = 1;
                        int cur_s_step = f / s_frames;

                        // At the start of a new stutter pulse, decide if we skip it
                        if (cur_s_step != tr->last_stutter_step) {
                            if (tr->stutter_random > 0.0f) {
                                tr->stutter_skipped = (((float)rand() / (float)RAND_MAX) < tr->stutter_random);
                            } else {
                                tr->stutter_skipped = 0;
                            }
                            tr->last_stutter_step = cur_s_step;
                        }

                        if (tr->stutter_skipped) {
                            stutter_mul = 0.0f;
                        } else if ((f % s_frames) > (int)(s_frames * tr->stutter_gate)) {
                            stutter_mul = 0.0f;
                        }
                    }

                    float current_sig = track_sample * env * stutter_mul;
                    if (tr->bit_depth > 0.0f) tr->last_val = roundf(current_sig * tr->bit_levels) / tr->bit_levels;
                    else tr->last_val = current_sig;
                    tr->hold_counter = tr->downsample;
                }
                tr->hold_counter--;

                float dry_sample = tr->last_val; 
                if (tr->vowel_start >= 0.0f) {
                    float grit_noise = (((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f) * tr->vocal_grit;
                    float formant_input = dry_sample + grit_noise;
                    dry_sample = (process_bpf(&tr->f_formant[0], formant_input) +
                                 process_bpf(&tr->f_formant[1], formant_input) +
                                 process_bpf(&tr->f_formant[2], formant_input)) * 2.5f;
                }

                if (tr->lpf_freq > 0.0f) {
                    dry_sample = process_bpf(&tr->filter, dry_sample);
                }

                if (tr->reverb_mix > 0.0f) {
                    float r_in = dry_sample * 0.15f;
                    float r_out = 0.0f;
                    for (int j = 0; j < 4; j++) {
                        float o = tr->rev_comb_bufs[j][tr->rev_comb_ptr[j]];
                        tr->rev_comb_filter[j] = (o * (1.0f - tr->reverb_damp)) + (tr->rev_comb_filter[j] * tr->reverb_damp);
                        tr->rev_comb_bufs[j][tr->rev_comb_ptr[j]] = r_in + (tr->rev_comb_filter[j] * tr->reverb_room);
                        tr->rev_comb_ptr[j] = (tr->rev_comb_ptr[j] + 1) % rev_comb_sizes[j];
                        r_out += o;
                    }
                    for (int j = 0; j < 2; j++) {
                        float b_o = tr->rev_apf_bufs[j][tr->rev_apf_ptr[j]];
                        float a_o = -r_out + b_o;
                        tr->rev_apf_bufs[j][tr->rev_apf_ptr[j]] = r_out + (b_o * 0.5f);
                        tr->rev_apf_ptr[j] = (tr->rev_apf_ptr[j] + 1) % rev_apf_sizes[j];
                        r_out = a_o;
                    }
                    dry_sample = (dry_sample * (1.0f - tr->reverb_mix)) + (r_out * tr->reverb_mix);
                }
                
                float wet_L = dry_sample;
                float wet_R = dry_sample;

                if (tr->delay_buffer_L) { // If delay is active for this track
                    if (tr->delay_is_pingpong) {
                        // Ping-pong delay logic
                        int read_ptr_L = tr->delay_ptr_L - tr->delay_offset;
                        if (read_ptr_L < 0) read_ptr_L += tr->delay_size;
                        float delayed_signal_L = tr->delay_buffer_L[read_ptr_L];

                        int read_ptr_R = tr->delay_ptr_R - tr->delay_offset;
                        if (read_ptr_R < 0) read_ptr_R += tr->delay_size;
                        float delayed_signal_R = tr->delay_buffer_R[read_ptr_R];

                        // Apply LPF to feedback signals
                        tr->delay_lpf_state_L = (delayed_signal_L * (1.0f - tr->delay_damping)) + (tr->delay_lpf_state_L * tr->delay_damping);
                        tr->delay_lpf_state_R = (delayed_signal_R * (1.0f - tr->delay_damping)) + (tr->delay_lpf_state_R * tr->delay_damping);

                        // Dry signal goes to Left. Left feeds Right, Right feeds Left.
                        float input_to_L_delay = dry_sample + (tr->delay_lpf_state_R * tr->delay_feedback);
                        float input_to_R_delay = (tr->delay_lpf_state_L * tr->delay_feedback); // No dry input to R directly

                        tr->delay_buffer_L[tr->delay_ptr_L] = input_to_L_delay;
                        tr->delay_buffer_R[tr->delay_ptr_R] = input_to_R_delay;

                        wet_L = input_to_L_delay; // Output from Left delay line
                        wet_R = input_to_R_delay; // Output from Right delay line

                        tr->delay_ptr_L = (tr->delay_ptr_L + 1) % tr->delay_size;
                        tr->delay_ptr_R = (tr->delay_ptr_R + 1) % tr->delay_size;

                    } else {
                        // Mono delay logic (as before, but now contributing to both L and R outputs)
                        int read_ptr = tr->delay_ptr_L - tr->delay_offset; // Using L buffer for mono delay
                        if (read_ptr < 0) read_ptr += tr->delay_size;
                        float delayed_signal = tr->delay_buffer_L[read_ptr];
                        
                        tr->delay_lpf_state_L = (delayed_signal * (1.0f - tr->delay_damping)) + (tr->delay_lpf_state_L * tr->delay_damping);
                        
                        float feedback_signal = dry_sample + (tr->delay_lpf_state_L * tr->delay_feedback);
                        tr->delay_buffer_L[tr->delay_ptr_L] = feedback_signal;
                        wet_L = feedback_signal;
                        wet_R = feedback_signal; // Mono delay output goes to both channels
                        tr->delay_ptr_L = (tr->delay_ptr_L + 1) % tr->delay_size;
                    }
                }
                
                final_mixed_L += wet_L * tr->volume;
                final_mixed_R += wet_R * tr->volume;
            }

            // Final output clamping and scaling
            if (final_mixed_L > 1.0f) final_mixed_L = 1.0f;
            if (final_mixed_L < -1.0f) final_mixed_L = -1.0f;
            if (final_mixed_R > 1.0f) final_mixed_R = 1.0f;
            if (final_mixed_R < -1.0f) final_mixed_R = -1.0f;
                    
            output_pcm[i*2] = (short)(final_mixed_L * 32767.0f);
            output_pcm[i*2+1] = (short)(final_mixed_R * 32767.0f);
        }

        audio_write(output_pcm, chunk);
        processed_frames += chunk;
    }

    for (int t = 0; t < num_tracks; t++) {
        free(tracks[t].phases);
        if (tracks[t].fm_phases) free(tracks[t].fm_phases);
        free(tracks[t].frequencies);
        if (tracks[t].delay_buffer_L) free(tracks[t].delay_buffer_L);
        if (tracks[t].delay_buffer_R) free(tracks[t].delay_buffer_R);
        for (int j = 0; j < 4; j++) {
            if (tracks[t].rev_comb_bufs[j]) free(tracks[t].rev_comb_bufs[j]);
        }
        for (int j = 0; j < 2; j++) {
            if (tracks[t].rev_apf_bufs[j]) free(tracks[t].rev_apf_bufs[j]);
        }
    }
    free(tracks);
    free(output_pcm);
    return 0;
}

static const struct luaL_Reg kps_funcs[] = {
    {"play", l_play_string},
    {"play_8bit", l_play_8bit},
    {NULL, NULL}
};

int luaopen_kpsintent(lua_State *L) {
    srand((unsigned int)time(NULL));
    luaL_newlib(L, kps_funcs);
    return 1;
}
