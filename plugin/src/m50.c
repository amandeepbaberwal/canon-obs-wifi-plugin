#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m50.h"

/* ---- protocol constants (verified against Rivet + M50 hardware) ---- */
#define PTP_INIT_CMD 1u
#define PTP_INIT_ACK 2u
#define PTP_INIT_EVT 3u
#define PTP_INIT_EVT_ACK 4u

#define MTP_CMD_REQ 6u
#define MTP_CMD_RESP 7u
#define MTP_DATA_START 9u
#define MTP_DATA 0x0au
#define MTP_DATA_LAST 0x0cu

#define DIR_CAM_TO_HOST 1u
#define DIR_HOST_TO_CAM 2u

#define MTP_OPEN_SESSION 0x1002u
#define MTP_GET_DEVICE_INFO 0x1001u

#define EOS_SET_REMOTE_MODE 0x9114u
#define EOS_SET_EVENT_MODE 0x9115u
#define EOS_GET_EVENT 0x9116u
#define EOS_SET_PROP_EX 0x9110u
#define EOS_SET_CAPACITY 0x913du
#define EOS_SET_PC_CONNECT 0x911au
#define EOS_GET_VF_DATA 0x9153u

#define PROP_CAPACITY 0xd11cu
#define PROP_EVF_MODE 0xd1b1u
#define PROP_EVF_OUT 0xd1b0u

#define R_OK 0x2001u
#define R_BUSY 0x2019u
#define R_NOT_READY 0xa102u

static const uint8_t CLIENT_GUID[16] = {
  0x43, 0x41, 0x4e, 0x4f, 0x4e, 0x57, 0x49, 0x46,
  0x49, 0x57, 0x45, 0x42, 0x43, 0x41, 0x4d, 0x01
};
static const char *CLIENT_NAME = "CanonWifiWebcam";
#define HOST_VERSION 0x00010000u

static __declspec(thread) char g_err[512];

static void (*g_log)(const char *) = NULL;

void m50_set_log(void (*cb)(const char *))
{
  g_log = cb;
}

static void phase(const char *msg)
{
  if (g_log)
    g_log(msg);
}

const char *m50_last_error(void)
{
  return g_err;
}

static void set_err(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_err, sizeof(g_err), fmt, ap);
  va_end(ap);
}

struct m50_session {
  SOCKET cmd;
  SOCKET evt;
  uint32_t txn;
  uint32_t session_id;
  int transient_n;
};

static uint32_t rd32le(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32le(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static int wait_sock(SOCKET s, int want_read, int timeout_ms)
{
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(s, &fds);
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  int n = select(0, want_read ? &fds : NULL, want_read ? NULL : &fds, NULL, &tv);
  if (n == 0) {
    set_err("timed out after %dms", timeout_ms);
    return -1;
  }
  if (n < 0) {
    set_err("select failed: %d", WSAGetLastError());
    return -1;
  }
  return 0;
}

static int send_all(SOCKET s, const uint8_t *buf, int len, int timeout_ms)
{
  int off = 0;
  while (off < len) {
    if (wait_sock(s, 0, timeout_ms) != 0)
      return -1;
    int n = send(s, (const char *)buf + off, len - off, 0);
    if (n <= 0) {
      set_err("send failed: %d", WSAGetLastError());
      return -1;
    }
    off += n;
  }
  return 0;
}

/* Reads one length-prefixed payload (returns malloc'd payload, sets *len). */
static uint8_t *read_payload(SOCKET s, int *len, int timeout_ms)
{
  uint8_t hdr[4];
  int off = 0;
  while (off < 4) {
    if (wait_sock(s, 1, timeout_ms) != 0)
      return NULL;
    int n = recv(s, (char *)hdr + off, 4 - off, 0);
    if (n == 0) {
      set_err("connection closed by camera");
      return NULL;
    }
    if (n < 0) {
      int e = WSAGetLastError();
      if (e == WSAECONNRESET) {
        set_err("read ECONNRESET");
        return NULL;
      }
      set_err("recv failed: %d", e);
      return NULL;
    }
    off += n;
  }
  uint32_t total = rd32le(hdr);
  if (total < 4 || total > (32u << 20)) {
    set_err("bad frame length %u", total);
    return NULL;
  }
  uint32_t plen = total - 4;
  uint8_t *p = (uint8_t *)malloc(plen ? plen : 1);
  if (!p) {
    set_err("out of memory");
    return NULL;
  }
  off = 0;
  while ((uint32_t)off < plen) {
    if (wait_sock(s, 1, timeout_ms) != 0) {
      free(p);
      return NULL;
    }
    int n = recv(s, (char *)p + off, plen - off, 0);
    if (n == 0) {
      free(p);
      set_err("connection closed by camera");
      return NULL;
    }
    if (n < 0) {
      int e = WSAGetLastError();
      free(p);
      if (e == WSAECONNRESET)
        set_err("read ECONNRESET");
      else
        set_err("recv failed: %d", e);
      return NULL;
    }
    off += n;
  }
  *len = (int)plen;
  return p;
}

static int write_payload(SOCKET s, const uint8_t *payload, int plen, int timeout_ms)
{
  uint8_t *frame = (uint8_t *)malloc((size_t)plen + 4);
  if (!frame) {
    set_err("out of memory");
    return -1;
  }
  wr32le(frame, (uint32_t)plen + 4);
  memcpy(frame + 4, payload, (size_t)plen);
  int r = send_all(s, frame, plen + 4, timeout_ms);
  free(frame);
  return r;
}

static SOCKET dial(const char *host, int port, const char *local_address, int timeout_ms)
{
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  char portstr[16];
  snprintf(portstr, sizeof(portstr), "%d", port);
  struct addrinfo *ai = NULL;
  if (getaddrinfo(host, portstr, &hints, &ai) != 0) {
    set_err("DNS/resolve failed for %s", host);
    return INVALID_SOCKET;
  }
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    set_err("socket failed: %d", WSAGetLastError());
    freeaddrinfo(ai);
    return INVALID_SOCKET;
  }
  if (local_address && local_address[0]) {
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, local_address, &bind_addr.sin_addr) != 1) {
      set_err("bad local address %s", local_address);
      closesocket(s);
      freeaddrinfo(ai);
      return INVALID_SOCKET;
    }
    if (bind(s, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
      set_err("bind %s failed: %d", local_address, WSAGetLastError());
      closesocket(s);
      freeaddrinfo(ai);
      return INVALID_SOCKET;
    }
  }
  u_long nb = 1;
  ioctlsocket(s, FIONBIO, &nb);
  int r = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
  freeaddrinfo(ai);
  if (r != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
    set_err("connect failed: %d", WSAGetLastError());
    closesocket(s);
    return INVALID_SOCKET;
  }
  if (wait_sock(s, 0, timeout_ms) != 0) {
    closesocket(s);
    return INVALID_SOCKET;
  }
  int err = 0;
  int errlen = sizeof(err);
  getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen);
  if (err != 0) {
    if (err == WSAECONNREFUSED)
      set_err("connect ECONNREFUSED");
    else if (err == WSAETIMEDOUT)
      set_err("connect ETIMEDOUT");
    else
      set_err("connect failed: %d", err);
    closesocket(s);
    return INVALID_SOCKET;
  }
  nb = 0;
  ioctlsocket(s, FIONBIO, &nb);
  return s;
}

