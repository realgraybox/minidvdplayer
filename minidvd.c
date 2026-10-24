#define _XOPEN_SOURCE 500
#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <limits.h>
#include <sys/time.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrender.h>
// Fallbacks for old RENDER headers (XFree86 4.x). These are protocol
// values, so they are the same for every server; whether the server
// accepts RepeatPad is checked at runtime (render_pad).
#ifndef RepeatNone
#define RepeatNone 0   // RENDER < 0.10 headers: repeat was just False/True
#endif
#ifndef RepeatPad
#define RepeatPad 2    // RENDER 0.10+
#endif
#ifndef PictOpSrc
#define PictOpSrc 1
#endif
#ifndef CPRepeat
#define CPRepeat (1 << 0)
#endif
#ifndef FilterNearest
#define FilterNearest "nearest"
#endif
#ifndef FilterBilinear
#define FilterBilinear "bilinear"
#endif
#ifndef XDoubleToFixed
#define XDoubleToFixed(f) ((XFixed) ((f) * 65536))
#endif
#include <sys/ipc.h>
#include <sys/shm.h>

#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>
#include <dvdread/nav_read.h>

#include <mpeg2.h>
#include <mpeg2convert.h>

#include <a52.h>

#include "audio.h"	//audio/audio.c: tinyalsa + OSS



/*
 * A/V timing
 *   default:       every video frame is shown at its PTS, measured against a
 *                  clock that is locked to the audio playback position.
 *                  Audio is written without blocking, so it never stalls video.
 *   -DSYNC_OLD:    previous behaviour: video is paced by blocking audio writes
 *                  (plus the "pacemaker").
 * Test hooks (not for normal builds):
 *   -DTEST_PLAIN_VOB   argv[1] is a plain MPEG-PS/VOB file instead of a DVD
 *   -DAUDIO_HAVE_SIM   simulated sound card; prints per-frame "AV" log lines
 */

// Diagnostics (new timing): MINIDVD_DEBUG=1 prints a summary on exit,
// MINIDVD_DEBUG=2 also one line per frame. MINIDVD_XSYNC=1 waits for the X
// server to finish each frame (XSync instead of XFlush).
static struct {
    int level, xsync;
    long frames, drops, slow_draws, slow_reads, blocks;
    double iv_sum, iv_sq, late_sum, late_max, draw_sum, draw_max;
    double dec_sum, dec_max, rd_max, rd_frame_max, dec_frame;
    double wait_ms, wait_sum, wait_max;  // waiting for the X server (in draw)
    long hist[9];                 // shown-frame interval buckets
    long clk_fwd, clk_back, clk_stall, clk_jump, clk_discont, starved;
    double clk_fwd_sum, clk_fwd_max, clk_back_sum, clk_stall_sum, clk_adj;
    int adly_min, adly_max;
} dbg = { .adly_min = 1 << 30, .adly_max = -1 };

static volatile sig_atomic_t sigint_received = 0;
static void on_sigint(int s) { (void)s; sigint_received = 1; }
static void audio_feed(uint8_t *data, int len, void *opaque);

//state globals
static mpeg2dec_t *mpeg2 = NULL;
static const mpeg2_info_t *info = NULL;
static dvd_file_t *vob = NULL;

static uint32_t dvd_block;        // current read block
static uint32_t total_blocks;

//ps_demux globals
#ifdef SYNC_OLD
#define VIDEO_ES_BUF_SIZE (512 * 1024)
static uint8_t video_es_buf[VIDEO_ES_BUF_SIZE];

static int video_es_len = 0;
#endif

static int seek_pending = 0;
static uint32_t seek_target_block = 0;

static int have_frame = 0;
static int need_redraw = 1;

static int running = 1;
static int paused = 0;
static int show_help = 0;

//scaling
static int scale_video = 0;   // toggled by key, eg 'f'
static int dst_w = 0;
static int dst_h = 0;

//audio output globals
static int audio_active = 0;	// stream activity

static int audio_rate = 48000;     // output rate
static int audio_channels = 2;     // output channels (stereo downmix)
static int audio_muted = 0;
static int audio_volume_internal = 100;
static int audio_ready = 0;   // audio device init attempted

//audio globals
typedef enum {
    AUDIO_NONE = 0,
    AUDIO_AC3
} audio_type_t;

static audio_type_t audio_type = AUDIO_NONE;
static int audio_enabled = 0;
static int audio_checked = 0;   // NEW: detect once only

static uint64_t last_audio_bytes = 0;
static uint64_t audio_bytes = 0;

//subtitles
#define MAX_SUBTITLE_STREAMS 8
#define MAX_SUBTITLE_SIZE (8 *  2048)  // adjust as needed 2048

#define SUB_MAX_W 720
#define SUB_MAX_H 576

static uint8_t sub_bitmap[SUB_MAX_W * SUB_MAX_H];
static int sub_x, sub_y, sub_w, sub_h;
static int sub_valid = 0;

typedef struct {
    uint8_t data[MAX_SUBTITLE_SIZE]; // raw subpicture bitmap
    int len;                         // data length
    uint8_t sid;                      // substream ID
} subtitle_frame_t;


static int subtitle_enabled = 1;	// user toggle (key 's')
static int current_sub_sid = -1;
static int subtitle_active = 0;    // SPU currently visible
static uint64_t last_sub_time_ms = 0;  // timestamp of last subtitle received
static int sub_display_duration_ms = 4000; // show each subtitle for 4 seconds

static uint8_t sub_sid_list[32] = {
    0x20, 0x21, 0x22, 0x23,
    0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2A, 0x2B,
    0x2C, 0x2D, 0x2E, 0x2F,
    0x30, 0x31, 0x32, 0x33,
    0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3A, 0x3B,
    0x3C, 0x3D, 0x3E, 0x3F
};

static int sub_count = 32;

static uint8_t audio_sid_list[8] = {
    0x80, 0x81, 0x82, 0x83,
    0x84, 0x85, 0x86, 0x87
};

static int audio_count = 8;
static int audio_sid_selected = -1;   // -1 = auto


#define MAX_SPU_STREAMS 32

struct spu_assembler {
    uint8_t buf[MAX_SUBTITLE_SIZE];
    int len;
    int waiting_for_header;
};

static struct spu_assembler spu_assemblers[MAX_SPU_STREAMS];

//AC
static uint8_t ac3_buf[8192];
static int ac3_len = 0;
static int ac3_need = 0;
static a52_state_t *a52;

//X11 globals
static int video_x = 0;
static int video_y = 0;
static int video_w = 720;
static int video_h = 576;
static int win_w = 720;
static int win_h = 576;
static Display *dpy;
static Window win;
static GC gc;
static XImage *ximg;
static XShmSegmentInfo shminfo;    // shminfo.shmaddr != NULL: ximg is in shared memory
static int use_shm = 0;            // -1 = try, 1 = in use, 0 = off
static int shm_failed = 0;
static int shm_put_pending = 0;    // server may still be reading ximg
static void recreate_ximage(int w, int h);

// XRender: the frame is uploaded at video size into a server-side pixmap
// and the X server scales it onto the window (on the GPU where the driver
// supports it). Without RENDER, or with MINIDVD_NORENDER=1, minidvd scales
// on the CPU and uploads the full window-sized image.
static int use_render = 0;
static int render_pad = 0;         // server supports RepeatPad (RENDER >= 0.10)
static XRenderPictFormat *render_fmt;
static Picture render_dst = None;  // the window
static Picture render_src = None;  // the video pixmap
static Pixmap render_pix = None;
static int render_pix_w, render_pix_h;
static int render_tr_w, render_tr_h;   // destination size the transform is set for

// Bilinear scaler for 32-bit xRGB with 8-bit weights. Each source row is
// scaled horizontally once (two rows cached), then pairs of rows are blended
// vertically. R+B and G are processed together in one 32-bit word.
#define SCALE_MAX_W 4096
static uint32_t scale_xofs[SCALE_MAX_W];      // source x0 per dest column
static uint32_t scale_xfrac[SCALE_MAX_W];     // weight of x0+1, 0..255
static uint32_t scale_rowbuf[2][SCALE_MAX_W]; // horizontally scaled rows
static int scale_tab_sw = -1, scale_tab_dw = -1;

static inline uint32_t mix32(uint32_t a, uint32_t b, uint32_t f) {
    // (a*(256-f) + b*f) / 256 for each 8-bit channel, f = 0..255
    uint32_t rb = ((a & 0x00FF00FF) * (256 - f) + (b & 0x00FF00FF) * f) >> 8;
    uint32_t g  = ((a & 0x0000FF00) * (256 - f) + (b & 0x0000FF00) * f) >> 8;
    return (rb & 0x00FF00FF) | (g & 0x0000FF00);
}

static void scale_hrow(uint32_t *out, const uint32_t *src, int dw) {
    for (int x = 0; x < dw; x++) {
        const uint32_t *p = src + scale_xofs[x];
        uint32_t f = scale_xfrac[x];
        out[x] = f ? mix32(p[0], p[1], f) : p[0];
    }
}

//scaling: 16.16 fixed-point positions, 8-bit blend weights
static void scale_bilinear_rgba(uint8_t *dst8, int dw, int dh,
    const uint8_t *src8, int sw, int sh) {
    const uint32_t *src = (const uint32_t *)src8;
    uint32_t *dst = (uint32_t *)dst8;
    uint32_t *top = scale_rowbuf[0], *bot = scale_rowbuf[1];
    int top_y = -2;                       // source row in "top" (bot = top_y + 1); -2 = none

    if (dw > SCALE_MAX_W) dw = SCALE_MAX_W;
    if (scale_tab_sw != sw || scale_tab_dw != dw) {
        uint32_t xstep = ((uint32_t)(sw - 1) << 16) / dw, sx = 0;
        for (int x = 0; x < dw; x++, sx += xstep) {
            scale_xofs[x] = sx >> 16;
            scale_xfrac[x] = (sx >> 8) & 0xFF;
            if (scale_xofs[x] + 1 >= (uint32_t)sw)   // never read past the row
                scale_xfrac[x] = 0;
        }
        scale_tab_sw = sw;
        scale_tab_dw = dw;
    }

    uint32_t ystep = ((uint32_t)(sh - 1) << 16) / dh, sy = 0;
    for (int y = 0; y < dh; y++, sy += ystep) {
        int y0 = sy >> 16;
        int y1 = y0 + 1 < sh ? y0 + 1 : y0;
        uint32_t fy = (sy >> 8) & 0xFF;
        uint32_t *d = dst + (size_t)y * dw;

        if (y0 != top_y) {
            if (top_y >= 0 && y0 == top_y + 1) {   // moved down one row: reuse
                uint32_t *t = top; top = bot; bot = t;
            } else {
                scale_hrow(top, src + (size_t)y0 * sw, dw);
            }
            scale_hrow(bot, src + (size_t)y1 * sw, dw);
            top_y = y0;
        }
        if (!fy) {
            memcpy(d, top, (size_t)dw * 4);
        } else {
            for (int x = 0; x < dw; x++)
                d[x] = mix32(top[x], bot[x], fy);
        }
    }
}

