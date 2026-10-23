/*
 * Copyright (C) 2026 M. Glargaard, aka graybox
 *
 * This software is provided "as-is", without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would
 *    be appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and must not
 *    be misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source distribution.
 */
 //audio.c
#define _XOPEN_SOURCE 500
#define _POSIX_C_SOURCE 200809L

#include "audio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef AUDIO_HAVE_OSS
	#include <sys/ioctl.h>
	#include <sys/soundcard.h>
#endif
#ifdef AUDIO_HAVE_TINYALSA
	#include "asoundlib.h"
#endif

//at main.c compile time include pcm.c (and pcm.h)
//and audio.c

static enum audio_backend current_backend = AUDIO_BACKEND_NONE;

static int audio_rate;
static int audio_channels;

#ifdef AUDIO_HAVE_OSS
static int oss_fd = -1;
#endif

#ifdef AUDIO_HAVE_TINYALSA
static struct pcm *tiny_pcm;
#endif

/*
 * TinyALSA buffer geometry, overridable at compile time
 * (e.g. -DAUDIO_TINYALSA_PERIOD_SIZE=4096).
 * A threshold of 0 means "use the ALSA default".
 */
#ifndef AUDIO_TINYALSA_PERIOD_SIZE
#define AUDIO_TINYALSA_PERIOD_SIZE 1024
#endif
#ifndef AUDIO_TINYALSA_PERIOD_COUNT
#define AUDIO_TINYALSA_PERIOD_COUNT 4
#endif
#ifndef AUDIO_TINYALSA_START_THRESHOLD
#define AUDIO_TINYALSA_START_THRESHOLD 0
#endif
#ifndef AUDIO_TINYALSA_STOP_THRESHOLD
#define AUDIO_TINYALSA_STOP_THRESHOLD 0
#endif

#ifdef AUDIO_HAVE_SIM
#include <time.h>
static int audio_init_sim(int rate, int channels);
static int audio_write_sim(const int16_t *pcm, int frames);
static void audio_close_sim(void);
static int audio_sim_delay(int quantized);
#endif

#ifdef AUDIO_HAVE_OSS
static int audio_init_oss(int rate, int channels);
static int audio_write_oss(const int16_t *pcm, int frames);
static void audio_close_oss(void);
#endif

#ifdef AUDIO_HAVE_TINYALSA
static int audio_init_tinyalsa(int rate, int channels);
static int audio_write_tinyalsa(const int16_t *pcm, int frames);
static void audio_close_tinyalsa(void);
#endif

enum audio_backend audio_get_backend(void) {
    return current_backend;
}

int audio_init_backend(enum audio_backend backend, int rate, int channels) {
    if (rate <= 0 || channels <= 0)
        return -1;

    audio_close();
#ifdef AUDIO_HAVE_OSS
    if (backend == AUDIO_BACKEND_OSS)
        return audio_init_oss(rate, channels);
#endif
#ifdef AUDIO_HAVE_TINYALSA
    if (backend == AUDIO_BACKEND_TINYALSA)
        return audio_init_tinyalsa(rate, channels);
#endif
#ifdef AUDIO_HAVE_SIM
    if (backend == AUDIO_BACKEND_SIM)
        return audio_init_sim(rate, channels);
#endif
    return -1;
}

int audio_init(int rate, int channels) {
    /*
     * Choose the desired policy here. This version tries OSS first,
     * then TinyALSA.
     */
#ifdef AUDIO_HAVE_OSS
    if (audio_init_backend(AUDIO_BACKEND_OSS, rate, channels) == 0) 
        return 0;
#endif
#if defined AUDIO_HAVE_OSS && AUDIO_HAVE_TINYALSA
    fprintf(stderr, "Audio: OSS unavailable; trying TinyALSA\n");
#endif
#ifdef AUDIO_HAVE_TINYALSA
    if (audio_init_backend(AUDIO_BACKEND_TINYALSA, rate, channels) == 0)
        return 0;
#endif
    current_backend = AUDIO_BACKEND_NONE;
    return -1;

}

int audio_write(const int16_t *pcm, int frames) {
    if (!pcm || frames <= 0)
        return -1;

    switch (current_backend) {
#ifdef AUDIO_HAVE_OSS
    case AUDIO_BACKEND_OSS:
        return audio_write_oss(pcm, frames);
#endif
#ifdef AUDIO_HAVE_TINYALSA
    case AUDIO_BACKEND_TINYALSA:
        return audio_write_tinyalsa(pcm, frames);
#endif
#ifdef AUDIO_HAVE_SIM
    case AUDIO_BACKEND_SIM:
        return audio_write_sim(pcm, frames);
#endif
    default:
        return -1;
    }
}

/*
 * Frames written to the device but not yet played (i.e. how far behind the
 * written data the speaker is). -1 if unknown. On many cards this only
 * changes once per period.
 */
int audio_get_delay(void) {
    switch (current_backend) {
#ifdef AUDIO_HAVE_OSS
    case AUDIO_BACKEND_OSS: {
        int bytes = 0;
        if (oss_fd < 0 || ioctl(oss_fd, SNDCTL_DSP_GETODELAY, &bytes) < 0)
            return -1;
        return bytes / (audio_channels * (int)sizeof(int16_t));
    }
#endif
#ifdef AUDIO_HAVE_TINYALSA
    case AUDIO_BACKEND_TINYALSA: {
        long d;
        if (!tiny_pcm)
            return -1;
        d = pcm_get_delay(tiny_pcm);
        if (d < 0)            /* e.g. not started yet or underrun */
            return 0;
        return (int)d;
    }
#endif
#ifdef AUDIO_HAVE_SIM
    case AUDIO_BACKEND_SIM:
        return audio_sim_delay(1);
#endif
    default:
        return -1;
    }
}

/*
 * Frames that can be written right now without blocking. -1 if unknown.
 */
int audio_get_space(void) {
    switch (current_backend) {
#ifdef AUDIO_HAVE_OSS
    case AUDIO_BACKEND_OSS: {
        audio_buf_info info;
        if (oss_fd < 0 || ioctl(oss_fd, SNDCTL_DSP_GETOSPACE, &info) < 0)
            return -1;
        return info.bytes / (audio_channels * (int)sizeof(int16_t));
    }
#endif
#ifdef AUDIO_HAVE_TINYALSA
    case AUDIO_BACKEND_TINYALSA: {
        int size, d;
        if (!tiny_pcm)
            return -1;
        size = (int)pcm_get_buffer_size(tiny_pcm);
        d = audio_get_delay();
        if (d < 0)
            return -1;
        return d < size ? size - d : 0;
    }
#endif
#ifdef AUDIO_HAVE_SIM
    case AUDIO_BACKEND_SIM:
        return AUDIO_TINYALSA_PERIOD_SIZE * AUDIO_TINYALSA_PERIOD_COUNT
               - audio_sim_delay(1);
#endif
    default:
        return -1;
    }
}

/*
 * Drop audio that is queued but not yet played (e.g. after a seek or a
 * stream change).
 * OSS: SNDCTL_DSP_RESET.
 * TinyALSA: pcm_stop() (DROP); the next write re-prepares the stream.
 */
void audio_flush(void) {
    switch (current_backend) {
#ifdef AUDIO_HAVE_OSS
    case AUDIO_BACKEND_OSS:
        if (oss_fd >= 0)
            (void)ioctl(oss_fd, SNDCTL_DSP_RESET, 0);
        break;
#endif
#ifdef AUDIO_HAVE_TINYALSA
    case AUDIO_BACKEND_TINYALSA:
        if (tiny_pcm)
            (void)pcm_stop(tiny_pcm);
        break;
#endif
#ifdef AUDIO_HAVE_SIM
    case AUDIO_BACKEND_SIM:
        audio_close_sim();
        audio_init_sim(audio_rate, audio_channels);
        break;
#endif
    default:
        break;
    }
}