/* Returns connection number, or -1. */
static int init_command(SOCKET s, uint32_t *conn_out, int timeout_ms)
{
  size_t namelen = strlen(CLIENT_NAME) + 1; /* with NUL */
  size_t u16len = namelen * 2;
  size_t plen = 4 + 16 + u16len + 4;
  uint8_t *p = (uint8_t *)malloc(plen);
  if (!p) {
    set_err("out of memory");
    return -1;
  }
  wr32le(p, PTP_INIT_CMD);
  memcpy(p + 4, CLIENT_GUID, 16);
  for (size_t i = 0; i < namelen; i++) {
    p[20 + i * 2] = (uint8_t)CLIENT_NAME[i];
    p[20 + i * 2 + 1] = 0;
  }
  /* DataView.setUint32 writes low 4 bytes of the u16 buffer region end */
  uint8_t *vend = p + 20 + u16len;
  vend[0] = (uint8_t)HOST_VERSION;
  vend[1] = (uint8_t)(HOST_VERSION >> 8);
  vend[2] = (uint8_t)(HOST_VERSION >> 16);
  vend[3] = (uint8_t)(HOST_VERSION >> 24);
  int w = write_payload(s, p, (int)plen, timeout_ms);
  free(p);
  if (w != 0)
    return -1;
  int alen = 0;
  uint8_t *ack = read_payload(s, &alen, timeout_ms);
  if (!ack)
    return -1;
  if (alen < 28 || rd32le(ack) != PTP_INIT_ACK) {
    set_err("unexpected Init ACK (len %d)", alen);
    free(ack);
    return -1;
  }
  *conn_out = rd32le(ack + 4);
  free(ack);
  return 0;
}

static int init_event(SOCKET s, uint32_t conn, int timeout_ms)
{
  uint8_t p[8];
  wr32le(p, PTP_INIT_EVT);
  wr32le(p + 4, conn);
  if (write_payload(s, p, 8, timeout_ms) != 0)
    return -1;
  int alen = 0;
  uint8_t *ack = read_payload(s, &alen, timeout_ms);
  if (!ack)
    return -1;
  int ok = (alen >= 4 && rd32le(ack) == PTP_INIT_EVT_ACK) ? 0 : -1;
  if (ok != 0)
    set_err("unexpected Init Event ACK");
  free(ack);
  return ok;
}

static int accepted(uint16_t code, const uint16_t *list, int n)
{
  for (int i = 0; i < n; i++)
    if (list[i] == code)
      return 1;
  return 0;
}