//subtitle stream demuxer
struct spu_state {
    int display;            // 0 = off, 1 = on
    int x1, y1, x2, y2;     // display area
    int have_area;
    uint32_t rle_top, rle_bottom;
    int have_rle;
    uint8_t palette[4];    // 4-bit indices
    int have_palette;
    uint8_t alpha[4];       // 4-bit alpha
	int have_alpha;
	int is_8bit;
};

static int64_t last_time_ms = 0;
static int64_t current_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t now =
        (int64_t)tv.tv_sec * 1000 +
        tv.tv_usec / 1000;

    if (now < last_time_ms)
        now = last_time_ms;   // clamp backwards jump

    last_time_ms = now;
    return now;
}

static inline uint16_t RB16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

static inline uint32_t RB32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |
           ((uint32_t)p[3]);
}

// Subtitles: ARGB palette and alpha setup for 2-bit SPU
// Works for both normal and dark DVDs

#define CLAMP(a,min,max) ((a)<(min)?(min):((a)>(max)?(max):(a)))

typedef struct {
    uint8_t y, cb, cr;  // YCbCr values
} dvd_clut_entry_t;

static dvd_clut_entry_t temp_clut[16];

static uint32_t sub_palette[4];  // ARGB values for indices 0..3
static uint8_t  sub_alpha[4];    // 0..255

// Initialize default CLUT values (can tweak for brighter shadows)
static void init_temp_clut(void) {
    temp_clut[0] = (dvd_clut_entry_t){16, 128, 128};   // transparent
    temp_clut[1] = (dvd_clut_entry_t){235, 128, 128};  // main text (white)
    temp_clut[2] = (dvd_clut_entry_t){210, 128, 128};  // secondary text / light gray
    temp_clut[3] = (dvd_clut_entry_t){140, 128, 128};  // shadow / dark gray

    // Fill rest safely with black
    for (int i = 4; i < 16; i++)
        temp_clut[i] = (dvd_clut_entry_t){16, 128, 128};
}

// YCbCr → ARGB conversion
static inline uint32_t clut_ycbcr_to_argb(dvd_clut_entry_t c, uint8_t alpha) {
    // alpha: 0-255
    int cY  = c.y;
    int cCb = c.cb - 128;
    int cCr = c.cr - 128;

    int r = CLAMP((int)(cY + 1.402 * cCr), 0, 255);
    int g = CLAMP((int)(cY - 0.344136 * cCb - 0.714136 * cCr), 0, 255);
    int b = CLAMP((int)(cY + 1.772 * cCb), 0, 255);

    return (alpha << 24) | (r << 16) | (g << 8) | b;
}

// Setup palette and alpha for SPU
// Inputs:
//   palette[4] -> SPU indices (0..3) from control
//   alpha[4]   -> 0..15 values from SPU
// Output:
//   sub_palette[4] -> ARGB
//   sub_alpha[4]   -> 0..255
static void setup_sub_palette_alpha(const uint8_t palette[4], const uint8_t alpha[4]) {
    for (int i = 0; i < 4; i++) {
        // Scale 0-15 -> 0-255
        uint8_t a = (alpha[i] << 4) | alpha[i];

        // Main text fully opaque
        if (i == 1) a = 255;

        // Optional: brighten shadows if too faint
        if ((palette[i] == 3 || palette[i] == 2) && a < 64)
            a = 64;

        sub_palette[i] = clut_ycbcr_to_argb(temp_clut[palette[i]], a);
        sub_alpha[i]   = a;
    }
}

// --- Minimal bit/nibble reader
typedef struct {
    const uint8_t *buf;
    int byte_pos;
    int bit_pos;
    int size_bytes;
} BitReader;

static void br_init(BitReader *br, const uint8_t *buf, int size) {
    br->buf = buf;
    br->byte_pos = 0;
    br->bit_pos = 0;
    br->size_bytes = size;
}

static int br_get1(BitReader *br) {
    if (br->byte_pos >= br->size_bytes)
        return 0;
    int bit = (br->buf[br->byte_pos] >> (7 - br->bit_pos)) & 1;
    br->bit_pos++;
    if (br->bit_pos == 8) {
        br->bit_pos = 0;
        br->byte_pos++;
    }
    return bit;
}

static int br_get_bits(BitReader *br, int n) {
    int val = 0;
    for (int i = 0; i < n; i++)
        val = (val << 1) | br_get1(br);
    return val;
}

static void br_align(BitReader *br) {
    if (br->bit_pos != 0) {
        br->bit_pos = 0;
        br->byte_pos++;
    }
}

// --- Decode one RLE run for 2-bit SPU
static int decode_run_2bit(BitReader *br, int *color) {
    unsigned int v = 0;
    unsigned int t;
    for (t = 1; v < t && t <= 0x40; t <<= 2)
        v = (v << 4) | br_get_bits(br, 4);
    *color = v & 3;
    if (v < 4)      // fill rest-of-line code
        return INT_MAX;
    return v >> 2;
}

// --- Decode one RLE run for 8-bit SPU
static int decode_run_8bit(BitReader *br, int *color) {
    int len;
    int has_run = br_get1(br);
    if (br_get1(br))
        *color = br_get_bits(br, 8);
    else
        *color = br_get_bits(br, 2);
    if (has_run) {
        if (br_get1(br)) {
            len = br_get_bits(br, 7);
            if (len == 0)
                len = INT_MAX;
            else
                len += 9;
        } else
            len = br_get_bits(br, 3) + 2;
    } else
        len = 1;
    return len;
}

// --- Decode RLE bitmap for a single field
static int decode_rle_field(uint8_t *bitmap, int linesize, int w, int h,
                            const uint8_t *buf, int start, int buf_size, int is_8bit) {
    BitReader br;
    br_init(&br, buf + start, buf_size - start);

    int x = 0, y = 0, len, color;
    uint8_t *d = bitmap;

    while (y < h) {
        if (is_8bit)
            len = decode_run_8bit(&br, &color);
        else
            len = decode_run_2bit(&br, &color);

        if (len == INT_MAX)
            len = w - x;  // fill rest-of-line

        if (len > w - x)
            len = w - x;

        memset(d + x, color, len);
        x += len;

        if (x >= w) {
            x = 0;
            y++;
            d += linesize;
            br_align(&br);
        }
    }
    return 0;
}

// --- Decode full SPU bitmap (even + odd fields)
int decode_spu_bitmap(uint8_t *bitmap, int w, int h,
                      const uint8_t *buf, int rle_top, int rle_bottom,
                      int is_8bit) {
    if (!bitmap || w <= 0 || h <= 0)
        return -1;

    memset(bitmap, 0, w * h);

    // Even field (lines 0,2,4...)
    decode_rle_field(bitmap, w * 2, w, (h + 1) / 2,
                     buf, rle_top, 0x7FFFFFFF, is_8bit);

    // Odd field (lines 1,3,5...)
    decode_rle_field(bitmap + w, w * 2, w, h / 2,
                     buf, rle_bottom, 0x7FFFFFFF, is_8bit);

    return 0;
}

static int luma(uint32_t rgb) {
    int r = (rgb >> 16) & 0xFF;
    int g = (rgb >>  8) & 0xFF;
    int b =  rgb        & 0xFF;
    return (r * 299 + g * 587 + b * 114) / 1000;
}

static void normalize_sub_palette(void) {
    int min_l = 255, max_l = 0;
    int text_idx = -1;
    int brightest = -1;
    for (int i = 1; i < 4; i++) {
        if (sub_alpha[i] == 0)
            continue;
        int l = luma(sub_palette[i]);
        if (l < min_l) min_l = l;
        if (l > max_l) max_l = l;
        if (l > brightest) {
            brightest = l;
            text_idx = i;
        }
    }
    if (text_idx < 0)
        return;
    int spread = max_l - min_l;
    // Case A: outlined / multi-layer → flatten
    if (spread >= 80) {
        uint32_t text_color = sub_palette[text_idx];

        for (int i = 1; i < 4; i++) {
            if (i == text_idx) {
                sub_palette[i] = text_color;
                sub_alpha[i] = 255;
            } else {
                sub_alpha[i] = 0;
            }
        }
        return;
    }

    // Case B: flat / low-contrast → enhance
    if (brightest < 160) {
        sub_palette[text_idx] = 0x00FFFFFF; // force white
        sub_alpha[text_idx] = 255;
    }
}

int spu_demux_control(const uint8_t *buf, int len, struct spu_state *st) {
	
    int offset_size;
    int ctrl_ofs;
    const uint8_t *p;
    if (len < 6)
        return -1;
    // --- detect offset size (DVD vs HD)
    if (RB16(buf) == 0) {
        // HD subpicture
        offset_size = 4;
        ctrl_ofs = RB32(buf + 2);
    } else {
        // normal DVD
        offset_size = 2;
        ctrl_ofs = RB16(buf + 2);
    }
    if (ctrl_ofs <= 0 || ctrl_ofs >= len)
        return -2;
    // --- initialize state ONCE per SPU
    memset(st, 0, sizeof(*st));
    st->x1 = st->y1 = st->x2 = st->y2 = -1;
    st->rle_top = st->rle_bottom = 0xFFFFFFFF;

    // --- iterate control blocks
    while (ctrl_ofs > 0 && ctrl_ofs + 2 + offset_size < len) {
        uint32_t next_ofs;
        if (offset_size == 2)
            next_ofs = RB16(buf + ctrl_ofs + 2);
        else
            next_ofs = RB32(buf + ctrl_ofs + 2);
        p = buf + ctrl_ofs + 2 + offset_size;
        const uint8_t *block_end =
            (next_ofs > 0 && next_ofs < len) ? buf + next_ofs : buf + len;
        // --- parse commands in this block
        while (p < block_end) {
            uint8_t raw = *p++;
            uint8_t cmd;
            int flag_8bit;
            if (raw == 0xFF) {
                break;
            }
            cmd = raw & 0x7F;
            flag_8bit = raw & 0x80;
            switch (cmd) {
            case 0x00: // menu flag (ignore)
                break;
            case 0x01: // display on
                st->display = 1;
                break;
            case 0x02: // display off
                st->display = 0;
                break;
            case 0x03: // palette
                if (p + 2 > block_end) return -3;
                {
                    uint16_t v = RB16(p);
                    p += 2;
                    st->palette[3] = (v >> 12) & 0x0F;
                    st->palette[2] = (v >> 8)  & 0x0F;
                    st->palette[1] = (v >> 4)  & 0x0F;
                    st->palette[0] = v & 0x0F;
                    st->have_palette = 1;
                }
                break;
            case 0x04: // alpha
                if (p + 2 > block_end) return -4;
                {
                    uint16_t v = RB16(p);
                    p += 2;
                    st->alpha[3] = (v >> 12) & 0x0F;
                    st->alpha[2] = (v >> 8)  & 0x0F;
                    st->alpha[1] = (v >> 4)  & 0x0F;
                    st->alpha[0] = v & 0x0F;
                    st->have_alpha = 1;
                }
                break;
            case 0x05: // display area
                if (p + 6 > block_end) return -5;
                st->x1 = (p[0] << 4) | (p[1] >> 4);
                st->x2 = ((p[1] & 0x0F) << 8) | p[2];
                st->y1 = (p[3] << 4) | (p[4] >> 4);
                st->y2 = ((p[4] & 0x0F) << 8) | p[5];
                p += 6;
                st->have_area = 1;
                st->is_8bit = flag_8bit;
                break;
            case 0x06: // RLE offsets
                if (flag_8bit) {
                    if (p + 8 > block_end) return -6;
                    st->rle_top    = RB32(p);
                    st->rle_bottom = RB32(p + 4);
                    p += 8;
                } else {
                    if (p + 4 > block_end) return -6;
                    st->rle_top    = RB16(p);
                    st->rle_bottom = RB16(p + 2);
                    p += 4;
                }
                st->have_rle = 1;
                break;
            default:
                // FFmpeg also just stops on unknown
                break;
            }
        }
        // stop if next offset is invalid or does not advance
		if (next_ofs <= ctrl_ofs || next_ofs >= len) {
			break;
		}
		ctrl_ofs = next_ofs;
    }
    
    int w = st->x2 - st->x1 + 1;
	int h = st->y2 - st->y1;
	memset(sub_bitmap, 0, w * h);

	decode_spu_bitmap(sub_bitmap, w, h, buf, st->rle_top, st->rle_bottom, st->is_8bit);
	
	sub_x = st->x1;
	sub_y = st->y1;
	sub_w = w;
	sub_h = h;

    setup_sub_palette_alpha(st->palette, st->alpha);
    normalize_sub_palette();

	sub_valid = 1;
    subtitle_active = sub_valid;  
	last_sub_time_ms = current_time_ms(); // store arrival time

    return 0;
}
//demuxer end

