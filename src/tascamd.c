/*
 * tascamd.c - Full-Duplex PipeWire Audio Bridge Daemon for TASCAM US-1641
 *
 * Copyright (c) 2026 Aerankas
 * Licensed under the MIT License.
 *
 * Hardware Specification (USB ID 0644:8017, USB 2.0 High-Speed):
 *   - Playback (EP 0x02 Isochronous OUT): 4 channels, 24-bit packed LE
 *     (Out 1/2 Main L/R + Headphones, Out 3/4 Line Outputs)
 *   - Feedback (EP 0x81 Isochronous IN): 1 ms hardware DAC frame consumption token
 *   - Capture  (EP 0x86 Bulk IN): 16 channels (14 analog bitplane-multiplexed
 *     24-bit MSB-first across EVEN/ODD 32-byte frames + 2 S/PDIF channels)
 *   - MIDI In  (EP 0x83 Bulk IN): 9-byte raw MIDI frames (0xFD pad bytes)
 *   - MIDI Out (EP 0x04 Bulk OUT): 9-byte raw MIDI frames (0x00 delimiter)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <math.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include <pipewire/pipewire.h>
#include <pipewire/thread-loop.h>
#include <spa/param/audio/format-utils.h>
#include <libusb.h>

/* Compatibility fallback macros for older libpipewire-0.3 versions (e.g. PipeWire < 1.2 on Ubuntu 24.04 LTS / Mint 22) */
#ifndef PW_KEY_NODE_PHYSICAL
#define PW_KEY_NODE_PHYSICAL "node.physical"
#endif

#ifndef PW_KEY_NODE_TERMINAL
#define PW_KEY_NODE_TERMINAL "node.terminal"
#endif

#ifndef PW_KEY_PORT_PHYSICAL
#define PW_KEY_PORT_PHYSICAL "port.physical"
#endif

#ifndef PW_KEY_PORT_TERMINAL
#define PW_KEY_PORT_TERMINAL "port.terminal"
#endif

#ifndef PW_KEY_NODE_DEVICE_PORT_NAME_PREFIX
#define PW_KEY_NODE_DEVICE_PORT_NAME_PREFIX "node.device-port-name-prefix"
#endif

#ifndef PW_KEY_NODE_CHANNELNAMES
#define PW_KEY_NODE_CHANNELNAMES "node.channel-names"
#endif

#ifndef TASCAMD_VERSION
#define TASCAMD_VERSION "1.0.0"
#endif

#define VENDOR_ID   0x0644
#define PRODUCT_ID  0x8017

#define EP_ISO_OUT    0x02
#define EP_ISO_IN_FB  0x81
#define EP_BULK_IN    0x86
#define EP_MIDI_IN    0x83
#define EP_MIDI_OUT   0x04

#define BULK_BUF_SIZE 16384 // 16 KB = 512 frames = 256 sample pairs (~5.3 ms at 48kHz)
#define NUM_BULK_TRANSFERS 4

#define NUM_ISO_TRANSFERS 4
#define NUM_ISO_PACKETS   40  // 40 microframes = 5 ms per transfer

#define NUM_FB_TRANSFERS  4
#define NUM_FB_PACKETS    5   // 5 packets = 5 ms (1 packet per ms)
#define FB_PACKET_SIZE    64

#define NUM_MIDI_IN_TRANSFERS 4
#define MIDI_IN_BUF_SIZE      512
#define MIDI_PAD_BYTE         0xFD
#define MIDI_DELIM_BYTE       0x00

#define SINK_CHANNELS   4
#define SOURCE_CHANNELS 16

/* Native 32-bit floating point frames for PipeWire DSP integration */
typedef struct {
    float ch[SINK_CHANNELS]; // ch[0]=Main L, ch[1]=Main R, ch[2]=Line 3, ch[3]=Line 4
} PlaybackFrame;

typedef struct {
    float ch[SOURCE_CHANNELS]; // ch[0..7]=Mic 1..8, ch[8..13]=Line 9..14, ch[14..15]=S/PDIF
} CaptureFrame;

#define RING_FRAMES 16384
#define RING_MASK (RING_FRAMES - 1)

typedef struct {
    PlaybackFrame frames[RING_FRAMES];
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
} PlaybackRingBuffer;

typedef struct {
    CaptureFrame frames[RING_FRAMES];
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
} CaptureRingBuffer;

static volatile uint64_t g_source_frames_pushed = 0;

static inline uint32_t ring_avail_read_sink(const PlaybackRingBuffer *rb) {
    return (rb->write_idx - rb->read_idx);
}

static inline uint32_t ring_avail_write_sink(const PlaybackRingBuffer *rb) {
    return (RING_FRAMES - ring_avail_read_sink(rb));
}

static inline void ring_push_sink(PlaybackRingBuffer *rb, PlaybackFrame f) {
    if (ring_avail_write_sink(rb) > 0) {
        rb->frames[rb->write_idx & RING_MASK] = f;
        __sync_synchronize();
        rb->write_idx++;
    }
}

static inline uint32_t ring_avail_read_source(const CaptureRingBuffer *rb) {
    return (rb->write_idx - rb->read_idx);
}

static inline uint32_t ring_avail_write_source(const CaptureRingBuffer *rb) {
    return (RING_FRAMES - ring_avail_read_source(rb));
}

static inline void ring_push_source(CaptureRingBuffer *rb, CaptureFrame f) {
    __sync_fetch_and_add(&g_source_frames_pushed, 1);
    if (ring_avail_write_source(rb) > 0) {
        rb->frames[rb->write_idx & RING_MASK] = f;
        __sync_synchronize();
        rb->write_idx++;
    } else {
        rb->read_idx++;
        rb->frames[rb->write_idx & RING_MASK] = f;
        __sync_synchronize();
        rb->write_idx++;
    }
}

/* Direct Hardware Feedback Token FIFO (EP 0x81 -> EP 0x02) */
#define FB_FIFO_SIZE 256
#define FB_FIFO_MASK (FB_FIFO_SIZE - 1)
static volatile uint8_t g_fb_fifo[FB_FIFO_SIZE];
static volatile uint32_t g_fb_fifo_w = 0;
static volatile uint32_t g_fb_fifo_r = 0;

static inline void fb_fifo_push(uint8_t token) {
    uint32_t w = g_fb_fifo_w;
    uint32_t r = g_fb_fifo_r;
    if (w - r < FB_FIFO_SIZE) {
        g_fb_fifo[w & FB_FIFO_MASK] = token;
        __sync_synchronize();
        g_fb_fifo_w = w + 1;
    }
}

static inline uint8_t fb_fifo_pop(uint8_t default_val) {
    uint32_t w = g_fb_fifo_w;
    uint32_t r = g_fb_fifo_r;
    if (w > r) {
        uint8_t val = g_fb_fifo[r & FB_FIFO_MASK];
        __sync_synchronize();
        g_fb_fifo_r = r + 1;
        return val;
    }
    return default_val;
}

typedef struct {
    int sample_rate;       // 48000 (default) or 44100
    int preroll_frames;    // default 2048
    bool debug_log;        // append CSV telemetry history only when requested
} TascamConfig;

