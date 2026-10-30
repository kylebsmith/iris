/* SPDX-License-Identifier: BSD-3-Clause
   Copyright (c) 2026 Kyle Smith */
/* ============================================================================
   iris_dump.c -- take an instrument off the board.

   A sketch prints its instrument as a saved file in hexadecimal, between a
   line "IRIS <bytes>" and a line "END" (the starter kit's stemma_bno055 does
   it when sent an x). This program finds that block in a capture of the
   serial output, turns it back into the file, checks it with iris_load, and
   writes what the performer taught:

     PREFIX.iris        the saved file itself, which any program built with
                        iris.h can load
     PREFIX-takes.csv   one row per take: its identifier, inputs and outputs
     PREFIX-grid.csv    the instrument played over a grid spanning the takes:
                        each input across the range its takes cover, or, past
                        the first two, held at the middle of that range
     PREFIX-model.h     with --c-array: the same bytes as C, and the lines
                        that load them. For deployment rather than analysis

   Build and run from the repository root:

     cc -std=c99 -O2 -Wall -Wextra -I. -o build/iris_dump tools/iris_dump.c
     ./build/iris_dump capture.txt session1 [--inputs a,b,c] [--outputs x,y]
                                            [--grid N] [--c-array]

   --inputs and --outputs name the columns (in0, in1, ... and out0, ... when
   not given); --grid sets the points per input (21). A capture holding
   several blocks gives the last one. Lines may carry the timestamp the
   Arduino IDE's Serial Monitor can add ("12:34:56.789 -> "); it is skipped.

   --c-array is the last hop to a finished instrument. It writes
   PREFIX-model.h:

     static const unsigned char model[] = { 0x69, 0x72, ... };

   which a sketch includes, or has the array pasted into it, and loads once
   with iris_load at boot. That instrument plays a trained mapping with no
   training interface, no buttons and no screen -- training in the loop is
   what this library is for, but it should not be the fee for a first
   instrument. The emitted file carries the iris_init and iris_load lines for
   this instrument's own shape, and says what it holds.

   Baking is not a dead end, because the takes travel inside the saved file:
   the baked instrument still answers iris_count and iris_get, and still
   records and trains. The emitted comment says so, with this instrument's
   numbers in it.

     ./build/iris_dump --check

   runs the self-check sh build.sh load runs: a saved file printed as a
   sketch prints it, noise around it, comes back byte for byte, loads, and
   gives back every take and every grid prediction to the bit; the --c-array
   header holds the same bytes inside one block comment, and the instrument
   that comes back out of it keeps every take and plays every one to the bit;
   a changed digit, a missing END or a short block is refused. Exit status 0
   means every check passed.

   iris.h itself is unchanged by this program: it uses the C library and
   the heap, which the header does not. The maxima are raised so that an
   instrument from any sketch loads, whatever maxima it was built with.
   ========================================================================= */
#define IRIS_MAX_IN  1024
#define IRIS_MAX_HID 1024
#define IRIS_MAX_OUT 1024
#include "iris.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- reading */

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* The line starting at p, without a Serial Monitor timestamp: returns its
   start and sets *len to its length without the line ending. */
static const char *line_body(const char *p, const char *end, size_t *len) {
  const char *e = p;
  while (e < end && *e != '\n') ++e;
  const char *arrow = 0;
  for (const char *q = p; q + 3 <= e && q < p + 24; ++q)
    if (q[0] == '-' && q[1] == '>' && q[2] == ' ') { arrow = q + 3; break; }
  if (arrow && p[0] >= '0' && p[0] <= '9') p = arrow;
  const char *t = e;
  while (t > p && (t[-1] == '\r' || t[-1] == ' ' || t[-1] == '\t')) --t;
  *len = (size_t)(t - p);
  return p;
}

/* Finds the last complete block in text[0..n) and decodes it into a new
   buffer: returns its length and sets *out, or returns 0 and says why. A
   block is complete when a line "IRIS <bytes>" is followed by lines of
   hexadecimal digits holding exactly <bytes> bytes and then a line "END". */