static inline uint32_t blend_argb(uint8_t a, uint32_t src, uint32_t dst) {
    uint32_t rb = (((src & 0x00FF00FF) * a +
                    (dst & 0x00FF00FF) * (255 - a)) >> 8) & 0x00FF00FF;

    uint32_t g  = (((src & 0x0000FF00) * a +
                    (dst & 0x0000FF00) * (255 - a)) >> 8) & 0x0000FF00;

    return 0xFF000000 | rb | g;
}

static void overlay_subtitle(uint8_t *rgb, int w, int h) {
    if (!sub_valid) return;
    //scale
    float sx = (float)w / (float)video_w;
	float sy = (float)h / (float)video_h;
    
    int bpl = ximg->bytes_per_line;
    int bpp = ximg->bits_per_pixel / 8;   // usually 4
    
    int dst_h = (int)(sub_h * sy);
	int dst_w = (int)(sub_w * sx);
    
    for (int y = 0; y < dst_h; y++) {
		int sy_src = y * sub_h / dst_h;
		int vy = (int)((sub_y * sy) + y);
		if ((unsigned)vy >= (unsigned)h) continue;

		uint8_t *row = rgb + vy * bpl;

		for (int x = 0; x < dst_w; x++) {
			int sx_src = x * sub_w / dst_w;
			uint8_t idx = sub_bitmap[sy_src * sub_w + sx_src];
			if (idx == 0) continue;

			int vx = (int)((sub_x * sx) + x);
			if ((unsigned)vx >= (unsigned)w) continue;

			uint32_t *dst = (uint32_t *)(row + vx * bpp);
			uint32_t src = sub_palette[idx];
			uint8_t  a   = sub_alpha[idx];

			if (a == 255)
				*dst = 0xFF000000 | src;
			else if (a)
				*dst = blend_argb(a, src, *dst);
		}
	}
}

//help text/functions

struct help_entry {
    const char *key;
    const char *desc;
};

static const struct help_entry help_entries[] = {
    { "Space",       "Pause" },
    { "m",           "Mute" },
    { "+ / -",       "Volume up / down" },
    { "0",           "Volume reset" },
    { "Mouse click", "Seek within title" },
    { "a",           "Next audio stream" },
    { "Shift + a",   "Previous audio stream" },    
    { "f",           "Fullscreen" },
    { "h",           "Toggle help" },
    { "n",           "Next subtitle stream" },
    { "Shift + n",   "Previous subtitle stream" },    
    { "s",           "Toggle subtitle ON/OFF" },
    { "q / Esc",     "Quit" },
    { NULL, NULL }
};

static void draw_help_overlay(void) {
    int cx = video_x + video_w / 2 - 150;
    int cy = video_y + video_h / 2 - 150;
    int line = 16;
    int y = cy;
    XDrawString(dpy, win, gc, cx, y, "minidvd help", 12);
    y += line * 2;
    XDrawString(dpy, win, gc, cx, y, "Keys:", 5);
    y += line;
    for (int i = 0; help_entries[i].key; i++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%-12s %s",
                 help_entries[i].key,
                 help_entries[i].desc);

        XDrawString(dpy, win, gc, cx, y, buf, strlen(buf));
        y += line;
    }
}

static void print_help(FILE *out) {
    fprintf(out, "minidvd help\n\n");
    fprintf(out, "Keys:\n");
    for (int i = 0; help_entries[i].key; i++) {
        fprintf(out, "  %-12s %s\n",
                help_entries[i].key,
                help_entries[i].desc);
    }
}

static void request_seek(double frac) {
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;

    seek_target_block = (uint32_t)(frac * (double)total_blocks);
    if (seek_target_block < 0) seek_target_block = 0;
	if (seek_target_block >= total_blocks)
    seek_target_block = total_blocks - 1;

    seek_pending = 1;

    fprintf(stderr, "Seek request: %.2f → block %u\n",
            frac, seek_target_block);
}

//Toggle fullscreen
static void toggle_fullscreen(Display *dpy, Window win) {
    Atom wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom fs_atom  = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    XEvent e;
    memset(&e, 0, sizeof(e));
    e.type = ClientMessage;
    e.xclient.window = win;
    e.xclient.message_type = wm_state;
    e.xclient.format = 32;
    e.xclient.data.l[0] = 2; // _NET_WM_STATE_TOGGLE
    e.xclient.data.l[1] = fs_atom;
    e.xclient.data.l[2] = 0;
    e.xclient.data.l[3] = 1;
    XSendEvent(dpy,
               DefaultRootWindow(dpy),
               False,
               SubstructureNotifyMask | SubstructureRedirectMask,
               &e);
}

// ------------------ A/V timing core ------------------
#define PTS_NONE INT64_MIN
#define PTS_HZ   90000

static __attribute__((unused)) int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// PTS of the PES packet starting at p (00 00 01 id ...), or PTS_NONE
static int64_t pes_get_pts(const uint8_t *p, int avail) {
    if (avail < 14) return PTS_NONE;
    if ((p[6] & 0xC0) != 0x80) return PTS_NONE;   // not an MPEG-2 PES header
    if (!(p[7] & 0x80)) return PTS_NONE;          // no PTS in this packet
    return ((int64_t)(p[9] & 0x0E) << 29) | ((int64_t)p[10] << 22) |
           ((int64_t)(p[11] & 0xFE) << 14) | ((int64_t)p[12] << 7) |
           (p[13] >> 1);
}

// Audio timestamps. A PES PTS belongs to the first AC3 frame that starts in
// that packet (at the "first access unit" offset), so positions are tracked
// as byte offsets into the AC3 stream fed to ac3_push().
static int64_t aq_end_pts = PTS_NONE;     // pts just after the last decoded audio
static __attribute__((unused)) int aq_fill = 0;  // decoded frames queued, not yet in device
static int64_t ac3_frame_pts = PTS_NONE;  // pts of the AC3 frame being decoded
static uint64_t ac3_in_pos = 0;           // AC3 bytes consumed so far
static uint64_t ac3_frame_pos = 0;        // stream position of current frame
#define AC3_PTSQ 16
static struct { uint64_t pos; int64_t pts; } ac3_ptsq[AC3_PTSQ];
static int ac3_ptsq_n = 0;

static void ac3_pts_register(uint64_t pos, int64_t pts) {
    if (ac3_ptsq_n == AC3_PTSQ) {
        memmove(ac3_ptsq, ac3_ptsq + 1, (AC3_PTSQ - 1) * sizeof(ac3_ptsq[0]));
        ac3_ptsq_n--;
    }
    ac3_ptsq[ac3_ptsq_n].pos = pos;
    ac3_ptsq[ac3_ptsq_n].pts = pts;
    ac3_ptsq_n++;
}

static int64_t ac3_pts_lookup(uint64_t frame_pos) {
    int64_t pts = PTS_NONE;
    int i = 0;
    while (i < ac3_ptsq_n && ac3_ptsq[i].pos <= frame_pos) {
        if (ac3_ptsq[i].pos == frame_pos)
            pts = ac3_ptsq[i].pts;
        i++;
    }
    memmove(ac3_ptsq, ac3_ptsq + i, (ac3_ptsq_n - i) * sizeof(ac3_ptsq[0]));
    ac3_ptsq_n -= i;
    return pts;
}

static void ac3_reset(void) {
    ac3_len = 0;
    ac3_need = 0;
    ac3_ptsq_n = 0;
    ac3_frame_pts = PTS_NONE;
}

#ifndef SYNC_OLD
// Decoded audio waiting for room in the sound card (1 s, stereo s16).
#define AQ_FRAMES 48000
static int16_t aq_buf[AQ_FRAMES * 2];
static int aq_rd = 0;

static void aq_push(const int16_t *pcm, int frames) {
    if (frames > AQ_FRAMES - aq_fill) {           // should not happen: reader checks space
        int drop = frames - (AQ_FRAMES - aq_fill);
        aq_rd = (aq_rd + drop) % AQ_FRAMES;
        aq_fill -= drop;
    }
    int wr = (aq_rd + aq_fill) % AQ_FRAMES;
    while (frames > 0) {
        int n = frames;
        if (n > AQ_FRAMES - wr) n = AQ_FRAMES - wr;
        memcpy(aq_buf + wr * 2, pcm, n * 2 * sizeof(int16_t));
        pcm += n * 2;
        frames -= n;
        aq_fill += n;
        wr = (wr + n) % AQ_FRAMES;
    }
}
#endif

static inline int16_t sample_to_s16(sample_t s) {
#if defined(LIBA52_FIXED)
    return s >> 15;
#else
    if (s > 1.0) s = 1.0;
    if (s < -1.0) s = -1.0;
    return (int16_t)(s * 32767.0);
#endif
}

static void ac3_push(uint8_t *data, int len) {
    while (len > 0) {
        // Sync search
        if (ac3_len < 2) {
            ac3_buf[ac3_len++] = *data++;
            len--;
            ac3_in_pos++;
            continue;
        }
        if (!(ac3_buf[0] == 0x0B && ac3_buf[1] == 0x77)) {
            ac3_buf[0] = ac3_buf[1];
            ac3_len = 1;
            continue;
        }
        // Accumulate header
        while (ac3_len < 7 && len > 0) {
            ac3_buf[ac3_len++] = *data++;
            len--;
            ac3_in_pos++;
        }
        if (ac3_len < 7) { return;}
        // Determine frame size
        if (ac3_need == 0) {
            int flags, rate, br;
            int sz = a52_syncinfo(ac3_buf, &flags, &rate, &br);
            if (sz <= 0 || sz > (int)sizeof(ac3_buf)) {
                // false sync — restart
                ac3_len = 0;
                ac3_need = 0;
                continue;
            }
            ac3_need = sz;
            ac3_frame_pos = ac3_in_pos - ac3_len;   // where this frame started
        }
        // Copy remaining data
        int copy = ac3_need - ac3_len;
        if (copy > len) { copy = len;}
        memcpy(ac3_buf + ac3_len, data, copy);
        ac3_len += copy;
        data += copy;
        len -= copy;
        ac3_in_pos += copy;
        // Full frame ready
        if (ac3_len == ac3_need) {
			int flags, rate, bitrate;
			int ret = a52_syncinfo(ac3_buf, &flags, &rate, &bitrate);
			if (ret < 0) {
				fprintf(stderr, "AC3 frame: a52_syncinfo failed\n");
				ac3_len = 0;
				ac3_need = 0;
				continue;
			}
            ac3_frame_pts = ac3_pts_lookup(ac3_frame_pos);
            audio_feed(ac3_buf, ac3_need, NULL);
         //   fprintf(stderr, "AC3 sync: frame_len=%d rate=%d bitrate=%d\n", ac3_need, rate, bitrate);
            ac3_len = 0;
            ac3_need = 0;
        }
    }
}

