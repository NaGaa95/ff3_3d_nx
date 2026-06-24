/* movie_player.c -- FFmpeg + GLES1 movie overlay for FF3 3D */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <threads.h>
#include <switch.h>

#include <EGL/egl.h>
#include <GLES/gl.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/version.h>

#include "config.h"
#include "util.h"
#include "movie_player.h"
#include "opensles.h"

#define NUM_VFRAMES 8
#define AUDIO_PREFILL_MS 120

typedef struct {
  uint8_t *rgba;
  double pts;
} VideoFrame;

static struct {
  volatile int active;
  volatile int paused;
  volatile int stop_req;
  volatile int decode_eof;
  int skippable;

  thrd_t thread;
  int thread_running;

  AVFormatContext *fmt;
  AVCodecContext *vctx;
  AVCodecContext *actx;
  struct SwsContext *sws;
  SwrContext *swr;
  int vstream;
  int astream;
  double video_tb;
  int width;
  int height;

  VideoFrame frames[NUM_VFRAMES];
  int frame_read;
  int frame_write;
  int frame_count;
  mtx_t lock;
  cnd_t can_produce;

  int has_audio;
  int audio_rate;
  int audio_primed;
  u64 tail_tick;
  double tail_base;

  u64 start_tick;
  u64 pause_tick;
  u64 paused_ticks;
  int frame_shown;
} mp;

static struct {
  GLuint tex;
  int tex_w;
  int tex_h;
  int ready;
} glmovie;

static volatile unsigned int movie_swaps = 0;

static double wall_clock(void) {
  const u64 now = mp.paused ? mp.pause_tick : armGetSystemTick();
  return (double)(now - mp.start_tick - mp.paused_ticks) / (double)armGetSystemTickFreq();
}

static double movie_clock(void) {
  if (!mp.has_audio)
    return wall_clock();

  mtx_lock(&mp.lock);
  const int primed = mp.audio_primed || mp.decode_eof;
  mtx_unlock(&mp.lock);

  if (!primed)
    return -1.0;

  double clk = (double)opensles_movie_samples_played() / (double)mp.audio_rate;

  if (mp.decode_eof && opensles_movie_buffered_frames() == 0) {
    const u64 now = armGetSystemTick();
    if (!mp.tail_tick) {
      mp.tail_tick = now;
      mp.tail_base = clk;
    }
    clk = mp.tail_base + (double)(now - mp.tail_tick) / (double)armGetSystemTickFreq();
  }
  return clk;
}

static int open_codec(AVCodecContext **out, AVStream *stream, int threads) {
  const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec)
    return -1;
  AVCodecContext *ctx = avcodec_alloc_context3(codec);
  if (!ctx)
    return -1;
  if (avcodec_parameters_to_context(ctx, stream->codecpar) < 0 ||
      (ctx->thread_count = threads, avcodec_open2(ctx, codec, NULL)) < 0) {
    avcodec_free_context(&ctx);
    return -1;
  }
  *out = ctx;
  return 0;
}

static void push_video_frame(const AVFrame *frm) {
  mtx_lock(&mp.lock);
  while (mp.frame_count == NUM_VFRAMES && !mp.stop_req)
    cnd_wait(&mp.can_produce, &mp.lock);
  if (mp.stop_req) {
    mtx_unlock(&mp.lock);
    return;
  }

  VideoFrame *slot = &mp.frames[mp.frame_write];
  uint8_t *dst[4] = { slot->rgba, NULL, NULL, NULL };
  int dst_stride[4] = { mp.width * 4, 0, 0, 0 };
  sws_scale(mp.sws, (const uint8_t * const *)frm->data, frm->linesize,
      0, mp.height, dst, dst_stride);

  int64_t ts = frm->best_effort_timestamp;
  if (ts == AV_NOPTS_VALUE)
    ts = frm->pts;
  slot->pts = (ts == AV_NOPTS_VALUE) ? 0.0 : (double)ts * mp.video_tb;

  mp.frame_write = (mp.frame_write + 1) % NUM_VFRAMES;
  mp.frame_count++;
  mtx_unlock(&mp.lock);
}

static void push_audio_frame(const AVFrame *frm) {
  if (!mp.has_audio)
    return;

  static int16_t pcm[8192 * 2];
  uint8_t *outp[1] = { (uint8_t *)pcm };
  const int out = swr_convert(mp.swr, outp, 8192,
      (const uint8_t **)frm->data, frm->nb_samples);
  if (out <= 0)
    return;

  if (opensles_movie_queue(pcm, out) <= 0)
    return;

  mtx_lock(&mp.lock);
  if (!mp.audio_primed &&
      opensles_movie_samples_queued() * 1000 >= (uint64_t)mp.audio_rate * AUDIO_PREFILL_MS) {
    mp.audio_primed = 1;
    opensles_movie_set_paused(0);
  }
  mtx_unlock(&mp.lock);
}

