/* gfx.c -- texture decode (libpng) and system-font raster (FreeType)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <png.h>
#include <switch.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "gfx.h"
#include "util.h"

// ---------------------------------------------------------------------------
// loadTexture: PNG -> { w, h, ARGB pixels }
// ---------------------------------------------------------------------------

typedef struct {
  const uint8_t *p;
  size_t len, off;
} MemSrc;

static void png_read_mem(png_structp png, png_bytep out, png_size_t n) {
  MemSrc *s = png_get_io_ptr(png);
  if (s->off + n > s->len)
    n = s->len - s->off;
  memcpy(out, s->p + s->off, n);
  s->off += n;
}

int *gfx_load_texture(const void *data, int len, int *out_count) {
  if (!data || len < 8)
    return NULL;

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png)
    return NULL;
  png_infop info = png_create_info_struct(png);
  if (!info) {
    png_destroy_read_struct(&png, NULL, NULL);
    return NULL;
  }

  int *out = NULL;
  png_bytep row = NULL;
  if (setjmp(png_jmpbuf(png))) {
    free(out);
    free(row);
    png_destroy_read_struct(&png, &info, NULL);
    return NULL;
  }

  MemSrc src = { data, (size_t)len, 0 };
  png_set_read_fn(png, &src, png_read_mem);
  png_read_info(png, info);

  const int w = png_get_image_width(png, info);
  const int h = png_get_image_height(png, info);
  const int color = png_get_color_type(png, info);
  const int depth = png_get_bit_depth(png, info);

  // normalise everything to 8-bit RGBA
  if (depth == 16) png_set_strip_16(png);
  if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
  if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
  if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
  if (color == PNG_COLOR_TYPE_RGB || color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_PALETTE)
    png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
  png_read_update_info(png, info);

  out = malloc(((size_t)w * h + 2) * sizeof(int));
  row = malloc((size_t)w * 4);
  if (!out || !row)
    png_longjmp(png, 1);

  out[0] = w;
  out[1] = h;
  int *dst = out + 2;
  for (int y = 0; y < h; y++) {
    png_read_row(png, row, NULL);
    for (int x = 0; x < w; x++) {
      const uint8_t *px = row + x * 4;
      // RGBA bytes -> (A<<24)|(R<<16)|(G<<8)|B (Android getPixels layout)
      dst[(size_t)y * w + x] =
          ((int)px[3] << 24) | ((int)px[0] << 16) | ((int)px[1] << 8) | (int)px[2];
    }
  }

  free(row);
  png_destroy_read_struct(&png, &info, NULL);
  if (out_count)
    *out_count = w * h + 2;
  return out;
}

// ---------------------------------------------------------------------------
// drawFont: UTF-8 text -> { measuredWidth, ARGB pixels } via the shared font
// ---------------------------------------------------------------------------

static FT_Library g_ft;
#define GFX_MAX_FACES 5
static FT_Face g_faces[GFX_MAX_FACES];
static int g_nfaces = 0;
static int g_font_ok = 0;

void gfx_init(void) {
  if (FT_Init_FreeType(&g_ft)) {
    debugPrintf("gfx: FT_Init_FreeType failed\n");
    return;
  }
  // The Standard font has the accented Latin letters but is missing some glyphs
  // (e.g. the Spanish inverted marks and all CJK), so load the whole shared-font
  // set and fall back through it per glyph in gfx_draw_font().
  static const PlSharedFontType types[GFX_MAX_FACES] = {
    PlSharedFontType_Standard,
    PlSharedFontType_ChineseSimplified,
    PlSharedFontType_ChineseTraditional,
    PlSharedFontType_ExtChineseSimplified,
    PlSharedFontType_KO,
  };
  for (int i = 0; i < GFX_MAX_FACES; i++) {
    PlFontData font;
    if (R_FAILED(plGetSharedFontByType(&font, types[i])))
      continue;
    if (!FT_New_Memory_Face(g_ft, font.address, font.size, 0, &g_faces[g_nfaces]))
      g_nfaces++;
  }
  g_font_ok = g_nfaces > 0;
}

// pick the first loaded face that has a glyph for cp (Standard first), sized to
// px; falls back to the primary face (.notdef) when none of them has it
static FT_Face face_for(uint32_t cp, int px) {
  FT_Face face = g_faces[0];
  for (int i = 0; i < g_nfaces; i++)
    if (FT_Get_Char_Index(g_faces[i], cp)) { face = g_faces[i]; break; }
  FT_Set_Pixel_Sizes(face, 0, px);
  return face;
}

// CP1252 0x80-0x9f -> Unicode; 0xa0-0xff are Latin-1 (identity)
static uint32_t cp1252_codepoint(uint8_t c) {
  static const uint16_t ext[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
    0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178
  };
  return (c >= 0x80 && c < 0xa0) ? ext[c - 0x80] : c;
}

// Decode the next character and advance *p. The engine feeds text in mixed
// encodings: whole UTF-8 strings, but also raw single-byte CP1252 (the stock
// OBB's Latin-1 text). So we only take a UTF-8 multi-byte sequence when its
// continuation bytes are valid (0x80-0xbf); otherwise the byte is CP1252. This
// renders genuine UTF-8 and stray CP1252 bytes correctly without guessing.
#define UTF8_CONT(b) (((b) & 0xc0) == 0x80)
static uint32_t utf8_next(const char **p) {
  const unsigned char *s = (const unsigned char *)*p;
  const uint32_t c = s[0];
  if (c >= 0xF0 && UTF8_CONT(s[1]) && UTF8_CONT(s[2]) && UTF8_CONT(s[3])) {
    *p += 4;
    return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
  }
  if (c >= 0xE0 && UTF8_CONT(s[1]) && UTF8_CONT(s[2])) {
    *p += 3;
    return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
  }
  if (c >= 0xC0 && UTF8_CONT(s[1])) {
    *p += 2;
    return ((c & 0x1F) << 6) | (s[1] & 0x3F);
  }
  *p += 1;
  return cp1252_codepoint((uint8_t)c);
}

int *gfx_draw_font(const char *text, int size, int width, int height, int *out_count) {
  if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
    return NULL;

  const size_t npix = (size_t)width * height;
  int *out = calloc(npix + 1, sizeof(int));
  if (!out)
    return NULL;

  if (!g_font_ok || !text) {
    out[0] = 0;
    if (out_count) *out_count = (int)(npix + 1);
    return out;
  }

  const int px = size > 0 ? size : 16;
  // baseline from the primary face so glyphs pulled from fallback faces align
  FT_Set_Pixel_Sizes(g_faces[0], 0, px);
  const int ascender = (int)(g_faces[0]->size->metrics.ascender >> 6);
  int pen_x = 0;
  const int baseline = ascender > 0 && ascender < height ? ascender : (height * 3) / 4;
  int *pix = out + 1; // pixels start at index 1

  const char *p = text;
  while (*p) {
    uint32_t cp = utf8_next(&p);
    FT_Face face = face_for(cp, px);
    if (FT_Load_Char(face, cp, FT_LOAD_RENDER))
      continue;
    FT_GlyphSlot g = face->glyph;
    const int gx = pen_x + g->bitmap_left;
    const int gy = baseline - g->bitmap_top;
    for (unsigned int ry = 0; ry < g->bitmap.rows; ry++) {
      const int dy = gy + (int)ry;
      if (dy < 0 || dy >= height) continue;
      const uint8_t *srow = g->bitmap.buffer + (size_t)ry * g->bitmap.pitch;
      for (unsigned int rx = 0; rx < g->bitmap.width; rx++) {
        const int dx = gx + (int)rx;
        if (dx < 0 || dx >= width) continue;
        const uint8_t a = srow[rx];
        if (!a) continue;
        // white glyph with coverage in alpha; engine tints via glColor
        pix[(size_t)dy * width + dx] = ((int)a << 24) | 0x00FFFFFF;
      }
    }
    pen_x += (int)(g->advance.x >> 6);
  }

  out[0] = pen_x; // measured text width in pixels
  if (out_count)
    *out_count = (int)(npix + 1);
  return out;
}