/* Camera-to-host command. Returns response code, sets *data (malloc'd, maybe empty). */
static int cam_command(m50_session *s, SOCKET sock, uint16_t op, const uint32_t *params, int nparams,
                       const uint16_t *accept, int naccept, uint8_t **data, int *dlen, int timeout_ms)
{
  uint32_t txn = s->txn++;
  size_t plen = 14 + (size_t)nparams * 4;
  uint8_t *p = (uint8_t *)malloc(plen);
  if (!p) {
    set_err("out of memory");
    return -1;
  }
  wr32le(p, MTP_CMD_REQ);
  wr32le(p + 4, DIR_CAM_TO_HOST);
  p[8] = (uint8_t)op;
  p[9] = (uint8_t)(op >> 8);
  wr32le(p + 10, txn);
  for (int i = 0; i < nparams; i++)
    wr32le(p + 14 + (size_t)i * 4, params[i]);
  if (write_payload(sock, p, (int)plen, timeout_ms) != 0) {
    free(p);
    return -1;
  }
  free(p);

  int flen = 0;
  uint8_t *first = read_payload(sock, &flen, timeout_ms);
  if (!first)
    return -1;
  if (flen < 4) {
    free(first);
    set_err("short MTP payload");
    return -1;
  }
  uint32_t ptype = rd32le(first);
  if (ptype == MTP_CMD_RESP) {
    if (flen < 10) {
      free(first);
      set_err("short MTP response");
      return -1;
    }
    uint16_t code = (uint16_t)(first[4] | (first[5] << 8));
    uint32_t rtxn = rd32le(first + 6);
    free(first);
    if (rtxn != txn) {
      set_err("txn mismatch (want %u got %u)", txn, rtxn);
      return -1;
    }
    if (!accepted(code, accept, naccept)) {
      set_err("MTP response 0x%x", code);
      return -1;
    }
    *data = (uint8_t *)malloc(1);
    if (!*data) {
      set_err("out of memory");
      return -1;
    }
    *dlen = 0;
    return code;
  }
  if (ptype != MTP_DATA_START) {
    free(first);
    set_err("unexpected MTP payload type %u", ptype);
    return -1;
  }
  if (flen < 16) {
    free(first);
    set_err("short MTP data start");
    return -1;
  }
  uint32_t dtxn = rd32le(first + 4);
  free(first);
  if (dtxn != txn) {
    set_err("data txn mismatch");
    return -1;
  }
  uint8_t *buf = NULL;
  size_t cap = 0, total = 0;
  for (;;) {
    int clen = 0;
    uint8_t *chunk = read_payload(sock, &clen, timeout_ms);
    if (!chunk) {
      free(buf);
      return -1;
    }
    if (clen < 8) {
      free(chunk);
      free(buf);
      set_err("short MTP data chunk");
      return -1;
    }
    uint32_t ctype = rd32le(chunk);
    if (ctype != MTP_DATA && ctype != MTP_DATA_LAST) {
      free(chunk);
      free(buf);
      set_err("unexpected MTP data type %u", ctype);
      return -1;
    }
    if (rd32le(chunk + 4) != txn) {
      free(chunk);
      free(buf);
      set_err("chunk txn mismatch");
      return -1;
    }
    size_t blen = (size_t)clen - 8;
    if (total + blen > cap) {
      size_t ncap = total + blen + 65536;
      uint8_t *nb = (uint8_t *)realloc(buf, ncap);
      if (!nb) {
        free(chunk);
        free(buf);
        set_err("out of memory");
        return -1;
      }
      buf = nb;
      cap = ncap;
    }
    memcpy(buf + total, chunk + 8, blen);
    total += blen;
    int last = (ctype == MTP_DATA_LAST);
    free(chunk);
    if (last)
      break;
  }
  int rlen = 0;
  uint8_t *resp = read_payload(sock, &rlen, timeout_ms);
  if (!resp) {
    free(buf);
    return -1;
  }
  if (rlen < 10 || rd32le(resp) != MTP_CMD_RESP) {
    free(resp);
    free(buf);
    set_err("missing MTP response");
    return -1;
  }
  uint16_t code = (uint16_t)(resp[4] | (resp[5] << 8));
  uint32_t rtxn = rd32le(resp + 6);
  free(resp);
  if (rtxn != txn) {
    free(buf);
    set_err("resp txn mismatch");
    return -1;
  }
  if (!accepted(code, accept, naccept)) {
    free(buf);
    set_err("MTP response 0x%x", code);
    return -1;
  }
  *data = buf ? buf : (uint8_t *)malloc(1);
  *dlen = (int)total;
  return code;
}

/* Host-to-camera data command (property writes). Returns response code. */
static int host_data_command(m50_session *s, uint16_t op, const uint8_t *bytes, int blen,
                             const uint32_t *params, int nparams,
                             const uint16_t *accept, int naccept, int timeout_ms)
{
  uint32_t txn = s->txn++;
  size_t plen = 14 + (size_t)nparams * 4;
  uint8_t *p = (uint8_t *)malloc(plen);
  if (!p) {
    set_err("out of memory");
    return -1;
  }
  wr32le(p, MTP_CMD_REQ);
  wr32le(p + 4, DIR_HOST_TO_CAM);
  p[8] = (uint8_t)op;
  p[9] = (uint8_t)(op >> 8);
  wr32le(p + 10, txn);
  for (int i = 0; i < nparams; i++)
    wr32le(p + 14 + (size_t)i * 4, params[i]);
  if (write_payload(s->cmd, p, (int)plen, timeout_ms) != 0) {
    free(p);
    return -1;
  }
  free(p);

  uint8_t ds[16];
  wr32le(ds, MTP_DATA_START);
  wr32le(ds + 4, txn);
  /* u64 length, little endian (lengths here are tiny) */
  wr32le(ds + 8, (uint32_t)blen);
  wr32le(ds + 12, 0);
  if (write_payload(s->cmd, ds, 16, timeout_ms) != 0)
    return -1;

  uint8_t *dp = (uint8_t *)malloc((size_t)blen + 8);
  if (!dp) {
    set_err("out of memory");
    return -1;
  }
  wr32le(dp, MTP_DATA_LAST);
  wr32le(dp + 4, txn);
  memcpy(dp + 8, bytes, (size_t)blen);
  int w = write_payload(s->cmd, dp, blen + 8, timeout_ms);
  free(dp);
  if (w != 0)
    return -1;

  int rlen = 0;
  uint8_t *resp = read_payload(s->cmd, &rlen, timeout_ms);
  if (!resp)
    return -1;
  if (rlen < 10 || rd32le(resp) != MTP_CMD_RESP) {
    free(resp);
    set_err("missing MTP response");
    return -1;
  }
  uint16_t code = (uint16_t)(resp[4] | (resp[5] << 8));
  uint32_t rtxn = rd32le(resp + 6);
  free(resp);
  if (rtxn != txn) {
    set_err("resp txn mismatch");
    return -1;
  }
  if (!accepted(code, accept, naccept)) {
    set_err("MTP response 0x%x", code);
    return -1;
  }
  return code;
}

static void msleep(int ms)
{
  Sleep((DWORD)(ms > 0 ? ms : 0));
}

static int close_session(m50_session *s, int timeout_ms)
{
  static const uint16_t acc[] = { R_OK, R_BUSY, 0x2003 };
  uint8_t *d = NULL;
  int dl = 0;
  int r = cam_command(s, s->cmd, 0x1003, NULL, 0, acc, 3, &d, &dl, timeout_ms);
  free(d);
  return r < 0 ? -1 : 0;
}

