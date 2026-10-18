#define _XOPEN_SOURCE 500
#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <signal.h>
#include <limits.h>
#include <sys/time.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>
#include <dvdread/nav_read.h>

#include <mpeg2.h>
#include <mpeg2convert.h>

#include <a52.h>

#include <asoundlib.h>	//tinyalsa



//frame/redraw logning
#ifdef LOGGING
// Globale eller statiske variabler til at huske tiden
static struct timespec sidste_tid;
static int forste_gennemlob = 1;
//end
#endif

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
#define VIDEO_ES_BUF_SIZE (512 * 1024)
static uint8_t video_es_buf[VIDEO_ES_BUF_SIZE];

static int video_es_len = 0;

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

//OSS globals
static int audio_fd = -1;
static int audio_active = 0;	// stream activity

static int audio_rate = 48000;     // actual OSS rate
static int audio_channels = 2;     // actual OSS channels
static int audio_muted = 0;
static int audio_volume_internal = 100;
static int audio_ready = 0;   // OSS device state

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

typedef enum {
    AUDIO_BACKEND_NONE = 0,
    AUDIO_BACKEND_TINYALSA,
    AUDIO_BACKEND_OSS
} audio_backend_t;

static audio_backend_t audio_backend = AUDIO_BACKEND_NONE;

static struct pcm *pcm_dev = NULL;

int audio_init_tinyalsa(int rate, int channels) {
    struct pcm_config cfg = {
        .channels = channels,
        .rate = rate,
        .format = PCM_FORMAT_S16_LE,
        .period_size = 4096,	//1024, 2048, 4096, 8192, 16384
        .period_count = 4,	//4 seems max
        .start_threshold = 8192,	//start_threshold = period_size * 2;
        .stop_threshold = 16384,		//stop_threshold = period_size * period_count;
        .silence_threshold = 0,
    };

    pcm_dev = pcm_open(0, 0, PCM_OUT, &cfg);
    if (!pcm_dev || !pcm_is_ready(pcm_dev)) {
        fprintf(stderr, "tinyalsa: open failed: %s\n", pcm_get_error(pcm_dev));
        pcm_dev = NULL;
        return -1;
    }

    fprintf(stderr, "Audio: tinyalsa %d Hz, %d ch\n", rate, channels);
    return 0;
}

void audio_write_tinyalsa(const int16_t *pcm, int samples) {
    if (!pcm_dev) return;
    pcm_write(pcm_dev, pcm, samples * sizeof(int16_t));
}

void audio_close_tinyalsa(void) {
    if (pcm_dev) {
        pcm_close(pcm_dev);
        pcm_dev = NULL;
    }
}

//scaling uses 8.8 fixed-point
static void scale_bilinear_rgba(uint8_t *dst, int dw, int dh,
    const uint8_t *src, int sw, int sh) {

    const int FP = 8;
    const int ONE = 1 << FP;

    int xstep = ((sw - 1) << FP) / dw;
    int ystep = ((sh - 1) << FP) / dh;

    for (int y = 0, sy = 0; y < dh; y++, sy += ystep) {
        int y0 = sy >> FP;
        int y1 = y0 + (y0 + 1 < sh);
        int fy = sy & (ONE - 1);

        const uint8_t *row0 = src + 4 * y0 * sw;
        const uint8_t *row1 = src + 4 * y1 * sw;
        uint8_t *d = dst + 4 * y * dw;

        for (int x = 0, sx = 0; x < dw; x++, sx += xstep, d += 4) {
            int x0 = sx >> FP;
            int x1 = x0 + (x0 + 1 < sw);
            int fx = sx & (ONE - 1);

            const uint8_t *p00 = row0 + 4 * x0;
            const uint8_t *p10 = row0 + 4 * x1;
            const uint8_t *p01 = row1 + 4 * x0;
            const uint8_t *p11 = row1 + 4 * x1;

            int wx0 = ONE - fx;
            int wy0 = ONE - fy;

            for (int c = 0; c < 4; c++) {
                int a = p00[c] * wx0 + p10[c] * fx;
                int b = p01[c] * wx0 + p11[c] * fx;
                d[c] = (uint8_t)((a * wy0 + b * fy) >> (FP * 2));
            }
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
        }
        // Copy remaining data
        int copy = ac3_need - ac3_len;
        if (copy > len) { copy = len;}
        memcpy(ac3_buf + ac3_len, data, copy);
        ac3_len += copy;
        data += copy;
        len -= copy;
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
            audio_feed(ac3_buf, ac3_need, NULL);
         //   fprintf(stderr, "AC3 sync: frame_len=%d rate=%d bitrate=%d\n", ac3_need, rate, bitrate);
            ac3_len = 0;
            ac3_need = 0;
        }
    }
}

