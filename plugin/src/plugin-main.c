#include <windows.h>
#include <objbase.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <obs-module.h>
#include <obs-source.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include "m50.h"
#include "jpeg_wic.h"

OBS_DECLARE_MODULE()

struct m50_ctx {
  obs_source_t *source;
  char *camera_ip;
  char *local_address;
  pthread_t thread;
  volatile bool running;
  volatile bool thread_alive;
  volatile bool retry_now;
  pthread_mutex_t lock;
  char status[256];
  char pending_ip[64];
  m50_found cams[M50_MAX_CAMERAS];
  int ncams;
  /* Latest decoded frame for synchronous output on the video thread. */
  uint8_t *frame_buf;
  size_t frame_cap;
  int frame_w;
  int frame_h;
  int frame_stride;
  unsigned long frame_seq;
  unsigned long frame_shown;
};

static void set_status(struct m50_ctx *ctx, const char *text)
{
  pthread_mutex_lock(&ctx->lock);
  snprintf(ctx->status, sizeof(ctx->status), "%s", text ? text : "");
  pthread_mutex_unlock(&ctx->lock);
}

/* Worker only decodes into the latest-frame slot; the video thread pushes it
 * out synchronously each tick so OBS never sits on a queue of stale frames. */
static void store_frame(struct m50_ctx *ctx, const uint8_t *bgra, int w, int h, int stride)
{
  size_t need = (size_t)h * (size_t)stride;
  pthread_mutex_lock(&ctx->lock);
  if (need > ctx->frame_cap) {
    uint8_t *nb = (uint8_t *)brealloc(ctx->frame_buf, need);
    if (!nb) {
      pthread_mutex_unlock(&ctx->lock);
      return;
    }
    ctx->frame_buf = nb;
    ctx->frame_cap = need;
  }
  memcpy(ctx->frame_buf, bgra, need);
  ctx->frame_w = w;
  ctx->frame_h = h;
  ctx->frame_stride = stride;
  ctx->frame_seq++;
  pthread_mutex_unlock(&ctx->lock);
}

static void m50_tick(void *data, float seconds){
  (void)seconds;
  struct m50_ctx *ctx = (struct m50_ctx *)data;
  pthread_mutex_lock(&ctx->lock);
  if (!ctx->frame_buf || ctx->frame_seq == ctx->frame_shown) {
    pthread_mutex_unlock(&ctx->lock);
    return;
  }
  struct obs_source_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.format = VIDEO_FORMAT_BGRX;
  frame.width = (uint32_t)ctx->frame_w;
  frame.height = (uint32_t)ctx->frame_h;
  frame.linesize[0] = (uint32_t)ctx->frame_stride;
  frame.data[0] = ctx->frame_buf;
  frame.timestamp = os_gettime_ns();
  ctx->frame_shown = ctx->frame_seq;
  obs_source_output_video(ctx->source, &frame);
  pthread_mutex_unlock(&ctx->lock);
}

static uint32_t m50_width(void *data)
{
  struct m50_ctx *ctx = (struct m50_ctx *)data;
  return (uint32_t)(ctx->frame_w > 0 ? ctx->frame_w : 960);
}

static uint32_t m50_height(void *data)
{
  struct m50_ctx *ctx = (struct m50_ctx *)data;
  return (uint32_t)(ctx->frame_h > 0 ? ctx->frame_h : 640);
}

