// Minimal APK (zip) reader: central directory index + stored/deflate entries.
// zlib is dlopen'd; every CFW ships libz.so.1.
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

typedef struct {
  const uint8_t *next_in; unsigned avail_in; unsigned long total_in;
  uint8_t *next_out; unsigned avail_out; unsigned long total_out;
  const char *msg; void *state; void *zalloc, *zfree, *opaque;
  int data_type; unsigned long adler, reserved;
} z_stream;

static int (*p_inflateInit2_)(z_stream *, int, const char *, int);
static int (*p_inflate)(z_stream *, int);
static int (*p_inflateEnd)(z_stream *);

typedef struct { char *name; uint32_t hash; uint16_t method; uint32_t csize, usize, lho; } zent;
static FILE *zf;
static pthread_mutex_t zlock = PTHREAD_MUTEX_INITIALIZER;
static zent *ents;
static int nents;
static int *tab;
static int tabsz;

static uint32_t hstr(const char *s) {
  uint32_t h = 2166136261u;
  while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
  return h;
}

static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int zip_open(const char *path) {
  zf = fopen(path, "rb");
  if (!zf) { LOG("cannot open %s", path); return -1; }
  fseeko(zf, 0, SEEK_END);
  off_t len = ftello(zf);
  size_t tail = len < 66000 ? (size_t)len : 66000;
  uint8_t *buf = malloc(tail);
  fseeko(zf, len - tail, SEEK_SET);
  if (fread(buf, 1, tail, zf) != tail) { free(buf); return -1; }
  long eocd = -1;
  for (long i = (long)tail - 22; i >= 0; i--)
    if (rd32(buf + i) == 0x06054b50) { eocd = i; break; }
  if (eocd < 0) { LOG("%s: no zip end record", path); free(buf); return -1; }
  nents = rd16(buf + eocd + 10);
  uint32_t cdsize = rd32(buf + eocd + 12), cdoff = rd32(buf + eocd + 16);
  free(buf);
  uint8_t *cd = malloc(cdsize);
  fseeko(zf, cdoff, SEEK_SET);
  if (fread(cd, 1, cdsize, zf) != cdsize) { free(cd); return -1; }
  ents = calloc(nents, sizeof(zent));
  tabsz = 1;
  while (tabsz < nents * 2) tabsz <<= 1;
  tab = malloc(tabsz * sizeof(int));
  memset(tab, -1, tabsz * sizeof(int));
  const uint8_t *p = cd;
  for (int i = 0; i < nents; i++) {
    if (rd32(p) != 0x02014b50) { nents = i; break; }
    uint16_t nl = rd16(p + 28), el = rd16(p + 30), cl = rd16(p + 32);
    zent *e = &ents[i];
    e->method = rd16(p + 10);
    e->csize = rd32(p + 20);
    e->usize = rd32(p + 24);
    e->lho = rd32(p + 42);
    e->name = strndup((const char *)p + 46, nl);
    e->hash = hstr(e->name);
    int s = e->hash & (tabsz - 1);
    while (tab[s] >= 0) s = (s + 1) & (tabsz - 1);
    tab[s] = i;
    p += 46 + nl + el + cl;
  }
  free(cd);
  LOG("apk %s: %d entries", path, nents);
  return 0;
}

static zent *find(const char *name) {
  if (!tab) return NULL;
  uint32_t h = hstr(name);
  for (int s = h & (tabsz - 1); tab[s] >= 0; s = (s + 1) & (tabsz - 1)) {
    zent *e = &ents[tab[s]];
    if (e->hash == h && !strcmp(e->name, name)) return e;
  }
  return NULL;
}

int zip_exists(const char *name) { return find(name) != NULL; }

static int zlib_load(void) {
  if (p_inflate) return 0;
  void *h = dlopen("libz.so.1", RTLD_NOW);
  if (!h) h = dlopen("libz.so", RTLD_NOW);
  if (!h) { LOG("cannot load zlib: %s", dlerror()); return -1; }
  p_inflateInit2_ = dlsym(h, "inflateInit2_");
  p_inflate = dlsym(h, "inflate");
  p_inflateEnd = dlsym(h, "inflateEnd");
  return p_inflate && p_inflateInit2_ && p_inflateEnd ? 0 : -1;
}

static void *zip_read_locked(zent *e, const char *name, size_t *size);

void *zip_read(const char *name, size_t *size) {
  zent *e = find(name);
  if (!e) return NULL;
  pthread_mutex_lock(&zlock);
  void *r = zip_read_locked(e, name, size);
  pthread_mutex_unlock(&zlock);
  return r;
}

static void *zip_read_locked(zent *e, const char *name, size_t *size) {
  uint8_t lh[30];
  fseeko(zf, e->lho, SEEK_SET);
  if (fread(lh, 1, 30, zf) != 30 || rd32(lh) != 0x04034b50) return NULL;
  fseeko(zf, e->lho + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);
  uint8_t *out = malloc(e->usize ? e->usize : 1);
  if (e->method == 0) {
    if (fread(out, 1, e->usize, zf) != e->usize) { free(out); return NULL; }
  } else if (e->method == 8) {
    if (zlib_load()) { free(out); return NULL; }
    uint8_t *in = malloc(e->csize);
    if (fread(in, 1, e->csize, zf) != e->csize) { free(in); free(out); return NULL; }
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    p_inflateInit2_(&zs, -15, "1.2.11", sizeof zs);
    zs.next_in = in; zs.avail_in = e->csize;
    zs.next_out = out; zs.avail_out = e->usize;
    int r = p_inflate(&zs, 4 /* Z_FINISH */);
    p_inflateEnd(&zs);
    free(in);
    if (r != 1 /* Z_STREAM_END */) { LOG("inflate %s failed (%d)", name, r); free(out); return NULL; }
  } else {
    LOG("%s: unsupported zip method %d", name, e->method);
    free(out);
    return NULL;
  }
  if (size) *size = e->usize;
  return out;
}