static void teardown(m50_session *s)
{
  /* Just close the sockets. An explicit CloseSession makes the M50 show
   * Err 12 and refuse follow-up sessions until dismissed. */
  m50_close(s);
}

static int poll_events(m50_session *s, int timeout_ms)
{
  static const uint16_t acc[] = { R_OK, R_BUSY, R_NOT_READY };
  uint8_t *d = NULL;
  int dl = 0;
  int r = cam_command(s, s->cmd, EOS_GET_EVENT, NULL, 0, acc, 3, &d, &dl, timeout_ms);
  free(d);
  return r < 0 ? -1 : 0;
}

/* Poll events and report payload size + first bytes (diagnoses stalls). */
static int poll_events_logged(m50_session *s, int timeout_ms, void (*logfn)(const char *))
{
  static const uint16_t acc[] = { R_OK, R_BUSY, R_NOT_READY };
  uint8_t *d = NULL;
  int dl = 0;
  int r = cam_command(s, s->cmd, EOS_GET_EVENT, NULL, 0, acc, 3, &d, &dl, timeout_ms);
  if (r >= 0 && logfn && dl > 8) {
    char msg[160];
    int n = dl < 40 ? dl : 40;
    int off = snprintf(msg, sizeof(msg), "event %d bytes: ", dl);
    for (int i = 0; i < n && off < (int)sizeof(msg) - 4; i++)
      off += snprintf(msg + off, sizeof(msg) - (size_t)off, "%02x", d[i]);
    logfn(msg);
  }
  free(d);
  return r < 0 ? -1 : 0;
}

/* Plain Canon command that must end OK; BUSY/NOT-READY retried ~8s. */
static int op_ok(m50_session *s, const char *name, uint16_t op, const uint32_t *params, int nparams,
                 int timeout_ms)
{
  static const uint16_t acc[] = { R_OK, R_BUSY, R_NOT_READY };
  DWORD deadline = GetTickCount() + 8000;
  for (;;) {
    uint8_t *d = NULL;
    int dl = 0;
    int code = cam_command(s, s->cmd, op, params, nparams, acc, 3, &d, &dl, timeout_ms);
    free(d);
    if (code == R_OK)
      return 0;
    if ((code == R_BUSY || code == R_NOT_READY) && GetTickCount() < deadline) {
      if (poll_events(s, timeout_ms) != 0)
        return -1;
      msleep(100);
      continue;
    }
    set_err("%s failed 0x%x", name, code < 0 ? 0xffff : (unsigned)code);
    return -1;
  }
}

static int write_prop_retry(m50_session *s, uint32_t prop, uint32_t value, int vlen, int timeout_ms)
{
  static const uint16_t acc[] = { R_OK, R_BUSY, R_NOT_READY };
  uint8_t payload[12];
  memset(payload, 0, sizeof(payload));
  wr32le(payload, 12);
  wr32le(payload + 4, prop);
  if (vlen == 2) {
    payload[8] = (uint8_t)value;
    payload[9] = (uint8_t)(value >> 8);
  } else {
    wr32le(payload + 8, value);
  }
  DWORD deadline = GetTickCount() + 5000;
  for (;;) {
    int code = host_data_command(s, EOS_SET_PROP_EX, payload, 12, NULL, 0, acc, 3, timeout_ms);
    if (code < 0)
      return -1;
    if (code == R_OK)
      return 0;
    if (GetTickCount() >= deadline) {
      set_err("timed out writing property 0x%x", prop);
      return -1;
    }
    if (poll_events(s, timeout_ms) != 0)
      return -1;
    msleep(10);
  }
}

m50_session *m50_connect(const char *host, int port, const char *local_address, int init_timeout_ms)
{
  static int ws_init = 0;
  if (!ws_init) {
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
      set_err("WSAStartup failed");
      return NULL;
    }
    ws_init = 1;
  }
  m50_session *s = (m50_session *)calloc(1, sizeof(*s));
  if (!s) {
    set_err("out of memory");
    return NULL;
  }
  s->cmd = INVALID_SOCKET;
  s->evt = INVALID_SOCKET;
  s->txn = 1;

  static const uint16_t acc_ok[] = { R_OK };
  static const uint16_t acc_all[] = { R_OK, R_BUSY, R_NOT_READY };

  s->cmd = dial(host, port, local_address, 10000);
  if (s->cmd == INVALID_SOCKET) {
    free(s);
    return NULL;
  }
  s->evt = dial(host, port, local_address, 10000);
  if (s->evt == INVALID_SOCKET) {
    set_err("event channel connect failed: %s", m50_last_error());
    closesocket(s->cmd);
    free(s);
    return NULL;
  }

  uint32_t conn = 0;
  phase("init command sent, waiting for ACK (accept prompt)");
  if (init_command(s->cmd, &conn, init_timeout_ms) != 0) {
    teardown(s);
    return NULL;
  }
  s->session_id = conn;
  phase("init command ACKed");

  if (init_event(s->evt, conn, 10000) != 0) {
    teardown(s);
    return NULL;
  }
  phase("event channel ACKed");

  /* MTP session */
  {
    uint32_t p = conn;
    uint8_t *d = NULL;
    int dl = 0;
    if (cam_command(s, s->cmd, MTP_OPEN_SESSION, &p, 1, acc_ok, 1, &d, &dl, 10000) < 0) {
      free(d);
      teardown(s);
      return NULL;
    }
    free(d);
  }
  {
    uint8_t *d = NULL;
    int dl = 0;
    if (cam_command(s, s->cmd, MTP_GET_DEVICE_INFO, NULL, 0, acc_ok, 1, &d, &dl, 10000) < 0) {
      free(d);
      teardown(s);
      return NULL;
    }
    free(d);
  }
  phase("MTP session open, device info read");

  /* Canon Live View bootstrap (T100-verified, M50-verified 2026-10-12) */
  {
    uint32_t p1 = 1;
    if (op_ok(s, "SetRemoteMode", EOS_SET_REMOTE_MODE, &p1, 1, 10000) != 0) {
      teardown(s);
      return NULL;
    }
    uint32_t p2 = 1;
    if (op_ok(s, "SetEventMode", EOS_SET_EVENT_MODE, &p2, 1, 10000) != 0) {
      teardown(s);
      return NULL;
    }
    uint32_t p3 = 0x1fff;
    if (op_ok(s, "SetCapacity", EOS_SET_CAPACITY, &p3, 1, 10000) != 0) {
      teardown(s);
      return NULL;
    }
    if (poll_events(s, 10000) != 0) {
      teardown(s);
      return NULL;
    }
    if (write_prop_retry(s, PROP_CAPACITY, 4, 4, 10000) != 0) {
      teardown(s);
      return NULL;
    }
    {
      uint32_t pp[3] = { 0x0fffffff, 0x1000, 1 };
      if (op_ok(s, "SetPcConnectMode", EOS_SET_PC_CONNECT, pp, 3, 10000) != 0) {
        teardown(s);
        return NULL;
      }
    }
    if (poll_events(s, 10000) != 0) {
      teardown(s);
      return NULL;
    }
    if (write_prop_retry(s, PROP_EVF_MODE, 1, 2, 10000) != 0) {
      /* Fresh Movie mode refuses this write; the output select below is
       * what actually starts the feed. Non-fatal by hardware evidence. */
      phase("EVF mode write refused, continuing");
    } else {
      phase("EVF mode set");
    }
    /* M50/DIGIC-8 output modes: 0 none, 1 camera, 2 host, 3 cameraAndHost.
     * cameraAndHost keeps the LCD live while streaming to the PC. */
    if (write_prop_retry(s, PROP_EVF_OUT, 3, 4, 10000) != 0) {
      teardown(s);
      return NULL;
    }
    phase("EVF output set");
    if (poll_events(s, 10000) != 0) {
      teardown(s);
      return NULL;
    }
  }

  /* One full event dump: right after bootstrap the camera reports its
   * complete property state (aperture/shutter/ISO/focus...). */
  poll_events_logged(s, 10000, phase);

  msleep(1000);
  return s;
}

