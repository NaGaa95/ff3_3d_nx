/* gfx.h -- texture decode (loadTexture) and system-font raster (drawFont)
 *
 * These replace the Android Bitmap/Canvas work the engine did through Java:
 *   loadTexture(byte[] png)            -> int[] { width, height, ARGB... }
 *   drawFont(text, size, width, height) -> int[] { measuredWidth, ARGB... }
 * pixels are ARGB_8888 in the layout Android Bitmap.getPixels produced,
 * i.e. each int is (A<<24)|(R<<16)|(G<<8)|B.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __GFX_H__
#define __GFX_H__

// must be called once after plInitialize(); loads the Switch shared font.
void gfx_init(void);

// decode a PNG blob into a freshly malloc'd int[] = {w, h, w*h ARGB pixels}.
// *out_count receives the element count (w*h + 2). returns NULL on failure.
int *gfx_load_texture(const void *png, int len, int *out_count);

// render UTF-8 text with the shared system font into a width x height ARGB
// bitmap. returns a fresh int[] = {measuredTextWidth, width*height ARGB},
// *out_count = width*height + 1. returns NULL on failure.
int *gfx_draw_font(const char *text, int size, int width, int height, int *out_count);

#endif