// ------------------ audio ------------------
// Output goes through audio/audio.c. Order: tinyalsa first, OSS fallback.
static int audio_open(int rate, int channels) {
#ifdef AUDIO_HAVE_SIM
    return audio_init_backend(AUDIO_BACKEND_SIM, rate, channels);
#endif
    if (audio_init_backend(AUDIO_BACKEND_TINYALSA, rate, channels) == 0)
        return 0;
    fprintf(stderr, "Audio: falling back to OSS\n");
    if (audio_init_backend(AUDIO_BACKEND_OSS, rate, channels) == 0) {
        audio_active = 1;
        return 0;
    }
    fprintf(stderr, "Warning: no audio device; continuing without audio.\n");
    return -1;
}

// ------------------ X11 ------------------ 

static void x11_init(int w, int h) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) exit(1);
    int s = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy,s),
                              0,0,w,h,0,0,0);
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | StructureNotifyMask);                          
    //Set X11 window title and icon name
	//char title[256]; snprintf(title, sizeof(title), "minidvd 2026 - %s", filename);
	XStoreName(dpy, win, "minidvd 2026");
	XSetIconName(dpy, win, "minidvd 2026");
   
    XMapWindow(dpy, win);
    gc = XCreateGC(dpy, win, 0, NULL);
    
    XSetForeground(dpy, gc, WhitePixel(dpy, DefaultScreen(dpy)));
	XSetBackground(dpy, gc, BlackPixel(dpy, DefaultScreen(dpy)));

	XFontStruct *font = XLoadQueryFont(dpy, "fixed");
	if (font) XSetFont(dpy, gc, font->fid);

    // MIT-SHM: the X server reads frames straight from shared memory instead
    // of receiving them through the socket. Off for remote displays (it
    // fails to attach) or with MINIDVD_NOSHM=1.
    use_shm = XShmQueryExtension(dpy) && !getenv("MINIDVD_NOSHM") ? -1 : 0;
    {
        int ev, er;
        if (!getenv("MINIDVD_NORENDER") && XRenderQueryExtension(dpy, &ev, &er) &&
            (render_fmt = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, s))) != NULL) {
            int maj = 0, min = 0;
            XRenderQueryVersion(dpy, &maj, &min);
            render_pad = maj > 0 || min >= 10;
            render_dst = XRenderCreatePicture(dpy, win, render_fmt, 0, NULL);
            use_render = 1;
            fprintf(stderr, "Video: XRender scaling\n");
        } else {
            fprintf(stderr, "Video: CPU scaling\n");
        }
    }
    recreate_ximage(w, h);
}

// server-side pixmap holding the current frame at video size
static void render_ensure_pixmap(int w, int h) {
    XRenderPictureAttributes pa;
    if (render_pix && render_pix_w == w && render_pix_h == h) return;
    if (render_src) XRenderFreePicture(dpy, render_src);
    if (render_pix) XFreePixmap(dpy, render_pix);
    render_pix = XCreatePixmap(dpy, win, w, h, DefaultDepth(dpy, DefaultScreen(dpy)));
    // bilinear edges: repeat border pixels (older servers: no padding,
    // the outermost pixels just blend towards black)
    pa.repeat = render_pad ? RepeatPad : RepeatNone;
    render_src = XRenderCreatePicture(dpy, render_pix, render_fmt, CPRepeat, &pa);
    render_pix_w = w;
    render_pix_h = h;
    render_tr_w = render_tr_h = -1;       // force a new transform
}

static void render_set_scale(int dw, int dh) {
    XTransform t;
    if (dw == render_tr_w && dh == render_tr_h) return;
    memset(&t, 0, sizeof(t));
    // maps destination pixels to source pixels
    t.matrix[0][0] = XDoubleToFixed((double)render_pix_w / dw);
    t.matrix[1][1] = XDoubleToFixed((double)render_pix_h / dh);
    t.matrix[2][2] = XDoubleToFixed(1.0);
    XRenderSetPictureTransform(dpy, render_src, &t);
    if (dw == render_pix_w && dh == render_pix_h)
        XRenderSetPictureFilter(dpy, render_src, FilterNearest, NULL, 0);  // 1:1 copy
    else
        XRenderSetPictureFilter(dpy, render_src, FilterBilinear, NULL, 0);
    render_tr_w = dw;
    render_tr_h = dh;
}

static void render_cleanup(void) {
    if (render_src) XRenderFreePicture(dpy, render_src);
    if (render_pix) XFreePixmap(dpy, render_pix);
    if (render_dst) XRenderFreePicture(dpy, render_dst);
    render_src = render_dst = None;
    render_pix = None;
}

static int shm_attach_error(Display *d, XErrorEvent *e) {
    (void)d; (void)e;
    shm_failed = 1;
    return 0;
}

static void destroy_ximage(void) {
    if (!ximg) return;
    if (shminfo.shmaddr) {
        XSync(dpy, False);                // server finished with the segment
        XShmDetach(dpy, &shminfo);
        XSync(dpy, False);
        shmdt(shminfo.shmaddr);
        shminfo.shmaddr = NULL;
        ximg->data = NULL;                // not malloc'ed: keep XDestroyImage off it
    }
    XDestroyImage(ximg);                  // frees malloc'ed data
    ximg = NULL;
    shm_put_pending = 0;
}

static XImage *create_shm_image(int w, int h) {
    int s = DefaultScreen(dpy);
    XErrorHandler old;
    XImage *img = XShmCreateImage(dpy, DefaultVisual(dpy, s), DefaultDepth(dpy, s),
                                  ZPixmap, NULL, &shminfo, w, h);
    if (!img) return NULL;
    shminfo.shmid = shmget(IPC_PRIVATE, (size_t)img->bytes_per_line * img->height,
                           IPC_CREAT | 0600);
    if (shminfo.shmid < 0) { XDestroyImage(img); return NULL; }
    shminfo.shmaddr = img->data = shmat(shminfo.shmid, NULL, 0);
    if (shminfo.shmaddr == (char *)-1) {
        shmctl(shminfo.shmid, IPC_RMID, NULL);
        shminfo.shmaddr = img->data = NULL;
        XDestroyImage(img);
        return NULL;
    }
    shminfo.readOnly = False;
    shm_failed = 0;
    old = XSetErrorHandler(shm_attach_error);
    XShmAttach(dpy, &shminfo);
    XSync(dpy, False);
    XSetErrorHandler(old);
    shmctl(shminfo.shmid, IPC_RMID, NULL);   // removed once both sides detach
    if (shm_failed) {
        shmdt(shminfo.shmaddr);
        shminfo.shmaddr = img->data = NULL;
        XDestroyImage(img);
        return NULL;
    }
    return img;
}

static void recreate_ximage(int w, int h) {
    int s = DefaultScreen(dpy);
    if (use_render) {                     // image = video frame, X scales it
        w = video_w;
        h = video_h;
        render_ensure_pixmap(w, h);
        if (ximg && ximg->width == w && ximg->height == h) return;
    }
    destroy_ximage();
    if (use_shm) {
        ximg = create_shm_image(w, h);
        if (ximg) {
            if (use_shm < 0) fprintf(stderr, "Video: MIT-SHM\n");
            use_shm = 1;
            return;
        }
        fprintf(stderr, "Video: MIT-SHM not available, using XPutImage\n");
        use_shm = 0;
    }
    ximg = XCreateImage(dpy, DefaultVisual(dpy, s), DefaultDepth(dpy, s), ZPixmap, 0,
		malloc(w * h * 4), w, h, 32, 0);
}

#define MAX_SPU_SIZE  65536   // spec max, practical upper bound

typedef struct {
    int active;
    int expected;
    int collected;
    int64_t pts;
    uint8_t buf[MAX_SPU_SIZE];
} spu_assembler_t;

static spu_assembler_t spu[0x20];   // one per substream (0x20–0x3F)

// helpers
static void audio_feed(uint8_t *data, int len, void *opaque) {
	if (!audio_enabled || audio_type != AUDIO_AC3) { return;}
	audio_bytes += len;
	if (ac3_frame_pts != PTS_NONE) { aq_end_pts = ac3_frame_pts;}
    int flags = A52_STEREO;
    sample_t level = 1.0;
    sample_t bias = 0.0;
    
    if (a52_frame(a52, data, &flags, &level, bias) < 0) { return;}
    for (int i = 0; i < 6; i++) {
        if (a52_block(a52) < 0) { break;}
        sample_t *s = a52_samples(a52);
        int16_t pcm[512];
        sample_t vol = audio_muted ? 0.0f : (sample_t)audio_volume_internal * 0.01f;
        for (int j = 0; j < 256; j++) {
			sample_t l   = s[j + 0*256]; // Left
			sample_t r   = s[j + 1*256]; // Right
			sample_t c   = s[j + 2*256]; // Center
			sample_t lfe = s[j + 3*256]; // LFE
			sample_t sl  = s[j + 4*256]; // Surround L
			sample_t sr  = s[j + 5*256]; // Surround R
			// simple stereo downmix
#if defined(LIBA52_FIXED)			
			left  = l + (c >> 1) + (sl >> 1) + (lfe >> 2);
			right  = l + (c >> 1) + (sr >> 1) + (lfe >> 2);
#else
			sample_t left  = (l + c/2 + sl/2 + lfe/4) * 0.5;
			sample_t right = (r + c/2 + sr/2 + lfe/4) * 0.5;
#endif
			left  *= vol;
			right *= vol;
			pcm[j*2+0] = sample_to_s16(left);
			pcm[j*2+1] = sample_to_s16(right);

        }
        int frames = 256;  // a52 always outputs 256 PCM frames per block
#ifdef SYNC_OLD
        audio_write(pcm, frames);
#else
        aq_push(pcm, frames);
#endif
        if (aq_end_pts != PTS_NONE) { aq_end_pts += (int64_t)frames * PTS_HZ / audio_rate;}
    }
}