static void *capture_thread(void *arg)
{
  struct m50_ctx *ctx = (struct m50_ctx *)arg;
  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  char ip[64] = "";
  char local[64] = "";
  snprintf(ip, sizeof(ip), "%s", ctx->camera_ip ? ctx->camera_ip : "");
  snprintf(local, sizeof(local), "%s", ctx->local_address ? ctx->local_address : "");

  while (ctx->running) {
    /* Adopt a newly searched IP and honor manual pair requests. */
    pthread_mutex_lock(&ctx->lock);
    if (ctx->pending_ip[0]) {
      snprintf(ip, sizeof(ip), "%s", ctx->pending_ip);
      ctx->pending_ip[0] = '\0';
    }
    bool now = ctx->retry_now;
    ctx->retry_now = false;
    pthread_mutex_unlock(&ctx->lock);
    (void)now;
    if (ip[0] == '\0') {
      set_status(ctx, "Press Search cameras, or type the Camera IP.");
      os_sleep_ms(1000);
      continue;
    }
    /* Auto-select the router-facing local address when unset. */
    char eff_local[64] = "";
    if (local[0])
      snprintf(eff_local, sizeof(eff_local), "%s", local);
    else if (!m50_pick_local(ip, eff_local, sizeof(eff_local)))
      eff_local[0] = '\0';
    /* Quick SSDP sweep: auto-adopt a smartphone-mode camera, and warn when
     * the camera sits in EOS Utility (WFT) mode whose bootstrap differs. */
    {
      m50_found sweep[M50_MAX_CAMERAS];
      int ns = m50_discover(sweep, M50_MAX_CAMERAS, 2500, eff_local[0] ? eff_local : NULL);
      int smart = -1, wft_only = 1;
      for (int i = 0; i < ns; i++) {
        if (strstr(sweep[i].service, "SmartPhone") != NULL) {
          smart = i;
          wft_only = 0;
        } else if (strstr(sweep[i].service, "WFTEOS") == NULL &&
                   strstr(sweep[i].service, "RemoteControl") == NULL) {
          wft_only = 0;
        }
      }
      if (smart >= 0 && ip[0] == '\0') {
        snprintf(ip, sizeof(ip), "%s", sweep[smart].ip);
        char msg[300];
        snprintf(msg, sizeof(msg), "Auto-selected camera at %s.", ip);
        set_status(ctx, msg);
        blog(LOG_INFO, "[m50] %s", msg);
      } else if (ns > 0 && wft_only) {
        set_status(ctx, "Camera is in EOS Utility mode: switch it to Wi-Fi function > "
                        "Connect to smartphone, then press Pair.");
        blog(LOG_WARNING, "[m50] camera in WFT mode (%s); need smartphone mode", sweep[0].ip);
        for (int i = 0; i < 100 && ctx->running && !ctx->retry_now; i++)
          os_sleep_ms(100);
        continue;
      }
    }
    char model[128] = "";
    if (m50_verify(ip, eff_local[0] ? eff_local : NULL, model, sizeof(model))) {
      char msg[300];
      snprintf(msg, sizeof(msg), "Found %.100s at %s. Connecting: accept any prompt (90s)...", model, ip);
      set_status(ctx, msg);
    } else {
      set_status(ctx, "Connecting: accept any prompt on the camera (90s)...");
    }
    m50_session *s = m50_connect(ip, 15740, eff_local[0] ? eff_local : NULL, 90000);
    if (!s) {
      char msg[300];
      snprintf(msg, sizeof(msg), "Connect failed: %.220s", m50_last_error());
      set_status(ctx, msg);
      blog(LOG_WARNING, "[m50] %s", msg);
      for (int i = 0; i < 100 && ctx->running && !ctx->retry_now; i++)
        os_sleep_ms(100);
      continue;
    }
    set_status(ctx, "Streaming.");
    blog(LOG_INFO, "[m50] connected, EVF bootstrap done; waiting for first frame");

    int transient_run = 0;
    long total_frames = 0;
    int wic_fails = 0;
    DWORD last_frame_ms = GetTickCount();
    while (ctx->running) {
      uint8_t *jpeg = NULL;
      int jlen = 0;
      int r = m50_get_frame(s, &jpeg, &jlen);
      if (r < 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Stream error: %.220s (reconnecting)", m50_last_error());
        set_status(ctx, msg);
        blog(LOG_WARNING, "[m50] %s", msg);
        free(jpeg);
        break;
      }
      if (r == 0) {
        if (++transient_run == 100)
          blog(LOG_WARNING, "[m50] viewfinder not-ready x100 (camera may need a moment)");
        else if (transient_run > 100 && transient_run % 600 == 0)
          blog(LOG_WARNING, "[m50] viewfinder not-ready x%d", transient_run);
        /* Idle watchdog only: never interrupt steady streaming. */
        if (GetTickCount() - last_frame_ms > 5000) {
          last_frame_ms = GetTickCount();
          if (m50_keepalive(s) != 0)
            blog(LOG_WARNING, "[m50] keepalive poll failed");
        }
        os_sleep_ms(50);
        continue;
      }
      transient_run = 0;
      last_frame_ms = GetTickCount();
      uint8_t *bgra = NULL;
      int w = 0, h = 0, stride = 0;
      if (jpeg_to_bgra(jpeg, jlen, &bgra, &w, &h, &stride)) {
        store_frame(ctx, bgra, w, h, stride);
        if (++total_frames == 1) {
          blog(LOG_INFO, "[m50] first frame: %dx%d (%d bytes jpeg)", w, h, jlen);
          /* Debug dump: %TEMP%\m50_firstframe.bmp for visual verification. */
          HANDLE f = CreateFileA("C:\\Windows\\Temp\\m50_firstframe.bmp", GENERIC_WRITE, 0, NULL,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
          if (f != INVALID_HANDLE_VALUE) {
            BITMAPFILEHEADER fh;
            BITMAPINFOHEADER ih;
            memset(&fh, 0, sizeof(fh));
            memset(&ih, 0, sizeof(ih));
            DWORD row = (DWORD)stride * (DWORD)h;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + row;
            ih.biSize = sizeof(ih);
            ih.biWidth = w;
            ih.biHeight = -h;
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            ih.biCompression = BI_RGB;
            DWORD wr = 0;
            WriteFile(f, &fh, sizeof(fh), &wr, NULL);
            WriteFile(f, &ih, sizeof(ih), &wr, NULL);
            WriteFile(f, bgra, row, &wr, NULL);
            CloseHandle(f);
            blog(LOG_INFO, "[m50] wrote C:\\Windows\\Temp\\m50_firstframe.bmp");
          }
        } else if (total_frames % 1200 == 0)
          blog(LOG_INFO, "[m50] %ld frames", total_frames);
      } else if (++wic_fails <= 3) {
        blog(LOG_WARNING, "[m50] JPEG decode failed (%d bytes)", jlen);
      }
      free(bgra);
      free(jpeg);
    }
    m50_disconnect(s);
    if (ctx->running)
      os_sleep_ms(1000);
  }

  set_status(ctx, "Stopped.");
  CoUninitialize();
  ctx->thread_alive = false;
  return NULL;
}

static void m50_blog(const char *msg)
{
  blog(LOG_INFO, "[m50] %s", msg);
}

static const char *m50_get_name(void *unused)
{
  (void)unused;
  return "M50 Wi-Fi Camera";
}

static void start_thread(struct m50_ctx *ctx)
{
  ctx->running = true;
  ctx->thread_alive = true;
  if (pthread_create(&ctx->thread, NULL, capture_thread, ctx) != 0) {
    ctx->thread_alive = false;
    set_status(ctx, "Thread start failed.");
  }
}

static void stop_thread(struct m50_ctx *ctx)
{
  ctx->running = false;
  if (ctx->thread_alive)
    pthread_join(ctx->thread, NULL);
}

static void *m50_create(obs_data_t *settings, obs_source_t *source)
{
  struct m50_ctx *ctx = (struct m50_ctx *)bzalloc(sizeof(*ctx));
  ctx->source = source;
  pthread_mutex_init(&ctx->lock, NULL);
  snprintf(ctx->status, sizeof(ctx->status), "Starting.");
  ctx->camera_ip = bstrdup(obs_data_get_string(settings, "camera_ip"));
  ctx->local_address = bstrdup(obs_data_get_string(settings, "local_address"));
  ctx->frame_w = 960;
  ctx->frame_h = 640;
  m50_set_log(m50_blog);
  start_thread(ctx);
  return ctx;
}

static void m50_destroy(void *data)
{
  struct m50_ctx *ctx = (struct m50_ctx *)data;
  stop_thread(ctx);
  pthread_mutex_destroy(&ctx->lock);
  bfree(ctx->camera_ip);
  bfree(ctx->local_address);
  bfree(ctx->frame_buf);
  bfree(ctx);
}

static void m50_update(void *data, obs_data_t *settings)
{
  struct m50_ctx *ctx = (struct m50_ctx *)data;
  const char *nip = obs_data_get_string(settings, "camera_ip");
  const char *nloc = obs_data_get_string(settings, "local_address");
  pthread_mutex_lock(&ctx->lock);
  bool same = strcmp(ctx->camera_ip ? ctx->camera_ip : "", nip ? nip : "") == 0 &&
              strcmp(ctx->local_address ? ctx->local_address : "", nloc ? nloc : "") == 0;
  pthread_mutex_unlock(&ctx->lock);
  if (same)
    return;
  /* IP changed: restart the worker so it dials the new address. */
  stop_thread(ctx);
  bfree(ctx->camera_ip);
  bfree(ctx->local_address);
  ctx->camera_ip = bstrdup(nip);
  ctx->local_address = bstrdup(nloc);
  start_thread(ctx);
}

static bool search_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
  (void)props;
  (void)property;
  struct m50_ctx *ctx = (struct m50_ctx *)data;
  set_status(ctx, "Searching LAN for Canon cameras (5s)...");
  blog(LOG_INFO, "[m50] SSDP search started");

  char local[64] = "";
  pthread_mutex_lock(&ctx->lock);
  snprintf(local, sizeof(local), "%s", ctx->local_address ? ctx->local_address : "");
  pthread_mutex_unlock(&ctx->lock);

  m50_found found[M50_MAX_CAMERAS];
  int n = m50_discover(found, M50_MAX_CAMERAS, 5000, local[0] ? local : NULL);

  pthread_mutex_lock(&ctx->lock);
  ctx->ncams = 0;
  for (int i = 0; i < n && i < M50_MAX_CAMERAS; i++) {
    ctx->cams[i] = found[i];
    ctx->ncams++;
  }
  if (n > 0) {
    snprintf(ctx->pending_ip, sizeof(ctx->pending_ip), "%s", found[0].ip);
    ctx->retry_now = true;
    snprintf(ctx->status, sizeof(ctx->status), "Found %d camera(s); connecting to %s. Accept the prompt.", n,
             found[0].ip);
  } else {
    snprintf(ctx->status, sizeof(ctx->status),
             "No cameras answered SSDP (M50 often stays silent). Type the IP from the camera's "
             "Connection Information, then press Pair.");
  }
  pthread_mutex_unlock(&ctx->lock);

  for (int i = 0; i < n; i++)
    blog(LOG_INFO, "[m50] found %s (%s)", found[i].ip, found[i].service);
  if (n == 0)
    blog(LOG_WARNING, "[m50] SSDP search found nothing");
  return true;
}