static TascamConfig g_config = {
    .sample_rate = 48000,
    .preroll_frames = 2048,
    .debug_log = false,
};

static int s_sink_target_buffer = 2048;
static int s_source_target_buffer = 2048;

static void load_config(void) {
    const char *home = getenv("HOME");
    char conf_path[512];
    if (home) {
        snprintf(conf_path, sizeof(conf_path), "%s/.config/tascamd.conf", home);
    } else {
        snprintf(conf_path, sizeof(conf_path), "/etc/tascamd.conf");
    }

    FILE *f = fopen(conf_path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char key[64], val[64];
        if (sscanf(line, " %63[^= \t] = %63s", key, val) == 2 ||
            sscanf(line, " %63[^= \t]=%63s", key, val) == 2) {
            if (strcmp(key, "sample_rate") == 0 || strcmp(key, "rate") == 0) {
                int r = atoi(val);
                if (r == 44100 || r == 48000) {
                    g_config.sample_rate = r;
                }
            } else if (strcmp(key, "preroll_frames") == 0 || strcmp(key, "preroll") == 0) {
                int p = atoi(val);
                if (p >= 256 && p <= 8192) {
                    g_config.preroll_frames = p;
                }
            }
        }
    }
    fclose(f);
}

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Full-Duplex PipeWire Audio Bridge Daemon for TASCAM US-1641 (v%s)\n\n", TASCAMD_VERSION);
    printf("Options:\n");
    printf("  -r, --rate <48000|44100>   Override hardware sample rate (default: 48000)\n");
    printf("  -p, --preroll <frames>     Override jitter pre-roll buffer frames (256..8192, default: 2048)\n");
    printf("  -d, --debug-log            Write continuous CSV telemetry to runtime directory\n");
    printf("  -v, --version              Print version and exit\n");
    printf("  -h, --help                 Show this help message\n");
}

static volatile bool g_running = true;

// Ring buffers for full-duplex bridge
static PlaybackRingBuffer g_sink_rb;   // Playback (PipeWire -> USB OUT, 4 Ch)
static CaptureRingBuffer g_source_rb;  // Capture (USB IN -> PipeWire, 16 Ch)

static libusb_context *g_ctx = NULL;
static libusb_device_handle *g_handle = NULL;
static uint32_t g_urb_counter = 0;
static volatile int g_active_iso_transfers = 0;
static volatile int g_active_fb_transfers = 0;
static volatile uint64_t g_fb_packets_count = 0;
static volatile uint32_t g_fb_last_val = 0;
static volatile uint32_t g_sink_underruns = 0;
static volatile uint32_t g_sink_overruns = 0;

static snd_seq_t *g_seq_handle = NULL;
static int g_seq_in_port = -1;
static int g_seq_out_port = -1;
static snd_midi_event_t *g_midi_in_coder = NULL;
static snd_midi_event_t *g_midi_out_coder = NULL;
static pthread_mutex_t g_seq_lock = PTHREAD_MUTEX_INITIALIZER;

static uint8_t g_midi_in_acc[512];
static int g_midi_in_acc_len = 0;
static pthread_mutex_t g_midi_in_lock = PTHREAD_MUTEX_INITIALIZER;

static volatile int g_active_midi_in_transfers = 0;
static volatile uint64_t g_midi_in_events_count = 0;
static volatile uint64_t g_midi_out_events_count = 0;
static pthread_t g_midi_out_thread;
static bool g_midi_out_thread_running = false;

static void sig_handler(int sig) {
    (void)sig;
    g_running = false;
}

// PipeWire Sink: System Playback Audio (F32) -> g_sink_rb
static void on_process_sink(void *userdata) {
    struct pw_stream *stream = userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(stream);
    if (!b) return;

    struct spa_buffer *buf = b->buffer;
    if (buf->datas[0].data) {
        uint32_t offset = buf->datas[0].chunk->offset;
        const PlaybackFrame *src = (const PlaybackFrame *)((const uint8_t *)buf->datas[0].data + offset);
        uint32_t n_frames = buf->datas[0].chunk->size / sizeof(PlaybackFrame);

        for (uint32_t i = 0; i < n_frames; i++) {
            ring_push_sink(&g_sink_rb, src[i]);
        }
    }
    pw_stream_queue_buffer(stream, b);
}

static volatile bool s_source_active = false;
static volatile double s_source_ratio = 1.0;
static float s_source_q_filtered = 4096.0f;
static volatile uint32_t s_source_underruns = 0;
static volatile uint32_t s_source_overruns = 0;

// PipeWire Source: g_source_rb -> System Capture / DAW (Bit-Perfect Native F32)
static void on_process_source(void *userdata) {
    struct pw_stream *stream = userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(stream);
    if (!b) return;

    struct spa_buffer *buf = b->buffer;
    if (buf->datas[0].data) {
        CaptureFrame *dest = (CaptureFrame *)buf->datas[0].data;
        uint32_t max_frames = buf->datas[0].maxsize / sizeof(CaptureFrame);

        uint32_t n_frames = b->requested > 0 ? (uint32_t)b->requested : 1024;
        if (n_frames > max_frames) n_frames = max_frames;

        uint32_t queued = ring_avail_read_source(&g_source_rb);

        // Pre-roll buffering: hold silence until ring buffer reaches target watermark
        if (!s_source_active) {
            if (queued < (uint32_t)s_source_target_buffer + (n_frames / 2)) {
                memset(dest, 0, n_frames * sizeof(CaptureFrame));
                buf->datas[0].chunk->offset = 0;
                buf->datas[0].chunk->size = n_frames * sizeof(CaptureFrame);
                buf->datas[0].chunk->stride = sizeof(CaptureFrame);
                pw_stream_queue_buffer(stream, b);
                return;
            }
            // Center quantum oscillation symmetrically around s_source_target_buffer
            g_source_rb.read_idx = g_source_rb.write_idx - s_source_target_buffer - (n_frames / 2);
            s_source_active = true;
            s_source_q_filtered = (float)s_source_target_buffer;
        }

        // Low-pass filter queue depth across quantums to reject PipeWire quantum sawtooth
        s_source_q_filtered = 0.99f * s_source_q_filtered + 0.01f * (float)(queued - (n_frames / 2));

        // Emergency overrun protection (e.g. system wake from suspend)
        if (queued > 12000) {
            g_source_rb.read_idx = g_source_rb.write_idx - s_source_target_buffer - (n_frames / 2);
            queued = s_source_target_buffer + (n_frames / 2);
            s_source_q_filtered = (float)s_source_target_buffer;
            s_source_overruns++;
        }

        // Wide-deadband filtered drift steering: 0 slips during normal operation,
        // only nudging 1 sample at a zero-crossing if long-term average drifts > 1024 frames
        int slip = 0;
        if (s_source_q_filtered > (float)(s_source_target_buffer + 1024)) {
            slip = 1; // drop 1 frame to catch up
            s_source_q_filtered -= 1.0f;
        } else if (s_source_q_filtered < (float)(s_source_target_buffer - 1024)) {
            slip = -1; // duplicate 1 frame to let queue accumulate
            s_source_q_filtered += 1.0f;
        }

        for (uint32_t i = 0; i < n_frames; i++) {
            uint32_t avail = ring_avail_read_source(&g_source_rb);
            if (avail == 0) {
                memset(&dest[i], 0, sizeof(CaptureFrame));
                s_source_underruns++;
                continue;
            }

            dest[i] = g_source_rb.frames[g_source_rb.read_idx & RING_MASK];
            g_source_rb.read_idx++;

            if (i == 0 && slip != 0) {
                if (slip == 1 && ring_avail_read_source(&g_source_rb) > 0) {
                    g_source_rb.read_idx++;
                } else if (slip == -1) {
                    g_source_rb.read_idx--;
                }
                slip = 0;
            }
        }

        buf->datas[0].chunk->offset = 0;
        buf->datas[0].chunk->size = n_frames * sizeof(CaptureFrame);
        buf->datas[0].chunk->stride = sizeof(CaptureFrame);
    }
    pw_stream_queue_buffer(stream, b);
}