// ------------------ OSS ------------------
static int audio_init_oss(int rate, int channels) {
	
	if (!audio_enabled) { return -1;}
    int fmt = AFMT_S16_LE;
    int ch = channels;
    int r  = rate;
    audio_fd = open("/dev/dsp", O_WRONLY);
    if (audio_fd < 0) {
        fprintf(stderr, "Warning: OSS open failed; continuing without audio.\n");
        audio_active = 0;
        return -1;
    }
    if (ioctl(audio_fd, SNDCTL_DSP_RESET, 0) < 0) { goto fail;}
    if (ioctl(audio_fd, SNDCTL_DSP_SETFMT, &fmt) < 0 || fmt != AFMT_S16_LE) { goto fail;}
    if (ioctl(audio_fd, SNDCTL_DSP_CHANNELS, &ch) < 0) { goto fail;}
    if (ioctl(audio_fd, SNDCTL_DSP_SPEED, &r) < 0) { goto fail;}
    // OSS may clamp values — accept what it gives
    audio_channels = ch;
    audio_rate     = r;
    audio_active   = 1;
    fprintf(stderr, "OSS audio: %d Hz, %d ch, S16_LE\n", r, ch);
    return 0;

fail:
    close(audio_fd);
    audio_fd = -1;
    audio_active = 0;
    fprintf(stderr, "OSS audio disabled (unsupported format)\n");
    return -1;
}

static void audio_write_oss(const int16_t *pcm, int samples) {
    if (audio_fd < 0) { return;}
    write(audio_fd, pcm, samples * sizeof(int16_t));
}

static void audio_close_oss(void) {
    if (audio_fd >= 0) {
        close(audio_fd);
        audio_fd = -1;
    }
}

// ------------------ audio ------------------ 
int audio_init(int rate, int channels) {
    if (audio_init_tinyalsa(rate, channels) == 0) {
        audio_backend = AUDIO_BACKEND_TINYALSA;
        return 0;
    }
    fprintf(stderr, "Audio: falling back to OSS\n");
    if (audio_init_oss(rate, channels) == 0) {
        audio_backend = AUDIO_BACKEND_OSS;
        return 0;
    }
    audio_backend = AUDIO_BACKEND_NONE;
    return -1;
}

static inline void audio_write(const int16_t *pcm, int samples) {
    switch (audio_backend) {
    case AUDIO_BACKEND_TINYALSA:
        audio_write_tinyalsa(pcm, samples);
        break;
    case AUDIO_BACKEND_OSS:
        audio_write_oss(pcm, samples);
        break;
    default:
        break;
    }
}

static void audio_close(void) {
    switch (audio_backend) {
    case AUDIO_BACKEND_TINYALSA:
        audio_close_tinyalsa();
        break;
    case AUDIO_BACKEND_OSS:
        audio_close_oss();
        break;
    default:
        break;
    }
    audio_backend = AUDIO_BACKEND_NONE;
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

    ximg = XCreateImage(dpy, DefaultVisual(dpy,s), DefaultDepth(dpy, s), ZPixmap, 0,
        malloc(w*h*4), w, h, 32, 0);
}

static void recreate_ximage(int w, int h) {
    if (ximg) {
        XDestroyImage(ximg);   // frees data automatically
        ximg = NULL;
    }
    int s = DefaultScreen(dpy);
    ximg = XCreateImage(dpy, DefaultVisual(dpy, s), DefaultDepth(dpy, s), ZPixmap, 0, 
		malloc(w * h * 4), w, h, 32, 0);
}

#define MAX_SPU_SIZE  65536   // spec max, practical upper bound

typedef struct {
    int active;
    int expected;
    int collected;
    uint8_t buf[MAX_SPU_SIZE];
} spu_assembler_t;

static spu_assembler_t spu[0x20];   // one per substream (0x20–0x3F)

