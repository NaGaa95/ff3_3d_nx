/* jni_fake.h -- fake JNI environment for the Cuore engine (libff3.so)
 *
 * The engine caches the JNIEnv + MainActivity class from resume()/render() and
 * drives everything else through static/instance method callbacks
 * (loadFile/loadSound/loadTexture/drawFont, viewport + language queries, save
 * file creation, Google Play / cloud stubs). We provide a functional JNIEnv so
 * those calls resolve to native C implementations.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

#include <stdint.h>

extern void *fake_vm;  // JavaVM *
extern void *fake_env; // JNIEnv *

// set when the engine asks the activity to finish (appEnd/finish)
extern volatile int jni_quit_requested;

void jni_init(void);

// the fake MainActivity instance handed to resume()/render()/etc.
void *jni_make_thiz(void);

// constructors for fake Java objects
void *jni_make_string(const char *utf);
void *jni_make_object(const char *label);

// letterboxed game view rectangle; computed from screen_width/height and the
// configured aspect. main.c uses it for the setViewport() call and the
// getView*/getRes* callbacks report it back to the engine.
extern int jni_view_x, jni_view_y, jni_view_w, jni_view_h;
void jni_compute_viewport(void);

// set the level-triggered button bitmask returned by getKeyEvent()
void jni_set_keys(int mask);
int jni_get_key_mask(void);

// engine's target frame rate for the main-loop cap
int jni_get_target_fps(void);

#endif