static const struct pw_stream_events sink_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process_sink,
};

static const struct pw_stream_events source_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process_source,
};

static volatile bool s_sink_active = false;
static volatile double s_sink_ratio = 1.0;
static uint32_t s_microframe_counter = 0;
static float s_sink_q_filtered = 2048.0f;
static uint32_t s_sink_underrun_streak = 0;

static inline int32_t float_to_s24(float s) {
    if (s > 1.0f) s = 1.0f;
    else if (s < -1.0f) s = -1.0f;
    int32_t val = (int32_t)lrintf(s * 8388607.0f);
    if (val > 8388607) val = 8388607;
    else if (val < -8388608) val = -8388608;
    return val;
}

// Fill contiguous memory with audio from g_sink_rb using bit-perfect 24-bit PCM conversion
static void fill_audio_packet(unsigned char *buf, int num_samples) {
    uint32_t queued = ring_avail_read_sink(&g_sink_rb);
    s_microframe_counter++;

    // If inactive, wait until buffer accumulates at least s_sink_target_buffer frames
    if (!s_sink_active) {
        if (queued < (uint32_t)s_sink_target_buffer + 512) {
            memset(buf, 0, num_samples * 12);
            return;
        }
        // Center quantum oscillation symmetrically around s_sink_target_buffer
        g_sink_rb.read_idx = g_sink_rb.write_idx - s_sink_target_buffer - 512;
        s_sink_active = true;
        s_sink_underrun_streak = 0;
        s_sink_q_filtered = (float)s_sink_target_buffer;
    }

    // Low-pass filter the queue level (EMA filter) to reject PipeWire quantum burst jitter
    s_sink_q_filtered = 0.9995f * s_sink_q_filtered + 0.0005f * (float)queued;

    // Emergency overrun protection (e.g. system wake from suspend)
    if (queued > 12000) {
        g_sink_rb.read_idx = g_sink_rb.write_idx - s_sink_target_buffer - 512;
        queued = s_sink_target_buffer + 512;
        s_sink_q_filtered = (float)s_sink_target_buffer;
        g_sink_overruns++;
    }

    // Wide-deadband filtered drift steering:
    // Checks filtered average queue (not raw quantum-sawtoothed queued!) with a +-1024 frame deadband.
    // Guarantees 0 sample slips during normal playback while preventing multi-hour crystal drift.
    int slip = 0;
    if (s_microframe_counter % 800 == 0) { // check every 100 ms (10 Hz max rate)
        if (s_sink_q_filtered > (float)(s_sink_target_buffer + 1024)) {
            slip = 1; // drop 1 frame to let queue drain towards target
            s_sink_q_filtered -= 1.0f;
        } else if (s_sink_q_filtered < (float)(s_sink_target_buffer - 1024)) {
            slip = -1; // repeat 1 frame to gently build queue towards target
            s_sink_q_filtered += 1.0f;
        }
    }

    for (int s = 0; s < num_samples; s++) {
        uint32_t avail = ring_avail_read_sink(&g_sink_rb);
        if (avail == 0) {
            memset(&buf[s * 12], 0, 12);
            s_sink_underrun_streak++;
            if (s_sink_underrun_streak >= 240) { // ~5 ms of sustained silence (stream paused/stopped)
                if (s_sink_active) {
                    s_sink_active = false; // clean drop back to pre-roll wait
                }
                s_sink_underrun_streak = 0;
                s_sink_q_filtered = (float)s_sink_target_buffer;
            } else {
                g_sink_underruns++;
            }
            continue;
        }

        s_sink_underrun_streak = 0;
        PlaybackFrame f = g_sink_rb.frames[g_sink_rb.read_idx & RING_MASK];
        g_sink_rb.read_idx++;

        if (s == 0 && slip != 0) {
            if (slip == 1 && ring_avail_read_sink(&g_sink_rb) > 0) {
                g_sink_rb.read_idx++;
            } else if (slip == -1) {
                g_sink_rb.read_idx--;
            }
            slip = 0;
        }

        // Bit-perfect 24-bit PCM conversion across all 3 bytes LE per channel
        for (int c = 0; c < 4; c++) {
            int32_t s24 = float_to_s24(f.ch[c]);
            buf[s * 12 + c * 3 + 0] = (unsigned char)(s24 & 0xFF);
            buf[s * 12 + c * 3 + 1] = (unsigned char)((s24 >> 8) & 0xFF);
            buf[s * 12 + c * 3 + 2] = (unsigned char)((s24 >> 16) & 0xFF);
        }
    }
}

static void prepare_iso_transfer_buffer(struct libusb_transfer *transfer) {
    unsigned char *p = transfer->buffer;
    uint32_t urb = g_urb_counter++;

    for (int m = 0; m < 5; m++) {
        uint8_t def_val;
        if (g_config.sample_rate == 48000) {
            def_val = 48;
        } else {
            // Nominal 44.1 kHz pattern: 44, 44, 44, 44, 45 (1 out of 10 ms is 45)
            uint32_t ms_idx = urb * 5 + m;
            def_val = (ms_idx % 10 == 9) ? 45 : 44;
        }

        uint8_t n_samples = fb_fifo_pop(def_val);
        if (n_samples < 40 || n_samples > 55) n_samples = def_val;

        int pat8[8];
        if (g_config.sample_rate == 48000) {
            // Nominal 48 samples = 6 samples per microframe
            // Deviations: microframe 0 absorbs the delta (+1 -> 7, -1 -> 5)
            int delta = (int)n_samples - 48;
            pat8[0] = 6 + delta;
            for (int u = 1; u < 8; u++) pat8[u] = 6;
        } else {
            // 44.1 kHz
            if (n_samples == 44) {
                for (int u = 0; u < 8; u++) pat8[u] = (u % 2 == 0) ? 6 : 5;
            } else if (n_samples == 45) {
                static const int k_45[8] = {6, 6, 5, 6, 5, 6, 5, 6};
                for (int u = 0; u < 8; u++) pat8[u] = k_45[u];
            } else {
                int delta = (int)n_samples - 44;
                for (int u = 0; u < 8; u++) pat8[u] = (u % 2 == 0) ? 6 : 5;
                pat8[0] += delta;
            }
        }

        for (int u = 0; u < 8; u++) {
            int pkt_idx = m * 8 + u;
            int samples = pat8[u];
            int pkt_len = samples * 12;

            fill_audio_packet(p, samples);
            transfer->iso_packet_desc[pkt_idx].length = pkt_len;
            p += pkt_len;
        }
    }
    transfer->length = (int)(p - transfer->buffer);
}

