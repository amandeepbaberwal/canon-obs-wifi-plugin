/* m50-probe: headless session tester.
 * Usage: m50-probe <camera-ip> <local-ip> <variant>
 * variant 1 = full bootstrap (9114,9115,913d,d11c,911a,EVF)
 * variant 2 = minimal bootstrap (9114,9115,EVF only)
 * Prints mean brightness of first decoded frames for each VF param set.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <objbase.h>
#include <winsock2.h>

#include "m50.h"
#include "jpeg_wic.h"

/* variant + vf params are driven through env-free globals patched below */
static int g_variant = 1;
static int g_p2 = 1, g_p3 = 0;

extern int m50_run_variant(const char *host, const char *local);

/* We reach into m50.c via a tiny hook: re-run bootstrap pieces is not
 * exported, so variants are implemented here using raw ops. */
#include <stdint.h>
extern int m50_raw_connect(const char *host, int port, const char *local, int tmo,
                           void **sess_out);
extern int m50_raw_bootstrap(void *s, int minimal);
extern int m50_raw_vf(void *s, int p2, int p3, uint8_t **jpg, int *len);
extern void m50_raw_close(void *s);

static double brightness(const uint8_t *bgra, int w, int h, int stride)
{
  long long sum = 0;
  long long n = 0;
  for (int y = 0; y < h; y += 8) {
    for (int x = 0; x < w; x += 8) {
      const uint8_t *p = bgra + (size_t)y * stride + (size_t)x * 4;
      sum += p[0] + p[1] + p[2];
      n += 3;
    }
  }
  return n ? (double)sum / (double)n : 0.0;
}

int main(int argc, char **argv)
{
  if (argc < 4) {
    printf("usage: m50-probe <camera-ip> <local-ip> <variant 1|2|3>\n");
    return 2;
  }
  const char *host = argv[1];
  const char *local = argv[2];
  g_variant = atoi(argv[3]);
  int tolerant = (g_variant == 3);
  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  void *s = NULL;
  if (m50_raw_connect(host, 15740, local, 30000, &s) != 0) {
    printf("CONNECT FAIL\n");
    return 1;
  }
  printf("connected\n");
  if (m50_raw_bootstrap_tol(s, g_variant == 2 ? 1 : 0, tolerant) != 0) {
    printf("BOOTSTRAP FAIL\n");
    m50_raw_close(s);
    return 1;
  }
  printf("bootstrap ok (variant %d)\n", g_variant);
  static const int combos[][2] = { { 1, 0 }, { 0, 0 }, { 1, 1 }, { 0, 1 }, { 2, 0 } };
  for (size_t c = 0; c < sizeof(combos) / sizeof(combos[0]); c++) {
    int got = 0;
    for (int i = 0; i < 4; i++) {
      uint8_t *jpg = NULL;
      int len = 0;
      int r = m50_raw_vf(s, combos[c][0], combos[c][1], &jpg, &len);
      if (r < 0) {
        printf("p[%d,%d] fatal\n", combos[c][0], combos[c][1]);
        break;
      }
      if (r == 0)
        continue;
      uint8_t *bgra = NULL;
      int w = 0, h = 0, stride = 0;
      if (jpeg_to_bgra(jpg, len, &bgra, &w, &h, &stride)) {
        printf("p[%d,%d] %dx%d %d bytes mean=%.1f\n", combos[c][0], combos[c][1], w, h,
               len, brightness(bgra, w, h, stride));
        free(bgra);
        got = 1;
        free(jpg);
        break;
      }
      free(jpg);
    }
    if (!got)
      printf("p[%d,%d] no data\n", combos[c][0], combos[c][1]);
  }
  m50_raw_close(s);
  CoUninitialize();
  return 0;
}
