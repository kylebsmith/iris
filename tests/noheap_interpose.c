/* ============================================================================
   noheap_interpose.c — a shared library that counts calls to the C
   library's allocator, for tests/noheap.c.

   Loaded ahead of the C library (DYLD_INSERT_LIBRARIES on macOS, LD_PRELOAD
   on Linux), it replaces every entry point of the allocator a program can
   reach and forwards each call to the real one, counting it only between
   hc_start() and hc_stop(). tests/noheap.c finds those two functions with
   dlsym, so without this library loaded it cannot count and refuses to run.

   macOS: the replacements are listed in the __DATA,__interpose section,
   which the dynamic loader reads to redirect every caller in the process,
   so each replacement can call the original by its own name.
   Linux (glibc): the replacements are simply named malloc, free and so on,
   which the loader finds before the C library's; they forward to glibc's
   __libc_malloc family, the real allocator under another name.

   Build (sh build.sh noheap does this):
     macOS:  cc -O1 -dynamiclib tests/noheap_interpose.c -o build/libnoheap.dylib
     Linux:  cc -O1 -shared -fPIC tests/noheap_interpose.c -o build/libnoheap.so
   ========================================================================= */
#if defined(__APPLE__)
#include <malloc/malloc.h>
#include <stdlib.h>
#else
#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#endif

static volatile int counting;
static volatile long calls;

void hc_start(void);
long hc_stop(void);
void hc_start(void) { calls = 0; counting = 1; }
long hc_stop(void) { counting = 0; return calls; }
#define COUNT() do { if (counting) ++calls; } while (0)

#if defined(__APPLE__)
static void *my_malloc(size_t n) { COUNT(); return malloc(n); }
static void *my_calloc(size_t a, size_t b) { COUNT(); return calloc(a, b); }
static void *my_realloc(void *p, size_t n) { COUNT(); return realloc(p, n); }
static void *my_reallocf(void *p, size_t n) { COUNT(); return reallocf(p, n); }
static void  my_free(void *p) { COUNT(); free(p); }
static int   my_posix_memalign(void **p, size_t a, size_t n) { COUNT(); return posix_memalign(p, a, n); }
static void *my_aligned_alloc(size_t a, size_t n) { COUNT(); return aligned_alloc(a, n); }
static void *my_valloc(size_t n) { COUNT(); return valloc(n); }
static void *my_zmalloc(malloc_zone_t *z, size_t n) { COUNT(); return malloc_zone_malloc(z, n); }
static void *my_zcalloc(malloc_zone_t *z, size_t a, size_t b) { COUNT(); return malloc_zone_calloc(z, a, b); }
static void *my_zrealloc(malloc_zone_t *z, void *p, size_t n) { COUNT(); return malloc_zone_realloc(z, p, n); }
static void *my_zmemalign(malloc_zone_t *z, size_t a, size_t n) { COUNT(); return malloc_zone_memalign(z, a, n); }
static void *my_zvalloc(malloc_zone_t *z, size_t n) { COUNT(); return malloc_zone_valloc(z, n); }
static void  my_zfree(malloc_zone_t *z, void *p) { COUNT(); malloc_zone_free(z, p); }

typedef struct { const void *replacement, *replacee; } interpose_t;
#define I(a, b) { (const void *)(unsigned long)&a, (const void *)(unsigned long)&b }
__attribute__((used)) static const interpose_t interposers[]
  __attribute__((section("__DATA,__interpose"))) = {
  I(my_malloc, malloc), I(my_calloc, calloc), I(my_realloc, realloc), I(my_reallocf, reallocf),
  I(my_free, free), I(my_posix_memalign, posix_memalign), I(my_aligned_alloc, aligned_alloc),
  I(my_valloc, valloc), I(my_zmalloc, malloc_zone_malloc), I(my_zcalloc, malloc_zone_calloc),
  I(my_zrealloc, malloc_zone_realloc), I(my_zmemalign, malloc_zone_memalign),
  I(my_zvalloc, malloc_zone_valloc), I(my_zfree, malloc_zone_free),
};
#else
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void  __libc_free(void *);
extern void *__libc_memalign(size_t, size_t);
extern void *__libc_valloc(size_t);
extern void *__libc_pvalloc(size_t);
void *malloc(size_t n) { COUNT(); return __libc_malloc(n); }
void *calloc(size_t a, size_t b) { COUNT(); return __libc_calloc(a, b); }
void *realloc(void *p, size_t n) { COUNT(); return __libc_realloc(p, n); }
void  free(void *p) { COUNT(); __libc_free(p); }
int posix_memalign(void **p, size_t a, size_t n) {
  void *r;
  COUNT();
  r = __libc_memalign(a, n);
  if (!r) return ENOMEM;
  *p = r;
  return 0;
}
void *aligned_alloc(size_t a, size_t n) { COUNT(); return __libc_memalign(a, n); }
void *memalign(size_t a, size_t n) { COUNT(); return __libc_memalign(a, n); }
void *valloc(size_t n) { COUNT(); return __libc_valloc(n); }
void *pvalloc(size_t n) { COUNT(); return __libc_pvalloc(n); }
void *reallocarray(void *p, size_t a, size_t b) {
  COUNT();
  if (b && a > (size_t)-1 / b) { errno = ENOMEM; return 0; }
  return __libc_realloc(p, a * b);
}
#endif