static int extract_jpeg(const uint8_t *frame, int flen, uint8_t **out, int *out_len)
{
  int off = 0;
  while (off < flen) {
    if (flen - off < 8)
      break;
    uint32_t blen = rd32le(frame + off);
    uint32_t type = rd32le(frame + off + 4);
    if (blen < 8 || off + (int)blen > flen)
      break;
    if (type == 1 || type == 9 || type == 11) {
      const uint8_t *cand = frame + off + 8;
      int clen = (int)blen - 8;
      if (clen >= 4 && cand[0] == 0xff && cand[1] == 0xd8 &&
          cand[clen - 2] == 0xff && cand[clen - 1] == 0xd9) {
        uint8_t *jpg = (uint8_t *)malloc((size_t)clen);
        if (!jpg) {
          set_err("out of memory");
          return -1;
        }
        memcpy(jpg, cand, (size_t)clen);
        *out = jpg;
        *out_len = clen;
        return 1;
      }
    }
    off += (int)blen;
  }
  return 0;
}

int m50_keepalive(m50_session *s)
{
  static const uint16_t acc[] = { R_OK, R_BUSY, R_NOT_READY };
  uint8_t *d = NULL;
  int dl = 0;
  int r = cam_command(s, s->cmd, EOS_GET_EVENT, NULL, 0, acc, 3, &d, &dl, 10000);
  free(d);
  return r < 0 ? -1 : 0;
}

void m50_raw_close(void *vs)
{
  m50_disconnect((m50_session *)vs);
}

int m50_raw_connect(const char *host, int port, const char *local_address, int init_timeout_ms,
                    void **sess_out)
{
  *sess_out = NULL;
  if (ensure_ws() != 0)
    return -1;
  m50_session *s = (m50_session *)calloc(1, sizeof(*s));
  if (!s) {
    set_err("out of memory");
    return -1;
  }
  s->cmd = INVALID_SOCKET;
  s->evt = INVALID_SOCKET;
  s->txn = 1;
  static const uint16_t acc_ok[] = { R_OK };
  s->cmd = dial(host, port, local_address, 10000);
  if (s->cmd == INVALID_SOCKET) {
    free(s);
    return -1;
  }
  s->evt = dial(host, port, local_address, 10000);
  if (s->evt == INVALID_SOCKET) {
    closesocket(s->cmd);
    free(s);
    return -1;
  }
  uint32_t conn = 0;
  if (init_command(s->cmd, &conn, init_timeout_ms) != 0) {
    teardown(s);
    return -1;
  }
  s->session_id = conn;
  if (init_event(s->evt, conn, 10000) != 0) {
    teardown(s);
    return -1;
  }
  {
    uint8_t *d = NULL;
    int dl = 0;
    if (cam_command(s, s->cmd, MTP_OPEN_SESSION, &conn, 1, acc_ok, 1, &d, &dl, 10000) < 0) {
      free(d);
      teardown(s);
      return -1;
    }
    free(d);
  }
  {
    uint8_t *d = NULL;
    int dl = 0;
    if (cam_command(s, s->cmd, MTP_GET_DEVICE_INFO, NULL, 0, acc_ok, 1, &d, &dl, 10000) < 0) {
      free(d);
      teardown(s);
      return -1;
    }
    free(d);
  }
  *sess_out = s;
  return 0;
}

int m50_raw_bootstrap(void *vs, int minimal)
{
  return m50_raw_bootstrap_tol(vs, minimal, 0);
}