static void LIBUSB_CALL iso_out_cb(struct libusb_transfer *transfer) {
    if (!g_running || transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
            fprintf(stderr, "[WARN] TASCAM US-1641 disconnected (Iso OUT).\n");
            g_running = false;
        }
        g_active_iso_transfers--;
        return;
    }
    if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
        fprintf(stderr, "[WARN] Iso OUT transfer status: %d\n", transfer->status);
    }
    prepare_iso_transfer_buffer(transfer);
    int r = libusb_submit_transfer(transfer);
    if (r < 0) {
        g_active_iso_transfers--;
        fprintf(stderr, "[ERROR] libusb_submit_transfer (Iso OUT) failed: %d\n", r);
        if (r == LIBUSB_ERROR_NO_DEVICE) g_running = false;
    }
}

// Isochronous Feedback IN callback (Endpoint 0x81):
// Receives explicit 1ms hardware DAC buffer tokens and pushes to fb_fifo
static void LIBUSB_CALL iso_in_fb_cb(struct libusb_transfer *transfer) {
    if (!g_running || transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
            g_running = false;
        }
        g_active_fb_transfers--;
        return;
    }
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        for (int i = 0; i < transfer->num_iso_packets; i++) {
            if (transfer->iso_packet_desc[i].status == LIBUSB_TRANSFER_COMPLETED &&
                transfer->iso_packet_desc[i].actual_length > 0) {
                unsigned char *p = transfer->buffer + (i * FB_PACKET_SIZE);
                int val = p[0];
                if (val >= 40 && val <= 55) {
                    fb_fifo_push((uint8_t)val);
                    g_fb_last_val = val;
                    g_fb_packets_count++;
                }
            }
        }
    }
    for (int i = 0; i < transfer->num_iso_packets; i++) {
        transfer->iso_packet_desc[i].length = FB_PACKET_SIZE;
        transfer->iso_packet_desc[i].actual_length = 0;
    }
    int r = libusb_submit_transfer(transfer);
    if (r < 0) {
        g_active_fb_transfers--;
        if (r == LIBUSB_ERROR_NO_DEVICE) g_running = false;
    }
}

static volatile int g_active_bulk_transfers = 0;
static uint64_t g_global_frame_idx = 0;
static float g_current_sample[SOURCE_CHANNELS] = {0.0f};

static void LIBUSB_CALL bulk_in_cb(struct libusb_transfer *transfer) {
    if (!g_running || transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
            fprintf(stderr, "[WARN] TASCAM US-1641 disconnected (Bulk IN).\n");
            g_running = false;
        }
        g_active_bulk_transfers--;
        return;
    }

    if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
        if (transfer->status == LIBUSB_TRANSFER_STALL) {
            fprintf(stderr, "[WARN] Bulk IN (EP 0x86) stalled. Clearing endpoint halt...\n");
            int cr = libusb_clear_halt(g_handle, EP_BULK_IN);
            if (cr < 0) {
                fprintf(stderr, "[ERROR] libusb_clear_halt failed: %s\n", libusb_strerror(cr));
            }
        } else if (transfer->status == LIBUSB_TRANSFER_ERROR) {
            fprintf(stderr, "[WARN] Bulk IN (EP 0x86) transfer error. Clearing halt...\n");
            int cr = libusb_clear_halt(g_handle, EP_BULK_IN);
            if (cr < 0) {
                fprintf(stderr, "[ERROR] libusb_clear_halt failed: %s\n", libusb_strerror(cr));
            }
        } else if (transfer->status == LIBUSB_TRANSFER_OVERFLOW) {
            fprintf(stderr, "[WARN] Bulk IN (EP 0x86) overflow.\n");
        } else {
            fprintf(stderr, "[WARN] Bulk IN (EP 0x86) transfer status: %d\n", transfer->status);
        }
    } else if (transfer->actual_length > 0) {
        const unsigned char *bulk_buf = transfer->buffer;
        int num_frames = transfer->actual_length / 32;

        for (int f = 0; f < num_frames; f++) {
            const unsigned char *frame = &bulk_buf[f * 32];
            int parity = (int)(g_global_frame_idx & 1); // 0 = EVEN, 1 = ODD
            g_global_frame_idx++;

            // Demux 7 channels from this 32-byte frame directly to full 24-bit resolution
            // Hardware framing: parity 0 = top row (In 1, 3, 5, 7, 9, 11, 13), parity 1 = bottom row (In 2, 4, 6, 8, 10, 12, 14)
            for (int b = 0; b < 7; b++) {
                int ch_idx = (parity == 0) ? (2 * b) : (2 * b + 1); // 0-based channel index (0..13)

                int32_t val = 0;
                for (int bit = 0; bit < 24; bit++) {
                    int bit_val = (frame[bit] >> b) & 1;
                    val = (val << 1) | bit_val;
                }
                if (val & 0x800000) val -= 0x1000000;
                g_current_sample[ch_idx] = (float)val * (1.0f / 8388608.0f);
            }

            // On completion of ODD frame, all 14 analog channels are sampled
            if (parity == 1) {
                CaptureFrame cf;
                memcpy(cf.ch, g_current_sample, sizeof(cf.ch));
                ring_push_source(&g_source_rb, cf);
            }
        }
    }

    int r = libusb_submit_transfer(transfer);
    if (r < 0) {
        g_active_bulk_transfers--;
        if (r == LIBUSB_ERROR_NO_DEVICE) g_running = false;
    }
}

// ALSA Sequencer initialization for hardware MIDI
static bool init_alsa_midi(void) {
    int err = snd_seq_open(&g_seq_handle, "default", SND_SEQ_OPEN_DUPLEX, 0);
    if (err < 0) {
        fprintf(stderr, "[WARN] Could not open ALSA Sequencer: %s\n", snd_strerror(err));
        return false;
    }
    snd_seq_set_client_name(g_seq_handle, "TASCAM US-1641");
    snd_seq_nonblock(g_seq_handle, 1);

    g_seq_in_port = snd_seq_create_simple_port(
        g_seq_handle,
        "MIDI In",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE | SND_SEQ_PORT_TYPE_PORT
    );

    g_seq_out_port = snd_seq_create_simple_port(
        g_seq_handle,
        "MIDI Out",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE | SND_SEQ_PORT_TYPE_PORT
    );

    snd_midi_event_new(256, &g_midi_in_coder);
    snd_midi_event_new(256, &g_midi_out_coder);
    if (g_midi_in_coder) snd_midi_event_init(g_midi_in_coder);
    if (g_midi_out_coder) snd_midi_event_init(g_midi_out_coder);

    return true;
}

