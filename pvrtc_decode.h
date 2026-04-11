/*
 * pvrtc_decode.h — PVRTC texture decompression (4bpp and 2bpp)
 * Decodes PowerVR compressed textures to RGBA8888 for desktop GL.
 *
 * Based on PowerVR Native_SDK PVRTDecompress.cpp (MIT license)
 * and public PVRTC specification by Imagination Technologies.
 */
#ifndef PVRTC_DECODE_H
#define PVRTC_DECODE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* PVRTC compressed format enums (matching GL constants) */
#define PVRTC_RGB_4BPP   0x8C00
#define PVRTC_RGB_2BPP   0x8C01
#define PVRTC_RGBA_4BPP  0x8C02
#define PVRTC_RGBA_2BPP  0x8C03

static inline int pvrtc_is_pvrtc(uint32_t fmt) {
    return fmt >= PVRTC_RGB_4BPP && fmt <= PVRTC_RGBA_2BPP;
}

typedef struct { int32_t r, g, b, a; } PVRColor;

/*
 * PVRTC block (64 bits):
 *   bits [0..31]  = modulation data (2 bits per pixel, 4x4 = 32 bits for 4bpp)
 *   bits [32..47] = color A data (16 bits: 1 opaque flag + 15 color bits)
 *   bits [48..63] = color B data (16 bits: 1 opaque flag + 15 color bits)
 *
 * Color A bit 32 = modulation mode (0 = standard, 1 = punch-through)
 */

/* Unpack Color A from bits [32..47] of the 64-bit block */
static PVRColor pvrtc_get_color_a(uint32_t color_data) {
    PVRColor c;
    /* color_data is the upper 32 bits; color A is the lower 16 bits of that */
    uint16_t raw = (uint16_t)(color_data & 0xFFFF);
    int opaque = raw & 0x8000;

    if (opaque) {
        /* Opaque: bit15=opaque, bit0=modmode, R=bits[14:10](5), G=bits[9:5](5), B=bits[4:1](4) */
        c.r = (raw >> 10) & 0x1F;
        c.g = (raw >> 5)  & 0x1F;
        c.b = (raw >> 1)  & 0x0F;
        c.a = 0x0F;
        /* Expand to 8-bit */
        c.r = (c.r << 3) | (c.r >> 2);
        c.g = (c.g << 3) | (c.g >> 2);
        c.b = (c.b << 4) | c.b;
        c.a = 255;
    } else {
        /* Transparent: A=bits[14:12](3), R=bits[11:8](4), G=bits[7:4](4), B=bits[3:0](4)
         * No mod-mode bit in 2BPP transparent either; bit0 is B LSB. */
        c.a = (raw >> 12) & 0x07;
        c.r = (raw >> 8) & 0x0F;
        c.g = (raw >> 4) & 0x0F;
        c.b = (raw >> 0) & 0x0F;
        /* Expand */
        c.a = (c.a << 5) | (c.a << 2) | (c.a >> 1); /* 3->8 bit */
        c.r = (c.r << 4) | c.r;                       /* 4->8 bit */
        c.g = (c.g << 4) | c.g;                       /* 4->8 bit */
        c.b = (c.b << 4) | c.b;                       /* 4->8 bit */
    }
    return c;
}

/* Unpack Color B from bits [48..63] of the 64-bit block */
static PVRColor pvrtc_get_color_b(uint32_t color_data) {
    PVRColor c;
    /* color B is the upper 16 bits of color_data */
    uint16_t raw = (uint16_t)((color_data >> 16) & 0xFFFF);
    int opaque = raw & 0x8000;

    if (opaque) {
        /* Opaque: R=bits[10..14](5), G=bits[5..9](5), B=bits[0..4](5) */
        c.r = (raw >> 10) & 0x1F;
        c.g = (raw >> 5) & 0x1F;
        c.b = (raw >> 0) & 0x1F;
        c.a = 255;
        c.r = (c.r << 3) | (c.r >> 2);
        c.g = (c.g << 3) | (c.g >> 2);
        c.b = (c.b << 3) | (c.b >> 2);
    } else {
        /* Transparent: A=bits[12..14](3), R=bits[8..11](4), G=bits[4..7](4), B=bits[0..3](4) */
        c.a = (raw >> 12) & 0x07;
        c.r = (raw >> 8) & 0x0F;
        c.g = (raw >> 4) & 0x0F;
        c.b = (raw >> 0) & 0x0F;
        c.a = (c.a << 5) | (c.a << 2) | (c.a >> 1);
        c.r = (c.r << 4) | c.r;
        c.g = (c.g << 4) | c.g;
        c.b = (c.b << 4) | c.b;
    }
    return c;
}

