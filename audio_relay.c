/*
 * audio_relay.c — low-latency FIFO-to-ALSA bridge for webOS PDK runner
 *
 * Reads raw S16_LE PCM from a named FIFO (written by SDL disk audio driver)
 * and plays it via ALSA with tight buffering and real-time sync.
 *
 * Audio that is more than SYNC_DROP_MS ahead of the wall clock is silently
 * dropped, keeping game audio in sync even when SDL pre-generates audio
 * during initialization before the first rendered frame.
 *
 * Usage: audio_relay FIFO_PATH RATE CHANNELS
 *   e.g: audio_relay /tmp/audio.fifo 22050 2
 */

#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <errno.h>

/* Drop audio that is more than this many ms ahead of the wall clock */
#define SYNC_DROP_MS   120

/* ALSA buffer/period sizes in frames */
#define ALSA_PERIOD_FRAMES  512
#define ALSA_BUFFER_FRAMES  2048

/* Poll timeout when waiting for FIFO data (ms) */
#define FIFO_POLL_MS   80

static volatile int running = 1;

static void sig_handler(int s) { (void)s; running = 0; }

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s FIFO RATE CHANNELS\n", argv[0]);
        return 1;
    }

    const char *fifo_path = argv[1];
    unsigned int rate     = (unsigned int)atoi(argv[2]);
    int channels          = atoi(argv[3]);
    int frame_bytes       = channels * 2;  /* S16_LE */

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Open FIFO — blocks until the SDL disk driver opens the write end */
    int fifo_fd = open(fifo_path, O_RDONLY);
    if (fifo_fd < 0) {
        perror("audio_relay: open fifo");
        return 1;
    }

    /* Open ALSA PCM */
    snd_pcm_t *pcm = NULL;
    int err;
    if ((err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr, "audio_relay: snd_pcm_open: %s\n", snd_strerror(err));
        return 1;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, hw, (unsigned int)channels);
    snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);

    snd_pcm_uframes_t period = ALSA_PERIOD_FRAMES;
    snd_pcm_uframes_t buffer = ALSA_BUFFER_FRAMES;
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0);
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);

    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
        fprintf(stderr, "audio_relay: snd_pcm_hw_params: %s\n", snd_strerror(err));
        return 1;
    }

    /* Get actual period/buffer sizes */
    snd_pcm_hw_params_get_period_size(hw, &period, NULL);
    snd_pcm_hw_params_get_buffer_size(hw, &buffer);

    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, period);  /* start after one period */
    snd_pcm_sw_params_set_avail_min(pcm, sw, period);
    snd_pcm_sw_params(pcm, sw);

    snd_pcm_prepare(pcm);

    int period_bytes = (int)period * frame_bytes;
    uint8_t *buf = (uint8_t *)malloc(period_bytes);
    if (!buf) { perror("malloc"); return 1; }

    /*
     * Sync tracking: count how many frames we've written, and compare
     * against how much time has elapsed.  If we're more than SYNC_DROP_MS
     * ahead of real-time, drain the FIFO without playing.
     */
    int64_t  t0             = now_us();
    int64_t  written_frames = 0;
    int      clock_started  = 0;

    while (running) {
        /* ---- sync check ---- */
        if (clock_started) {
            int64_t elapsed_us   = now_us() - t0;
            int64_t expected_f   = elapsed_us * (int64_t)rate / 1000000LL;
            int64_t ahead_f      = written_frames - expected_f;
            int64_t ahead_ms     = ahead_f * 1000 / (int64_t)rate;

            if (ahead_ms > SYNC_DROP_MS) {
                /* Drain excess audio from FIFO without playing it */
                int64_t drop_frames = ahead_f - (int64_t)(SYNC_DROP_MS / 2) * rate / 1000;
                int drop_bytes      = (int)(drop_frames * frame_bytes);
                uint8_t tmp[4096];
                while (drop_bytes > 0 && running) {
                    struct pollfd pfd = { fifo_fd, POLLIN, 0 };
                    if (poll(&pfd, 1, 5) <= 0) break;
                    int n = (int)read(fifo_fd, tmp,
                                      drop_bytes > (int)sizeof(tmp)
                                      ? (int)sizeof(tmp) : drop_bytes);
                    if (n <= 0) break;
                    drop_bytes -= n;
                }
                continue;
            }
        }

        /* ---- read one period from FIFO ---- */
        int got = 0;
        while (got < period_bytes && running) {
            struct pollfd pfd = { fifo_fd, POLLIN, 0 };
            int r = poll(&pfd, 1, FIFO_POLL_MS);
            if (r < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (r == 0) {
                /* timeout — fill rest with silence so ALSA doesn't underrun */
                memset(buf + got, 0, period_bytes - got);
                got = period_bytes;
                break;
            }
            int n = (int)read(fifo_fd, buf + got, period_bytes - got);
            if (n <= 0) {
                if (n == 0 || errno == EAGAIN) {
                    memset(buf + got, 0, period_bytes - got);
                    got = period_bytes;
                }
                break;
            }
            got += n;
        }
        if (!running) break;

        /* ---- write to ALSA ---- */
        snd_pcm_sframes_t frames = snd_pcm_writei(pcm, buf, period);
        if (frames == -EPIPE) {
            /* underrun — recover silently */
            snd_pcm_recover(pcm, (int)frames, 1);
            frames = snd_pcm_writei(pcm, buf, period);
        } else if (frames < 0) {
            snd_pcm_recover(pcm, (int)frames, 0);
            frames = 0;
        }

        if (frames > 0) {
            if (!clock_started) {
                /* Start clock from the moment the first frame is actually written */
                t0             = now_us();
                written_frames = (int64_t)frames;
                clock_started  = 1;
            } else {
                written_frames += (int64_t)frames;
            }
        }
    }

    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    free(buf);
    close(fifo_fd);
    return 0;
}
