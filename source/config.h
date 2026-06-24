/* config.h -- FF3 3D Switch wrapper configuration
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

#define MEMORY_MB 512

#define SO_NAME "libff3.so"
#define OBB_NAME "main.obb"
#define CONFIG_NAME "config.txt"
#define LOG_NAME "ff3_3d_debug.log"

#define DEBUG_LOG 0

extern int screen_width;
extern int screen_height;

// language index (config.language), in the engine's canonical order:
//   0 ja  1 en  2 fr  3 de  4 it  5 es  6 zh_CN  7 zh_TW  8 ko  9 th
#define LANG_EN 1

typedef struct {
  int screen_width;
  int screen_height;
  int widescreen;
  int language;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif

