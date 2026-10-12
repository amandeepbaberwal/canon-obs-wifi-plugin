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
  volatile bool cancelled;
  volatile bool conflict;
  pthread_mutex_t lock;
  char status[256];
  char pending_ip[64];
  m50_found cams[M50_MAX_CAMERAS];
  int ncams;
  int frame_w;
  int frame_h;
};

/* Process-wide guard: one PTP/IP client per camera. */
static CRITICAL_SECTION g_inst_lock;
static volatile bool g_inst_init = false;
static volatile LONG g_inst_started = 0;
static int g_live_count = 0;
static char g_live_ip[64] = "";

static void inst_lock(void)
{
  if (!g_inst_init) {
    if (InterlockedCompareExchange(&g_inst_started, 1, 0) == 0) {
      InitializeCriticalSection(&g_inst_lock);
      g_inst_init = true;
    } else {
      while (!g_inst_init)
        Sleep(1);
    }
  }
  EnterCriticalSection(&g_inst_lock);
}

static void inst_unlock(void)
{
  LeaveCriticalSection(&g_inst_lock);
}

static void set_status(struct m50_ctx *ctx, const char *text)
{
  pthread_mutex_lock(&ctx->lock);
  snprintf(ctx->status, sizeof(ctx->status), "%s", text ? text : "");
  pthread_mutex_unlock(&ctx->lock);
  /* Persist so the Properties dialog shows it on open. */
  if (ctx->source) {
    obs_data_t *s = obs_source_get_settings(ctx->source);
    if (s) {
      obs_data_set_string(s, "live_status", text ? text : "");
      obs_data_release(s);
    }
  }
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

static void wait_retry(struct m50_ctx *ctx, int ms)
{
  for (int i = 0; i < ms / 100 && ctx->running && !ctx->cancelled && !ctx->retry_now; i++)
    os_sleep_ms(100);
  pthread_mutex_lock(&ctx->lock);
  ctx->retry_now = false;
  pthread_mutex_unlock(&ctx->lock);
}

static void *capture_thread(void *arg)
{
  struct m50_ctx *ctx = (struct m50_ctx *)arg;
  CoInitializeEx(NULL, COINIT_MULTITHREADED);
  m50_set_cancel(&ctx->cancelled);
  if (ctx->conflict) {
    set_status(ctx, "Camera in use by another M50 source in OBS.");
    blog(LOG_WARNING, "[m50] duplicate source for this camera; not connecting");
    ctx->thread_alive = false;
    CoUninitialize();
    return NULL;
  }

  char ip[64] = "";
  char local[64] = "";
  snprintf(ip, sizeof(ip), "%s", ctx->camera_ip ? ctx->camera_ip : "");
  snprintf(local, sizeof(local), "%s", ctx->local_address ? ctx->local_address : "");

  /* One mode check at startup. When an IP is already configured we then
   * connect directly every round (no per-iteration sweep) so the first
   * attempt lands inside the camera's short wait window. */
  bool wft_guard = false;
  if (ip[0] != '\0') {
    char eff0[64] = "";
    if (local[0])
      snprintf(eff0, sizeof(eff0), "%s", local);
    else if (!m50_pick_local(ip, eff0, sizeof(eff0)))
      eff0[0] = '\0';
    m50_found sweep0[M50_MAX_CAMERAS];
    int ns0 = m50_discover(sweep0, M50_MAX_CAMERAS, 2500, eff0[0] ? eff0 : NULL);
    wft_guard = ns0 > 0;
    for (int i = 0; i < ns0; i++) {
      if (strstr(sweep0[i].service, "SmartPhone") != NULL) {
        wft_guard = false;
        break;
      }
    }
    if (wft_guard)
      blog(LOG_WARNING, "[m50] camera in WFT mode; need smartphone mode");
  }

  int fails = 0, dial_tmo = 0;
  while (ctx->running && !ctx->cancelled) {
    /* Adopt a newly searched IP and honor manual pair requests. */
    pthread_mutex_lock(&ctx->lock);
    if (ctx->pending_ip[0]) {
      snprintf(ip, sizeof(ip), "%s", ctx->pending_ip);
      ctx->pending_ip[0] = '\0';
      wft_guard = false;
    }
    pthread_mutex_unlock(&ctx->lock);
    if (wft_guard) {
      set_status(ctx, "Camera is in EOS Utility mode: switch it to Wi-Fi function > "
                      "Connect to smartphone, then press Pair.");
      for (int i = 0; i < 100 && ctx->running && !ctx->retry_now; i++)
        os_sleep_ms(100);
      pthread_mutex_lock(&ctx->lock);
      ctx->retry_now = false;
      pthread_mutex_unlock(&ctx->lock);
      continue;
    }
    if (ip[0] == '\0') {
      /* No configured IP: sweep to find/adopt a smartphone camera. */
      char eff0[64] = "";
      if (local[0])
        snprintf(eff0, sizeof(eff0), "%s", local);
      m50_found sweep[M50_MAX_CAMERAS];
      int ns = m50_discover(sweep, M50_MAX_CAMERAS, 2500, eff0[0] ? eff0 : NULL);
      int smart = -1;
      for (int i = 0; i < ns; i++) {
        if (strstr(sweep[i].service, "SmartPhone") != NULL) {
          smart = i;
          break;
        }
      }
      if (smart < 0) {
        set_status(ctx, "No camera found. Press Search cameras, or type the Camera IP.");
        os_sleep_ms(5000);
        continue;
      }
      snprintf(ip, sizeof(ip), "%s", sweep[smart].ip);
      blog(LOG_INFO, "[m50] auto-selected camera at %s", ip);
    }
    /* Fast path: connect immediately, no verify/sweep delays. */
    char eff_local[64] = "";
    if (local[0])
      snprintf(eff_local, sizeof(eff_local), "%s", local);
    else if (!m50_pick_local(ip, eff_local, sizeof(eff_local)))
      eff_local[0] = '\0';
    pthread_mutex_lock(&ctx->lock);
    ctx->retry_now = false;
    pthread_mutex_unlock(&ctx->lock);
    set_status(ctx, "Connecting: accept any prompt on the camera (90s)...");
    m50_session *s = m50_connect(ip, 15740, eff_local[0] ? eff_local : NULL, 90000);
    if (!s) {
      char err[512], msg[300];
      snprintf(err, sizeof(err), "%s", m50_last_error());
      snprintf(msg, sizeof(msg), "Connect failed: %.220s", err);
      set_status(ctx, msg);
      blog(LOG_WARNING, "[m50] %s", msg);
      fails++;
      if (m50_last_stage() == 0) {
        /* Camera not listening yet (off / joining): cheap fast polls so we
         * land inside its 3-4s previous-connection window when it opens. */
        wait_retry(ctx, 2000);
      } else if (strstr(err, "ECONNRESET") != NULL || strstr(err, "10054") != NULL) {
        wait_retry(ctx, 15000);
      } else if (strstr(err, "timed out") != NULL || strstr(err, "ETIMEDOUT") != NULL ||
                 strstr(err, "refused") != NULL || strstr(err, "ECONNREFUSED") != NULL) {
        if (++dial_tmo >= 3) {
          set_status(ctx, "Camera not listening. Dismiss any error on the camera, re-enter "
                          "Wi-Fi > Connect to smartphone (waiting screen), then press Pair.");
          blog(LOG_WARNING, "[m50] dial failing; auto-retry paused until Pair");
          while (ctx->running && !ctx->cancelled && !ctx->retry_now)
            os_sleep_ms(500);
          pthread_mutex_lock(&ctx->lock);
          ctx->retry_now = false;
          pthread_mutex_unlock(&ctx->lock);
          dial_tmo = 0;
        } else {
          int waits[] = { 3000, 6000, 12000, 24000, 30000 };
          int w = waits[fails - 1 < 5 ? fails - 1 : 4];
          if (w > 30000)
            w = 30000;
          wait_retry(ctx, w);
        }
      } else {
        wait_retry(ctx, 3000);
      }
      continue;
    }
    fails = 0;
    dial_tmo = 0;
    set_status(ctx, "Streaming.");
    blog(LOG_INFO, "[m50] connected, EVF bootstrap done; waiting for first frame");

    int transient_run = 0;
    long total_frames = 0;
    int wic_fails = 0;
    int ev_stage = 0;
    DWORD birth_ms = GetTickCount();
    DWORD last_frame_ms = birth_ms;
    while (ctx->running && !ctx->cancelled) {
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
        /* Escalate only while no frame has ever arrived on this session. */
        if (total_frames == 0) {
          DWORD el = GetTickCount() - birth_ms;
          if (el > 3000 && ev_stage == 0) {
            ev_stage = 1;
            blog(LOG_INFO, "[m50] no frames yet; restarting EVF (3)");
            if (m50_restart_evf(s, 3) != 0)
              blog(LOG_WARNING, "[m50] EVF restart failed: %s", m50_last_error());
          } else if (el > 8000 && ev_stage == 1) {
            ev_stage = 2;
            blog(LOG_INFO, "[m50] no frames yet; restarting EVF (2)");
            if (m50_restart_evf(s, 2) != 0)
              blog(LOG_WARNING, "[m50] EVF restart failed: %s", m50_last_error());
          } else if (el > 15000) {
            set_status(ctx, "No frames from camera; reconnecting.");
            blog(LOG_WARNING, "[m50] no frames in 15s; reconnecting");
            break;
          }
        }
        /* Idle watchdog only: never interrupt steady streaming. */
        if (GetTickCount() - last_frame_ms > 5000) {
          last_frame_ms = GetTickCount();
          if (m50_keepalive(s) != 0)
            blog(LOG_WARNING, "[m50] keepalive poll failed");
        }
        os_sleep_ms(100);
        continue;
      }
      transient_run = 0;
      last_frame_ms = GetTickCount();
      uint8_t *bgra = NULL;
      int w = 0, h = 0, stride = 0;
      if (jpeg_to_bgra(jpeg, jlen, &bgra, &w, &h, &stride)) {
        struct obs_source_frame f;
        memset(&f, 0, sizeof(f));
        f.format = VIDEO_FORMAT_BGRX;
        f.width = (uint32_t)w;
        f.height = (uint32_t)h;
        f.linesize[0] = (uint32_t)stride;
        f.data[0] = bgra;
        f.full_range = true;
        f.timestamp = os_gettime_ns();
        pthread_mutex_lock(&ctx->lock);
        ctx->frame_w = w;
        ctx->frame_h = h;
        obs_source_output_video(ctx->source, &f);
        pthread_mutex_unlock(&ctx->lock);
        if (++total_frames == 1)
          blog(LOG_INFO, "[m50] first frame: %dx%d (%d bytes jpeg)", w, h, jlen);
        else if (total_frames % 1200 == 0)
          blog(LOG_INFO, "[m50] %ld frames", total_frames);
      } else if (++wic_fails <= 3) {
        blog(LOG_WARNING, "[m50] JPEG decode failed (%d bytes)", jlen);
      }
      free(bgra);
      free(jpeg);
    }
    /* Unfreeze OBS instead of holding the last frame. */
    obs_source_output_video(ctx->source, NULL);
    m50_disconnect(s);
    fails++;
    if (ctx->running && !ctx->cancelled)
      wait_retry(ctx, 5000);
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
  ctx->cancelled = false;
  ctx->running = true;
  ctx->thread_alive = true;
  if (pthread_create(&ctx->thread, NULL, capture_thread, ctx) != 0) {
    ctx->thread_alive = false;
    set_status(ctx, "Thread start failed.");
  }
}

static void stop_thread(struct m50_ctx *ctx)
{
  ctx->cancelled = true;
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
  inst_lock();
  if (g_live_count > 0) {
    ctx->conflict = true;
    snprintf(ctx->status, sizeof(ctx->status),
             "Camera in use by another M50 source; remove the duplicate.");
  } else {
    g_live_count = 1;
    snprintf(g_live_ip, sizeof(g_live_ip), "%s", ctx->camera_ip ? ctx->camera_ip : "");
  }
  inst_unlock();
  obs_source_set_async_unbuffered(source, true);
  m50_set_log(m50_blog);
  start_thread(ctx);
  return ctx;
}

static void m50_destroy(void *data)
{
  struct m50_ctx *ctx = (struct m50_ctx *)data;
  stop_thread(ctx);
  inst_lock();
  if (!ctx->conflict) {
    g_live_count = 0;
    g_live_ip[0] = '\0';
  }
  inst_unlock();
  pthread_mutex_destroy(&ctx->lock);
  bfree(ctx->camera_ip);
  bfree(ctx->local_address);
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
  inst_lock();
  if (!ctx->conflict)
    snprintf(g_live_ip, sizeof(g_live_ip), "%s", nip ? nip : "");
  inst_unlock();
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
  obs_properties_add_text(props, "camera_ip", "Camera IP (router LAN, blank = auto-find)",
                          OBS_TEXT_DEFAULT);
  obs_properties_add_text(props, "local_address",
                          "PC local address (blank = auto-pick router-facing interface)",
                          OBS_TEXT_DEFAULT);
  obs_properties_add_button2(props, "search", "Search cameras on LAN", search_clicked, data);
  obs_properties_add_button2(props, "pair", "Pair now (camera in pairing mode)", pair_clicked, data);
  obs_property_t *live = obs_properties_add_text(props, "live_status", "Live status", OBS_TEXT_INFO);
  obs_property_text_set_info_type(live, OBS_TEXT_INFO_NORMAL);
  obs_property_t *help = obs_properties_add_text(props, "help", "Setup", OBS_TEXT_INFO);
  obs_property_text_set_info_type(help, OBS_TEXT_INFO_NORMAL);
  return props;
}

static void m50_get_defaults(obs_data_t *settings)
{
  obs_data_set_default_string(settings, "camera_ip", "");
  obs_data_set_default_string(settings, "local_address", "");
  obs_data_set_default_string(
    settings, "help",
    "Camera: Wi-Fi function > Connect to smartphone (same router), waiting screen. "
    "Press Search cameras, or type the IP and press Pair now, then accept the prompt "
    "within 90s. Preview size follows the camera feed. No audio over this path. "
    "Status details: Help > Log Files, search [m50].");
}

static struct obs_source_info m50_source_info = {
  .id = "m50_wifi_source",
  .type = OBS_SOURCE_TYPE_INPUT,
  .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
  .get_name = m50_get_name,
  .create = m50_create,
  .destroy = m50_destroy,
  .update = m50_update,
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