// Background worker thread for ALSA Sequencer -> US-1641 MIDI OUT (Endpoint 0x04)
static void *midi_out_worker(void *arg) {
    (void)arg;
    if (!g_seq_handle) return NULL;

    int npfds = snd_seq_poll_descriptors_count(g_seq_handle, POLLIN);
    if (npfds <= 0) return NULL;
    struct pollfd *pfds = calloc(npfds, sizeof(struct pollfd));
    if (!pfds) return NULL;

    while (g_running) {
        snd_seq_poll_descriptors(g_seq_handle, pfds, npfds, POLLIN);
        int ret = poll(pfds, npfds, 100);
        if (ret <= 0) continue;

        pthread_mutex_lock(&g_seq_lock);
        snd_seq_event_t *ev = NULL;
        while (snd_seq_event_input(g_seq_handle, &ev) >= 0) {
            if (!ev) break;
            if (ev->dest.port == g_seq_out_port && g_midi_out_coder && g_handle) {
                uint8_t raw_buf[128];
                long bytes = snd_midi_event_decode(g_midi_out_coder, raw_buf, sizeof(raw_buf), ev);
                if (bytes > 0) {
                    for (long b = 0; b < bytes; b += 8) {
                        uint8_t frame[9];
                        memset(frame, MIDI_PAD_BYTE, 8);
                        int chunk = (bytes - b < 8) ? (int)(bytes - b) : 8;
                        memcpy(frame, raw_buf + b, chunk);
                        frame[8] = MIDI_DELIM_BYTE;
                        int transferred = 0;
                        libusb_bulk_transfer(g_handle, EP_MIDI_OUT, frame, 9, &transferred, 50);
                        g_midi_out_events_count++;
                    }
                }
            }
        }
        pthread_mutex_unlock(&g_seq_lock);
    }
    free(pfds);
    return NULL;
}

// Libusb Bulk IN callback for hardware MIDI Input (Endpoint 0x83)
static void LIBUSB_CALL midi_in_cb(struct libusb_transfer *transfer) {
    if (!g_running || transfer->status == LIBUSB_TRANSFER_NO_DEVICE || transfer->status == LIBUSB_TRANSFER_CANCELLED) {
        if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
            fprintf(stderr, "[WARN] TASCAM US-1641 disconnected (MIDI IN).\n");
            g_running = false;
        }
        g_active_midi_in_transfers--;
        return;
    }

    if (transfer->status == LIBUSB_TRANSFER_COMPLETED && transfer->actual_length > 0) {
        pthread_mutex_lock(&g_midi_in_lock);
        if (g_midi_in_acc_len + transfer->actual_length <= (int)sizeof(g_midi_in_acc)) {
            memcpy(g_midi_in_acc + g_midi_in_acc_len, transfer->buffer, transfer->actual_length);
            g_midi_in_acc_len += transfer->actual_length;
        } else {
            g_midi_in_acc_len = 0;
        }

        while (g_midi_in_acc_len >= 9) {
            int payload_len = 8;
            while (payload_len > 0 && g_midi_in_acc[payload_len - 1] == MIDI_PAD_BYTE) {
                payload_len--;
            }

            for (int i = 0; i < payload_len; i++) {
                uint8_t byte = g_midi_in_acc[i];
                if (g_midi_in_coder && g_seq_handle) {
                    snd_seq_event_t ev;
                    snd_seq_ev_clear(&ev);
                    int res = snd_midi_event_encode_byte(g_midi_in_coder, byte, &ev);
                    if (res > 0) {
                        snd_seq_ev_set_source(&ev, g_seq_in_port);
                        snd_seq_ev_set_subs(&ev);
                        snd_seq_ev_set_direct(&ev);
                        pthread_mutex_lock(&g_seq_lock);
                        snd_seq_event_output_direct(g_seq_handle, &ev);
                        pthread_mutex_unlock(&g_seq_lock);
                        if (ev.type != SND_SEQ_EVENT_SENSING) {
                            g_midi_in_events_count++;
                        }
                    }
                }
            }

            memmove(g_midi_in_acc, g_midi_in_acc + 9, g_midi_in_acc_len - 9);
            g_midi_in_acc_len -= 9;
        }
        pthread_mutex_unlock(&g_midi_in_lock);
    } else if (transfer->status == LIBUSB_TRANSFER_STALL) {
        libusb_clear_halt(g_handle, EP_MIDI_IN);
    }

    if (g_running) {
        int r = libusb_submit_transfer(transfer);
        if (r < 0) {
            g_active_midi_in_transfers--;
        }
    } else {
        g_active_midi_in_transfers--;
    }
}

// Background thread for isochronous clock & output event handling
static void *usb_event_worker(void *arg) {
    (void)arg;
    struct timeval tv = {0, 1000};
    while (g_running || g_active_iso_transfers > 0 || g_active_fb_transfers > 0 || g_active_bulk_transfers > 0 || g_active_midi_in_transfers > 0) {
        libusb_handle_events_timeout_completed(g_ctx, &tv, NULL);
    }
    return NULL;
}

static bool init_tascam_hardware(void) {
    g_handle = libusb_open_device_with_vid_pid(g_ctx, VENDOR_ID, PRODUCT_ID);
    if (!g_handle) return false;

    libusb_set_auto_detach_kernel_driver(g_handle, 1);
    libusb_set_configuration(g_handle, 1);

    libusb_claim_interface(g_handle, 0);
    libusb_set_interface_alt_setting(g_handle, 0, 1);

    libusb_claim_interface(g_handle, 1);
    libusb_set_interface_alt_setting(g_handle, 1, 1);

    unsigned char buf[64];
    libusb_control_transfer(g_handle, 0xC0, 0x56, 0x0000, 0x0000, buf, 15, 1000);
    libusb_control_transfer(g_handle, 0xC0, 0x56, 0x0000, 0x0000, buf, 15, 1000);
    libusb_control_transfer(g_handle, 0xC0, 0x49, 0x0000, 0x0000, buf, 1, 1000);
    libusb_control_transfer(g_handle, 0x40, 0x49, 0x0000, 0x0002, NULL, 0, 1000);

    unsigned char rate_data[3];
    int clock_reg;
    if (g_config.sample_rate == 48000) {
        rate_data[0] = 0x80;
        rate_data[1] = 0xBB;
        rate_data[2] = 0x00;
        clock_reg = 0x1002;
    } else {
        rate_data[0] = 0x44;
        rate_data[1] = 0xAC;
        rate_data[2] = 0x00;
        clock_reg = 0x1000;
    }

    libusb_control_transfer(g_handle, 0x22, 0x01, 0x0100, 0x0086, rate_data, 3, 1000);
    libusb_control_transfer(g_handle, 0x22, 0x01, 0x0100, 0x0002, rate_data, 3, 1000);

    libusb_control_transfer(g_handle, 0xC0, 0x41, 0x0D00, 0x0101, buf, 5, 1000);
    libusb_control_transfer(g_handle, 0x40, 0x41, 0x0D04, 0x0101, NULL, 0, 1000);
    libusb_control_transfer(g_handle, 0x40, 0x41, 0x0E00, 0x0101, NULL, 0, 1000);
    libusb_control_transfer(g_handle, 0x40, 0x41, 0x0F00, 0x0101, NULL, 0, 1000);
    libusb_control_transfer(g_handle, 0x40, 0x41, clock_reg, 0x0101, NULL, 0, 1000);
    libusb_control_transfer(g_handle, 0x40, 0x41, 0x110B, 0x0101, NULL, 0, 1000);

    libusb_control_transfer(g_handle, 0x22, 0x01, 0x0100, 0x0086, rate_data, 3, 1000);
    libusb_control_transfer(g_handle, 0x22, 0x01, 0x0100, 0x0002, rate_data, 3, 1000);
    libusb_control_transfer(g_handle, 0x40, 0x49, 0x0032, 0x0000, NULL, 0, 1000);

    return true;
}