// helpers
static void audio_feed(uint8_t *data, int len, void *opaque) {
	if (!audio_enabled || audio_type != AUDIO_AC3) { return;}
	audio_bytes += len;
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
        audio_write(pcm, frames * audio_channels);
    }
}

static void video_feed(uint8_t *data, int len) {
	if (video_es_len + len > VIDEO_ES_BUF_SIZE) {
		fprintf(stderr, "ES buffer overflow — dropping data\n");
        video_es_len = 0;
    }
    memcpy(video_es_buf + video_es_len, data, len);
    video_es_len += len;
}

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

            if (stream_id >= 0xE0 && stream_id <= 0xEF) {
				if (payload_len > 0) {
                     video_feed(buf + payload_start, payload_len);
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
                    if (payload_len > 4)
                        ac3_push(buf + payload_start + 4, payload_len - 4);
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
								struct spu_state st;
								memset(&st, 0, sizeof(st));
								spu_demux_control(s->buf, s->expected, &st);
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

    XPutImage(dpy, win, gc, ximg, 0, 0, dx, dy, dst_w, dst_h);
    if (show_help) draw_help_overlay();
    XFlush(dpy);
}

static void audio_flush(void) {
    if (audio_fd >= 0)
        ioctl(audio_fd, SNDCTL_DSP_RESET, 0);
}

static uint8_t buf[2048];

static void playback_step(void) {
	if (dvd_block >= total_blocks) { running = 0; return; }
	static const mpeg2_fbuf_t *last_displayed = NULL;
	//framelog
#ifdef LOGGING
	static struct timespec pacemaker_tid;
static int pacemaker_initialiseret = 0;
#endif	
	//end
	

	if (seek_pending) {
		seek_pending = 0;
		dvd_block = seek_target_block;
		audio_flush();	// Flush audio device buffer (optional but good)
		last_displayed = NULL;
		return; // refill pipeline cleanly
	}
	
    if (mpeg2_parse(mpeg2) == STATE_BUFFER) {	// Feed decoder if it needs data
        if (DVDReadBlocks(vob, dvd_block++, 1, buf) != 1) {
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
		
		if ((state == STATE_SLICE || state == STATE_END) &&	info->display_fbuf &&
			info->display_picture && info->display_fbuf != last_displayed) {
			
			last_displayed = info->display_fbuf;
			have_frame = 1;

			// center video in window
			video_x = (win_w - video_w) / 2;
			video_y = (win_h - video_h) / 2;
#ifdef LOGGING		
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




			
			//framelogning
			struct timespec nuvrende_tid;
clock_gettime(CLOCK_MONOTONIC, &nuvrende_tid);

if (!forste_gennemlob) {
    // Beregn forskellen i mikrosekunder
    long diff_us = (nuvrende_tid.tv_sec - sidste_tid.tv_sec) * 1000000 +
                   (nuvrende_tid.tv_nsec - sidste_tid.tv_nsec) / 1000;
    
    // Print værdien ud (gem evt. i en fil eller kig i terminalen)
    printf("Tid siden sidste redraw: %ld us\n", diff_us);
} else {
    forste_gennemlob = 0;
}
sidste_tid = nuvrende_tid;
			
			
			//end
#endif
			redraw();
			
            if (!audio_active) {
				usleep(35000); // pace ONLY on video output if no audio  
            }    
		}
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s /dev/dvd\n", argv[0]);
        return 1;
    }
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
	fprintf(stderr, "VOB size: %d blocks\n", total_blocks);

    dvd_block = 0;
	mpeg2 = mpeg2_init();
	mpeg2_tag_picture(mpeg2, 0, 1);
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
						audio_flush();
						ac3_len = 0;
						ac3_need = 0;
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
			audio_init(audio_rate, audio_channels);
			audio_ready = 1;
		}
		
		//if silent audio substream:
		static int audio_check_counter = 0;
		audio_check_counter++;
		if (audio_check_counter >= 50) {   // ~1 sec at 50 fps loop
			if (audio_bytes == last_audio_bytes) {
				audio_active = 0;
			} else {
				audio_active = 1;
			}
			last_audio_bytes = audio_bytes;
			audio_check_counter = 0;
		}
	} //while running

	if (ximg) {
		XDestroyImage(ximg);
	}
	XFreeGC(dpy, gc);
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);
	audio_close();
	a52_free(a52);
    DVDCloseFile(vob);
    ifoClose(ifo);
    DVDClose(dvd);
    return 0;
}