static void drain_codec(AVCodecContext *ctx, AVFrame *frm, int is_video) {
  while (avcodec_receive_frame(ctx, frm) == 0) {
    if (mp.stop_req)
      return;
    if (is_video)
      push_video_frame(frm);
    else
      push_audio_frame(frm);
  }
}

static int decoder_main(void *arg) {
  (void)arg;
  AVPacket *pkt = av_packet_alloc();
  AVFrame *frm = av_frame_alloc();

  while (!mp.stop_req) {
    if (mp.paused) {
      thrd_sleep(&(struct timespec){ .tv_nsec = 10 * 1000 * 1000 }, NULL);
      continue;
    }
    if (av_read_frame(mp.fmt, pkt) < 0)
      break;
    if (pkt->stream_index == mp.vstream) {
      if (avcodec_send_packet(mp.vctx, pkt) == 0)
        drain_codec(mp.vctx, frm, 1);
    } else if (mp.has_audio && pkt->stream_index == mp.astream) {
      if (avcodec_send_packet(mp.actx, pkt) == 0)
        drain_codec(mp.actx, frm, 0);
    }
    av_packet_unref(pkt);
  }

  if (!mp.stop_req) {
    avcodec_send_packet(mp.vctx, NULL);
    drain_codec(mp.vctx, frm, 1);
    if (mp.has_audio) {
      avcodec_send_packet(mp.actx, NULL);
      drain_codec(mp.actx, frm, 0);
    }
  }

  av_frame_free(&frm);
  av_packet_free(&pkt);
  mp.audio_primed = 1;
  mp.decode_eof = 1;
  if (mp.has_audio)
    opensles_movie_set_paused(0);
  return 0;
}

static void free_session(void) {
  for (int i = 0; i < NUM_VFRAMES; i++)
    free(mp.frames[i].rgba);
  if (mp.has_audio)
    opensles_movie_end();
  if (mp.swr)
    swr_free(&mp.swr);
  if (mp.sws)
    sws_freeContext(mp.sws);
  if (mp.vctx)
    avcodec_free_context(&mp.vctx);
  if (mp.actx)
    avcodec_free_context(&mp.actx);
  if (mp.fmt)
    avformat_close_input(&mp.fmt);
  mtx_destroy(&mp.lock);
  cnd_destroy(&mp.can_produce);
  memset(&mp, 0, sizeof(mp));
}

void movie_stop(void) {
  if (!mp.active)
    return;
  mp.stop_req = 1;
  mtx_lock(&mp.lock);
  cnd_broadcast(&mp.can_produce);
  mtx_unlock(&mp.lock);
  if (mp.has_audio)
    opensles_movie_end();
  if (mp.thread_running) {
    int res;
    thrd_join(mp.thread, &res);
  }
  free_session();
}

void movie_pause(void) {
  if (!mp.active || mp.paused)
    return;
  mp.paused = 1;
  mp.pause_tick = armGetSystemTick();
  if (mp.has_audio)
    opensles_movie_set_paused(1);
}

void movie_resume(void) {
  if (!mp.active || !mp.paused)
    return;
  mp.paused_ticks += armGetSystemTick() - mp.pause_tick;
  mp.pause_tick = 0;
  mp.paused = 0;
  if (mp.has_audio && mp.audio_primed)
    opensles_movie_set_paused(0);
}

void movie_skip(void) {
  if (mp.active && mp.skippable) {
    movie_stop();
  }
}

static int setup_audio(void) {
  if (mp.astream < 0 || open_codec(&mp.actx, mp.fmt->streams[mp.astream], 1) < 0)
    return 0;

  mp.audio_rate = opensles_movie_begin(mp.actx->sample_rate);
  if (mp.audio_rate <= 0)
    return 0;

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
  AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
  if (swr_alloc_set_opts2(&mp.swr, &out_layout, AV_SAMPLE_FMT_S16, mp.audio_rate,
                          &mp.actx->ch_layout, mp.actx->sample_fmt, mp.actx->sample_rate, 0, NULL) < 0)
    mp.swr = NULL;
#else
  const uint64_t in_layout = mp.actx->channel_layout ?
      mp.actx->channel_layout : av_get_default_channel_layout(mp.actx->channels);
  mp.swr = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, mp.audio_rate,
                              in_layout, mp.actx->sample_fmt, mp.actx->sample_rate, 0, NULL);