void audio_close(void) {
    switch (current_backend) {
#ifdef AUDIO_HAVE_OSS
    case AUDIO_BACKEND_OSS:
        audio_close_oss();
        break;
#endif
#ifdef AUDIO_HAVE_TINYALSA
    case AUDIO_BACKEND_TINYALSA:
        audio_close_tinyalsa();
        break;
#endif
#ifdef AUDIO_HAVE_SIM
    case AUDIO_BACKEND_SIM:
        audio_close_sim();
        break;
#endif
    default:
        break;
    }

    current_backend = AUDIO_BACKEND_NONE;
    audio_rate = 0;
    audio_channels = 0;
}

//OSS backend
#ifdef AUDIO_HAVE_OSS
static int write_all(int fd, const void *data, size_t bytes) {
    const unsigned char *p = data;

    while (bytes != 0) {
        ssize_t n = write(fd, p, bytes);

        if (n > 0) {
            p += n;
            bytes -= (size_t)n;
            continue;
        }

        if (n < 0 && errno == EINTR)
            continue;

        return -1;
    }

    return 0;
}

static int audio_init_oss(int rate, int channels) {
    int fd;
    int format = AFMT_S16_LE;
    int actual_channels = channels;
    int actual_rate = rate;

    fd = open("/dev/dsp", O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "OSS: open /dev/dsp: %s\n", strerror(errno));
        return -1;
    }

    if (ioctl(fd, SNDCTL_DSP_RESET, 0) < 0)
        goto fail;

    if (ioctl(fd, SNDCTL_DSP_SETFMT, &format) < 0)
        goto fail;

    if (format != AFMT_S16_LE) {
        fprintf(stderr, "OSS: device rejected S16_LE\n");
        goto fail;
    }

    if (ioctl(fd, SNDCTL_DSP_CHANNELS, &actual_channels) < 0)
        goto fail;

    if (actual_channels != channels) {
        fprintf(stderr, "OSS: requested %d channels, got %d\n",
                channels, actual_channels);
        goto fail;
    }

    if (ioctl(fd, SNDCTL_DSP_SPEED, &actual_rate) < 0)
        goto fail;

    /*
     * Decide whether rate clamping is acceptable. If the synthesizer
     * cannot tolerate a different rate, reject it instead.
     */
    if (actual_rate != rate) {
        fprintf(stderr, "OSS: requested %d Hz, got %d Hz\n",
                rate, actual_rate);
    }

    oss_fd = fd;
    audio_rate = actual_rate;
    audio_channels = actual_channels;
    current_backend = AUDIO_BACKEND_OSS;

    fprintf(stderr, "Audio: OSS %d Hz, %d channel(s), S16_LE\n",
            audio_rate, audio_channels);

    return 0;

fail:
    fprintf(stderr, "OSS: unsupported audio configuration\n");
    close(fd);
    return -1;
}

static int audio_write_oss(const int16_t *pcm, int frames) {
    size_t bytes;

    if (oss_fd < 0)
        return -1;

    bytes = (size_t)frames * (size_t)audio_channels * sizeof(*pcm);

    return write_all(oss_fd, pcm, bytes);
}

static void audio_close_oss(void) {
    if (oss_fd >= 0) {
		(void)ioctl(oss_fd, SNDCTL_DSP_SYNC, 0);
        close(oss_fd);
        oss_fd = -1;
    }
}
#endif
//TinyALSA backend
#ifdef AUDIO_HAVE_TINYALSA
static int audio_init_tinyalsa(int rate, int channels) {
    struct pcm_config config = {
        .channels = (unsigned int)channels,
        .rate = (unsigned int)rate,
        .format = PCM_FORMAT_S16_LE,
        .period_size = AUDIO_TINYALSA_PERIOD_SIZE,
        .period_count = AUDIO_TINYALSA_PERIOD_COUNT,
        .start_threshold = AUDIO_TINYALSA_START_THRESHOLD,
        .stop_threshold = AUDIO_TINYALSA_STOP_THRESHOLD,
        .silence_threshold = 0
    };

    tiny_pcm = pcm_open(0, 0, PCM_OUT, &config);
    if (!tiny_pcm) {
        fprintf(stderr, "TinyALSA: pcm_open returned NULL\n");
        return -1;
    }

    if (!pcm_is_ready(tiny_pcm)) {
        fprintf(stderr, "TinyALSA: %s\n", pcm_get_error(tiny_pcm));
        pcm_close(tiny_pcm);
        tiny_pcm = NULL;
        return -1;
    }

    audio_rate = rate;
    audio_channels = channels;
    current_backend = AUDIO_BACKEND_TINYALSA;

    fprintf(stderr, "Audio: TinyALSA %d Hz, %d channel(s), S16_LE\n",
            rate, channels);

    return 0;
}