static size_t find_block(const char *text, size_t n, unsigned char **out, int *blocks,
                         const char **why) {
  const char *p = text, *end = text + n;
  size_t best = 0;
  *out = 0; *blocks = 0; *why = "no line \"IRIS <bytes>\" in the capture";
  while (p < end) {
    size_t len;
    const char *b = line_body(p, end, &len);
    const char *next = memchr(p, '\n', (size_t)(end - p));
    next = next ? next + 1 : end;
    if (len > 5 && strncmp(b, "IRIS ", 5) == 0) {
      char *stop = 0;
      char num[32];
      const size_t m = len - 5 < sizeof num - 1 ? len - 5 : sizeof num - 1;
      memcpy(num, b + 5, m); num[m] = 0;
      const unsigned long want = strtoul(num, &stop, 10);
      if (stop != num && *stop == 0 && want > 0 && want < (1ul << 30)) {
        unsigned char *buf = (unsigned char *)malloc(want);
        size_t got = 0;
        int ok = 0, bad = 0;
        const char *q = next;
        while (q < end && !bad) {
          size_t l2;
          const char *b2 = line_body(q, end, &l2);
          const char *n2 = memchr(q, '\n', (size_t)(end - q));
          n2 = n2 ? n2 + 1 : end;
          q = n2;
          if (l2 == 3 && strncmp(b2, "END", 3) == 0) { ok = got == want; if (!ok) *why = "a block holds fewer bytes than its IRIS line says"; break; }
          if (l2 % 2) { bad = 1; *why = "a line of the block has an odd number of hexadecimal digits"; break; }
          for (size_t i = 0; i < l2; i += 2) {
            const int hi = hexval(b2[i]), lo = hexval(b2[i + 1]);
            if (hi < 0 || lo < 0) { bad = 1; *why = "a line of the block is not hexadecimal"; break; }
            if (got == want) { bad = 1; *why = "a block holds more bytes than its IRIS line says"; break; }
            buf[got++] = (unsigned char)(hi * 16 + lo);
          }
        }
        if (!ok && !bad && q >= end) *why = "a block has no END line";
        if (ok) {
          free(*out);
          *out = buf; best = got; ++*blocks;
          next = q;
        } else {
          free(buf);
        }
      }
    }
    p = next;
  }
  return best;
}

static uint32_t u32_at(const unsigned char *b, size_t off) {
  return (uint32_t)b[off] | (uint32_t)b[off + 1] << 8 | (uint32_t)b[off + 2] << 16
       | (uint32_t)b[off + 3] << 24;
}

/* An instrument of the file's own shape, loaded from it: 0, with the reason,
   when the file is refused. The caller frees *mem. */
static iris *load_file(const unsigned char *file, size_t n, unsigned char **mem, const char **why) {
  *mem = 0;
  if (n < IRIS_FILE_HEADER + 4u || memcmp(file, IRIS_FILE_MAGIC, 4) != 0) {
    *why = "the block is not a saved iris instrument"; return 0; }
  if (u32_at(file, 4) != IRIS_FILE_VERSION) {
    *why = "the file is another format than this iris.h reads"; return 0; }
  const uint32_t ni = u32_at(file, 16), nh = u32_at(file, 20), no = u32_at(file, 24),
                 nex = u32_at(file, 28);
  if (ni < 1 || ni > IRIS_MAX_IN || nh < 8 || nh > IRIS_MAX_HID || no < 1 || no > IRIS_MAX_OUT
      || nex > (uint32_t)IRIS_MAX_EX) {
    *why = "the file's shape is not one iris makes"; return 0; }
  const int cap = nex > 0 ? (int)nex : 1;
  const size_t bytes = iris_size((int)ni, (int)nh, (int)no, cap);
  if (!bytes) { *why = "the file's shape cannot be sized"; return 0; }
  *mem = (unsigned char *)malloc(bytes);
  iris *k = iris_init(*mem, bytes, (int)ni, (int)nh, (int)no, cap, 1u);
  if (!k || !iris_load(k, file, n)) {
    *why = "iris_load refused the file: it is damaged (a digit changed on the way) "
           "or was not written by iris_save";
    return 0;
  }
  return k;
}

/* ---------------------------------------------------------------- writing */

