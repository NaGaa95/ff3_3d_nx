/* obb.h -- FF3 3D encrypted asset archive reader (main.obb)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * main.obb is a Square Enix "ARC1" archive. Every blob (header, table of
 * contents and each file) is XOR'd with an LCG keystream seeded by
 * 0x050EE91A + blobOffset, and the plaintext is [uint32 BE size][gzip stream].
 * The TOC holds <count> 12-byte little-endian entries
 * { nameOffset, fileOffset, fileLength } followed by a NUL-terminated name
 * region. The whole scheme was reverse-engineered from MainActivity and
 * validated against the real file (see obbtest*.py).
 */

#ifndef __OBB_H__
#define __OBB_H__

#include <stddef.h>

// open the archive, parse + decrypt the header and table of contents and keep
// the file handle open for on-demand reads. returns 0 on success.
int obb_open(const char *path);
void obb_close(void);

// returns 1 if a file with this exact archive name exists.
int obb_exists(const char *name);

// decrypt + decompress the named file into a freshly malloc'd buffer. caller
// frees. *out_size receives the decompressed size. returns NULL if the name is
// not in the archive or on any read/decode error.
void *obb_read(const char *name, size_t *out_size);

#endif