#ifdef SYNC_OLD
static void video_feed(uint8_t *data, int len, int64_t pts) {
	(void)pts;
	if (video_es_len + len > VIDEO_ES_BUF_SIZE) {
		fprintf(stderr, "ES buffer overflow — dropping data\n");
        video_es_len = 0;
    }
    memcpy(video_es_buf + video_es_len, data, len);
    video_es_len += len;
}
#else
// Compressed video waiting for the decoder: one slot per PES payload, with
// its PTS. Lets the reader run ahead of the display without decoding early.
#define VQ_SLOTS 512                      // 1 MB, roughly 1-2 s of DVD video
static uint8_t  vq_data[VQ_SLOTS][2048];
static uint16_t vq_len[VQ_SLOTS];
static int64_t  vq_pts[VQ_SLOTS];
static int vq_tail = 0, vq_count = 0;
static int64_t vq_last_pts = PTS_NONE;   // newest video pts read from the disc

static void video_feed(uint8_t *data, int len, int64_t pts) {
    if (len <= 0) return;
    if (len > 2048) len = 2048;
    if (vq_count == VQ_SLOTS) {
        fprintf(stderr, "video queue overflow — dropping data\n");
        return;
    }
    int slot = (vq_tail + vq_count) % VQ_SLOTS;
    memcpy(vq_data[slot], data, len);
    vq_len[slot] = (uint16_t)len;
    vq_pts[slot] = pts;
    vq_count++;
    if (pts != PTS_NONE) vq_last_pts = pts;
}

// Subtitle waiting for its PTS
static uint8_t spu_pending[MAX_SPU_SIZE];
static int spu_pending_len = 0;
static int64_t spu_pending_pts = PTS_NONE;

static void spu_show_pending(void) {
    struct spu_state st;
    memset(&st, 0, sizeof(st));
    spu_demux_control(spu_pending, spu_pending_len, &st);
    spu_pending_len = 0;
}

static void spu_queue(const uint8_t *data, int len, int64_t pts) {
    if (spu_pending_len)
        spu_show_pending();               // an older one is still waiting: show it now
    memcpy(spu_pending, data, len);
    spu_pending_len = len;
    spu_pending_pts = pts;
}
#endif

static void demux_ps(uint8_t *buf, int len) {
    int i = 0;
    while (i + 6 <= len) {  // minimal PES header
        if (buf[i] == 0x00 && buf[i+1] == 0x00 && buf[i+2] == 0x01) {
            uint8_t stream_id = buf[i+3];
            // Pack header
            if (stream_id == 0xBA) {
                if (i + 14 <= len) i += 14;
                else break;
                continue;
            }
            // System header
            if (stream_id == 0xBB) {
                if (i + 6 > len) break;
                int l = (buf[i+4] << 8) | buf[i+5];
                if (i + 6 + l > len) break;
                i += 6 + l;
                continue;
            }
            // PES length
            if (i + 6 > len) break;
            int pes_len = (buf[i+4] << 8) | buf[i+5];
            // Make sure we have the PES header flags
            if (i + 9 > len) break;
            int pes_header_data_len = buf[i+8];
            int payload_start = i + 9 + pes_header_data_len;
            if (payload_start > len) break;

            int payload_len = (pes_len > 0) ? pes_len - 3 - pes_header_data_len : len - payload_start;
            if (payload_len < 0) payload_len = 0;
            if (payload_start + payload_len > len) payload_len = len - payload_start;
            int64_t pes_pts = pes_get_pts(buf + i, len - i);

            if (stream_id >= 0xE0 && stream_id <= 0xEF) {
				if (payload_len > 0) {
                     video_feed(buf + payload_start, payload_len, pes_pts);
                }
            } else if (stream_id == 0xBD) {
                // Private stream 1 (audio/subtitle)
                if (payload_len < 1) goto next_packet;
                uint8_t sid = buf[payload_start];
                // Audio detection
                if (!audio_checked) {
                    audio_checked = 1;
                    if ((sid & 0xF8) == 0x80) {
                        audio_type = AUDIO_AC3;
                        audio_enabled = 1;
                        fprintf(stderr, "Audio: AC3 detected using (sid=0x%02x)\n", audio_sid_selected);
                    } else {
                        audio_type = AUDIO_NONE;
                        audio_enabled = 0;
                        fprintf(stderr, "Audio: unsupported format (sid=0x%02x)\n", sid);
                        audio_checked = 0;  // retry next packet
                    }
                }
                // Feed AC3 audio
                if (audio_enabled && audio_type == AUDIO_AC3 && sid == audio_sid_selected) {
                    if (payload_len > 4) {
                        // first_access_unit_pointer: 1 = frame starts right after the 4-byte header
                        int fap = (buf[payload_start + 2] << 8) | buf[payload_start + 3];
                        if (pes_pts != PTS_NONE && fap > 0)
                            ac3_pts_register(ac3_in_pos + fap - 1, pes_pts);
                        ac3_push(buf + payload_start + 4, payload_len - 4);
                    }
                } 
                // Subtitle: substream 0x20–0x3F
                else if (sid >= 0x20 && sid <= 0x3F && subtitle_enabled) {
                    if (payload_len > 1) {
						spu_assembler_t *s = &spu[sid - 0x20];

						int off = payload_start + 1;
						int len = payload_len - 1;
						// Start of a new SPU packet
						if (!s->active) {
							if (len < 2)
								goto next_packet;

							s->expected = (buf[off] << 8) | buf[off + 1];
							if (s->expected <= 0 || s->expected > MAX_SPU_SIZE)
								goto next_packet;

							s->collected = len - 2;
							s->buf[0] = (s->expected >> 8) & 0xFF;
							s->buf[1] = s->expected & 0xFF;
							memcpy(s->buf + 2, buf + off + 2, s->collected);
							s->collected += 2;
							s->active = 1;
							s->pts = pes_pts;
						}
						// Continuation of SPU packet
						else {
							int copy = s->expected - s->collected;
							if (copy > len)
								copy = len;
								memcpy(s->buf + s->collected, buf + off, copy);
								s->collected += copy;
						}
						// Completed SPU packet
						if (s->active && s->collected >= s->expected) {
							if (current_sub_sid >= 0 && sid == current_sub_sid) {
#ifdef SYNC_OLD
								struct spu_state st;
								memset(&st, 0, sizeof(st));
								spu_demux_control(s->buf, s->expected, &st);
#else
								spu_queue(s->buf, s->expected, s->pts);
#endif
							}
							s->active = 0;
						}	
                    }	//end payload_len > 1
                }	//end sid >= 0x20 && sid <= 0x3F
            } // end stream_id == 0xBD

        next_packet:
            // Move to next PES packet
            if (pes_len > 0)
                i += 6 + pes_len;
            else
                i = payload_start + payload_len;  // unbounded PES, skip until next start code
        } else {
            i++;
        }
    }
}

// ------------------ DVD ------------------

static void redraw(void) {
    if (!have_frame || !ximg) {return;}
    
    // decide destination size
    dst_w = scale_video ? win_w : video_w;
    dst_h = scale_video ? win_h : video_h;
    // center
    int dx = (win_w - dst_w) / 2;
    int dy = (win_h - dst_h) / 2;
    const uint8_t *src = info->display_fbuf->buf[0];
    if (shm_put_pending) {                // previous frame still being read?
        int64_t w0 = now_us();
        XSync(dpy, False);
        shm_put_pending = 0;
        dbg.wait_ms += (now_us() - w0) / 1000.0;
    }
    if (use_render) {
        // video-sized frame + subtitles -> pixmap; X scales it to the window
        memcpy(ximg->data, src, video_w * video_h * 4);
        uint64_t now_subtitle = current_time_ms();
        if (sub_valid && (now_subtitle - last_sub_time_ms > sub_display_duration_ms))
            sub_valid = 0;
        if (subtitle_enabled && subtitle_active && sub_valid)
            overlay_subtitle((uint8_t *)ximg->data, video_w, video_h);
        if (shminfo.shmaddr) {
            XShmPutImage(dpy, render_pix, gc, ximg, 0, 0, 0, 0, video_w, video_h, False);
            shm_put_pending = 1;
        } else {
            XPutImage(dpy, render_pix, gc, ximg, 0, 0, 0, 0, video_w, video_h);
        }
        render_set_scale(dst_w, dst_h);
        XRenderComposite(dpy, PictOpSrc, render_src, None, render_dst,
                         0, 0, 0, 0, dx, dy, dst_w, dst_h);
        if (show_help) draw_help_overlay();
        if (dbg.xsync) XSync(dpy, False);
        else XFlush(dpy);
        return;
    }
    // scale or copy
    if (!scale_video) {
        memcpy(ximg->data, src, video_w * video_h * 4);
    } else {
        scale_bilinear_rgba(
            (uint8_t *)ximg->data, dst_w, dst_h,
            src, video_w, video_h
        );
    }
    // subtitle timing
    uint64_t now_subtitle = current_time_ms();
    if (sub_valid && (now_subtitle - last_sub_time_ms > sub_display_duration_ms)) {
        sub_valid = 0;
    }
    // overlay subtitles in DESTINATION space
    if (subtitle_enabled && subtitle_active && sub_valid) {
        overlay_subtitle(ximg->data, dst_w, dst_h);
    }

    if (shminfo.shmaddr) {
        XShmPutImage(dpy, win, gc, ximg, 0, 0, dx, dy, dst_w, dst_h, False);
        shm_put_pending = 1;
    } else {
        XPutImage(dpy, win, gc, ximg, 0, 0, dx, dy, dst_w, dst_h);
    }
    if (show_help) draw_help_overlay();
    if (dbg.xsync) XSync(dpy, False);
    else XFlush(dpy);
}

static uint8_t buf[2048];

#ifdef TEST_PLAIN_VOB
static int vob_fd = -1;
static int read_block(uint32_t lba, uint8_t *b) {
    return pread(vob_fd, b, 2048, (off_t)lba * 2048) == 2048 ? 1 : 0;
}
#else
static int read_block(uint32_t lba, uint8_t *b) {
    return DVDReadBlocks(vob, lba, 1, b);
}
#endif

// Seek or audio stream change: drop queued/decoding audio
static void audio_stream_reset(void) {
    audio_flush();
    ac3_reset();
    aq_end_pts = PTS_NONE;
#ifndef SYNC_OLD
    aq_rd = aq_fill = 0;
#endif
}

#ifdef AUDIO_HAVE_SIM
// test log: frame pts vs. the pts of the audio actually being heard
static void av_test_log(int64_t pts, int dropped) {
    int64_t heard = PTS_NONE;
    if (aq_end_pts != PTS_NONE && audio_get_backend() != AUDIO_BACKEND_NONE)
        heard = aq_end_pts - (int64_t)(aq_fill + audio_sim_true_delay()) * PTS_HZ / audio_rate;
    printf("AV %lld %lld %lld %d\n", (long long)now_us(), (long long)pts,
           heard == PTS_NONE ? -1LL : (long long)heard, dropped);
}
#endif

#ifdef SYNC_OLD
static void playback_step(void) {
	if (dvd_block >= total_blocks) { running = 0; return; }
	static struct timespec pacemaker_tid;
	static int pacemaker_initialiseret = 0;
	

	if (seek_pending) {
		seek_pending = 0;
		dvd_block = seek_target_block;
		audio_flush();	// Flush audio device buffer (optional but good)
		return; // refill pipeline cleanly
	}
	
    if (mpeg2_parse(mpeg2) == STATE_BUFFER) {	// Feed decoder if it needs data
        if (read_block(dvd_block++, buf) != 1) {
            return;
        }
		demux_ps(buf, 2048);
        if (video_es_len > 0) {
			mpeg2_buffer(mpeg2, video_es_buf, video_es_buf + video_es_len);
			video_es_len = 0;
		}
    }
    
	int state;
	while ((state = mpeg2_parse(mpeg2)) != STATE_BUFFER) { // Drain decoder until it blocks again
		if (state == STATE_SEQUENCE) {
			int new_w = info->sequence->width;
			int new_h = info->sequence->height;
			if (new_w != video_w || new_h != video_h) {
				video_w = new_w;
				video_h = new_h;
				recreate_ximage(video_w, video_h);
				XResizeWindow(dpy, win, video_w, video_h);
			}
		}
		
		// libmpeg2 returns STATE_SLICE/STATE_END once per finished picture.
		// Don't compare display_fbuf with the previous one: every B-frame is
		// decoded into the same buffer, so that would skip every second B-frame.
		if ((state == STATE_SLICE || state == STATE_END) &&	info->display_fbuf &&
			info->display_picture) {
			
			have_frame = 1;

			// center video in window
			video_x = (win_w - video_w) / 2;
			video_y = (win_h - video_h) / 2;
			// --- AUDIO-AWARE PACEMAKER START ---
struct timespec nu_tid;
clock_gettime(CLOCK_MONOTONIC, &nu_tid);

if (pacemaker_initialiseret) {
    long elapsed_us = (nu_tid.tv_sec - pacemaker_tid.tv_sec) * 1000000 +
                   (nu_tid.tv_nsec - pacemaker_tid.tv_nsec) / 1000;

    // Hvis der er gået lang tid (f.eks. over 60ms), betyder det, at tråden
    // har været låst af lydkortet. Vi skal IKKE bremse dette billede,
    // da det skal matche den lyd, der lige er blevet spillet!
    if (elapsed_us > 60000) {
        // Lad billedet komme ud med det samme for at bevare lipsync
    } 
    // Hvis tråden kører i høj fart (under 38ms pr. billede), bremser vi den en lille smule,
    // så bevægelserne bliver mere flydende, men uden at vi over-bremser.
    else if (elapsed_us < 38000) {
        long sleep_us = 38000 - elapsed_us;
        if (sleep_us > 25000) sleep_us = 25000; // Sikkerhedsbarriere
        usleep(sleep_us);
        clock_gettime(CLOCK_MONOTONIC, &nu_tid); // Opdater tiden efter søvn
    }
} else {
    pacemaker_initialiseret = 1;
}
pacemaker_tid = nu_tid;
// --- AUDIO-AWARE PACEMAKER SLUT ---
			redraw();
#ifdef AUDIO_HAVE_SIM
			av_test_log(PTS_NONE, 0);
#endif
			
            if (!audio_active) {
				usleep(35000); // pace ONLY on video output if no audio  
            }    
		}
    }
}

#else /* new A/V timing */

// ---- master clock ----------------------------------------------------------
// Runs on the system clock (smooth) and is locked to the audio position.
// Many sound cards report their position only once per period (85 ms with
// 4096-frame periods), so between updates the reported position lags. The
// clock therefore follows the upper edge of the reports:
//  - a report ahead of the clock moves the clock forward to it
//  - once a second, if every report in that second was behind the clock,
//    the clock is pulled back by half of the smallest gap
//  - more than CLOCK_SLACK ahead of the audio (underrun, pause): fall back
#define CLOCK_SLACK (PTS_HZ / 10)        // 100 ms
#define CLOCK_JUMP  (PTS_HZ / 5)         // 200 ms: beyond this, re-anchor
static int64_t clk_pts = PTS_NONE;
static int64_t clk_us = 0;
static int64_t audio_ignore_until_us = 0;
static int64_t clk_win_max = PTS_NONE;   // max (report - clock) this window
static int64_t clk_win_us = 0;
static int64_t av_offset = 0;            // MINIDVD_AV_OFFSET (ms) in 90 kHz units
static int64_t draw_early = 0;           // start drawing this much early (90 kHz)
static int64_t clk_audio_us = 0;         // last time an audio report was used

// is the clock currently following the audio?
static int clock_on_audio(void) {
    return clk_audio_us && now_us() - clk_audio_us < 500000;
}

static int64_t clock_now(void) {
    if (clk_pts == PTS_NONE) return PTS_NONE;
    return clk_pts + (now_us() - clk_us) * 9 / 100;
}

static void clock_set(int64_t pts) {
    clk_pts = pts;
    clk_us = now_us();
}

static void clock_window_reset(void) {
    clk_win_max = PTS_NONE;
    clk_win_us = now_us();
}

// pts of the audio coming out of the speaker right now, if known
static int audio_clock_sample(int64_t *out) {
    if (audio_get_backend() == AUDIO_BACKEND_NONE || aq_end_pts == PTS_NONE)
        return 0;
    int d = audio_get_delay();
    if (d < 0) return 0;
    if (d == 0 && aq_fill == 0) {           // starved: no audio to follow
        if (aq_end_pts != PTS_NONE) dbg.starved++;
        return 0;
    }
    *out = aq_end_pts - (int64_t)(aq_fill + d) * PTS_HZ / audio_rate;
    return 1;
}

static void clock_update(void) {
    int64_t s, est;
    if (!audio_clock_sample(&s)) return;
    est = clock_now();
    if (est == PTS_NONE) { clock_set(s); clock_window_reset(); clk_audio_us = now_us(); return; }
    if (s - est > CLOCK_JUMP || est - s > CLOCK_JUMP) {
        if (now_us() < audio_ignore_until_us) return;   // video just re-anchored
        dbg.clk_jump++; dbg.clk_adj += (s - est) / 90.0;
        clock_set(s);
        clock_window_reset();
        clk_audio_us = now_us();
        return;
    }
    audio_ignore_until_us = 0;
    clk_audio_us = now_us();
    {
        int d = audio_get_delay();
        if (d < dbg.adly_min) dbg.adly_min = d;
        if (d > dbg.adly_max) dbg.adly_max = d;
    }
    if (s > est) {
        double ms = (s - est) / 90.0;
        if (ms > 2.0) {
            dbg.clk_fwd++; dbg.clk_fwd_sum += ms;
            if (ms > dbg.clk_fwd_max) dbg.clk_fwd_max = ms;
        }
        dbg.clk_adj += ms;
        clock_set(s);                       // audio is further than we thought
        est = s;
    } else if (est - s > CLOCK_SLACK) {
        dbg.clk_stall++; dbg.clk_stall_sum += (est - s - CLOCK_SLACK) / 90.0;
        dbg.clk_adj -= (est - s - CLOCK_SLACK) / 90.0;
        clock_set(s + CLOCK_SLACK);         // audio stalled (underrun, pause)
        clock_window_reset();
        return;
    }
    if (clk_win_max == PTS_NONE || s - est > clk_win_max)
        clk_win_max = s - est;
    if (now_us() - clk_win_us >= 1000000) {
        if (clk_win_max != PTS_NONE && clk_win_max < -(PTS_HZ / 500)) { // > 2 ms ahead
            clk_pts += clk_win_max / 2;
            dbg.clk_back++; dbg.clk_back_sum += -clk_win_max / 180.0;
            dbg.clk_adj += clk_win_max / 180.0;
        }
        clock_window_reset();
    }
}

// ---- audio output: never blocks ---------------------------------------------
static void audio_pump(void) {
    if (audio_get_backend() == AUDIO_BACKEND_NONE) {
        if (audio_ready)                    // opening failed: nothing to play it on
            aq_rd = aq_fill = 0;
        return;                             // else: device not opened yet, keep it
    }
    int budget = audio_get_space();
    if (budget < 0) budget = 1024;          // unknown: small (possibly blocking) writes
    while (budget > 0 && aq_fill > 0) {
        int n = aq_fill;
        if (n > budget) n = budget;
        if (n > AQ_FRAMES - aq_rd) n = AQ_FRAMES - aq_rd;
        if (audio_write(aq_buf + aq_rd * 2, n) < 0) break;
        aq_rd = (aq_rd + n) % AQ_FRAMES;
        aq_fill -= n;
        budget -= n;
    }
}

// ---- video -----------------------------------------------------------------
static int frame_ready = 0;              // a decoded frame is waiting to be shown
static int64_t frame_pts = PTS_NONE;
static int64_t frame_dur = PTS_HZ / 25;
static int64_t vid_next_pts = PTS_NONE;  // predicted pts of the next frame
static int frame_drops = 0;
static int frame_discont = 0;            // pts jumped vs. the previous frame
static int vq_feeding = 0;               // decoder is reading vq slot vq_tail
static int64_t clock_wait_since_us = 0;
static int64_t last_step_us = 0;

static int need_more_data(void) {
    if (dvd_block >= total_blocks) return 0;
    if (vq_count > VQ_SLOTS - 8) return 0;
    if (AQ_FRAMES - aq_fill < 8 * 1536) return 0;
    // don't read far ahead once there is audio queued and video waiting
    if (aq_fill > audio_rate / 2 && (frame_ready || vq_count > 64)) return 0;
    // keep the video read-ahead to ~0.7 s, unless we are still short of audio
    {
        int64_t now = clock_now();
        if (now != PTS_NONE && vq_last_pts != PTS_NONE &&
            vq_last_pts - now > 7 * PTS_HZ / 10 && (frame_ready || vq_count > 16) &&
            (!audio_active || aq_fill > audio_rate / 4))
            return 0;
    }
    return 1;
}

// Decode until the next picture is ready for display (kept in libmpeg2's
// display buffer until shown). Returns 0 if more data is needed.
static int decode_next_frame(void) {
    for (;;) {
        int state = mpeg2_parse(mpeg2);
        switch (state) {
        case STATE_BUFFER:
            if (vq_feeding) {                // previous slot fully consumed
                vq_tail = (vq_tail + 1) % VQ_SLOTS;
                vq_count--;
                vq_feeding = 0;
            }
            if (vq_count == 0) return 0;
            if (vq_pts[vq_tail] != PTS_NONE)
                mpeg2_tag_picture(mpeg2, (uint32_t)vq_pts[vq_tail],
                                  (uint32_t)(vq_pts[vq_tail] >> 32));
            mpeg2_buffer(mpeg2, vq_data[vq_tail], vq_data[vq_tail] + vq_len[vq_tail]);
            vq_feeding = 1;
            break;
        case STATE_SEQUENCE: {
            int new_w = info->sequence->width;
            int new_h = info->sequence->height;
            if (new_w != video_w || new_h != video_h) {
                video_w = new_w;
                video_h = new_h;
                recreate_ximage(video_w, video_h);
                XResizeWindow(dpy, win, video_w, video_h);
            }
            break;
        }
        case STATE_SLICE:
        case STATE_END:
            // one finished picture per STATE_SLICE/STATE_END; B-frames all
            // share one buffer, so display_fbuf must not be used to detect
            // "new picture" (that skipped every second B-frame)
            if (info->display_fbuf && info->display_picture) {
                const mpeg2_picture_t *pic = info->display_picture;
                if (info->sequence && info->sequence->frame_period)
                    frame_dur = info->sequence->frame_period / 300;   // 27 MHz -> 90 kHz
                if (pic->flags & PIC_FLAG_TAGS) {
                    frame_pts = ((int64_t)pic->tag2 << 32) | pic->tag;
                    if (vid_next_pts != PTS_NONE &&
                        (frame_pts - vid_next_pts > PTS_HZ || vid_next_pts - frame_pts > PTS_HZ))
                        frame_discont = 1;                            // e.g. cell change
                } else {
                    frame_pts = vid_next_pts;                         // extrapolate
                }
                if (frame_pts != PTS_NONE)
                    vid_next_pts = frame_pts + frame_dur * pic->nb_fields / 2;
                frame_ready = 1;
                return 1;
            }
            break;
        default:
            break;
        }
    }
}