/* The name of column i of `list` (comma separated), or <prefix><i>. */
static void column(char *dst, size_t cap, const char *list, const char *prefix, int i) {
  if (list) {
    const char *p = list;
    for (int c = 0; c < i && p; ++c) { p = strchr(p, ','); if (p) ++p; }
    if (p && *p && *p != ',') {
      size_t m = 0;
      while (p[m] && p[m] != ',' && m + 1 < cap) { dst[m] = p[m]; ++m; }
      dst[m] = 0;
      return;
    }
  }
  snprintf(dst, cap, "%s%d", prefix, i);
}

static void header(FILE *f, int with_id, int ni, int no, const char *in_names, const char *out_names) {
  char name[128];
  if (with_id) fputs("id,", f);
  for (int i = 0; i < ni; ++i) { column(name, sizeof name, in_names, "in", i); fprintf(f, "%s,", name); }
  for (int o = 0; o < no; ++o) { column(name, sizeof name, out_names, "out", o); fprintf(f, o + 1 < no ? "%s," : "%s\n", name); }
}

/* %.9g gives every float back exactly when it is read again. */
static void write_takes(FILE *f, iris *k, const char *in_names, const char *out_names) {
  int ni, nh, no, cap;
  iris_shape(k, &ni, &nh, &no, &cap);
  float *in = (float *)malloc(sizeof(float) * (size_t)ni), *out = (float *)malloc(sizeof(float) * (size_t)no);
  header(f, 1, ni, no, in_names, out_names);
  for (int r = 0; r < iris_count(k); ++r) {
    const int id = iris_get(k, r, in, out);
    fprintf(f, "%d,", id);
    for (int i = 0; i < ni; ++i) fprintf(f, "%.9g,", (double)in[i]);
    for (int o = 0; o < no; ++o) fprintf(f, o + 1 < no ? "%.9g," : "%.9g\n", (double)out[o]);
  }
  free(in); free(out);
}

/* The instrument played over a grid: g points along the first input and g
   along the second, each across the span its takes cover, every other input
   at the middle of its span. */
static void write_grid(FILE *f, iris *k, int g, const char *in_names, const char *out_names) {
  int ni, nh, no, cap;
  iris_shape(k, &ni, &nh, &no, &cap);
  float *in = (float *)malloc(sizeof(float) * (size_t)ni), *out = (float *)malloc(sizeof(float) * (size_t)no);
  float *lo = (float *)malloc(sizeof(float) * (size_t)ni), *hi = (float *)malloc(sizeof(float) * (size_t)ni);
  for (int r = 0; r < iris_count(k); ++r) {
    iris_get(k, r, in, out);
    for (int i = 0; i < ni; ++i) {
      if (r == 0 || in[i] < lo[i]) lo[i] = in[i];
      if (r == 0 || in[i] > hi[i]) hi[i] = in[i];
    }
  }
  header(f, 0, ni, no, in_names, out_names);
  const int gb = ni >= 2 ? g : 1;
  for (int a = 0; a < g; ++a)
    for (int b = 0; b < gb; ++b) {
      for (int i = 0; i < ni; ++i) {
        const int step = i == 0 ? a : i == 1 ? b : -1;
        in[i] = step < 0 || g < 2 ? 0.5f * lo[i] + 0.5f * hi[i]
                                  : lo[i] + (hi[i] - lo[i]) * ((float)step / (float)(g - 1));
      }
      iris_predict(k, in, out);
      for (int i = 0; i < ni; ++i) fprintf(f, "%.9g,", (double)in[i]);
      for (int o = 0; o < no; ++o) fprintf(f, o + 1 < no ? "%.9g," : "%.9g\n", (double)out[o]);
    }
  free(in); free(out); free(lo); free(hi);
}

/* The instrument as C: the bytes iris_save wrote, and the lines that load
   them again. `from` names where the block came from and `prefix` names the
   file this is being written to. Everything it says about the instrument it
   reads from the instrument.

   Nothing it writes before the array may contain a comment opener or closer:
   all of it is inside one block comment, so a nested one would end that
   comment early and the header would not compile. The example lines below
   use // for that reason, and the self-check holds this. */
