/* jni_fake.c -- fake JNI environment for the Cuore engine (libff3.so)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "jni_fake.h"
#include "obb.h"
#include "gfx.h"
#include "movie_player.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

// the engine renders a 3:2 (480x320) scene by default; widescreen uses 16:9
#define GAME_ASPECT_W 480
#define GAME_ASPECT_H 320
#define WIDE_ASPECT_W 16
#define WIDE_ASPECT_H 9

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'  heap object (freeable)
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_ID     = 0x4d494431, // 'MID1'  pooled, never freed
  TAG_CLASS  = 0x434c5331, // 'CLS1'  singleton, never freed
};

typedef struct { uint32_t tag; char label[64]; } FakeObject;
typedef struct { uint32_t tag; char *utf; } FakeString;
typedef struct { uint32_t tag; int len; void **items; } FakeObjArray;
typedef struct { uint32_t tag; int len; int elem_size; void *data; } FakePriArray;
typedef struct { uint32_t tag; char name[64]; char sig[96]; } FakeID;

volatile int jni_quit_requested = 0;
int jni_view_x = 0, jni_view_y = 0, jni_view_w = 1280, jni_view_h = 720;

// ---------------------------------------------------------------------------
// local reference registry: native code that never returns to Java must free
// the refs it creates (or leak them). the engine brackets its JNI use with
// DeleteLocalRef / Push+PopLocalFrame, which we honour here so the thousands
// of loadFile/texture arrays don't accumulate.
// ---------------------------------------------------------------------------

#define MAX_LOCALS 16384
#define MAX_FRAMES 64
static void *locals[MAX_LOCALS];
static int locals_top = 0;
static int frames[MAX_FRAMES];
static int frame_top = 0;
// the engine loads assets from worker threads, so the registry is shared state
static Mutex locals_lock;

static void *reg_local(void *ref) {
  if (ref) {
    mutexLock(&locals_lock);
    if (locals_top < MAX_LOCALS)
      locals[locals_top++] = ref;
    else
      debugPrintf("JNI: local ref table full, leaking %p\n", ref);
    mutexUnlock(&locals_lock);
  }
  return ref;
}

static void free_ref(void *ref) {
  if (!ref)
    return;
  switch (*(uint32_t *)ref) {
    case TAG_STRING: { FakeString *s = ref; free(s->utf); free(s); break; }
    case TAG_PRIARR: { FakePriArray *a = ref; free(a->data); free(a); break; }
    case TAG_OBJARR: { FakeObjArray *a = ref; free(a->items); free(a); break; }
    case TAG_OBJECT: free(ref); break;
    default: break; // TAG_ID / TAG_CLASS are never freed
  }
}

static void delete_local(void *ref) {
  if (!ref)
    return;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--) {
    if (locals[i] == ref) {
      locals[i] = locals[--locals_top];
      free_ref(ref);
      break;
    }
  }
  mutexUnlock(&locals_lock);
}

// ---------------------------------------------------------------------------
// fake object constructors (all register as local refs unless noted)
// ---------------------------------------------------------------------------

void *jni_make_object(const char *label) {
  FakeObject *o = calloc(1, sizeof(*o));
  o->tag = TAG_OBJECT;
  strncpy(o->label, label, sizeof(o->label) - 1);
  return reg_local(o);
}

void *jni_make_string(const char *utf) {
  FakeString *s = calloc(1, sizeof(*s));
  s->tag = TAG_STRING;
  s->utf = strdup(utf ? utf : "");
  return reg_local(s);
}

// adopt an existing buffer as a primitive array (no copy)
static void *make_pri_array_adopt(void *data, int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = len;
  a->elem_size = elem_size;
  a->data = data;
  return reg_local(a);
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING)
    return s->utf;
  return "";
}

static char g_edit_text[256] = "";

static int show_switch_keyboard(const char *initial) {
  SwkbdConfig swkbd;
  char out[sizeof(g_edit_text)];
  const char *start = initial ? initial : "";

  snprintf(out, sizeof(out), "%s", start);
  g_edit_text[0] = 0;

  Result rc = swkbdCreate(&swkbd, 0);
  if (R_FAILED(rc)) {
    debugPrintf("swkbdCreate failed: 0x%x\n", rc);
    return 0;
  }

  swkbdConfigMakePresetDefault(&swkbd);
  swkbdConfigSetHeaderText(&swkbd, "Enter text");
  swkbdConfigSetOkButtonText(&swkbd, "OK");
  swkbdConfigSetInitialText(&swkbd, out);
  swkbdConfigSetStringLenMax(&swkbd, (u32)sizeof(g_edit_text) - 1);

  rc = swkbdShow(&swkbd, out, sizeof(out));
  swkbdClose(&swkbd);

  if (R_SUCCEEDED(rc)) {
    snprintf(g_edit_text, sizeof(g_edit_text), "%s", out);
    return 1;
  }

  return 0;
}

// singletons (never freed)
static FakeObject *g_class = NULL;  // MainActivity class
static FakeObject *g_thiz = NULL;   // MainActivity instance

void *jni_make_thiz(void) {
  if (!g_thiz) {
    g_thiz = calloc(1, sizeof(*g_thiz));
    g_thiz->tag = TAG_CLASS;
    strcpy(g_thiz->label, "MainActivity");
  }
  return g_thiz;
}

static void *get_class(void) {
  if (!g_class) {
    g_class = calloc(1, sizeof(*g_class));
    g_class->tag = TAG_CLASS;
    strcpy(g_class->label, "MainActivityClass");
  }
  return g_class;
}

// ---------------------------------------------------------------------------
// method/field ID pool
// ---------------------------------------------------------------------------

#define MAX_IDS 256
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *name, const char *sig) {
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig))
      return &id_pool[i];
  if (id_count >= MAX_IDS) {
    debugPrintf("JNI: id pool exhausted (%s)\n", name);
    return &id_pool[0];
  }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->name, name, sizeof(id->name) - 1);
  strncpy(id->sig, sig, sizeof(id->sig) - 1);
  return id;
}

// ---------------------------------------------------------------------------
// viewport: letterbox the selected game aspect inside the screen
// ---------------------------------------------------------------------------

void jni_compute_viewport(void) {
  const int aw = config.widescreen ? WIDE_ASPECT_W : GAME_ASPECT_W;
  const int ah = config.widescreen ? WIDE_ASPECT_H : GAME_ASPECT_H;
  const float ga = (float)aw / (float)ah;
  const float sa = (float)screen_width / (float)screen_height;
  if (sa >= ga) {
    jni_view_h = screen_height;
    jni_view_w = (int)(screen_height * ga + 0.5f);
    jni_view_x = (screen_width - jni_view_w) / 2;
    jni_view_y = 0;
  } else {
    jni_view_w = screen_width;
    jni_view_h = (int)(screen_width / ga + 0.5f);
    jni_view_x = 0;
    jni_view_y = (screen_height - jni_view_h) / 2;
  }
}

// ---------------------------------------------------------------------------
// input: getKeyEvent() returns a level-triggered button bitmask (field "g").
// the bit values come from MainActivity's keycode->bit table (see extract_E.py).
// ---------------------------------------------------------------------------

static volatile int g_key_mask = 0;

void jni_set_keys(int mask) {
  g_key_mask = mask;
}

int jni_get_key_mask(void) {
  return g_key_mask;
}

// engine target frame rate (set via setFPS); used by getCurrentFrame's limiter
static volatile int g_target_fps = 30;

int jni_get_target_fps(void) {
  if (g_target_fps <= 0)
    return 30;
  return g_target_fps < 30 ? g_target_fps : 30;
}

// getCurrentFrame(prev) returns a monotonic FRAME INDEX, not milliseconds:
// render() runs clamp(now - prev, 1, 3) game-logic ticks per frame, so this must
// advance by exactly 1 per call (1 tick/render). Returning a real ms clock made
// the delta ~33 -> clamped to 3 and ran the whole game several times too fast.
static uint64_t get_current_frame(int64_t prev) {
  return (uint64_t)(prev + 1);
}

// ---------------------------------------------------------------------------
// data loading helpers (loadFile/loadSound -> OBB)
// ---------------------------------------------------------------------------

// Reads stock OBB assets.
static int has_suffix(const char *s, const char *suffix) {
  const size_t sl = strlen(s);
  const size_t tl = strlen(suffix);
  return sl >= tl && !strcmp(s + sl - tl, suffix);
}

static int read_disk_file(const char *path, void **out, size_t *out_size) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 0;
  }
  long n = ftell(f);
  if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return 0;
  }
  uint8_t *buf = malloc((size_t)n ? (size_t)n : 1);
  if (!buf) {
    fclose(f);
    return 0;
  }
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    free(buf);
    return 0;
  }
  fclose(f);
  *out = buf;
  *out_size = (size_t)n;
  return 1;
}

static int asset_read(const char *path, void **out, size_t *out_size) {
  *out = obb_read(path, out_size);
  if (*out)
    return 1;
  return read_disk_file(path, out, out_size);
}

static int asset_exists(const char *path) {
  if (obb_exists(path))
    return 1;
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
  fclose(f);
  return 1;
}

// ---------------------------------------------------------------------------
// MSD text re-encoding. The engine parses message text as UTF-8 and silently
// drops bytes 0x80-0xbf (UTF-8 continuation bytes) that appear without a lead,
// which is where CP1252 punctuation like the Spanish inverted marks (0xbf, 0xa1)
// lives -- so those would vanish. The stock data mixes encodings (some strings
// are CP1252, some already UTF-8), so we re-emit each string as UTF-8: copy it
// verbatim if it is already valid UTF-8, otherwise treat it as CP1252.
// ---------------------------------------------------------------------------

static uint32_t rd_le32_buf(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_le32_buf(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t cp1252_codepoint(uint8_t c) {
  static const uint16_t ext[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
    0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178
  };
  return (c >= 0x80 && c < 0xa0) ? ext[c - 0x80] : c;
}

static int is_valid_utf8(const uint8_t *src, size_t len) {
  for (size_t i = 0; i < len; i++) {
    uint8_t c = src[i];
    if (c < 0x80)
      continue;
    size_t need;
    if ((c & 0xe0) == 0xc0) { need = 1; if ((c & 0x1e) == 0) return 0; }
    else if ((c & 0xf0) == 0xe0) need = 2;
    else if ((c & 0xf8) == 0xf0) need = 3;
    else return 0;
    if (i + need >= len) return 0;
    for (size_t j = 0; j < need; j++)
      if ((src[++i] & 0xc0) != 0x80) return 0;
  }
  return 1;
}

static int reserve_bytes(uint8_t **out, size_t *cap, size_t need) {
  if (need <= *cap)
    return 1;
  size_t next = *cap ? *cap * 2 : 256;
  while (next < need)
    next *= 2;
  uint8_t *tmp = realloc(*out, next);
  if (!tmp)
    return 0;
  *out = tmp;
  *cap = next;
  return 1;
}

static int append_utf8_codepoint(uint8_t **out, size_t *pos, size_t *cap, uint32_t cp) {
  if (!reserve_bytes(out, cap, *pos + 3))
    return 0;
  if (cp < 0x80) {
    (*out)[(*pos)++] = (uint8_t)cp;
  } else if (cp < 0x800) {
    (*out)[(*pos)++] = (uint8_t)(0xc0 | (cp >> 6));
    (*out)[(*pos)++] = (uint8_t)(0x80 | (cp & 0x3f));
  } else {
    (*out)[(*pos)++] = (uint8_t)(0xe0 | (cp >> 12));
    (*out)[(*pos)++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
    (*out)[(*pos)++] = (uint8_t)(0x80 | (cp & 0x3f));
  }
  return 1;
}

// Append a source string as UTF-8: verbatim if already valid UTF-8, else CP1252.
static int append_text(uint8_t **out, size_t *pos, size_t *cap, const uint8_t *src, size_t len) {
  if (is_valid_utf8(src, len)) {
    if (!reserve_bytes(out, cap, *pos + len))
      return 0;
    memcpy(*out + *pos, src, len);
    *pos += len;
    return 1;
  }
  for (size_t i = 0; i < len; i++)
    if (!append_utf8_codepoint(out, pos, cap, cp1252_codepoint(src[i])))
      return 0;
  return 1;
}

// Rebuild an "MSDA" message archive with every string re-emitted as UTF-8.
static void *decode_msd(uint8_t *buf, size_t size, size_t *out_size) {
  if (size < 16 || memcmp(buf, "MSDA", 4))
    return NULL;

  uint32_t count = rd_le32_buf(buf + 8);
  size_t header = 16 + (size_t)count * 12;
  if (header > size)
    return NULL;

  size_t cap = size * 4 + 16;
  uint8_t *out = malloc(cap ? cap : 1);
  if (!out)
    return NULL;
  memcpy(out, buf, header);
  size_t dst = header;

  for (uint32_t i = 0; i < count; i++) {
    size_t entry = 16 + (size_t)i * 12;
    uint32_t strings = buf[entry + 4];
    size_t src = rd_le32_buf(buf + entry + 8);
    wr_le32_buf(out + entry + 8, (uint32_t)dst);
    for (uint32_t j = 0; j < strings; j++) {
      if (src >= size)
        goto fail;
      size_t end = src;
      while (end < size && buf[end])
        end++;
      if (end >= size)
        goto fail;
      if (!append_text(&out, &dst, &cap, buf + src, end - src))
        goto fail;
      if (!reserve_bytes(&out, &cap, dst + 1))
        goto fail;
      out[dst++] = 0;
      src = end + 1;
    }
    if (!reserve_bytes(&out, &cap, dst + 1))
      goto fail;
    out[dst++] = 0;
  }

  *out_size = dst;
  return out;

fail:
  free(out);
  return NULL;
}

static int should_decode_msd(const char *name) {
  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;
  return has_suffix(base, ".msd") && strncmp(base, "eoa", 3);
}

static int load_file_bytes(const char *name, void **buf, size_t *size) {
  if (!name)
    return 0;
  if (!strncmp(name, "rom:/", 5)) name += 5;
  while (name[0] == '/' || (name[0] == '.' && name[1] == '/'))
    name += (name[0] == '/') ? 1 : 2;

  // engine language order; index must match getLanguage()/setLanguage()
  static const char *langs[] = {
    "ja", "en", "fr", "de", "it", "es", "zh_CN", "zh_TW", "ko", "th"
  };
  const int li = (config.language >= 0 && config.language < 10) ? config.language : LANG_EN;
  char path[512];

  snprintf(path, sizeof(path), "%s.lproj/%s", langs[li], name);
  if (asset_read(path, buf, size))
    return 1;
  if (li != LANG_EN) {
    snprintf(path, sizeof(path), "en.lproj/%s", name);
    if (asset_read(path, buf, size))
      return 1;
  }
  snprintf(path, sizeof(path), "files/%s", name);
  if (asset_read(path, buf, size))
    return 1;
  return asset_read(name, buf, size);
}

static void *load_file_array(const char *name) {
  void *buf = NULL;
  size_t size = 0;
  if (!load_file_bytes(name, &buf, &size))
    return NULL;
  if (should_decode_msd(name)) {
    size_t decoded_size = 0;
    void *decoded = decode_msd(buf, size, &decoded_size);
    if (decoded) {
      free(buf);
      buf = decoded;
      size = decoded_size;
    }
  }
  return make_pri_array_adopt(buf, (int)size, 1);
}

static void ff3_sound_base(const char *name, char *base, size_t base_size) {
  if (!strncmp(name, "voice/", 6)) {
    snprintf(base, base_size, "%s", name + 6);
  } else if (has_suffix(name, ".akb")) {
    snprintf(base, base_size, "%s", name);
  } else {
    snprintf(base, base_size, "%s.akb", name);
  }
}

static int ff3_read_sound_candidate(const char *name, void **buf, size_t *size) {
  if (!name || !name[0])
    return 0;

  char base[256];
  char path[512];
  static const char *dirs[] = { "BGM", "SE", "VOICE" };
  ff3_sound_base(name, base, sizeof(base));

  snprintf(path, sizeof(path), "sound/%s", base);
  if (asset_read(path, buf, size))
    return 1;

  for (unsigned i = 0; i < sizeof(dirs) / sizeof(*dirs); i++) {
    snprintf(path, sizeof(path), "files/SOUND/%s/%s", dirs[i], base);
    if (asset_read(path, buf, size))
      return 1;
  }

  return asset_read(name, buf, size);
}

static int ff3_sound_exists(const char *name) {
  if (!name || !name[0])
    return 0;

  char base[256];
  char path[512];
  static const char *dirs[] = { "BGM", "SE", "VOICE" };
  ff3_sound_base(name, base, sizeof(base));

  snprintf(path, sizeof(path), "sound/%s", base);
  if (asset_exists(path))
    return 1;
  for (unsigned i = 0; i < sizeof(dirs) / sizeof(*dirs); i++) {
    snprintf(path, sizeof(path), "files/SOUND/%s/%s", dirs[i], base);
    if (asset_exists(path))
      return 1;
  }
  return asset_exists(name);
}

static void *load_sound_array(const char *name) {
  if (!name)
    return NULL;
  size_t size = 0;
  void *buf = NULL;
  if (!ff3_read_sound_candidate(name, &buf, &size))
    return NULL;
  return make_pri_array_adopt(buf, (int)size, 1);
}

// ---------------------------------------------------------------------------
// MainActivity method dispatch (by name; static and instance share handlers)
// ---------------------------------------------------------------------------

static const char *save_dir(void);
static void ensure_save_file(const char *name, int size);

static juint call_int(const char *name, va_list va) {
  if (!strcmp(name, "getLanguage"))   return (juint)config.language;
  if (!strcmp(name, "getLanguage2"))  return (juint)config.language;
  if (!strcmp(name, "getResWidth"))   return (juint)screen_width;
  if (!strcmp(name, "getResHeight"))  return (juint)screen_height;
  if (!strcmp(name, "getViewWidth"))  return (juint)jni_view_w;
  if (!strcmp(name, "getViewHeight")) return (juint)jni_view_h;
  if (!strcmp(name, "getViewPosX"))   return (juint)jni_view_x;
  if (!strcmp(name, "getViewPosY"))   return (juint)jni_view_y;
  if (!strcmp(name, "getKeyEvent"))
    return (juint)g_key_mask;
  if (!strcmp(name, "checkIncentive")) return 0;
  if (!strcmp(name, "isOKAchievement")) return 0;
  if (!strcmp(name, "isSoundFileExist")) {
    const char *snd = obj_str(va_arg(va, void *));
    return (juint)ff3_sound_exists(snd);
  }
  if (!strcmp(name, "getDeviceAndroidTV") ||
      !strcmp(name, "isDeviceAndroidTV") ||
      !strcmp(name, "getDownloadState") ||
      !strcmp(name, "GetAutoLoginRefusal"))
    return 0;
  if (!strcmp(name, "getMovieState"))
    return (juint)movie_is_playing();
  if (!strcmp(name, "getSoundState") || !strcmp(name, "showingDialog") || !strcmp(name, "checkEditText"))
    return 0;
  (void)va;
  return 0;
}

static juint call_long(const char *name, va_list va) {
  if (!strcmp(name, "getCurrentFrame")) {
    const int64_t prev = va_arg(va, int64_t);
    return (juint)get_current_frame(prev);
  }
  (void)va;
  return 0;
}

static void *byte_array_from_string(const char *s) {
  const int len = (int)strlen(s);
  char *buf = malloc(len ? len : 1);
  if (!buf)
    return NULL;
  memcpy(buf, s, len);
  return make_pri_array_adopt(buf, len, 1);
}

static void *call_object(const char *name, va_list va) {
  if (!strcmp(name, "loadFile") || !strcmp(name, "loadRawFile")) {
    const char *path = obj_str(va_arg(va, void *));
    void *arr = load_file_array(path);
    if (!arr) debugPrintf("JNI: loadFile(%s) -> NULL\n", path);
    return arr;
  }
  if (!strcmp(name, "loadSound")) {
    const char *snd = obj_str(va_arg(va, void *));
    void *arr = load_sound_array(snd);
    if (!arr) debugPrintf("JNI: loadSound(%s) -> null\n", snd);
    return arr;
  }
  if (!strcmp(name, "loadTexture")) {
    FakePriArray *in = va_arg(va, void *);
    if (!in || in->tag != TAG_PRIARR)
      return NULL;
    int count = 0;
    int *pix = gfx_load_texture(in->data, in->len, &count);
    return pix ? make_pri_array_adopt(pix, count, 4) : NULL;
  }
  if (!strcmp(name, "drawFont")) {
    // The engine expects text metrics before the ARGB pixels.
    const char *text = obj_str(va_arg(va, void *));
    const int dim = va_arg(va, int);
    const int fsize = va_arg(va, int);
    (void)va_arg(va, int); // baseline/descent hint, unused
    int count = 0;
    int *pix = gfx_draw_font(text, fsize, dim, dim, &count);
    if (!pix)
      return NULL;
    const int pixels = dim * dim;
    int *font_pix = calloc((size_t)pixels + 5, sizeof(*font_pix));
    if (!font_pix) {
      free(pix);
      return NULL;
    }
    font_pix[0] = pix[0] < 0 ? 0 : pix[0];
    font_pix[1] = 0;
    font_pix[2] = 0;
    font_pix[3] = 0;
    font_pix[4] = fsize;
    int copy_pixels = count > 1 ? count - 1 : 0;
    if (copy_pixels > pixels)
      copy_pixels = pixels;
    if (copy_pixels > 0)
      memcpy(font_pix + 5, pix + 1, (size_t)copy_pixels * sizeof(*font_pix));
    free(pix);
    return make_pri_array_adopt(font_pix, pixels + 5, 4);
  }
  if (!strcmp(name, "getSaveFileName")) {
    // the engine treats this as the save DIRECTORY and appends "/save.bin" etc.
    const char *dir = save_dir();
    const int len = (int)strlen(dir);
    char *buf = malloc(len ? len : 1);
    memcpy(buf, dir, len);
    return make_pri_array_adopt(buf, len, 1);
  }
  if (!strcmp(name, "getStoragePath")) {
    const char *dir = save_dir();
    return byte_array_from_string(dir);
  }
  if (!strcmp(name, "getSaveDataPath")) {
    char path[512];
    snprintf(path, sizeof(path), "%s/save.bin", save_dir());
    return byte_array_from_string(path);
  }
  if (!strcmp(name, "getEditText"))
    return jni_make_string(g_edit_text);
  if (!strcmp(name, "createEditText")) {
    // pass the field's current text so the keyboard opens pre-filled with it
    const char *initial = obj_str(va_arg(va, void *));
    show_switch_keyboard(initial);
    return jni_make_string(g_edit_text);
  }
  if (!strcmp(name, "getErrorMessage"))
    return jni_make_string("");
  (void)va;
  return NULL;
}

// the save directory the engine appends filenames to (the game folder / CWD)
static const char *save_dir(void) {
  static char dir[256];
  if (!dir[0]) {
    if (!getcwd(dir, sizeof(dir)) || !dir[0])
      strcpy(dir, ".");
    // strip any trailing slash; the engine adds its own
    size_t n = strlen(dir);
    if (n > 1 && dir[n - 1] == '/')
      dir[n - 1] = 0;
  }
  return dir;
}

// create a fixed-size save file (zero-filled) if it doesn't already exist,
// so the engine's subsequent "r+b" open + random-access read/write succeeds
static void ensure_save_file(const char *name, int size) {
  FILE *f = fopen(name, "rb");
  if (f) { fclose(f); return; } // keep existing saves intact
  f = fopen(name, "wb");
  if (!f) { debugPrintf("ensure_save_file: cannot create %s\n", name); return; }
  static const char zeros[4096] = { 0 };
  for (int rem = size; rem > 0; ) {
    int n = rem > (int)sizeof(zeros) ? (int)sizeof(zeros) : rem;
    fwrite(zeros, 1, n, f);
    rem -= n;
  }
  fclose(f);
}

static void call_void(const char *name, va_list va) {
  if (!strcmp(name, "setFPS"))            { g_target_fps = va_arg(va, int); return; }
  if (!strcmp(name, "setLanguage"))       { config.language = va_arg(va, int); return; }
  if (!strcmp(name, "assignBackButton"))  { (void)va_arg(va, int); return; }
  if (!strcmp(name, "updateViewportSize")) { jni_compute_viewport(); return; }
  if (!strcmp(name, "trace"))             { (void)obj_str(va_arg(va, void *)); return; }
  if (!strcmp(name, "openMovie") || !strcmp(name, "playMovie")) {
    movie_play("opening.mp4", 1);
    return;
  }
  if (!strcmp(name, "stopMovie") || !strcmp(name, "closeMovie")) {
    movie_stop();
    return;
  }
  if (!strcmp(name, "pauseMovie")) {
    movie_pause();
    return;
  }
  if (!strcmp(name, "resumeMovie")) {
    movie_resume();
    return;
  }
  if (!strcmp(name, "createEditText")) {
    const char *initial = obj_str(va_arg(va, void *));
    show_switch_keyboard(initial);
    return;
  }
  if (!strcmp(name, "deleteEditText") || !strcmp(name, "closeEditText"))
    return;
  if (!strcmp(name, "setDeviceAndroidTV") ||
      !strcmp(name, "setMovieState") ||
      !strcmp(name, "playSound") ||
      !strcmp(name, "pauseSound") ||
      !strcmp(name, "stopSound") ||
      !strcmp(name, "setSoundVolume") ||
      !strcmp(name, "openStore") ||
      !strcmp(name, "sendBroadcast") ||
      !strcmp(name, "setAutoLoginRefusal"))
    return;
  if (!strcmp(name, "createSaveFile")) {
    const int sz = va_arg(va, int);
    ensure_save_file("save.bin", sz);
    return;
  }
  if (!strcmp(name, "createAchieveFile")) {
    const int sz = va_arg(va, int);
    ensure_save_file("report_achi.bin", sz);
    return;
  }
  if (!strcmp(name, "createFile")) {
    const char *p = obj_str(va_arg(va, void *));
    const int sz = va_arg(va, int);
    ensure_save_file(p, sz);
    return;
  }
  if (!strcmp(name, "appEnd") || !strcmp(name, "finish")) {
    jni_quit_requested = 1;
    return;
  }
  (void)va;
}

// ---------------------------------------------------------------------------
// JNIEnv function implementations
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }
static void *j_FindClass(void *env, const char *name) { (void)env; (void)name; return get_class(); }
static void *j_GetObjectClass(void *env, void *obj) { (void)env; (void)obj; return get_class(); }
static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; (void)cls; return get_id(name, sig);
}

static void *j_NewGlobalRef(void *env, void *obj) {
  (void)env;
  // promote out of the local table so it survives frame pops
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--)
    if (locals[i] == obj) { locals[i] = locals[--locals_top]; break; }
  mutexUnlock(&locals_lock);
  return obj;
}
static void j_DeleteGlobalRef(void *env, void *obj) { (void)env; free_ref(obj); }
static void j_DeleteLocalRef(void *env, void *obj) { (void)env; delete_local(obj); }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static juint j_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }
static juint j_EnsureLocalCapacity(void *env, int cap) { (void)env; (void)cap; return 0; }

static juint j_PushLocalFrame(void *env, int cap) {
  (void)env; (void)cap;
  mutexLock(&locals_lock);
  if (frame_top < MAX_FRAMES)
    frames[frame_top++] = locals_top;
  mutexUnlock(&locals_lock);
  return 0;
}
static void *j_PopLocalFrame(void *env, void *result) {
  (void)env;
  mutexLock(&locals_lock);
  const int mark = frame_top > 0 ? frames[--frame_top] : 0;
  for (int i = mark; i < locals_top; i++)
    if (locals[i] != result)
      free_ref(locals[i]);
  locals_top = mark;
  if (result && locals_top < MAX_LOCALS)
    locals[locals_top++] = result; // re-register in the parent frame
  mutexUnlock(&locals_lock);
  return result;
}

// --- Call<type>Method (instance + static share name dispatch) ---------------

#define CALL_VARIADIC(fn, ret_t, dispatch) \
  static ret_t fn(void *env, void *recv, FakeID *id, ...) { \
    (void)env; (void)recv; va_list va; va_start(va, id); \
    ret_t r = dispatch(id->name, va); va_end(va); return r; } \
  static ret_t fn##V(void *env, void *recv, FakeID *id, va_list va) { \
    (void)env; (void)recv; return dispatch(id->name, va); }

CALL_VARIADIC(j_CallObjectMethod, void *, call_object)
CALL_VARIADIC(j_CallIntMethod, juint, call_int)
CALL_VARIADIC(j_CallBooleanMethod, juint, call_int)
CALL_VARIADIC(j_CallLongMethod, juint, call_long)

static void j_CallVoidMethod(void *env, void *recv, FakeID *id, ...) {
  (void)env; (void)recv; va_list va; va_start(va, id); call_void(id->name, va); va_end(va);
}
static void j_CallVoidMethodV(void *env, void *recv, FakeID *id, va_list va) {
  (void)env; (void)recv; call_void(id->name, va);
}

// static variants reuse the same dispatchers
#define j_CallStaticObjectMethod   j_CallObjectMethod
#define j_CallStaticObjectMethodV  j_CallObjectMethodV
#define j_CallStaticIntMethod      j_CallIntMethod
#define j_CallStaticIntMethodV     j_CallIntMethodV
#define j_CallStaticBooleanMethod  j_CallBooleanMethod
#define j_CallStaticBooleanMethodV j_CallBooleanMethodV
#define j_CallStaticLongMethod     j_CallLongMethod
#define j_CallStaticLongMethodV    j_CallLongMethodV
#define j_CallStaticVoidMethod     j_CallVoidMethod
#define j_CallStaticVoidMethodV    j_CallVoidMethodV

// --- strings ----------------------------------------------------------------

static void *j_NewStringUTF(void *env, const char *utf) { (void)env; return jni_make_string(utf); }
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0; return obj_str(jstr);
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) { (void)env; (void)jstr; (void)utf; }
static juint j_GetStringUTFLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }
static juint j_GetStringLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }

// --- arrays -----------------------------------------------------------------

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr; // len is at the same offset in FakeObjArray/FakePriArray
  if (a && (a->tag == TAG_PRIARR || a->tag == TAG_OBJARR))
    return a->len;
  return 0;
}

static void *new_pri_array(int len, int elem_size) {
  void *data = calloc(len ? len : 1, elem_size);
  return make_pri_array_adopt(data, len, elem_size);
}
static void *j_NewByteArray(void *env, int len) { (void)env; return new_pri_array(len, 1); }
static void *j_NewIntArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
static void *j_NewFloatArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  return (a && a->tag == TAG_PRIARR) ? a->data : NULL;
}
static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + (size_t)start * a->elem_size, (size_t)len * a->elem_size);
}
static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + (size_t)start * a->elem_size, buf, (size_t)len * a->elem_size);
}

// --- misc -------------------------------------------------------------------

static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env; (void)cls; (void)methods; (void)n;
  return 0;
}
static juint j_GetJavaVM(void *env, void **vm) { (void)env; *vm = fake_vm; return JNI_OK; }
static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void j_void1(void *env) { (void)env; }
static juint j_unimplemented(void) {
  debugPrintf("JNI: unimplemented slot called\n");
  return 0;
}

// ---------------------------------------------------------------------------
// table assembly (indices per the JNI specification)
// ---------------------------------------------------------------------------

static void *env_table[233];
static void **env_table_ptr = env_table;
void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK;
}
static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  mutexInit(&locals_lock);

  for (int i = 0; i < 233; i++)
    env_table[i] = (void *)j_unimplemented;

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void1; // ExceptionDescribe
  env_table[17]  = (void *)j_void1; // ExceptionClear
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteGlobalRef;
  env_table[23]  = (void *)j_DeleteLocalRef;
  env_table[24]  = (void *)j_IsSameObject;
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_EnsureLocalCapacity;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[53]  = (void *)j_CallLongMethod;
  env_table[54]  = (void *)j_CallLongMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  env_table[113] = (void *)j_GetMethodID;            // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[132] = (void *)j_CallStaticLongMethod;
  env_table[133] = (void *)j_CallStaticLongMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  env_table[144] = (void *)j_GetMethodID;            // GetStaticFieldID
  env_table[164] = (void *)j_GetStringLength;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[176] = (void *)j_NewByteArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[181] = (void *)j_NewFloatArray;
  for (int i = 183; i <= 190; i++) env_table[i] = (void *)j_GetPriArrayElements;
  for (int i = 191; i <= 198; i++) env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++) env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++) env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[219] = (void *)j_GetJavaVM;
  env_table[222] = (void *)j_GetPriArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePriArrayElements; // ReleasePrimitiveArrayCritical
  env_table[226] = (void *)j_NewGlobalRef;            // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteGlobalRef;         // DeleteWeakGlobalRef
  env_table[228] = (void *)j_ExceptionCheck;

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread;

}