#endif
  if (!mp.swr || swr_init(mp.swr) < 0) {
    opensles_movie_end();
    return 0;
  }

  return 1;
}

int movie_play(const char *path, int skippable) {
  if (mp.active)
    movie_stop();

  memset(&mp, 0, sizeof(mp));
  mp.skippable = skippable;
  mp.vstream = -1;
  mp.astream = -1;
  mtx_init(&mp.lock, mtx_plain);
  cnd_init(&mp.can_produce);

  if (path && path[0] == '/')
    path++;

  if (avformat_open_input(&mp.fmt, path, NULL, NULL) < 0) {
    debugPrintf("movie: could not open %s\n", path ? path : "(null)");
    mp.fmt = NULL;
    goto fail;
  }
  if (avformat_find_stream_info(mp.fmt, NULL) < 0)
    goto fail;

  mp.vstream = av_find_best_stream(mp.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  mp.astream = av_find_best_stream(mp.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (mp.vstream < 0)
    goto fail;

  AVStream *vs = mp.fmt->streams[mp.vstream];
  if (open_codec(&mp.vctx, vs, 2) < 0)
    goto fail;
  mp.width = mp.vctx->width;
  mp.height = mp.vctx->height;
  mp.video_tb = av_q2d(vs->time_base);
  if (mp.width <= 0 || mp.height <= 0)
    goto fail;

  mp.sws = sws_getContext(mp.width, mp.height, mp.vctx->pix_fmt,
      mp.width, mp.height, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
  if (!mp.sws)
    goto fail;

  for (int i = 0; i < NUM_VFRAMES; i++) {
    mp.frames[i].rgba = malloc((size_t)mp.width * mp.height * 4);
    if (!mp.frames[i].rgba)
      goto fail;
  }

  mp.has_audio = setup_audio();
  if (!mp.has_audio) {
    if (mp.swr)
      swr_free(&mp.swr);
    if (mp.actx)
      avcodec_free_context(&mp.actx);
  }

  mp.start_tick = armGetSystemTick();
  if (thrd_create(&mp.thread, decoder_main, NULL) != thrd_success)
    goto fail;
  mp.thread_running = 1;
  mp.active = 1;
  return 1;

fail:
  debugPrintf("movie: failed to start %s\n", path ? path : "(null)");
  mp.stop_req = 1;
  free_session();
  return 0;
}

int movie_is_playing(void) {
  if (!mp.active)
    return 0;

  mtx_lock(&mp.lock);
  const int frames_left = mp.frame_count;
  const int eof = mp.decode_eof;
  mtx_unlock(&mp.lock);

  if (eof && frames_left == 0) {
    int audio_done = 1;
    if (mp.has_audio)
      audio_done = opensles_movie_buffered_frames() == 0;
    if (audio_done) {
      movie_stop();
      return 0;
    }
  }
  return 1;
}

static int gl_ready(void) {
  if (glmovie.ready)
    return 1;
  glGenTextures(1, &glmovie.tex);
  glmovie.ready = glmovie.tex != 0;
  return glmovie.ready;
}

static void upload_frame(const VideoFrame *f) {
  glBindTexture(GL_TEXTURE_2D, glmovie.tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (glmovie.tex_w != mp.width || glmovie.tex_h != mp.height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mp.width, mp.height, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, f->rgba);
    glmovie.tex_w = mp.width;
    glmovie.tex_h = mp.height;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mp.width, mp.height,
        GL_RGBA, GL_UNSIGNED_BYTE, f->rgba);
  }
}

static void movie_render(void) {
  if (!mp.active || !gl_ready())
    return;

  const double now = movie_clock();
  int adopted = -1;
  mtx_lock(&mp.lock);
  if (!mp.frame_shown && mp.frame_count > 0) {
    adopted = mp.frame_read;
    mp.frame_read = (mp.frame_read + 1) % NUM_VFRAMES;
    mp.frame_count--;
  }
  while (now >= 0.0 && mp.frame_count > 0 && mp.frames[mp.frame_read].pts <= now) {
    adopted = mp.frame_read;
    mp.frame_read = (mp.frame_read + 1) % NUM_VFRAMES;
    mp.frame_count--;
    if (mp.frame_count == 0 || mp.frames[mp.frame_read].pts > now)
      break;
  }
  if (adopted >= 0) {
    upload_frame(&mp.frames[adopted]);
    cnd_signal(&mp.can_produce);
    mp.frame_shown = 1;
  }
  mtx_unlock(&mp.lock);

  if (!mp.frame_shown)
    return;

  GLint prev_tex = 0, prev_matrix = GL_MODELVIEW, prev_viewport[4] = {0};
  GLfloat prev_color[4] = {1, 1, 1, 1};
  GLboolean prev_tex2d = glIsEnabled(GL_TEXTURE_2D);
  GLboolean prev_blend = glIsEnabled(GL_BLEND);
  GLboolean prev_depth = glIsEnabled(GL_DEPTH_TEST);
  GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
  GLboolean prev_cull = glIsEnabled(GL_CULL_FACE);
  GLboolean prev_light = glIsEnabled(GL_LIGHTING);
  GLboolean prev_fog = glIsEnabled(GL_FOG);
  GLboolean prev_alpha = glIsEnabled(GL_ALPHA_TEST);
  GLboolean prev_vtx = glIsEnabled(GL_VERTEX_ARRAY);
  GLboolean prev_uv = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
  GLboolean prev_col = glIsEnabled(GL_COLOR_ARRAY);
  GLboolean prev_norm = glIsEnabled(GL_NORMAL_ARRAY);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  glGetIntegerv(GL_MATRIX_MODE, &prev_matrix);
  glGetIntegerv(GL_VIEWPORT, prev_viewport);
  glGetFloatv(GL_CURRENT_COLOR, prev_color);

  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_LIGHTING);
  glDisable(GL_FOG);
  glDisable(GL_ALPHA_TEST);
  glEnable(GL_TEXTURE_2D);
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  glViewport(0, 0, screen_width, screen_height);

  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrthof(0.0f, (float)screen_width, (float)screen_height, 0.0f, -1.0f, 1.0f);
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();

  float x0 = 0.0f, y0 = 0.0f, x1 = (float)screen_width, y1 = (float)screen_height;
  const float va = (float)mp.width / (float)mp.height;
  const float sa = (float)screen_width / (float)screen_height;
  if (va > sa) {
    const float h = (float)screen_width / va;
    y0 = ((float)screen_height - h) * 0.5f;
    y1 = y0 + h;
  } else {
    const float w = (float)screen_height * va;
    x0 = ((float)screen_width - w) * 0.5f;
    x1 = x0 + w;
  }

  const GLfloat verts[8] = { x0, y1, x1, y1, x0, y0, x1, y0 };
  const GLfloat uvs[8] = { 0, 1, 1, 1, 0, 0, 1, 0 };

  glBindTexture(GL_TEXTURE_2D, glmovie.tex);
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_COLOR_ARRAY);
  glDisableClientState(GL_NORMAL_ARRAY);
  glVertexPointer(2, GL_FLOAT, 0, verts);
  glTexCoordPointer(2, GL_FLOAT, 0, uvs);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  if (!prev_vtx) glDisableClientState(GL_VERTEX_ARRAY);
  if (!prev_uv) glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  if (prev_col) glEnableClientState(GL_COLOR_ARRAY);
  if (prev_norm) glEnableClientState(GL_NORMAL_ARRAY);
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(prev_matrix);
  glColor4f(prev_color[0], prev_color[1], prev_color[2], prev_color[3]);
  glBindTexture(GL_TEXTURE_2D, prev_tex);
  glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
  if (!prev_tex2d) glDisable(GL_TEXTURE_2D);
  if (prev_blend) glEnable(GL_BLEND);
  if (prev_depth) glEnable(GL_DEPTH_TEST);
  if (prev_scissor) glEnable(GL_SCISSOR_TEST);
  if (prev_cull) glEnable(GL_CULL_FACE);
  if (prev_light) glEnable(GL_LIGHTING);
  if (prev_fog) glEnable(GL_FOG);
  if (prev_alpha) glEnable(GL_ALPHA_TEST);
}

unsigned int movie_swap_buffers(void *display, void *surface) {
  if (mp.active)
    movie_render();
  movie_swaps++;
  return eglSwapBuffers((EGLDisplay)display, (EGLSurface)surface);
}

void movie_main_loop_tick(void) {
  static unsigned int last_swaps = 0;
  if (!mp.active) {
    last_swaps = movie_swaps;
    return;
  }
  if (movie_swaps != last_swaps) {
    last_swaps = movie_swaps;
    return;
  }

  EGLDisplay d = eglGetCurrentDisplay();
  EGLSurface s = eglGetCurrentSurface(EGL_DRAW);
  if (d == EGL_NO_DISPLAY || s == EGL_NO_SURFACE) {
    debugPrintf("movie: no current EGL surface, aborting playback\n");
    movie_stop();
    return;
  }
  movie_render();
  eglSwapBuffers(d, s);
}