static void show_frame(int64_t late) {
    have_frame = 1;
    video_x = (win_w - video_w) / 2;
    video_y = (win_h - video_h) / 2;
    {
        static int64_t prev_end = 0;
        int64_t t0 = now_us(), t1;
        double draw, iv;
        redraw();
        t1 = now_us();
        draw = (t1 - t0) / 1000.0;
        // learn how long drawing takes, so the next frame can be started
        // that much early and appear on time (at most 3/4 of a frame)
        {
            int64_t d90 = (t1 - t0) * 9 / 100;
            draw_early += (d90 - draw_early) / 8;
            if (draw_early > frame_dur * 3 / 4) draw_early = frame_dur * 3 / 4;
            if (draw_early < 0) draw_early = 0;
        }
        iv = prev_end ? (t1 - prev_end) / 1000.0 : 40.0;
        prev_end = t1;
        if (dbg.level) {
            static const double edge[8] = { 30, 35, 38, 42, 45, 50, 70, 100 };
            int b = 0;
            while (b < 8 && iv >= edge[b]) b++;
            dbg.frames++;
            if (dbg.frames > 25) {          // skip start-up
                dbg.hist[b]++;
                dbg.iv_sum += iv; dbg.iv_sq += iv * iv;
            }
            dbg.late_sum += late / 90.0;
            if (late / 90.0 > dbg.late_max) dbg.late_max = late / 90.0;
            dbg.draw_sum += draw;
            dbg.wait_sum += dbg.wait_ms;
            if (dbg.wait_ms > dbg.wait_max) dbg.wait_max = dbg.wait_ms;
            dbg.wait_ms = 0;
            if (draw > dbg.draw_max) dbg.draw_max = draw;
            if (draw > 10.0) dbg.slow_draws++;
            if (dbg.level > 1)
                fprintf(stderr, "DBG iv=%5.1f late=%5.1f draw=%5.1f dec=%5.1f rdmax=%5.1f "
                        "adly=%5d aq=%5d clk=%+6.1f\n", iv, late / 90.0, draw,
                        dbg.dec_frame, dbg.rd_frame_max, audio_get_delay(), aq_fill, dbg.clk_adj);
            dbg.dec_frame = 0; dbg.rd_frame_max = 0; dbg.clk_adj = 0;
        }
    }
}

static void dbg_summary(void) {
    static const char *lbl[9] = { "<30", "30-35", "35-38", "38-42", "42-45",
                                  "45-50", "50-70", "70-100", ">100" };
    long n = 0;
    double mean = 0, sd = 0;
    if (!dbg.level || !dbg.frames) return;
    for (int i = 0; i < 9; i++) n += dbg.hist[i];
    if (n) {
        mean = dbg.iv_sum / n;
        sd = dbg.iv_sq / n - mean * mean;
        sd = sd > 0 ? __builtin_sqrt(sd) : 0;
    }
    fprintf(stderr, "\n==== minidvd timing summary ====\n");
    fprintf(stderr, "audio backend: %s   xsync: %s   shm: %s   scaling: %s\n",
            audio_get_backend() == AUDIO_BACKEND_TINYALSA ? "tinyalsa" :
            audio_get_backend() == AUDIO_BACKEND_OSS ? "OSS" :
            audio_get_backend() == AUDIO_BACKEND_NONE ? "none" : "simulated",
            dbg.xsync ? "on" : "off", shminfo.shmaddr ? "on" : "off",
            use_render ? "XRender" : "CPU");
    fprintf(stderr, "frames shown %ld, dropped %ld\n", dbg.frames, dbg.drops);
    fprintf(stderr, "interval after draw (ms): mean %.1f sd %.1f\n", mean, sd);
    fprintf(stderr, "  ");
    for (int i = 0; i < 9; i++) fprintf(stderr, "%s:%ld ", lbl[i], dbg.hist[i]);
    fprintf(stderr, "\n");
    fprintf(stderr, "late at show (ms): avg %.1f max %.1f\n",
            dbg.late_sum / dbg.frames, dbg.late_max);
    fprintf(stderr, "draw (ms): avg %.1f max %.1f, >10ms: %ld  (of which waiting for X: avg %.1f max %.1f)\n",
            dbg.draw_sum / dbg.frames, dbg.draw_max, dbg.slow_draws,
            dbg.wait_sum / dbg.frames, dbg.wait_max);
    fprintf(stderr, "decode per frame (ms): avg %.1f max %.1f\n",
            dbg.dec_sum / dbg.frames, dbg.dec_max);
    fprintf(stderr, "disc reads: %ld blocks, slowest %.1f ms, >20ms: %ld\n",
            dbg.blocks, dbg.rd_max, dbg.slow_reads);
    fprintf(stderr, "audio delay reported (frames): min %d max %d, starved %ld\n",
            dbg.adly_max < 0 ? -1 : dbg.adly_min, dbg.adly_max, dbg.starved);
    fprintf(stderr, "clock: forward >2ms %ld (avg %.1f, max %.1f ms), pulled back %ld (avg %.1f ms)\n",
            dbg.clk_fwd, dbg.clk_fwd ? dbg.clk_fwd_sum / dbg.clk_fwd : 0, dbg.clk_fwd_max,
            dbg.clk_back, dbg.clk_back ? dbg.clk_back_sum / dbg.clk_back : 0);
    fprintf(stderr, "clock: audio stalls %ld (%.0f ms total), re-anchored to audio %ld, pts jumps %ld\n",
            dbg.clk_stall, dbg.clk_stall_sum, dbg.clk_jump, dbg.clk_discont);
    fprintf(stderr, "================================\n");
}

// Full reset after a seek
static void av_reset(void) {
    audio_stream_reset();
    vq_tail = vq_count = 0;
    vq_last_pts = PTS_NONE;
    vq_feeding = 0;
    frame_discont = 0;
    mpeg2_reset(mpeg2, 0);
    frame_ready = 0;
    vid_next_pts = PTS_NONE;
    spu_pending_len = 0;
    for (int i = 0; i < 0x20; i++) spu[i].active = 0;
    clk_pts = PTS_NONE;
    clock_wait_since_us = 0;
}

// Sleep for (up to) 'us', waking a little early by the amount the system
// tends to oversleep (learned as we go). The last <= 4 ms are not slept at
// all: playback_step just loops, so frames go out on time even on kernels
// with coarse timers.
static void sleep_us(int64_t us) {
    static int64_t oversleep = 1000;     // estimate, us
    int64_t spin = oversleep < 4000 ? oversleep : 4000;
    if (us <= spin + 500) return;        // close to due: don't sleep
    us -= oversleep + 500;
    if (us < 1000) us = 1000;
    if (us > 10000) us = 10000;          // stay responsive (audio, X events)
    int64_t t0 = now_us();
    usleep((useconds_t)us);
    int64_t over = now_us() - t0 - us;
    if (over < 0) over = 0;
    oversleep += (over - oversleep) / 8;
}

static void playback_step(void) {
    int busy = 0;
    int64_t t = now_us();

    if (seek_pending) {
        seek_pending = 0;
        dvd_block = seek_target_block;
        av_reset();
        return;
    }
    // not called for a while (pause, help screen): re-anchor the clock
    if (last_step_us && t - last_step_us > 300000)
        clk_pts = PTS_NONE;
    last_step_us = t;

    audio_pump();
    clock_update();

    // 1. read ahead: as long as the queues want data, for up to 8 ms per
    //    pass, but stop 2 ms before the next frame is due. (A fixed number of
    //    blocks per pass could not keep up with high-bitrate scenes when
    //    drawing is slow and the loop only runs about once per frame.)
    int64_t read_until = t + 8000;
    if (frame_ready && frame_pts != PTS_NONE) {
        int64_t now = clock_now();
        if (now != PTS_NONE) {
            int64_t due = t + (frame_pts + av_offset - draw_early - now) * 100 / 9 - 2000;
            if (due < read_until) read_until = due;
        }
    }
    for (int n = 0; need_more_data() && (n == 0 || now_us() < read_until); n++) {
        int64_t r0 = now_us();
        int ok = read_block(dvd_block++, buf);
        if (dbg.level) {
            double ms = (now_us() - r0) / 1000.0;
            dbg.blocks++;
            if (ms > dbg.rd_max) dbg.rd_max = ms;
            if (ms > dbg.rd_frame_max) dbg.rd_frame_max = ms;
            if (ms > 20.0) dbg.slow_reads++;
        }
        if (ok != 1) continue;
        demux_ps(buf, 2048);
        busy = 1;
    }
    audio_pump();

    // 2. decode one frame ahead
    if (!frame_ready && dbg.level) {
        int64_t d0 = now_us();
        int got = decode_next_frame();
        double ms = (now_us() - d0) / 1000.0;
        dbg.dec_frame += ms;
        if (got) {
            dbg.dec_sum += dbg.dec_frame;
            if (dbg.dec_frame > dbg.dec_max) dbg.dec_max = dbg.dec_frame;
            busy = 1;
        }
    } else if (!frame_ready && decode_next_frame())
        busy = 1;

    // 3. subtitles
    if (spu_pending_len) {
        int64_t now = clock_now();
        if (spu_pending_pts == PTS_NONE || now == PTS_NONE || now >= spu_pending_pts ||
            spu_pending_pts - now > 5 * PTS_HZ) {
#ifdef AUDIO_HAVE_SIM
            printf("SPU %lld %lld\n", (long long)spu_pending_pts, (long long)(now == PTS_NONE ? -1 : now));
#endif
            spu_show_pending();
        }
    }

    // 4. show the frame when its time has come
    if (frame_ready) {
        int64_t now = clock_now();
        if (now == PTS_NONE) {
            // no clock yet: give the audio up to 1 s to start it, else run from video
            int audio_coming = audio_enabled && audio_get_backend() != AUDIO_BACKEND_NONE;
            if (!clock_wait_since_us) clock_wait_since_us = t;
            if (frame_pts == PTS_NONE || !audio_coming || t - clock_wait_since_us > 1000000) {
                clock_set(frame_pts == PTS_NONE ? 0 : frame_pts);
                now = clock_now();
            }
        }
        if (now != PTS_NONE) {
            int64_t diff;
            clock_wait_since_us = 0;
            if (frame_pts == PTS_NONE) frame_pts = now;
            diff = frame_pts + av_offset - draw_early - now;
            if (frame_discont || diff > 2 * PTS_HZ ||
                (diff < -PTS_HZ && !clock_on_audio())) {
                // timestamp jump (cell change etc.), or far off with nothing
                // else to follow: restart the clock from this frame and give
                // the audio 1.5 s to catch up before following it again
                clock_set(frame_pts + av_offset);
                if (frame_discont || diff > 2 * PTS_HZ)
                    audio_ignore_until_us = t + 1500000;
                dbg.clk_discont++;
                frame_discont = 0;
                diff = 0;
            }
            if (diff <= 0) {
                // late: drop to catch up, but show at least every 5th frame
                int drop = -diff > 2 * frame_dur && frame_drops < 4;
                if (drop) { frame_drops++; dbg.drops++; }
                else { frame_drops = 0; show_frame(-diff); }
#ifdef AUDIO_HAVE_SIM
                av_test_log(frame_pts, drop);
#endif
                frame_ready = 0;
                busy = 1;
            } else if (!busy) {
                sleep_us(diff * 100 / 9);
            }
        } else if (!busy) {
            sleep_us(5000);
        }
    } else if (!busy) {
        if (dvd_block >= total_blocks && vq_count == 0) {
            running = 0;                      // end of title
            return;
        }
        sleep_us(5000);
    }
}
#endif /* SYNC_OLD */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s /dev/dvd\n", argv[0]);
        return 1;
    }