static bool pair_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
  (void)props;
  (void)property;
  struct m50_ctx *ctx = (struct m50_ctx *)data;
  ctx->retry_now = true;
  set_status(ctx, "Pairing: put the camera in pairing mode and accept the prompt (90s).");
  blog(LOG_INFO, "[m50] manual pair requested");
  return true;
}

static obs_properties_t *m50_properties(void *data)
{
  struct m50_ctx *ctx = (struct m50_ctx *)data;
  obs_properties_t *props = obs_properties_create();
  obs_properties_add_text(props, "camera_ip", "Camera IP (router LAN, e.g. 192.168.1.53)",
                          OBS_TEXT_DEFAULT);
  obs_properties_add_text(props, "local_address",
                          "PC local address (blank = auto-pick router-facing interface)",
                          OBS_TEXT_DEFAULT);
  obs_properties_add_button2(props, "search", "Search cameras on LAN", search_clicked, data);
  obs_properties_add_button2(props, "pair", "Pair now (camera in pairing mode)", pair_clicked, data);
  obs_property_t *info = obs_properties_add_text(props, "help", "Setup", OBS_TEXT_INFO);
  obs_property_text_set_info_type(info, OBS_TEXT_INFO_NORMAL);
  return props;
}

static void m50_get_defaults(obs_data_t *settings)
{
  obs_data_set_default_string(settings, "camera_ip", "192.168.1.53");
  obs_data_set_default_string(settings, "local_address", "192.168.1.43");
  obs_data_set_default_string(
    settings, "help",
    "Wi-Fi preview is fixed 960x640 (camera limitation); OBS scales it. "
    "No audio over this path. Camera: Wi-Fi function > Connect to smartphone (same router), "
    "waiting screen. Press Search cameras, or type the IP and press Pair now, then accept "
    "the prompt within 90s. Status details: Help > Log Files, search [m50].");
}

static struct obs_source_info m50_source_info = {
  .id = "m50_wifi_source",
  .type = OBS_SOURCE_TYPE_INPUT,
  .output_flags = OBS_SOURCE_ASYNC_VIDEO,
  .get_name = m50_get_name,
  .create = m50_create,
  .destroy = m50_destroy,
  .update = m50_update,
  .video_tick = m50_tick,
  .get_width = m50_width,
  .get_height = m50_height,
  .get_properties = m50_properties,
  .get_defaults = m50_get_defaults,
  .icon_type = OBS_ICON_TYPE_CAMERA,
};

bool obs_module_load(void)
{
  obs_register_source(&m50_source_info);
  blog(LOG_INFO, "[m50] M50 Wi-Fi source loaded");
  return true;
}