int m50_raw_bootstrap_tol(void *vs, int minimal, int tolerant)
{
  m50_session *s = (m50_session *)vs;
  static const uint16_t acc_all[] = { R_OK, R_BUSY, R_NOT_READY };
  /* Plain commands: retry BUSY/NOT-READY briefly (movie transitions lag). */
#define OP(name, op, pp, np)                                                  \
  do {                                                                        \
    DWORD dl = GetTickCount() + 8000;                                         \
    for (;;) {                                                                \
      uint8_t *dd = NULL;                                                     \
      int ddl = 0;                                                            \
      int cc = cam_command(s, s->cmd, (op), (pp), (np), acc_all, 3, &dd, &ddl, \
                           10000);                                            \
      free(dd);                                                               \
      if (cc == R_OK)                                                         \
        break;                                                                \
      if ((cc == R_BUSY || cc == R_NOT_READY) && GetTickCount() < dl) {       \
        poll_events(s, 10000);                                                \
        msleep(100);                                                          \
        continue;                                                             \
      }                                                                       \
      printf("FAIL %s code=0x%x\n", (name), cc < 0 ? 0xffff : (unsigned)cc);   \
      return -1;                                                              \
    }                                                                         \
    printf("ok %s\n", (name));                                                \
  } while (0)
  {
    uint32_t p = 1;
    OP("remote", EOS_SET_REMOTE_MODE, &p, 1);
  }
  {
    uint32_t p = 1;
    OP("eventmode", EOS_SET_EVENT_MODE, &p, 1);
  }
  if (!minimal) {
    uint32_t p = 0x1fff;
    OP("capacity", EOS_SET_CAPACITY, &p, 1);
    if (poll_events(s, 10000) != 0) {
      printf("FAIL poll\n");
      return -1;
    }
    if (write_prop_retry(s, PROP_CAPACITY, 4, 4, 10000) != 0) {
      printf("FAIL d11c: %s\n", m50_last_error());
      return -1;
    }
    printf("ok d11c\n");
    {
      uint32_t pp[3] = { 0x0fffffff, 0x1000, 1 };
      OP("pcconnect", EOS_SET_PC_CONNECT, pp, 3);
    }
    if (poll_events(s, 10000) != 0) {
      printf("FAIL poll\n");
      return -1;
    }
  }
  if (write_prop_retry(s, PROP_EVF_MODE, 1, 2, 10000) != 0) {
    if (tolerant) {
      printf("warn evfmode: %s (continuing)\n", m50_last_error());
    } else {
      printf("FAIL evfmode: %s\n", m50_last_error());
      return -1;
    }
  } else {
    printf("ok evfmode\n");
  }
  if (write_prop_retry(s, PROP_EVF_OUT, 3, 4, 10000) != 0) {
    printf("FAIL evfout: %s\n", m50_last_error());
    return -1;
  }
  printf("ok evfout\n");
  if (poll_events(s, 10000) != 0) {
    printf("FAIL poll\n");
    return -1;
  }
  msleep(1000);
  return 0;
}

int m50_raw_vf(void *vs, int p2, int p3, uint8_t **out_jpeg, int *out_len)
{
  m50_session *s = (m50_session *)vs;
  static const uint16_t acc[] = { R_OK, R_BUSY, R_NOT_READY };
  uint32_t params[3] = { 0x00200000, (uint32_t)p2, (uint32_t)p3 };
  uint8_t *data = NULL;
  int dlen = 0;
  int code = cam_command(s, s->cmd, EOS_GET_VF_DATA, params, 3, acc, 3, &data, &dlen, 10000);
  if (code < 0) {
    free(data);
    return -1;
  }
  if (code == R_BUSY || code == R_NOT_READY) {
    free(data);
    poll_events(s, 10000);
    return 0;
  }
  uint8_t *jpg = NULL;
  int jlen = 0;
  int r = extract_jpeg(data, dlen, &jpg, &jlen);
  free(data);
  if (r <= 0)
    return 0;
  *out_jpeg = jpg;
  *out_len = jlen;
  return 1;
}

int m50_raw_evf(void *vs, int value)
{
  m50_session *s = (m50_session *)vs;
  uint8_t payload[12];
  memset(payload, 0, sizeof(payload));
  payload[0] = 12;
  payload[4] = 0xb0;
  payload[5] = 0xd1;
  payload[8] = (uint8_t)value;
  static const uint16_t acc[] = { R_OK, R_BUSY, R_NOT_READY };
  int code = host_data_command(s, EOS_SET_PROP_EX, payload, 12, NULL, 0, acc, 3, 10000);
  return code == R_OK ? 0 : -1;
}

int m50_get_frame(m50_session *s, uint8_t **out_jpeg, int *out_len)
{
  static const uint16_t acc[] = { R_OK, R_BUSY, R_NOT_READY };
  uint32_t params[3] = { 0x00200000, 1, 0 };
  uint8_t *data = NULL;
  int dlen = 0;
  int code = cam_command(s, s->cmd, EOS_GET_VF_DATA, params, 3, acc, 3, &data, &dlen, 10000);
  if (code < 0) {
    free(data);
    return -1;
  }
  if (code == R_BUSY || code == R_NOT_READY) {
    free(data);
    s->transient_n++;
    if (s->transient_n % 100 == 0) {
      if (poll_events_logged(s, 10000, phase) != 0)
        return -1;
    } else if (poll_events(s, 10000) != 0) {
      return -1;
    }
    return 0;
  }
  uint8_t *jpg = NULL;
  int jlen = 0;
  int r = extract_jpeg(data, dlen, &jpg, &jlen);
  free(data);
  if (r < 0)
    return -1;
  if (r == 0)
    return 0;
  *out_jpeg = jpg;
  *out_len = jlen;
  return 1;
}

void m50_close(m50_session *s)
{
  if (!s)
    return;
  if (s->evt != INVALID_SOCKET)
    closesocket(s->evt);
  if (s->cmd != INVALID_SOCKET)
    closesocket(s->cmd);
  free(s);
}

void m50_disconnect(m50_session *s)
{
  /* See teardown(): never send CloseSession, just close. */
  m50_close(s);
}