static void write_c_array(FILE *f, const unsigned char *file, size_t n, iris *k,
                          const char *prefix, const char *from) {
  int ni, nh, no, stored;             /* stored: the store's capacity, unused */
  iris_shape(k, &ni, &nh, &no, &stored);
  const int takes = iris_count(k);
  const int cap = takes > 0 ? takes : 1;   /* a store of 0 cannot be sized */
  const int fitted = (int)(u32_at(file, 12) & 1u);
  char guard[256];
  size_t gi = 0;
  const char *lead = "IRIS_BAKED_";
  while (lead[gi]) { guard[gi] = lead[gi]; ++gi; }
  for (const char *p = prefix; *p && gi + 3 < sizeof guard; ++p)
    guard[gi++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 'a' + 'A')
                : (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ? *p : '_';
  guard[gi++] = '_'; guard[gi++] = 'H'; guard[gi] = 0;

  fprintf(f,
    "/* An iris instrument, baked: the bytes iris_save wrote, as C.\n"
    "\n"
    "   Written by iris_dump from %s.\n"
    "   %d input%s, %d hidden units, %d output%s, %d demonstration%s.\n"
    "   %s. %lu bytes, save format %lu, seed %lu, smoothing %g.\n",
    from,
    ni, ni == 1 ? "" : "s", nh, no, no == 1 ? "" : "s",
    takes, takes == 1 ? "" : "s",
    iris_is_trained(k) ? "Trained on them"
                       : fitted ? "Fitted, then given more demonstrations or fewer"
                                : "NEVER TRAINED",
    (unsigned long)n, (unsigned long)IRIS_FILE_VERSION,
    (unsigned long)iris_seed(k), (double)iris_get_smoothing(k));

  if (!fitted)
    fprintf(f,
      "\n"
      "   WARNING: this instrument was never fitted. iris_predict will not play\n"
      "   it: it writes the middle of each output's range and reports\n"
      "   IRIS_NOT_FITTED. Call iris_train(k) after iris_load below -- the\n"
      "   demonstrations are in these bytes, so that works -- or bake a trained\n"
      "   one instead.\n");
  else if (!iris_is_trained(k))
    fprintf(f,
      "\n"
      "   WARNING: this instrument was fitted and then had demonstrations added\n"
      "   or removed, so what it plays is not a fit of the %d it now holds.\n"
      "   Call iris_train(k) after iris_load below to make it one.\n", takes);

  fprintf(f,
    "\n"
    "   Keep this file in the sketch's folder and include it, or paste the\n"
    "   array below into the sketch. Then load it once, at boot:\n"
    "\n"
    "     #include \"iris.h\"\n"
    "     #include \"%s-model.h\"\n"
    "\n"
    "     static unsigned char memory[IRIS_ARENA(%d, %d, %d, %d)];\n"
    "     static iris *k;\n"
    "\n"
    "     void setup(void) {\n"
    "       Serial.begin(115200);\n"
    "       k = iris_init(memory, sizeof memory, %d, %d, %d, %d, %luu);\n"
    "       if (!k || !iris_load(k, model, sizeof model)) {\n"
    "         Serial.println(F(\"the baked instrument was REFUSED. not playing it.\"));\n"
    "         for (;;);            // never play what did not load\n"
    "       }\n"
    "       Serial.println(F(\"loaded the baked instrument.\"));\n"
    "     }\n"
    "\n"
    "   iris_predict(k, in, out) then plays it, and that is the whole\n"
    "   instrument: no SAVE button, no screen, no training interface.\n",
    prefix, ni, nh, no, cap, ni, nh, no, cap, (unsigned long)iris_seed(k));

  fprintf(f,
    "\n"
    "   It is not a dead end. The demonstrations travel inside these bytes, so\n"
    "   a baked instrument is still the whole instrument: iris_count(k) is %d\n"
    "   here, iris_get(k, r, in, out) reads any of them back, and iris_record\n"
    "   and iris_train go on working. Raise the last argument of IRIS_ARENA and\n"
    "   of iris_init above %d to leave room, and you can add a demonstration to\n"
    "   a finished instrument and refit it on the spot.\n"
    "*/\n"
    "#ifndef %s\n"
    "#define %s\n"
    "static const unsigned char model[] = {\n",
    takes, cap, guard, guard);

  for (size_t i = 0; i < n; ++i)
    fprintf(f, i % 12 == 0 ? "  0x%02X,%s" : " 0x%02X,%s",
            file[i], (i % 12 == 11 || i + 1 == n) ? "\n" : "");
  fprintf(f, "};\n#endif\n");
}

/* ------------------------------------------------------------ self-check */

static int fails = 0;
static void check(const char *name, int ok) {
  printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) fails++;
}