static int audio_write_tinyalsa(const int16_t *pcm, int frames) {
    unsigned int bytes;
    int rc;

    if (!tiny_pcm)
        return -1;

    bytes = (unsigned int)((size_t)frames * (size_t)audio_channels * sizeof(*pcm));

    rc = pcm_write(tiny_pcm, pcm, bytes);
    if (rc < 0) {
        fprintf(stderr, "TinyALSA: pcm_write: %s\n", pcm_get_error(tiny_pcm));
        return -1;
    }

    return 0;
}

static void audio_close_tinyalsa(void) {
    if (tiny_pcm) {
        pcm_close(tiny_pcm);
        tiny_pcm = NULL;
    }
}
#endif

/*
 * Simulated sound card (testing only, -DAUDIO_HAVE_SIM).
 * Consumes audio in real time with the TinyALSA buffer geometry, blocks
 * writes when full, and - like many real cards - reports its play position
 * only once per period. audio_sim_true_delay() gives the exact value so a
 * test can measure the real A/V offset.
 */
#ifdef AUDIO_HAVE_SIM
static long long sim_written;      /* frames written */
static long long sim_played_base;  /* frames played when (re)started */
static long long sim_start_us;     /* -1 = stopped */

static long long sim_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static long long sim_played(void) {
    long long p;
    if (sim_start_us < 0)
        return sim_played_base;
    p = sim_played_base + (sim_now_us() - sim_start_us) * audio_rate / 1000000LL;
    if (p >= sim_written) {          /* underrun: stop */
        sim_played_base = sim_written;
        sim_start_us = -1;
        return sim_written;
    }
    return p;
}

static int audio_sim_delay(int quantized) {
    long long p = sim_played();
    if (quantized)
        p -= p % AUDIO_TINYALSA_PERIOD_SIZE;
    return (int)(sim_written - p);
}

int audio_sim_true_delay(void) {
    return audio_sim_delay(0);
}

static int audio_init_sim(int rate, int channels) {
    audio_rate = rate;
    audio_channels = channels;
    sim_written = sim_played_base = 0;
    sim_start_us = -1;
    current_backend = AUDIO_BACKEND_SIM;
    fprintf(stderr, "Audio: simulated device %d Hz, %d channel(s)\n", rate, channels);
    return 0;
}

static int audio_write_sim(const int16_t *pcm, int frames) {
    const long long size = AUDIO_TINYALSA_PERIOD_SIZE * AUDIO_TINYALSA_PERIOD_COUNT;
    long long start = AUDIO_TINYALSA_START_THRESHOLD > 0 ?
                      AUDIO_TINYALSA_START_THRESHOLD : AUDIO_TINYALSA_PERIOD_SIZE;
    (void)pcm;
    /* block until the data fits; space frees up a period at a time */
    while (sim_written + frames - (sim_played() - sim_played() % AUDIO_TINYALSA_PERIOD_SIZE) > size)
    {
        struct timespec ms = { 0, 1000000 };
        nanosleep(&ms, NULL);
    }
    sim_written += frames;
    if (sim_start_us < 0 && sim_written - sim_played_base >= start) {
        sim_start_us = sim_now_us();
    }
    return 0;
}

static void audio_close_sim(void) {
    sim_start_us = -1;
}
#endif
