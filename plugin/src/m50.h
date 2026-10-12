#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A connected camera session. Opaque handle owned by the worker thread. */
typedef struct m50_session m50_session;

/* Last error text, valid until the next call on the same thread. */
const char *m50_last_error(void);

/*
 * Open command + event channels, run the MTP session, perform the Canon EOS
 * Live View bootstrap (SetRemoteMode, SetEventMode, SetCapacity, capacity
 * property, SetPcConnectMode, EVF mode, EVF output device 3=cameraAndHost).
 * init_timeout_ms covers the on-camera pairing prompt wait (camera holds the
 * Init ACK open). Returns NULL on failure (see m50_last_error).
 */
m50_session *m50_connect(const char *host, int port, const char *local_address,
                         int init_timeout_ms);

/*
 * Poll one Live View frame. Returns 1 and sets *out_jpeg/*out_len to a
 * malloc'd marker-validated JPEG on success (caller frees). Returns 0 on
 * transient not-ready (call again), -1 on fatal error.
 */
int m50_get_frame(m50_session *s, uint8_t **out_jpeg, int *out_len);

/* Re-run the EVF tail of the bootstrap (0 -> pause -> mode -> out_value).
 * Approximates the dial switch that revives a stalled feed. */
int m50_restart_evf(m50_session *s, int out_value);

/* Best-effort GetEvent keepalive; keeps the camera from idling to sleep.
 * Returns 0 on any answered poll, -1 on transport failure. */
int m50_keepalive(m50_session *s);

void m50_close(m50_session *s);

/* Best-effort CloseSession before closing sockets. */
void m50_disconnect(m50_session *s);

/* Headless probe hooks (m50-probe CLI): raw session without bootstrap. */
int m50_raw_connect(const char *host, int port, const char *local_address, int init_timeout_ms,
                    void **sess_out);
int m50_raw_bootstrap(void *sess, int minimal);
int m50_raw_bootstrap_tol(void *sess, int minimal, int tolerant);
int m50_raw_evf(void *sess, int value);
int m50_raw_vf(void *sess, int p2, int p3, uint8_t **out_jpeg, int *out_len);
void m50_raw_close(void *sess);

/* Optional phase logger (e.g. OBS log). */
void m50_set_log(void (*cb)(const char *));

/* Worker sets its cancel flag so blocking waits abort promptly on stop. */
void m50_set_cancel(volatile bool *flag);

/* Where the last connect attempt died (see g_stage). */
int m50_last_stage(void);

#define M50_MAX_CAMERAS 16

typedef struct m50_found {
  char ip[64];
  char location[256];
  char service[128];
} m50_found;

/*
 * SSDP discovery: M-SEARCH for Canon smartphone / remote-control / WFT
 * services plus NOTIFY listen, for timeout_ms. Returns count (0..max).
 * Note: the M50 often stays silent to M-SEARCH; empty is common and
 * means "enter the IP manually", not "no camera".
 */
int m50_discover(m50_found *out, int max, int timeout_ms, const char *local_address);

/*
 * Fetch the UPnP descriptor and extract <modelName>/<friendlyName>.
 * Returns 1 on success. Best effort: HTTP on the camera is flaky.
 */
int m50_verify(const char *host, const char *local_address, char *model, int model_len);

/* Same as m50_verify but also extracts the Canon <serviceType>. */
int m50_verify_ex(const char *host, const char *local_address, char *model, int model_len,
                  char *service, int service_len);

/* Pick a local IPv4 for reaching camera_ip (empty camera_ip = first usable).
 * Returns 1 on success. */
int m50_pick_local(const char *camera_ip, char *out, int out_len);

#ifdef __cplusplus
}
#endif