/* The capture a sketch's serial output would give: noise, the block as
   stemma_bno055 prints it (upper-case digits, 32 bytes a line), noise. */
static char *capture_of(const unsigned char *file, size_t n, int corrupt, int drop_end, int short_by) {
  const size_t cap = 128 + 3 * n + 4 * (n / 32 + 4);
  char *t = (char *)malloc(cap);
  size_t w = 0;
  w += (size_t)snprintf(t + w, cap - w, "12:00:01.000 -> demonstrations: 8\r\nIRIS %zu\r\n", n);
  const size_t digits = w;
  for (size_t i = 0; i + (size_t)short_by < n; ++i) {
    w += (size_t)snprintf(t + w, cap - w, "%02X", file[i]);
    if (i % 32 == 31 || i + 1 + (size_t)short_by == n) w += (size_t)snprintf(t + w, cap - w, "\r\n");
  }
  if (corrupt) t[digits + 20] = t[digits + 20] == '0' ? '1' : '0';   /* byte 10 of the file */
  if (drop_end) return t;                  /* a capture cut off before END */
  w += (size_t)snprintf(t + w, cap - w, "END\r\n");
  w += (size_t)snprintf(t + w, cap - w, "12:00:02.000 -> 0.52 0.31\r\n");
  return t;
}