static inline int32_t pvrtc_clamp(int32_t v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

/*
 * Morton (Z-order / twiddled) block index.
 * PVRTC1 data from PVR tools is stored in Morton order.
 * Maps 2D block coordinates (bx, by) to the linear memory index.
 * For non-square grids the shorter dimension is fully interleaved;
 * remaining bits from the longer dimension are appended.
 */
static uint32_t pvrtc_morton(uint32_t num_x, uint32_t num_y, uint32_t bx, uint32_t by) {
    uint32_t result = 0;
    uint32_t bit    = 0;
    uint32_t min_dim = (num_x < num_y) ? num_x : num_y;

    for (uint32_t s = 1; s < min_dim; s <<= 1, bit++) {
        /* PowerVR twiddled layout: Y in even bit positions, X in odd positions */
        if (by & s) result |= 1u << (2 * bit);
        if (bx & s) result |= 1u << (2 * bit + 1);
    }

    /* Append remaining bits of the longer dimension */
    uint32_t out = bit * 2;
    if (num_x >= num_y) {
        for (uint32_t s = min_dim; s < num_x; s <<= 1, out++)
            if (bx & s) result |= 1u << out;
    } else {
        for (uint32_t s = min_dim; s < num_y; s <<= 1, out++)
            if (by & s) result |= 1u << out;
    }
    return result;
}

/*
 * Decode PVRTC 4bpp to RGBA8888.
 * width and height must be powers of two and >= 8.
 * data_size should be (width * height) / 2 bytes.
 * Returns malloc'd RGBA buffer (caller must free), or NULL on error.
 */
static uint8_t *pvrtc_decompress_4bpp(const void *data, int width, int height,
                                       int has_alpha) {
    if (width < 8) width = 8;
    if (height < 8) height = 8;

    int num_x = width / 4;  /* blocks in X */
    int num_y = height / 4; /* blocks in Y */

    /* PVRTC data is stored in a morton/Z-order. For simplicity, treat as linear.
     * Most mobile PVRTC data is stored linearly for 4bpp. */
    const uint32_t *words = (const uint32_t *)data;
    /* Each block is 64 bits = two uint32s: [modulation_data, color_data] */

    uint8_t *output = (uint8_t *)calloc(width * height, 4);
    if (!output) return NULL;

    /* Pre-decode all block colors */
    int num_blocks = num_x * num_y;
    PVRColor *colors_a = (PVRColor *)malloc(num_blocks * sizeof(PVRColor));
    PVRColor *colors_b = (PVRColor *)malloc(num_blocks * sizeof(PVRColor));
    int *mod_modes = (int *)malloc(num_blocks * sizeof(int));

    if (!colors_a || !colors_b || !mod_modes) {
        free(output); free(colors_a); free(colors_b); free(mod_modes);
        return NULL;
    }

    for (int by2 = 0; by2 < num_y; by2++) {
        for (int bx2 = 0; bx2 < num_x; bx2++) {
            int lin = by2 * num_x + bx2;
            int mem = (int)pvrtc_morton((uint32_t)num_x, (uint32_t)num_y, (uint32_t)bx2, (uint32_t)by2);
            uint32_t col_data = words[mem * 2 + 1];
            colors_a[lin] = pvrtc_get_color_a(col_data);
            colors_b[lin] = pvrtc_get_color_b(col_data);
            mod_modes[lin] = col_data & 1;
        }
    }

    /* For each 4x4 block, bilinearly interpolate colors from surrounding blocks
     * and apply modulation */
    for (int by = 0; by < num_y; by++) {
        for (int bx = 0; bx < num_x; bx++) {
            int block_idx = by * num_x + bx;
            int mem_idx   = (int)pvrtc_morton((uint32_t)num_x, (uint32_t)num_y, (uint32_t)bx, (uint32_t)by);
            uint32_t mod_data = words[mem_idx * 2];
            int mod_mode = mod_modes[block_idx];

            /* Get the 4 surrounding block colors for bilinear interpolation.
             * The upscaled image has block centers at (2,2) within each 4x4 block. */
            int bx0 = bx;
            int by0 = by;
            int bx1 = (bx + 1) % num_x;
            int by1 = (by + 1) % num_y;

            int i00 = by0 * num_x + bx0;
            int i10 = by0 * num_x + bx1;
            int i01 = by1 * num_x + bx0;
            int i11 = by1 * num_x + bx1;

            for (int py = 0; py < 4; py++) {
                for (int px = 0; px < 4; px++) {
                    /* Bilinear weights: offset by 2 pixels (block center) */
                    int wx = px + 2;  /* 2..5 range for 4-wide lerp */
                    int wy = py + 2;

                    /* Wrap if we go past the 4-pixel boundary */
                    int sx = wx;
                    int sy = wy;
                    int use_bx0 = bx, use_by0 = by;
                    int use_bx1 = bx1, use_by1 = by1;

                    if (sx >= 4) { sx -= 4; }
                    else { use_bx1 = bx; use_bx0 = (bx - 1 + num_x) % num_x; }
                    if (sy >= 4) { sy -= 4; }
                    else { use_by1 = by; use_by0 = (by - 1 + num_y) % num_y; }

                    int ci00 = use_by0 * num_x + use_bx0;
                    int ci10 = use_by0 * num_x + use_bx1;
                    int ci01 = use_by1 * num_x + use_bx0;
                    int ci11 = use_by1 * num_x + use_bx1;

                    /* Bilinear interpolation of color A */
                    PVRColor ca;
                    ca.r = ((colors_a[ci00].r * (4-sx) + colors_a[ci10].r * sx) * (4-sy) +
                            (colors_a[ci01].r * (4-sx) + colors_a[ci11].r * sx) * sy + 8) / 16;
                    ca.g = ((colors_a[ci00].g * (4-sx) + colors_a[ci10].g * sx) * (4-sy) +
                            (colors_a[ci01].g * (4-sx) + colors_a[ci11].g * sx) * sy + 8) / 16;
                    ca.b = ((colors_a[ci00].b * (4-sx) + colors_a[ci10].b * sx) * (4-sy) +
                            (colors_a[ci01].b * (4-sx) + colors_a[ci11].b * sx) * sy + 8) / 16;
                    ca.a = ((colors_a[ci00].a * (4-sx) + colors_a[ci10].a * sx) * (4-sy) +
                            (colors_a[ci01].a * (4-sx) + colors_a[ci11].a * sx) * sy + 8) / 16;

                    /* Bilinear interpolation of color B */
                    PVRColor cb;
                    cb.r = ((colors_b[ci00].r * (4-sx) + colors_b[ci10].r * sx) * (4-sy) +
                            (colors_b[ci01].r * (4-sx) + colors_b[ci11].r * sx) * sy + 8) / 16;
                    cb.g = ((colors_b[ci00].g * (4-sx) + colors_b[ci10].g * sx) * (4-sy) +
                            (colors_b[ci01].g * (4-sx) + colors_b[ci11].g * sx) * sy + 8) / 16;
                    cb.b = ((colors_b[ci00].b * (4-sx) + colors_b[ci10].b * sx) * (4-sy) +
                            (colors_b[ci01].b * (4-sx) + colors_b[ci11].b * sx) * sy + 8) / 16;
                    cb.a = ((colors_b[ci00].a * (4-sx) + colors_b[ci10].a * sx) * (4-sy) +
                            (colors_b[ci01].a * (4-sx) + colors_b[ci11].a * sx) * sy + 8) / 16;

                    /* Get modulation value (2 bits per pixel, row-major within block) */
                    int mod_val = (mod_data >> ((py * 4 + px) * 2)) & 3;

                    int32_t fr, fg, fb, fa;
                    if (mod_mode == 0) {
                        /* Standard: 0=0/8, 1=3/8, 2=5/8, 3=8/8 */
                        static const int weights[4] = {0, 3, 5, 8};
                        int w = weights[mod_val];
                        fr = (ca.r * (8 - w) + cb.r * w + 4) / 8;
                        fg = (ca.g * (8 - w) + cb.g * w + 4) / 8;
                        fb = (ca.b * (8 - w) + cb.b * w + 4) / 8;
                        fa = (ca.a * (8 - w) + cb.a * w + 4) / 8;
                    } else {
                        /* Punch-through: 0=0/8, 1=4/8, 2=4/8+transparent, 3=8/8 */
                        switch (mod_val) {
                            case 0:
                                fr = ca.r; fg = ca.g; fb = ca.b; fa = ca.a;
                                break;
                            case 1:
                                fr = (ca.r + cb.r + 1) / 2;
                                fg = (ca.g + cb.g + 1) / 2;
                                fb = (ca.b + cb.b + 1) / 2;
                                fa = (ca.a + cb.a + 1) / 2;
                                break;
                            case 2:
                                fr = (ca.r + cb.r + 1) / 2;
                                fg = (ca.g + cb.g + 1) / 2;
                                fb = (ca.b + cb.b + 1) / 2;
                                fa = 0;
                                break;
                            default:
                                fr = cb.r; fg = cb.g; fb = cb.b; fa = cb.a;
                                break;
                        }
                    }

                    int x = bx * 4 + px;
                    int y = by * 4 + py;
                    int out_idx = (y * width + x) * 4;
                    output[out_idx + 0] = pvrtc_clamp(fr);
                    output[out_idx + 1] = pvrtc_clamp(fg);
                    output[out_idx + 2] = pvrtc_clamp(fb);
                    output[out_idx + 3] = has_alpha ? pvrtc_clamp(fa) : 255;
                }
            }
        }
    }

    free(colors_a);
    free(colors_b);
    free(mod_modes);
    return output;
}

/*
 * Decode PVRTC 2bpp to RGBA8888.
 * Blocks are 8x4 pixels, 64 bits each.
 * Each pixel's color is bilinearly interpolated from the 4 surrounding
 * block color samples (same algorithm as 4bpp, scaled for 8x4 blocks).
 * Modulation: 1 bit per pixel (32 pixels × 1 bit = 32 bits).
 */
static uint8_t *pvrtc_decompress_2bpp(const void *data, int width, int height,
                                       int has_alpha) {
    if (width < 16) width = 16;
    if (height < 8) height = 8;

    int num_x = width / 8;
    int num_y = height / 4;
    int num_blocks = num_x * num_y;

    const uint32_t *words = (const uint32_t *)data;

    uint8_t *output = (uint8_t *)calloc(width * height, 4);
    PVRColor *colors_a = (PVRColor *)malloc(num_blocks * sizeof(PVRColor));
    PVRColor *colors_b = (PVRColor *)malloc(num_blocks * sizeof(PVRColor));

    if (!output || !colors_a || !colors_b) {
        free(output); free(colors_a); free(colors_b);
        return NULL;
    }

    int n_opaque = 0, n_trans = 0;
    for (int by2 = 0; by2 < num_y; by2++) {
        for (int bx2 = 0; bx2 < num_x; bx2++) {
            int lin = by2 * num_x + bx2;
            int mem = (int)pvrtc_morton((uint32_t)num_x, (uint32_t)num_y, (uint32_t)bx2, (uint32_t)by2);
            uint32_t col_data = words[mem * 2 + 1];
            colors_a[lin] = pvrtc_get_color_a(col_data);
            colors_b[lin] = pvrtc_get_color_b(col_data);
            if (col_data & 0x8000) n_opaque++; else n_trans++;
        }
    }
    fprintf(stderr, "[PVRTC2] %dx%d: opaque=%d trans=%d\n", width, height, n_opaque, n_trans);

    for (int by = 0; by < num_y; by++) {
        for (int bx = 0; bx < num_x; bx++) {
            int block_idx = by * num_x + bx;
            int mem_idx   = (int)pvrtc_morton((uint32_t)num_x, (uint32_t)num_y, (uint32_t)bx, (uint32_t)by);
            uint32_t mod_data = words[mem_idx * 2];

            for (int py = 0; py < 4; py++) {
                for (int px = 0; px < 8; px++) {
                    /* Bilinear interpolation — same logic as 4bpp but for 8x4 blocks.
                     * Offset by half-block (4,2) to center the sample point.
                     * Block width=8 so divisor=8; block height=4 so divisor=4. */
                    int wx = px + 4;   /* 4..11 */
                    int wy = py + 2;   /* 2..5  */

                    int sx, sy, cbx0, cbx1, cby0, cby1;

                    if (wx >= 8) {
                        sx   = wx - 8;                  /* 0..3 */
                        cbx0 = bx;
                        cbx1 = (bx + 1) % num_x;
                    } else {
                        sx   = wx;                      /* 4..7 */
                        cbx0 = (bx - 1 + num_x) % num_x;
                        cbx1 = bx;
                    }

                    if (wy >= 4) {
                        sy   = wy - 4;                  /* 0..1 */
                        cby0 = by;
                        cby1 = (by + 1) % num_y;
                    } else {
                        sy   = wy;                      /* 2..3 */
                        cby0 = (by - 1 + num_y) % num_y;
                        cby1 = by;
                    }

                    int ci00 = cby0 * num_x + cbx0;
                    int ci10 = cby0 * num_x + cbx1;
                    int ci01 = cby1 * num_x + cbx0;
                    int ci11 = cby1 * num_x + cbx1;

                    /* Bilinear blend — divisors 8 (x) × 4 (y) = 32 */
                    #define BLERP2(ch) \
                        (((colors_a[ci00].ch*(8-sx)+colors_a[ci10].ch*sx)*(4-sy) + \
                          (colors_a[ci01].ch*(8-sx)+colors_a[ci11].ch*sx)*sy + 16) / 32)
                    #define BLERP2B(ch) \
                        (((colors_b[ci00].ch*(8-sx)+colors_b[ci10].ch*sx)*(4-sy) + \
                          (colors_b[ci01].ch*(8-sx)+colors_b[ci11].ch*sx)*sy + 16) / 32)

                    PVRColor ca, cb;
                    ca.r = BLERP2(r); ca.g = BLERP2(g); ca.b = BLERP2(b); ca.a = BLERP2(a);
                    cb.r = BLERP2B(r); cb.g = BLERP2B(g); cb.b = BLERP2B(b); cb.a = BLERP2B(a);
                    #undef BLERP2
                    #undef BLERP2B

                    /* 1-bit modulation: row-major raster scan within 8x4 block */
                    int mod_val = (mod_data >> (py * 8 + px)) & 1;

                    int x = bx * 8 + px;
                    int y = by * 4 + py;
                    int out_idx = (y * width + x) * 4;
                    output[out_idx+0] = pvrtc_clamp(mod_val ? cb.r : ca.r);
                    output[out_idx+1] = pvrtc_clamp(mod_val ? cb.g : ca.g);
                    output[out_idx+2] = pvrtc_clamp(mod_val ? cb.b : ca.b);
                    output[out_idx+3] = has_alpha ? pvrtc_clamp(mod_val ? cb.a : ca.a) : 255;
                }
            }
        }
    }

    free(colors_a);
    free(colors_b);
    return output;
}

/*
 * Decompress a PVRTC texture to RGBA8888.
 * Returns malloc'd buffer or NULL. Sets *out_size to output byte count.
 */
static uint8_t *pvrtc_decompress(uint32_t format, const void *data,
                                  int width, int height, int *out_size) {
    int has_alpha = (format == PVRTC_RGBA_4BPP || format == PVRTC_RGBA_2BPP);
    uint8_t *result = NULL;

    if (format == PVRTC_RGB_4BPP || format == PVRTC_RGBA_4BPP) {
        result = pvrtc_decompress_4bpp(data, width, height, has_alpha);
    } else {
        result = pvrtc_decompress_2bpp(data, width, height, has_alpha);
    }

    if (result && out_size) {
        *out_size = width * height * 4;
    }
    return result;
}

/* ================================================================
 * ATC (Adreno Texture Compression) decompression
 * GL_ATC_RGB_AMD (0x8C92) and GL_ATC_RGBA_EXPLICIT_ALPHA_AMD (0x8C93)
 *
 * ATC RGB block (8 bytes):
 *   [0..1] color0: RGB555 (bit 15 = interpolation mode flag)
 *          if bit15=0: color0 is 15-bit RGB (5:5:5)
 *          if bit15=1: color0 is 15-bit RGB (5:5:5), different interp
 *   [2..3] color1: RGB565 (same as DXT1)
 *   [4..7] 2-bit lookup table (32 bits, 16 pixels)
 *
 * ATC RGBA explicit alpha block (16 bytes):
 *   [0..7]  4-bit alpha values (64 bits, 16 pixels)
 *   [8..15] ATC RGB block (same as above)
 * ================================================================ */

#define ATC_RGB_AMD                     0x8C92
#define ATC_RGBA_EXPLICIT_ALPHA_AMD     0x8C93
#define ATC_RGBA_INTERPOLATED_ALPHA_AMD 0x87EE

static inline int atc_is_atc(uint32_t fmt) {
    return fmt == ATC_RGB_AMD || fmt == ATC_RGBA_EXPLICIT_ALPHA_AMD ||
           fmt == ATC_RGBA_INTERPOLATED_ALPHA_AMD;
}

static inline void atc_rgb555_to_rgb(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b) {
    *r = ((c >> 10) & 0x1F) * 255 / 31;
    *g = ((c >>  5) & 0x1F) * 255 / 31;
    *b = ((c >>  0) & 0x1F) * 255 / 31;
}

static inline void atc_rgb565_to_rgb(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b) {
    *r = ((c >> 11) & 0x1F) * 255 / 31;
    *g = ((c >>  5) & 0x3F) * 255 / 63;
    *b = ((c >>  0) & 0x1F) * 255 / 31;
}

static inline uint8_t atc_clamp(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static void atc_decompress_rgb_block(const uint8_t *block, uint8_t out[4*4*4]) {
    uint16_t c0_raw = block[0] | (block[1] << 8);
    uint16_t c1_raw = block[2] | (block[3] << 8);
    uint32_t lut = block[4] | (block[5] << 8) | (block[6] << 16) | ((uint32_t)block[7] << 24);

    uint8_t r0, g0, b0, r1, g1, b1;
    atc_rgb555_to_rgb(c0_raw & 0x7FFF, &r0, &g0, &b0);
    atc_rgb565_to_rgb(c1_raw, &r1, &g1, &b1);

    int method = (c0_raw >> 15) & 1;

    /* Build 4-color palette */
    uint8_t palette[4][3];
    if (method == 0) {
        /* Method 0: DXT1-compatible 1/3 + 2/3 interpolation (Adreno 220 hardware) */
        palette[0][0] = r0; palette[0][1] = g0; palette[0][2] = b0;
        palette[1][0] = (2 * r0 + r1 + 1) / 3;
        palette[1][1] = (2 * g0 + g1 + 1) / 3;
        palette[1][2] = (2 * b0 + b1 + 1) / 3;
        palette[2][0] = (r0 + 2 * r1 + 1) / 3;
        palette[2][1] = (g0 + 2 * g1 + 1) / 3;
        palette[2][2] = (b0 + 2 * b1 + 1) / 3;
        palette[3][0] = r1; palette[3][1] = g1; palette[3][2] = b1;
    } else {
        /* Method 1 (hardware-verified on Adreno 220):
         * idx 0: black
         * idx 1: clamp(c0 - c1/4)
         * idx 2: c0
         * idx 3: c1 */
        palette[0][0] = 0; palette[0][1] = 0; palette[0][2] = 0;
        palette[1][0] = atc_clamp(r0 - r1 / 4);
        palette[1][1] = atc_clamp(g0 - g1 / 4);
        palette[1][2] = atc_clamp(b0 - b1 / 4);
        palette[2][0] = r0; palette[2][1] = g0; palette[2][2] = b0;
        palette[3][0] = r1; palette[3][1] = g1; palette[3][2] = b1;
    }

    for (int i = 0; i < 16; i++) {
        int idx = (lut >> (i * 2)) & 3;
        out[i * 4 + 0] = palette[idx][0];
        out[i * 4 + 1] = palette[idx][1];
        out[i * 4 + 2] = palette[idx][2];
        out[i * 4 + 3] = 255;
    }
}

static uint8_t *atc_decompress(uint32_t format, const void *data,
                                int width, int height, int *out_size) {
    int bw = (width + 3) / 4;
    int bh = (height + 3) / 4;
    int has_alpha = (format != ATC_RGB_AMD);
    int block_bytes = has_alpha ? 16 : 8;

    uint8_t *result = (uint8_t *)malloc(width * height * 4);
    if (!result) return NULL;
    memset(result, 255, width * height * 4);

    const uint8_t *src = (const uint8_t *)data;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            uint8_t block_rgba[4 * 4 * 4];
            const uint8_t *block_ptr = src;

            if (has_alpha && format == ATC_RGBA_EXPLICIT_ALPHA_AMD) {
                /* First 8 bytes: explicit 4-bit alpha per pixel */
                atc_decompress_rgb_block(src + 8, block_rgba);
                /* Apply explicit alpha from first 8 bytes */
                for (int i = 0; i < 16; i++) {
                    int byte_idx = i / 2;
                    int nibble = (i & 1) ? (src[byte_idx] >> 4) : (src[byte_idx] & 0xF);
                    block_rgba[i * 4 + 3] = nibble * 17; /* 0..15 → 0..255 */
                }
            } else if (has_alpha && format == ATC_RGBA_INTERPOLATED_ALPHA_AMD) {
                /* DXT5-style interpolated alpha block + ATC RGB */
                uint8_t a0 = src[0], a1 = src[1];
                uint8_t alpha_palette[8];
                alpha_palette[0] = a0;
                alpha_palette[1] = a1;
                if (a0 > a1) {
                    for (int j = 1; j <= 6; j++)
                        alpha_palette[1 + j] = ((7 - j) * a0 + j * a1) / 7;
                } else {
                    for (int j = 1; j <= 4; j++)
                        alpha_palette[1 + j] = ((5 - j) * a0 + j * a1) / 5;
                    alpha_palette[6] = 0;
                    alpha_palette[7] = 255;
                }
                /* 48-bit alpha lookup (3 bits per pixel) */
                uint64_t abits = 0;
                for (int j = 0; j < 6; j++)
                    abits |= (uint64_t)src[2 + j] << (8 * j);
                atc_decompress_rgb_block(src + 8, block_rgba);
                for (int i = 0; i < 16; i++) {
                    int aidx = (abits >> (i * 3)) & 7;
                    block_rgba[i * 4 + 3] = alpha_palette[aidx];
                }
            } else {
                atc_decompress_rgb_block(src, block_rgba);
            }

            /* Copy 4x4 block into output */
            for (int py = 0; py < 4; py++) {
                int dy = by * 4 + py;
                if (dy >= height) break;
                for (int px = 0; px < 4; px++) {
                    int dx = bx * 4 + px;
                    if (dx >= width) break;
                    int si = (py * 4 + px) * 4;
                    int di = (dy * width + dx) * 4;
                    result[di + 0] = block_rgba[si + 0];
                    result[di + 1] = block_rgba[si + 1];
                    result[di + 2] = block_rgba[si + 2];
                    result[di + 3] = block_rgba[si + 3];
                }
            }

            src += block_bytes;
        }
    }

    if (out_size) *out_size = width * height * 4;
    return result;
}

#endif /* PVRTC_DECODE_H */