static void get_runtime_file_path(char *out_buf, size_t buf_size, const char *filename) {
    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime && xdg_runtime[0] != '\0') {
        snprintf(out_buf, buf_size, "%s/%s", xdg_runtime, filename);
    } else {
        snprintf(out_buf, buf_size, "/tmp/%s-%u", filename, (unsigned int)getuid());
    }
}

int main(int argc, char **argv) {
    setlinebuf(stdout);
    setlinebuf(stderr);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    load_config();

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--rate") == 0) && i + 1 < argc) {
            int r = atoi(argv[++i]);
            if (r == 44100 || r == 48000) {
                g_config.sample_rate = r;
            } else {
                fprintf(stderr, "[ERROR] Unsupported sample rate: %d (use 48000 or 44100)\n", r);
                return 1;
            }
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--preroll") == 0) && i + 1 < argc) {
            int p = atoi(argv[++i]);
            if (p >= 256 && p <= 8192) {
                g_config.preroll_frames = p;
            } else {
                fprintf(stderr, "[ERROR] Pre-roll frames must be between 256 and 8192\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug-log") == 0) {
            g_config.debug_log = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("tascamd version %s\n", TASCAMD_VERSION);
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    s_sink_target_buffer = g_config.preroll_frames;
    s_source_target_buffer = g_config.preroll_frames;

    printf("[INFO] Starting TASCAM US-1641 PipeWire Bridge Daemon v%s (tascamd)...\n", TASCAMD_VERSION);
    printf("[INFO] Active Configuration: Sample Rate = %d Hz, Pre-roll = %d frames\n",
           g_config.sample_rate, g_config.preroll_frames);

    if (libusb_init(&g_ctx) < 0) {
        fprintf(stderr, "[ERROR] libusb_init failed\n");
        return 1;
    }

    if (!init_tascam_hardware()) {
        fprintf(stderr, "[ERROR] Could not open TASCAM US-1641 device (0644:8017)\n");
        libusb_exit(g_ctx);
        return 1;
    }
    printf("[SUCCESS] Connected to TASCAM US-1641 hardware at %d Hz!\n", g_config.sample_rate);

    char rate_str[16];
    snprintf(rate_str, sizeof(rate_str), "%d", g_config.sample_rate);

    pw_init(&argc, &argv);
    struct pw_thread_loop *pw_tloop = pw_thread_loop_new("tascam-thread-loop", NULL);
    struct pw_context *pw_ctx = pw_context_new(pw_thread_loop_get_loop(pw_tloop), NULL, 0);

    struct spa_hook sink_listener;
    memset(&sink_listener, 0, sizeof(sink_listener));
    struct spa_hook source_listener;
    memset(&source_listener, 0, sizeof(source_listener));

    // 1. Create Playback Sink (Headphones / Main Out + Line Out 3/4) - Native F32
    struct pw_properties *sink_props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_CLASS, "Audio/Sink",
        PW_KEY_NODE_NAME, "tascam_us1641_sink",
        PW_KEY_NODE_DESCRIPTION, "TASCAM US-1641 Outputs",
        PW_KEY_NODE_NICK, "US-1641 Out",
        PW_KEY_AUDIO_CHANNELS, "4",
        "audio.position", "[ FL, FR, RL, RR ]",
        PW_KEY_AUDIO_RATE, rate_str,
        PW_KEY_NODE_DEVICE_PORT_NAME_PREFIX, "",
        PW_KEY_NODE_CHANNELNAMES, "[ \"Out 1 (Main L - Phones)\", \"Out 2 (Main R - Phones)\", \"Out 3 (Line Out 3)\", \"Out 4 (Line Out 4)\" ]",
        PW_KEY_NODE_PHYSICAL, "true",
        PW_KEY_NODE_TERMINAL, "true",
        PW_KEY_PORT_PHYSICAL, "true",
        PW_KEY_PORT_TERMINAL, "true",
        NULL);
    struct pw_stream *sink_stream = pw_stream_new(pw_context_connect(pw_ctx, NULL, 0), "tascam-sink", sink_props);
    pw_stream_add_listener(sink_stream, &sink_listener, &sink_events, sink_stream);

    uint8_t sink_buf[4096];
    struct spa_pod_builder b1 = SPA_POD_BUILDER_INIT(sink_buf, sizeof(sink_buf));
    struct spa_audio_info_raw info_sink = {
        .format = SPA_AUDIO_FORMAT_F32_LE,
        .channels = SINK_CHANNELS,
        .rate = g_config.sample_rate,
        .position = {
            SPA_AUDIO_CHANNEL_FL,
            SPA_AUDIO_CHANNEL_FR,
            SPA_AUDIO_CHANNEL_RL,
            SPA_AUDIO_CHANNEL_RR
        }
    };
    const struct spa_pod *params_sink[1];
    params_sink[0] = spa_format_audio_raw_build(&b1, SPA_PARAM_EnumFormat, &info_sink);
    pw_stream_connect(sink_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                      PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
                      params_sink, 1);

    // 2. Create Capture Source (8 Mics + 6 Line Ins + S/PDIF) - Native F32
    struct pw_properties *source_props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_CLASS, "Audio/Source",
        PW_KEY_NODE_NAME, "tascam_us1641_source",
        PW_KEY_NODE_DESCRIPTION, "TASCAM US-1641 Inputs",
        PW_KEY_NODE_NICK, "US-1641 In",
        PW_KEY_AUDIO_CHANNELS, "16",
        "audio.position", "[ AUX0, AUX1, AUX2, AUX3, AUX4, AUX5, AUX6, AUX7, AUX8, AUX9, AUX10, AUX11, AUX12, AUX13, AUX14, AUX15 ]",
        "channelmix.disable", "true",
        PW_KEY_AUDIO_RATE, rate_str,
        PW_KEY_NODE_DEVICE_PORT_NAME_PREFIX, "",
        PW_KEY_NODE_CHANNELNAMES, "[ \"In 01 (Mic 1)\", \"In 02 (Mic 2)\", \"In 03 (Mic 3)\", \"In 04 (Mic 4)\", \"In 05 (Mic 5)\", \"In 06 (Mic 6)\", \"In 07 (Mic 7)\", \"In 08 (Mic 8)\", \"In 09 (Line 9)\", \"In 10 (Line 10)\", \"In 11 (Line 11)\", \"In 12 (Line 12)\", \"In 13 (Line 13)\", \"In 14 (Line 14)\", \"In 15 (SPDIF L)\", \"In 16 (SPDIF R)\" ]",
        PW_KEY_NODE_PHYSICAL, "true",
        PW_KEY_NODE_TERMINAL, "true",
        PW_KEY_PORT_PHYSICAL, "true",
        PW_KEY_PORT_TERMINAL, "true",
        NULL);
    struct pw_stream *source_stream = pw_stream_new(pw_context_connect(pw_ctx, NULL, 0), "tascam-source", source_props);
    pw_stream_add_listener(source_stream, &source_listener, &source_events, source_stream);

    uint8_t source_buf[4096];
    struct spa_pod_builder b2 = SPA_POD_BUILDER_INIT(source_buf, sizeof(source_buf));
    struct spa_audio_info_raw info_source = {
        .format = SPA_AUDIO_FORMAT_F32_LE,
        .channels = SOURCE_CHANNELS,
        .rate = g_config.sample_rate,
        .position = {
            SPA_AUDIO_CHANNEL_AUX0,  // Ch 1 (Mic 1)
            SPA_AUDIO_CHANNEL_AUX1,  // Ch 2 (Mic 2)
            SPA_AUDIO_CHANNEL_AUX2,  // Ch 3 (Mic 3)
            SPA_AUDIO_CHANNEL_AUX3,  // Ch 4 (Mic 4)
            SPA_AUDIO_CHANNEL_AUX4,  // Ch 5 (Mic 5)
            SPA_AUDIO_CHANNEL_AUX5,  // Ch 6 (Mic 6)
            SPA_AUDIO_CHANNEL_AUX6,  // Ch 7 (Mic 7)
            SPA_AUDIO_CHANNEL_AUX7,  // Ch 8 (Mic 8)
            SPA_AUDIO_CHANNEL_AUX8,  // Ch 9 (Line In 9)
            SPA_AUDIO_CHANNEL_AUX9,  // Ch 10 (Line In 10)
            SPA_AUDIO_CHANNEL_AUX10, // Ch 11 (Line In 11)
            SPA_AUDIO_CHANNEL_AUX11, // Ch 12 (Line In 12)
            SPA_AUDIO_CHANNEL_AUX12, // Ch 13 (Line In 13)
            SPA_AUDIO_CHANNEL_AUX13, // Ch 14 (Line In 14)
            SPA_AUDIO_CHANNEL_AUX14, // Ch 15 (S/PDIF L)
            SPA_AUDIO_CHANNEL_AUX15  // Ch 16 (S/PDIF R)
        }
    };
    const struct spa_pod *params_source[1];
    params_source[0] = spa_format_audio_raw_build(&b2, SPA_PARAM_EnumFormat, &info_source);
    pw_stream_connect(source_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                      PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
                      params_source, 1);

    pw_thread_loop_start(pw_tloop);

    if (init_alsa_midi()) {
        pthread_create(&g_midi_out_thread, NULL, midi_out_worker, NULL);
        g_midi_out_thread_running = true;
    }

    printf("[SUCCESS] PipeWire Nodes active in sound server at %d Hz (Native 32-bit Float)!\n", g_config.sample_rate);
    printf("          - Output Node: TASCAM US-1641 Outputs (4 Ch: Main L/R + Line 3/4)\n");
    printf("          - Input Node:  TASCAM US-1641 Inputs  (16 Ch: 8 Mics + 6 Line + S/PDIF)\n");
    if (g_seq_handle) {
        printf("          - MIDI Ports:  TASCAM US-1641 MIDI In & MIDI Out (ALSA Seq / PipeWire)\n");
    }

    // Pre-allocate Isochronous Feedback IN transfers (Endpoint 0x81)
    unsigned char *fb_bufs[NUM_FB_TRANSFERS];
    struct libusb_transfer *fb_xfers[NUM_FB_TRANSFERS];

    for (int t = 0; t < NUM_FB_TRANSFERS; t++) {
        fb_bufs[t] = calloc(1, NUM_FB_PACKETS * FB_PACKET_SIZE);
        fb_xfers[t] = libusb_alloc_transfer(NUM_FB_PACKETS);
        libusb_fill_iso_transfer(fb_xfers[t], g_handle, EP_ISO_IN_FB, fb_bufs[t],
                                NUM_FB_PACKETS * FB_PACKET_SIZE,
                                NUM_FB_PACKETS, iso_in_fb_cb, NULL, 1000);
        for (int i = 0; i < NUM_FB_PACKETS; i++) {
            fb_xfers[t]->iso_packet_desc[i].length = FB_PACKET_SIZE;
        }
        g_active_fb_transfers++;
        libusb_submit_transfer(fb_xfers[t]);
    }

    // Pre-allocate Isochronous OUT transfers (Endpoint 0x02)
    int max_transfer_buf_size = NUM_ISO_PACKETS * 96;
    unsigned char *iso_bufs[NUM_ISO_TRANSFERS];
    struct libusb_transfer *iso_xfers[NUM_ISO_TRANSFERS];

    for (int t = 0; t < NUM_ISO_TRANSFERS; t++) {
        iso_bufs[t] = calloc(1, max_transfer_buf_size);
        iso_xfers[t] = libusb_alloc_transfer(NUM_ISO_PACKETS);
        libusb_fill_iso_transfer(iso_xfers[t], g_handle, EP_ISO_OUT, iso_bufs[t], max_transfer_buf_size,
                                NUM_ISO_PACKETS, iso_out_cb, NULL, 1000);
        prepare_iso_transfer_buffer(iso_xfers[t]);
        g_active_iso_transfers++;
        libusb_submit_transfer(iso_xfers[t]);
    }

    pthread_t iso_thread;
    pthread_create(&iso_thread, NULL, usb_event_worker, NULL);

    // Pre-allocate Bulk IN transfers (Endpoint 0x86) for asynchronous, jitter-free capture
    unsigned char *bulk_bufs[NUM_BULK_TRANSFERS];
    struct libusb_transfer *bulk_xfers[NUM_BULK_TRANSFERS];

    for (int t = 0; t < NUM_BULK_TRANSFERS; t++) {
        bulk_bufs[t] = calloc(1, BULK_BUF_SIZE);
        bulk_xfers[t] = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(bulk_xfers[t], g_handle, EP_BULK_IN, bulk_bufs[t],
                                  BULK_BUF_SIZE, bulk_in_cb, NULL, 1000);
        g_active_bulk_transfers++;
        libusb_submit_transfer(bulk_xfers[t]);
    }

    // Pre-allocate Bulk IN transfers (Endpoint 0x83) for hardware MIDI Input
    unsigned char *midi_in_bufs[NUM_MIDI_IN_TRANSFERS];
    struct libusb_transfer *midi_in_xfers[NUM_MIDI_IN_TRANSFERS];

    for (int t = 0; t < NUM_MIDI_IN_TRANSFERS; t++) {
        midi_in_bufs[t] = calloc(1, MIDI_IN_BUF_SIZE);
        midi_in_xfers[t] = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(midi_in_xfers[t], g_handle, EP_MIDI_IN, midi_in_bufs[t],
                                  MIDI_IN_BUF_SIZE, midi_in_cb, NULL, 0);
        g_active_midi_in_transfers++;
        int r = libusb_submit_transfer(midi_in_xfers[t]);
        if (r < 0) {
            fprintf(stderr, "[ERROR] libusb_submit_transfer MIDI IN %d failed: %d (%s)\n",
                    t, r, libusb_error_name(r));
            g_active_midi_in_transfers--;
        } else {
            fprintf(stderr, "[INFO] Submitted MIDI IN transfer %d (EP 0x%02X, size %d)\n",
                    t, EP_MIDI_IN, MIDI_IN_BUF_SIZE);
        }
    }

    printf("[RUNNING] Full-Duplex Multi-Track Audio Bridge active! Managing closed-loop hardware pacing...\n");

    char stats_path[512];
    char hist_path[512];
    get_runtime_file_path(stats_path, sizeof(stats_path), "tascamd.stats");
    get_runtime_file_path(hist_path, sizeof(hist_path), "tascamd_history.csv");

    uint64_t last_src_pushed = 0;
    uint64_t loop_ticks = 0;

    // Main thread sleeps while background threads and callbacks process transfers
    while (g_running) {
        usleep(50000); // 50 ms
        loop_ticks++;

        // Exit cleanly if USB transfers have died (e.g. hardware powered off or unplugged)
        if (g_active_iso_transfers == 0 || g_active_bulk_transfers == 0) {
            fprintf(stderr, "[WARN] Active USB transfers stopped. Exiting daemon...\n");
            g_running = false;
            break;
        }

        // Non-realtime telemetry snapshot once per second (every 20 * 50ms ticks)
        if (loop_ticks % 20 == 0) {
            uint64_t cur_pushed = g_source_frames_pushed;
            uint32_t src_fps = (uint32_t)(cur_pushed - last_src_pushed);
            last_src_pushed = cur_pushed;

            uint32_t sink_q = ring_avail_read_sink(&g_sink_rb);
            uint32_t src_q = ring_avail_read_source(&g_source_rb);
            long q_diff = s_sink_active ? (long)(s_sink_q_filtered - (float)s_sink_target_buffer) : 0;
            int locked = (g_fb_packets_count > 10) ? 1 : 0;

            FILE *fstats = fopen(stats_path, "w");
            if (fstats) {
                fprintf(fstats,
                        "rate=%d preroll=%d locked=%d hw_err=%ld | "
                        "sink_q=%u sink_r=%.6f sink_act=%d u_runs=%u o_runs=%u | "
                        "src_fps=%u src_q=%u src_r=%.6f src_act=%d src_u=%u src_o=%u | "
                        "xfers=%d fb_pkts=%lu fb_last=%u | "
                        "midi_in=%lu midi_out=%lu\n",
                        g_config.sample_rate, g_config.preroll_frames, locked, q_diff,
                        sink_q, s_sink_ratio, (int)s_sink_active, g_sink_underruns, g_sink_overruns,
                        src_fps, src_q, s_source_ratio, (int)s_source_active, s_source_underruns, s_source_overruns,
                        g_active_iso_transfers, (unsigned long)g_fb_packets_count, (unsigned int)g_fb_last_val,
                        (unsigned long)g_midi_in_events_count, (unsigned long)g_midi_out_events_count);
                fclose(fstats);
            }

            if (g_config.debug_log) {
                FILE *fhist = fopen(hist_path, "a");
                if (fhist) {
                    fprintf(fhist, "%lu,%u,%.6f,%d,%u,%.6f,%d,%ld,%u,%u,%u,%u\n",
                            (unsigned long)time(NULL), sink_q, s_sink_ratio, (int)s_sink_active,
                            src_q, s_source_ratio, (int)s_source_active,
                            q_diff, g_sink_underruns, g_sink_overruns, s_source_underruns, s_source_overruns);
                    fclose(fhist);
                }
            }
        }
    }

    printf("\n[INFO] Draining transfers and stopping PipeWire streams...\n");

    if (g_midi_out_thread_running) {
        pthread_join(g_midi_out_thread, NULL);
    }

    for (int t = 0; t < NUM_MIDI_IN_TRANSFERS; t++) {
        if (midi_in_xfers[t]) {
            libusb_cancel_transfer(midi_in_xfers[t]);
        }
    }

    for (int wait_ms = 0; wait_ms < 500 && (g_active_iso_transfers > 0 || g_active_fb_transfers > 0 || g_active_bulk_transfers > 0 || g_active_midi_in_transfers > 0); wait_ms++) {
        usleep(1000);
    }
    pthread_join(iso_thread, NULL);

    // Stop FPGA streaming engine and mute hardware DAC to prevent underrun screech on shutdown
    if (g_handle) {
        libusb_control_transfer(g_handle, 0x40, 0x49, 0x0000, 0x0002, NULL, 0, 1000);
    }

    for (int t = 0; t < NUM_ISO_TRANSFERS; t++) {
        libusb_free_transfer(iso_xfers[t]);
        free(iso_bufs[t]);
    }
    for (int t = 0; t < NUM_FB_TRANSFERS; t++) {
        libusb_free_transfer(fb_xfers[t]);
        free(fb_bufs[t]);
    }
    for (int t = 0; t < NUM_BULK_TRANSFERS; t++) {
        libusb_free_transfer(bulk_xfers[t]);
        free(bulk_bufs[t]);
    }
    for (int t = 0; t < NUM_MIDI_IN_TRANSFERS; t++) {
        if (midi_in_xfers[t]) {
            libusb_free_transfer(midi_in_xfers[t]);
            free(midi_in_bufs[t]);
        }
    }

    if (g_seq_handle) {
        pthread_mutex_lock(&g_seq_lock);
        if (g_midi_in_coder) snd_midi_event_free(g_midi_in_coder);
        if (g_midi_out_coder) snd_midi_event_free(g_midi_out_coder);
        snd_seq_close(g_seq_handle);
        g_seq_handle = NULL;
        pthread_mutex_unlock(&g_seq_lock);
    }

    unlink(stats_path);

    pw_thread_loop_stop(pw_tloop);
    pw_stream_destroy(sink_stream);
    pw_stream_destroy(source_stream);
    pw_context_destroy(pw_ctx);
    pw_thread_loop_destroy(pw_tloop);
    pw_deinit();

    libusb_release_interface(g_handle, 0);
    libusb_release_interface(g_handle, 1);
    libusb_close(g_handle);
    libusb_exit(g_ctx);

    printf("[SUCCESS] tascamd cleanly shut down.\n");
    return 0;
}