#ifdef TEST_PLAIN_VOB
    vob_fd = open(argv[1], O_RDONLY);
    if (vob_fd < 0) { perror(argv[1]); return 1;}
    total_blocks = (uint32_t)(lseek(vob_fd, 0, SEEK_END) / 2048);
    current_sub_sid = sub_sid_list[0];
    audio_sid_selected = 0x80;
#else
    dvd_reader_t *dvd = DVDOpen(argv[1]);
    if (!dvd) {return 1;}
	//find largest VTS
    int best_vts = -1;
	int best_blocks = 0;

	ifo_handle_t *vmg = ifoOpen(dvd, 0);
	if (!vmg) return 1;

	for (int vts = 1; vts <= vmg->vmgi_mat->vmg_nr_of_title_sets; vts++) {
		dvd_file_t *f = DVDOpenFile(dvd, vts, DVD_READ_TITLE_VOBS);
		if (!f) continue;

		int blocks = DVDFileSize(f);
		DVDCloseFile(f);

		if (blocks > best_blocks) {
			best_blocks = blocks;
			best_vts = vts;
		}
	}

	if (best_vts < 0) return 1;
	// Subtitles OFF by default
	//current_sub_sid = -1;
	// Subtitles ON by default
	current_sub_sid = (sub_count > 0) ? sub_sid_list[0] : -1;
	
	// default behavior
	audio_sid_selected = 0x80;   // start at first legal audio stream
     
	ifo_handle_t *ifo = ifoOpen(dvd, best_vts);
    if (!ifo) { return 1;}

    vob = DVDOpenFile(dvd, best_vts, DVD_READ_TITLE_VOBS);
    if (!vob) { return 1;}
    total_blocks = DVDFileSize(vob);  // number of 2048-byte blocks
#endif
	fprintf(stderr, "VOB size: %d blocks\n", total_blocks);

    dvd_block = 0;
	mpeg2 = mpeg2_init();
#ifdef SYNC_OLD
	mpeg2_tag_picture(mpeg2, 0, 1);
#else
	{   // optional A/V fine tuning: + = show video later
		const char *dl = getenv("MINIDVD_DEBUG");
		const char *xs = getenv("MINIDVD_XSYNC");
		if (dl) dbg.level = atoi(dl) > 0 ? atoi(dl) : 1;
		if (xs) dbg.xsync = atoi(xs) > 0;
		const char *e = getenv("MINIDVD_AV_OFFSET");
		if (e) {
			av_offset = (int64_t)atoi(e) * 90;
			fprintf(stderr, "A/V offset: %d ms\n", atoi(e));
		}
	}
#endif
	info = mpeg2_info(mpeg2);
    mpeg2_convert(mpeg2, mpeg2convert_rgb32, NULL);
    
    a52 = a52_init(0);
	if (!a52) { fprintf(stderr, "a52_init failed\n"); exit(1); }

    x11_init(720, 576);
    print_help(stderr);
    
    for (int i = 0; i < MAX_SPU_STREAMS; i++) {
		spu_assemblers[i].len = 0;
		spu_assemblers[i].waiting_for_header = 1;
	}
	init_temp_clut();
    
    signal(SIGINT, on_sigint);
    
	while (running) {
		if (sigint_received) { running = 0; break; }
#ifdef TEST_PLAIN_VOB
		{   // test script: MINIDVD_TEST="sec:seek:frac,sec:audio,sec:pause:secs,sec:size:WxH,sec:help"
			static const char *ts = NULL;
			static int64_t t_start, unpause_at;
			int64_t el;
			if (!ts) { ts = getenv("MINIDVD_TEST"); if (!ts) ts = ""; t_start = now_us(); }
			el = now_us() - t_start;
			if (unpause_at && el >= unpause_at) { paused = 0; unpause_at = 0; fprintf(stderr, "TEST unpause\n"); }
			if (*ts && el >= (int64_t)(atof(ts) * 1e6)) {
				const char *a = strchr(ts, ':');
				a = a ? a + 1 : "";
				fprintf(stderr, "TEST %.40s\n", a);
				if (!strncmp(a, "seek:", 5)) request_seek(atof(a + 5));
				else if (!strncmp(a, "pause:", 6)) { paused = 1; unpause_at = el + (int64_t)(atof(a + 6) * 1e6); }
				else if (!strncmp(a, "help", 4)) { show_help = !show_help; need_redraw = 1; }
				else if (!strncmp(a, "size:", 5)) {   // size:WxH resizes the window
					int w = atoi(a + 5), h = strchr(a, 'x') ? atoi(strchr(a, 'x') + 1) : 0;
					if (w > 0 && h > 0) XResizeWindow(dpy, win, w, h);
				}
				else if (!strncmp(a, "audio", 5)) {
					audio_sid_selected = audio_sid_selected == 0x80 ? 0x81 : 0x80;
					audio_stream_reset();
					audio_active = 1; audio_bytes = 0; last_audio_bytes = 0;
				}
				ts = strchr(ts, ',');
				ts = ts ? ts + 1 : "";
			}
		}
#endif
	
		//Handle X events
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == ConfigureNotify) {
				show_help = 0;
				paused = 0;
				win_w = ev.xconfigure.width;
				win_h = ev.xconfigure.height;
				
				int want_scale = (win_w != video_w) || (win_h != video_h);
				
				if (want_scale) {
					recreate_ximage(win_w, win_h);
					scale_video = 1;
				} else {
					recreate_ximage(video_w, video_h);
					scale_video = 0;
				}
				need_redraw = 1;
				break;
			} else if (ev.type == ButtonPress && ev.xbutton.button == Button1) {
				double frac = (double)ev.xbutton.x / win_w;
				request_seek(frac);
				break;
			} else if (ev.type == KeyPress) {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                if (ks == XK_q || ks == XK_Escape) {
					running = 0;
					break;
				} else if (ks == XK_space) {
					if (show_help) {
						show_help = 0;
						paused = 0;
					} else {
						paused = !paused;
					}
					need_redraw = 1;
					break;
				} else if (ks == XK_a && audio_count > 0) {
					if (audio_type == AUDIO_AC3) {
						int backwards = (ev.xkey.state & ShiftMask);
						int idx = 0;

						if (audio_sid_selected >= 0) {
							for (int i = 0; i < audio_count; i++) {
								if (audio_sid_list[i] == audio_sid_selected) {
									if (backwards)   // backward
										idx = (i - 1 + audio_count) % audio_count;
									else              // forward
										idx = (i + 1) % audio_count;
									break;
								}
							}
						}
						audio_sid_selected = audio_sid_list[idx];
						audio_stream_reset();
						audio_active = 1;      // optimistic
						audio_bytes = 0;
						last_audio_bytes = 0;
						fprintf(stderr, "Audio: sid=0x%02x\n", audio_sid_selected);
					}
					break;	
				} else if (ks == XK_m) {
					audio_muted = !audio_muted;
					fprintf(stderr, "Mute: %s\n", audio_muted ? "on" : "off");
					break;
				} else if (ks == XK_plus || ks == XK_equal) {
					if (audio_volume_internal < 100) { audio_volume_internal += 5;}
					fprintf(stderr, "Volume: %d\n", audio_volume_internal);
					break;
				} else if (ks == XK_minus) {
					if (audio_volume_internal > 0) { audio_volume_internal -= 5;}
					fprintf(stderr, "Volume: %d\n", audio_volume_internal);
					break;
				} else if (ks == XK_0) {
					audio_volume_internal = 100;
					fprintf(stderr, "Volume reset\n");
					break;
				} else if (ks == XK_h) {
					show_help = !show_help;
					paused = show_help;
					need_redraw = 1;
					break;
				} else if (ks == XK_s) {	// toggle subtitle ON/OFF
					subtitle_enabled = !subtitle_enabled;
					fprintf(stderr, "Subtitles: %s\n", subtitle_enabled ? "on" : "off");
					break;
				} else if (ks == XK_n && sub_count > 0) {	// next subtitle stream / language
					int backwards = (ev.xkey.state & ShiftMask);
					int idx = 0;
					if (current_sub_sid >= 0) {
						for (int i = 0; i < sub_count; i++) {
							if (sub_sid_list[i] == current_sub_sid) {
								if (backwards)   // backward
									idx = (i - 1 + sub_count) % sub_count;
								else              // forward
									idx = (i + 1) % sub_count;
								break;
							}
						}
					}
					current_sub_sid = sub_sid_list[idx];
					fprintf(stderr, "Subtitle: sid=0x%02x\n", current_sub_sid);	
					break;
				} else if (ks == XK_f) {
					show_help = 0;
					paused = 0;
					scale_video = !scale_video;
					fprintf(stderr, "Video scaling: %s\n",
						scale_video ? "ON" : "OFF");
					toggle_fullscreen(dpy, win);
					break;
					}
            }
        }
        if (paused) {
			if (need_redraw) {
				redraw();
				need_redraw = 0;
			}
			usleep(20000);
			continue;
		}

		playback_step();
		
		if (audio_enabled && !audio_ready) {
			audio_open(audio_rate, audio_channels);
			audio_ready = 1;
		}
		
		//if silent audio substream:
#ifdef SYNC_OLD
		static int audio_check_counter = 0;
		audio_check_counter++;
		if (audio_check_counter >= 50) {   // ~1 sec at 50 fps loop
#else
		static int64_t audio_check_us = 0;
		if (!audio_check_us) audio_check_us = now_us();
		if (now_us() - audio_check_us >= 1000000) {   // once a second
			audio_check_us = now_us();
#endif
			if (audio_bytes == last_audio_bytes) {
				audio_active = 0;
			} else {
				audio_active = 1;
			}
			last_audio_bytes = audio_bytes;
#ifdef SYNC_OLD
			audio_check_counter = 0;
#endif
		}
	} //while running

#ifndef SYNC_OLD
	dbg_summary();
#endif
	render_cleanup();
	if (ximg) {
		destroy_ximage();
	}
	XFreeGC(dpy, gc);
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);
	audio_close();
	a52_free(a52);
#ifdef TEST_PLAIN_VOB
    close(vob_fd);
#else
    DVDCloseFile(vob);
    ifoClose(ifo);
    DVDClose(dvd);
#endif
    return 0;
}
