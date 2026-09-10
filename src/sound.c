/*
 * TermmiK
 * Copyright (C) 2026 SfymmiK
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Key sound effects: play a short sample on every keypress through ALSA.
// 16-bit PCM WAV files are parsed in-house (no decoder dependencies); the
// special path "default" synthesizes a mechanical-style click. Everything
// degrades silently if ALSA or the sample is unavailable — typing must
// never be affected.

#include "sound.h"
#include "config.h"
#include "alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef DISABLE_SOUND
#include <alsa/asoundlib.h>

static snd_pcm_t *pcm = NULL;
static short *samples = NULL;   // interleaved frames
static int sample_frames = 0;
static int enabled = 0;

static void warn_once(const char *msg) {
    static int warned = 0;
    if (!warned) {
        warned = 1;
        my_print(msg);
        my_print(" (typing sound disabled)\n");
    }
}

// Parse a 16-bit PCM WAV file. Returns interleaved samples or NULL.
static short *load_wav(const char *path, int *out_frames, int *out_channels) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fclose(f);
        return NULL;
    }

    int fmt_channels = 0, fmt_rate = 0, fmt_bits = 0, fmt_format = 1;
    short *data = NULL;
    int data_size = 0;

    for (;;) {
        unsigned char ch[8];
        if (fread(ch, 1, 8, f) != 8) break;
        int size = (int)((uint32_t)ch[4] | ((uint32_t)ch[5] << 8) |
                         ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24));
        if (memcmp(ch, "fmt ", 4) == 0) {
            unsigned char fmt[16];
            if (size < 16 || fread(fmt, 1, 16, f) != 16) break;
            if (size > 16) fseek(f, size - 16, SEEK_CUR);
            fmt_format = (int)(fmt[0] | (fmt[1] << 8));
            fmt_channels = (int)(fmt[2] | (fmt[3] << 8));
            fmt_rate = (int)(fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24));
            fmt_bits = (int)(fmt[14] | (fmt[15] << 8));
        } else if (memcmp(ch, "data", 4) == 0) {
            if (size <= 0 || size > (64 << 20)) break;
            data = malloc(size);
            if (!data) break;
            data_size = fread(data, 1, size, f);
            break; // everything after data is ignored
        } else {
            fseek(f, size + (size & 1), SEEK_CUR); // skip (pad to even)
        }
    }
    fclose(f);

    if (!data || fmt_format != 1 || fmt_bits != 16 || fmt_channels < 1 || fmt_channels > 2) {
        free(data);
        return NULL;
    }
    (void)fmt_rate;
    int bytes_per_frame = fmt_channels * 2;
    *out_frames = data_size / bytes_per_frame;
    *out_channels = fmt_channels;
    return data;
}

// Synthesize a short mechanical-style click: a burst of filtered noise with
// an exponential decay (~16ms at 48kHz mono). No asset files needed.
static short *synth_click(int *out_frames, int *out_channels) {
    const int rate = 48000;
    const int len = rate * 16 / 1000; // 16 ms
    short *buf = malloc(len * sizeof(short));
    if (!buf) return NULL;
    unsigned int seed = 0x1234abcd;
    float lp = 0.0f;
    for (int i = 0; i < len; i++) {
        float t = (float)i / len;
        float env = expf(-t * 9.0f) * (1.0f - t * 0.3f);
        seed = seed * 1103515245 + 12345;
        float noise = ((int)(seed >> 16) % 2000) / 1000.0f - 1.0f;
        lp += 0.35f * (noise - lp); // soften the highs a little
        float v = lp * env * 0.85f;
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        buf[i] = (short)(v * 32000);
    }
    *out_frames = len;
    *out_channels = 1;
    return buf;
}

void sound_init(void) {
    const char *path = g_config.key_sound;
    if (!path[0] || !g_config.key_sound_enabled) return;

    int frames = 0, channels = 0;
    if (strcmp(path, "default") == 0) {
        samples = synth_click(&frames, &channels);
        if (!samples) { warn_once("TermmiK: key_sound=default synthesis failed"); return; }
    } else {
        samples = load_wav(path, &frames, &channels);
        if (!samples) {
            warn_once("TermmiK: key_sound file not found or not 16-bit PCM WAV");
            return;
        }
    }
    sample_frames = frames;

    // Apply volume once at load
    float vol = g_config.key_sound_volume;
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    if (vol != 1.0f) {
        for (int i = 0; i < frames * channels; i++) {
            samples[i] = (short)(samples[i] * vol);
        }
    }

    int rate = 48000;
    if (channels == 0) channels = 1;
    int err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) { warn_once("TermmiK: could not open ALSA device"); return; }
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                             channels, rate, 1, 50000 /* 50ms latency */);
    if (err < 0) {
        warn_once("TermmiK: ALSA parameter setup failed");
        snd_pcm_close(pcm);
        pcm = NULL;
        return;
    }
    enabled = 1;
}

void sound_play_key(void) {
    if (!enabled || !pcm) return;
    long r = snd_pcm_writei(pcm, samples, sample_frames);
    if (r < 0) {
        // Underrun between keystrokes — prepare and retry once
        snd_pcm_prepare(pcm);
        snd_pcm_writei(pcm, samples, sample_frames);
    }
}

void sound_cleanup(void) {
    if (pcm) snd_pcm_close(pcm);
    pcm = NULL;
    if (samples) free(samples);
    samples = NULL;
    enabled = 0;
}

#else // DISABLE_SOUND — no-op stubs keep the call sites simple

void sound_init(void) {}
void sound_play_key(void) {}
void sound_cleanup(void) {}

#endif