static int ensure_ws(void)
{
  static int done = 0;
  if (!done) {
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
      set_err("WSAStartup failed");
      return -1;
    }
    done = 1;
  }
  return 0;
}

/* ---- LAN discovery + local address selection ---- */

static const char *KNOWN_SERVICES[] = {
  "urn:schemas-canon-com:service:ICPO-SmartPhoneEOSSystemService:1",
  "urn:schemas-canon-com:service:ICPO-RemoteControlEOSSystemService:1",
  "urn:schemas-canon-com:service:ICPO-WFTEOSSystemService:1"
};

static void header_value(const char *msg, const char *name, char *out, int out_len)
{
  out[0] = '\0';
  size_t nlen = strlen(name);
  const char *line = msg;
  /* skip start line */
  const char *eol = strstr(line, "\n");
  line = eol ? eol + 1 : "";
  while (*line) {
    eol = strstr(line, "\n");
    size_t llen = eol ? (size_t)(eol - line) : strlen(line);
    while (llen > 0 && (line[llen - 1] == '\r' || line[llen - 1] == ' ' || line[llen - 1] == '\t'))
      llen--;
    if (llen > nlen + 1 && _strnicmp(line, name, nlen) == 0 && line[nlen] == ':') {
      const char *v = line + nlen + 1;
      while (*v == ' ' || *v == '\t')
        v++;
      size_t vlen = llen - (size_t)(v - line);
      if (vlen >= (size_t)out_len)
        vlen = (size_t)out_len - 1;
      memcpy(out, v, vlen);
      out[vlen] = '\0';
      return;
    }
    line = eol ? eol + 1 : "";
  }
}

static int known_service(const char *st)
{
  for (size_t i = 0; i < sizeof(KNOWN_SERVICES) / sizeof(KNOWN_SERVICES[0]); i++)
    if (strcmp(st, KNOWN_SERVICES[i]) == 0)
      return 1;
  return 0;
}