static int self_check(void) {
  static unsigned char mem[IRIS_ARENA(3, 12, 2, 16)];
  iris *k = iris_init(mem, sizeof mem, 3, 12, 2, 16, 1234u);
  for (int r = 0; r < 8; ++r) {
    const float in[3] = { (float)r / 7.0f, (float)((r * 3) % 8) / 7.0f, 100.0f + (float)r };
    const float out[2] = { 0.2f + 0.1f * (float)(r % 5), 3.0f - (float)r / 4.0f };
    iris_record(k, in, out);
  }
  iris_delete_id(k, 3);                            /* an identifier missing */
  iris_train(k);
  const size_t n = iris_save_size(k);
  unsigned char *file = (unsigned char *)malloc(n);
  iris_save(k, file, n);

  unsigned char *back = 0, *mem2 = 0;
  int blocks = 0;
  const char *why = "";
  char *cap = capture_of(file, n, 0, 0, 0);
  size_t m = find_block(cap, strlen(cap), &back, &blocks, &why);
  check("a printed block comes back byte for byte", m == n && memcmp(back, file, n) == 0);
  iris *r = m ? load_file(back, m, &mem2, &why) : 0;
  int same = r && iris_count(r) == iris_count(k);
  unsigned char *again = (unsigned char *)malloc(n);
  same = same && iris_save(r, again, n) == n && memcmp(again, file, n) == 0;
  check("it loads, and saves the same bytes again", same);

  /* the takes' table: every value read back equals the take, to the bit */
  FILE *f = tmpfile();
  int takes_ok = f && r;
  if (takes_ok) {
    write_takes(f, r, "tilt.x,tilt.y,distance", "pitch,bright");
    rewind(f);
    char line[1024];
    takes_ok = fgets(line, sizeof line, f) && strcmp(line, "id,tilt.x,tilt.y,distance,pitch,bright\n") == 0;
    for (int row = 0; takes_ok && row < iris_count(k); ++row) {
      float in[3], out[2];
      const int id = iris_get(k, row, in, out);
      int rid; float v[5];
      takes_ok = fscanf(f, "%d,%f,%f,%f,%f,%f\n", &rid, &v[0], &v[1], &v[2], &v[3], &v[4]) == 6
              && rid == id && v[0] == in[0] && v[1] == in[1] && v[2] == in[2]
              && v[3] == out[0] && v[4] == out[1];
    }
    fclose(f);
  }
  check("every take comes back from the table to the bit, with its name", takes_ok);

  /* the grid: every row is the instrument played at that row's reading */
  f = tmpfile();
  int grid_ok = f && r, rows = 0;
  if (grid_ok) {
    write_grid(f, r, 5, 0, 0);
    rewind(f);
    char line[1024];
    grid_ok = fgets(line, sizeof line, f) && strcmp(line, "in0,in1,in2,out0,out1\n") == 0;
    float v[5];
    while (grid_ok && fscanf(f, "%f,%f,%f,%f,%f\n", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5) {
      float out[2];
      iris_predict(k, v, out);
      grid_ok = out[0] == v[3] && out[1] == v[4];
      ++rows;
    }
    fclose(f);
  }
  check("the grid is the instrument played at each row, 25 rows", grid_ok && rows == 25);

  /* the C array: the file's own bytes, and an instrument that comes back
     from them whole -- every demonstration, and the same playing */
  f = tmpfile();
  int c_ok = f && r, c_loads = 0;
  unsigned char *baked_mem = 0;
  if (c_ok) {
    const size_t room = 4096 + 8 * n;
    char *text = (char *)malloc(room);
    write_c_array(f, file, n, r, "session1", "capture.txt");
    rewind(f);
    const size_t len = fread(text, 1, room - 1, f);
    text[len] = 0;
    fclose(f);
    const char *at = strstr(text, "model[] = {");
    unsigned char *got = (unsigned char *)malloc(n + 16);
    size_t g = 0;
    for (const char *p = at ? at : text; p && p[0] && p[1] && p[2] && p[3]; ++p)
      if (p[0] == '0' && p[1] == 'x' && hexval(p[2]) >= 0 && hexval(p[3]) >= 0) {
        if (g < n + 16) got[g++] = (unsigned char)(hexval(p[2]) * 16 + hexval(p[3]));
        p += 3;
      }
    /* One block comment, opened once and closed once. A nested opener or
       closer in the prose above the array ends the comment early and the
       header stops compiling, which a byte-for-byte check cannot see. */
    const char *first_close = strstr(text, "*/");
    int one_comment = text[0] == '/' && text[1] == '*'
                   && first_close && strstr(first_close + 2, "*/") == 0
                   && strstr(text + 2, "/*") == 0;
    c_ok = at && one_comment && g == n && memcmp(got, file, n) == 0;
    const char *why2 = "";
    iris *baked = c_ok ? load_file(got, g, &baked_mem, &why2) : 0;
    c_loads = baked && iris_count(baked) == iris_count(k)
           && iris_is_trained(baked) == iris_is_trained(k);
    for (int row = 0; c_loads && row < iris_count(k); ++row) {
      float ia[3], oa[2], ib[3], ob[2];
      const int ida = iris_get(k, row, ia, oa);
      const int idb = iris_get(baked, row, ib, ob);
      c_loads = ida == idb && memcmp(ia, ib, sizeof ia) == 0
                           && memcmp(oa, ob, sizeof oa) == 0;
      if (c_loads) {                         /* and it plays the same there */
        iris_predict(k, ia, oa);
        iris_predict(baked, ia, ob);
        c_loads = oa[0] == ob[0] && oa[1] == ob[1];
      }
    }
    free(text); free(got);
  }
  check("--c-array holds the saved file's own bytes, in one block comment", c_ok);
  check("the baked array loads, keeps every demonstration and plays the same", c_loads);
  free(baked_mem);

  free(cap); free(back); back = 0;
  cap = capture_of(file, n, 1, 0, 0);
  m = find_block(cap, strlen(cap), &back, &blocks, &why);
  free(mem2); mem2 = 0;
  const int refused_digit = m == n && load_file(back, m, &mem2, &why) == 0;
  check("a changed digit is refused by the checksum", refused_digit);
  free(cap); free(back); back = 0;
  cap = capture_of(file, n, 0, 1, 0);
  check("a block with no END is refused", find_block(cap, strlen(cap), &back, &blocks, &why) == 0);
  free(cap); free(back); back = 0;
  cap = capture_of(file, n, 0, 0, 3);
  check("a block three bytes short is refused", find_block(cap, strlen(cap), &back, &blocks, &why) == 0);
  free(cap); free(back); free(mem2); free(file); free(again);
  printf(fails ? "  %d FAILING\n" : "  iris_dump: every check passed\n", fails);
  return fails ? 1 : 0;
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--check") == 0) return self_check();
  const char *in_names = 0, *out_names = 0, *capture = 0, *prefix = 0;
  int g = 21, c_array = 0;
  for (int a = 1; a < argc; ++a) {
    if (strcmp(argv[a], "--inputs") == 0 && a + 1 < argc) in_names = argv[++a];
    else if (strcmp(argv[a], "--outputs") == 0 && a + 1 < argc) out_names = argv[++a];
    else if (strcmp(argv[a], "--grid") == 0 && a + 1 < argc) g = atoi(argv[++a]);
    else if (strcmp(argv[a], "--c-array") == 0) c_array = 1;
    else if (!capture) capture = argv[a];
    else if (!prefix) prefix = argv[a];
    else { capture = 0; break; }
  }
  if (!capture || !prefix || g < 1 || g > 1000) {
    fprintf(stderr, "usage: iris_dump CAPTURE PREFIX [--inputs a,b,..] [--outputs x,y,..]\n"
                    "                               [--grid N] [--c-array]\n"
                    "       iris_dump --check\n");
    return 2;
  }
  FILE *f = fopen(capture, "rb");
  if (!f) { fprintf(stderr, "iris_dump: cannot open %s\n", capture); return 1; }
  size_t cap = 1 << 16, n = 0;
  char *text = (char *)malloc(cap);
  for (size_t got; (got = fread(text + n, 1, cap - n, f)) > 0; ) {
    n += got;
    if (n == cap) { cap *= 2; text = (char *)realloc(text, cap); }
  }
  fclose(f);
  unsigned char *file = 0, *mem = 0;
  int blocks = 0;
  const char *why = "";
  const size_t bytes = find_block(text, n, &file, &blocks, &why);
  free(text);
  if (!bytes) { fprintf(stderr, "iris_dump: %s\n", why); return 1; }
  iris *k = load_file(file, bytes, &mem, &why);
  if (!k) { fprintf(stderr, "iris_dump: %s\n", why); return 1; }

  char path[4096];
  int ni, nh, no, c;
  iris_shape(k, &ni, &nh, &no, &c);
  snprintf(path, sizeof path, "%s.iris", prefix);
  if (!(f = fopen(path, "wb")) || fwrite(file, 1, bytes, f) != bytes || fclose(f)) {
    fprintf(stderr, "iris_dump: cannot write %s\n", path); return 1; }
  snprintf(path, sizeof path, "%s-takes.csv", prefix);
  if (!(f = fopen(path, "w"))) { fprintf(stderr, "iris_dump: cannot write %s\n", path); return 1; }
  write_takes(f, k, in_names, out_names);
  fclose(f);
  const int fitted = (int)(u32_at(file, 12) & 1u);
  if (fitted && iris_count(k) > 0) {
    snprintf(path, sizeof path, "%s-grid.csv", prefix);
    if (!(f = fopen(path, "w"))) { fprintf(stderr, "iris_dump: cannot write %s\n", path); return 1; }
    write_grid(f, k, g, in_names, out_names);
    fclose(f);
  }
  if (c_array) {
    snprintf(path, sizeof path, "%s-model.h", prefix);
    if (!(f = fopen(path, "w"))) { fprintf(stderr, "iris_dump: cannot write %s\n", path); return 1; }
    write_c_array(f, file, bytes, k, prefix, capture);
    if (fclose(f)) { fprintf(stderr, "iris_dump: cannot write %s\n", path); return 1; }
  }
  printf("%d-%d-%d instrument, %d takes, %s, seed %lu, smoothing %g (block %d of %d in the capture)\n"
         "wrote %s.iris, %s-takes.csv%s%s%s%s%s%s\n",
         ni, nh, no, iris_count(k),
         iris_is_trained(k) ? "trained on these takes" : fitted ? "fitted, then given more takes or fewer"
                                                                  : "never trained: no grid",
         (unsigned long)iris_seed(k), (double)iris_get_smoothing(k), blocks, blocks,
         prefix, prefix, fitted && iris_count(k) > 0 ? ", " : "",
         fitted && iris_count(k) > 0 ? prefix : "", fitted && iris_count(k) > 0 ? "-grid.csv" : "",
         c_array ? ", " : "", c_array ? prefix : "", c_array ? "-model.h" : "");
  if (c_array && !fitted)
    printf("%s-model.h holds an instrument that was never fitted: it says so, and says\n"
           "to call iris_train after iris_load. Do not ship it as it stands.\n", prefix);
  free(file); free(mem);
  return 0;
}