int m50_discover(m50_found *out, int max, int timeout_ms, const char *local_address)
{
  if (ensure_ws() != 0 || max <= 0)
    return 0;
  SOCKET rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  SOCKET tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (rx == INVALID_SOCKET || tx == INVALID_SOCKET) {
    if (rx != INVALID_SOCKET)
      closesocket(rx);
    if (tx != INVALID_SOCKET)
      closesocket(tx);
    set_err("UDP socket failed");
    return 0;
  }
  int reuse = 1;
  setsockopt(rx, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

  struct sockaddr_in bind_addr;
  memset(&bind_addr, 0, sizeof(bind_addr));
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons(1900);
  if (local_address && local_address[0]) {
    if (inet_pton(AF_INET, local_address, &bind_addr.sin_addr) != 1) {
      closesocket(rx);
      closesocket(tx);
      set_err("bad local address %s", local_address);
      return 0;
    }
  } else {
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  }
  if (bind(rx, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
    /* 1900 may be held by another listener; discovery still works via tx. */
    closesocket(rx);
    rx = INVALID_SOCKET;
  } else {
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
    mreq.imr_interface.s_addr = (local_address && local_address[0] &&
                                 inet_pton(AF_INET, local_address, &mreq.imr_interface) == 1)
                                ? mreq.imr_interface.s_addr
                                : htonl(INADDR_ANY);
    setsockopt(rx, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mreq, sizeof(mreq));
  }

  if (local_address && local_address[0]) {
    struct in_addr iface;
    if (inet_pton(AF_INET, local_address, &iface) == 1)
      setsockopt(tx, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&iface, sizeof(iface));
  }
  unsigned char ttl = 1;
  setsockopt(tx, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));

  struct sockaddr_in dest;
  memset(&dest, 0, sizeof(dest));
  dest.sin_family = AF_INET;
  dest.sin_port = htons(1900);
  dest.sin_addr.s_addr = inet_addr("239.255.255.250");

  for (size_t i = 0; i < sizeof(KNOWN_SERVICES) / sizeof(KNOWN_SERVICES[0]); i++) {
    char msg[512];
    snprintf(msg, sizeof(msg),
             "M-SEARCH * HTTP/1.1\r\nHOST:239.255.255.250:1900\r\nMAN:\"ssdp:discover\"\r\nMX:1\r\nST:%s\r\n\r\n",
             KNOWN_SERVICES[i]);
    sendto(tx, msg, (int)strlen(msg), 0, (struct sockaddr *)&dest, sizeof(dest));
  }

  int found = 0;
  DWORD deadline = GetTickCount() + (DWORD)timeout_ms;
  char buf[2048];
  for (;;) {
    DWORD now = GetTickCount();
    if (now >= deadline)
      break;
    fd_set fds;
    FD_ZERO(&fds);
    int nfds = 0;
    if (rx != INVALID_SOCKET) {
      FD_SET(rx, &fds);
      nfds = 1;
    }
    FD_SET(tx, &fds);
    struct timeval tv;
    long remain = (long)(deadline - now);
    tv.tv_sec = remain / 1000;
    tv.tv_usec = (remain % 1000) * 1000;
    int n = select(0, &fds, NULL, NULL, &tv);
    if (n <= 0)
      break;
    for (int k = 0; k < 2; k++) {
      SOCKET s = (k == 0) ? rx : tx;
      if (s == INVALID_SOCKET || !FD_ISSET(s, &fds))
        continue;
      struct sockaddr_in from;
      int fromlen = sizeof(from);
      int r = recvfrom(s, buf, (int)sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fromlen);
      if (r <= 0)
        continue;
      buf[r] = '\0';
      char st[128] = "", nt[128] = "", loc[256] = "", usn[256] = "", nts[64] = "";
      header_value(buf, "ST", st, sizeof(st));
      header_value(buf, "NT", nt, sizeof(nt));
      header_value(buf, "LOCATION", loc, sizeof(loc));
      header_value(buf, "USN", usn, sizeof(usn));
      header_value(buf, "NTS", nts, sizeof(nts));
      const char *svc = st[0] ? st : nt;
      if (!known_service(svc) || loc[0] == '\0')
        continue;
      if (nts[0] && strcmp(nts, "ssdp:alive") != 0)
        continue;
      char ip[64];
      inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
      int dup = 0;
      for (int j = 0; j < found; j++)
        if (strcmp(out[j].ip, ip) == 0 && strcmp(out[j].service, svc) == 0)
          dup = 1;
      if (!dup && found < max) {
        snprintf(out[found].ip, sizeof(out[found].ip), "%s", ip);
        snprintf(out[found].location, sizeof(out[found].location), "%s", loc);
        snprintf(out[found].service, sizeof(out[found].service), "%s", svc);
        (void)usn;
        found++;
      }
    }
  }
  if (rx != INVALID_SOCKET)
    closesocket(rx);
  closesocket(tx);
  return found;
}

/* Minimal HTTP GET for the UPnP descriptor (best effort, camera HTTP is flaky). */
int m50_verify_ex(const char *host, const char *local_address, char *model, int model_len,
                  char *service, int service_len)
{
  model[0] = '\0';
  if (service)
    service[0] = '\0';
  if (ensure_ws() != 0)
    return 0;
  SOCKET s = dial(host, 49152, local_address, 4000);
  if (s == INVALID_SOCKET)
    return 0;
  const char *req = "GET /upnp/CameraDevDesc.xml HTTP/1.0\r\nHost: x\r\n\r\n";
  if (send_all(s, (const uint8_t *)req, (int)strlen(req), 4000) != 0) {
    closesocket(s);
    return 0;
  }
  char buf[8192];
  int total = 0;
  for (;;) {
    if (wait_sock(s, 1, 4000) != 0)
      break;
    int n = recv(s, buf + total, (int)sizeof(buf) - 1 - total, 0);
    if (n <= 0)
      break;
    total += n;
    buf[total] = '\0';
    if (total > 128 && strstr(buf, "</root>") != NULL)
      break;
    if (total >= (int)sizeof(buf) - 1)
      break;
  }
  closesocket(s);
  if (total <= 0)
    return 0;
  buf[total] = '\0';
  const char *tag = strstr(buf, "<modelName>");
  if (!tag)
    return 0;
  tag += 11;
  const char *end = strstr(tag, "</modelName>");
  if (!end)
    return 0;
  size_t len = (size_t)(end - tag);
  if (len >= (size_t)model_len)
    len = (size_t)model_len - 1;
  memcpy(model, tag, len);
  model[len] = '\0';
  if (service) {
    const char *stag = strstr(buf, "<serviceType>");
    if (stag) {
      stag += 13;
      const char *send = strstr(stag, "</serviceType>");
      if (send) {
        size_t slen = (size_t)(send - stag);
        if (slen >= (size_t)service_len)
          slen = (size_t)service_len - 1;
        memcpy(service, stag, slen);
        service[slen] = '\0';
      }
    }
  }
  return 1;
}

int m50_verify(const char *host, const char *local_address, char *model, int model_len)
{
  return m50_verify_ex(host, local_address, model, model_len, NULL, 0);
}

int m50_pick_local(const char *camera_ip, char *out, int out_len)
{
  out[0] = '\0';
  ULONG size = 16 * 1024;
  IP_ADAPTER_ADDRESSES *addrs = (IP_ADAPTER_ADDRESSES *)malloc(size);
  if (!addrs)
    return 0;
  DWORD r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, addrs, &size);
  if (r == ERROR_BUFFER_OVERFLOW) {
    free(addrs);
    addrs = (IP_ADAPTER_ADDRESSES *)malloc(size);
    if (!addrs)
      return 0;
    r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, addrs, &size);
  }
  if (r != NO_ERROR) {
    free(addrs);
    return 0;
  }
  char first[64] = "";
  char same24[64] = "";
  for (IP_ADAPTER_ADDRESSES *a = addrs; a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp)
      continue;
    if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
      continue;
    for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
      if (u->Address.lpSockaddr->sa_family != AF_INET)
        continue;
      struct sockaddr_in *sin = (struct sockaddr_in *)u->Address.lpSockaddr;
      if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
        continue;
      char ip[64];
      inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
      if (!first[0]) {
        snprintf(first, sizeof(first), "%s", ip);
      }
      if (camera_ip && camera_ip[0]) {
        /* same /24 heuristic for a router LAN */
        const char *c1 = camera_ip, *c2 = ip;
        int dots = 0;
        size_t i = 0, j = 0, match = 0;
        while (c1[i] && c2[j] && dots < 3) {
          if (c1[i] != c2[j])
            break;
          if (c1[i] == '.')
            dots++;
          i++;
          j++;
        }
        if (dots == 3) {
          /* verify the /24 octets match fully (avoid 192.168.1 vs 192.168.10) */
          char n1[64], n2[64];
          snprintf(n1, sizeof(n1), "%s", camera_ip);
          snprintf(n2, sizeof(n2), "%s", ip);
          char *p1 = n1, *p2 = n2;
          match = 1;
          for (int k = 0; k < 3; k++) {
            char *d1 = strchr(p1, '.'), *d2 = strchr(p2, '.');
            if (!d1 || !d2) {
              match = 0;
              break;
            }
            *d1 = '\0';
            *d2 = '\0';
            if (strcmp(p1, p2) != 0) {
              match = 0;
              break;
            }
            p1 = d1 + 1;
            p2 = d2 + 1;
          }
          if (match && !same24[0])
            snprintf(same24, sizeof(same24), "%s", ip);
        }
      }
    }
  }
  free(addrs);
  const char *pick = same24[0] ? same24 : first;
  if (!pick[0])
    return 0;
  snprintf(out, out_len, "%s", pick);
  return 1;
}
