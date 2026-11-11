// Fixed-function OpenGL ES 1.1 on top of an OpenGL ES 2.0 context, for devices
// whose driver cannot create a GLES 1.1 context. Covers exactly what
// librobberybob imports: matrix stacks, client arrays, glColor, texture
// environment (per texture format), alpha test, OES framebuffer aliases.
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

typedef unsigned GLenum;
typedef unsigned GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;

// ---- real GLES2 entry points ---------------------------------------------------
#define REAL(X) \
  X(GLuint, glCreateShader, (GLenum)) \
  X(void, glShaderSource, (GLuint, GLsizei, const char *const *, const GLint *)) \
  X(void, glCompileShader, (GLuint)) \
  X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
  X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, char *)) \
  X(GLuint, glCreateProgram, (void)) \
  X(void, glAttachShader, (GLuint, GLuint)) \
  X(void, glBindAttribLocation, (GLuint, GLuint, const char *)) \
  X(void, glLinkProgram, (GLuint)) \
  X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
  X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, char *)) \
  X(void, glUseProgram, (GLuint)) \
  X(GLint, glGetUniformLocation, (GLuint, const char *)) \
  X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
  X(void, glUniform1i, (GLint, GLint)) \
  X(void, glUniform1f, (GLint, GLfloat)) \
  X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
  X(void, glEnableVertexAttribArray, (GLuint)) \
  X(void, glDisableVertexAttribArray, (GLuint)) \
  X(void, glVertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) \
  X(void, glBindBuffer, (GLenum, GLuint)) \
  X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
  X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *)) \
  X(void, glEnable, (GLenum)) \
  X(void, glDisable, (GLenum)) \
  X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
  X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
  X(void, glTexParameterf, (GLenum, GLenum, GLfloat)) \
  X(void, glGenerateMipmap, (GLenum)) \
  X(void, glBindTexture, (GLenum, GLuint)) \
  X(void, glActiveTexture, (GLenum)) \
  X(void, glGetIntegerv, (GLenum, GLint *)) \
  X(const char *, glGetString, (GLenum)) \
  X(void, glHint, (GLenum, GLenum)) \
  X(void, glDeleteTextures, (GLsizei, const GLuint *))   X(void, glBindFramebuffer, (GLenum, GLuint))   X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))   X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))   X(GLenum, glCheckFramebufferStatus, (GLenum))

#define DECL(ret, name, args) static ret(*r_##name) args;
REAL(DECL)
#undef DECL

// Logs each distinct message once (verbose mode): which GLES 1.1 features the
// game really uses, for checking the emulation against a black or wrong screen.
static void note(const char *fmt, ...) {
  static unsigned seen[1024];
  static int nseen;
  if (!g_verbose || nseen >= 1024) return;
  char b[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  unsigned h = 2166136261u;
  for (const char *c = b; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
  for (int i = 0; i < nseen; i++)
    if (seen[i] == h) return;
  seen[nseen++] = h;
  LOG("gl: %s", b);
}

// ---- state -------------------------------------------------------------------
#define MV 0
#define PROJ 1
#define TEX0 2 // + unit
#define NSTACK 4
#define DEPTH 32
#define UNITS 2

typedef struct { GLfloat m[16]; } mat4;
static mat4 stacks[NSTACK][DEPTH];
static int sp[NSTACK];
static int mode = MV, active_unit, client_unit;

typedef struct { int on; GLint size; GLenum type; GLsizei stride; const void *ptr; GLuint buf; } array;
static array a_vertex, a_color, a_tc[UNITS];
static GLfloat cur_color[4] = { 1, 1, 1, 1 };
static int tex_on[UNITS];
// Texture environment per unit, raw GLES 1.1 values. src/op: 0-2 RGB, 3-5 alpha.
typedef struct {
  GLenum mode, crgb, calpha, src[6], op[6];
  GLfloat srgb, salpha;
} texenv_t;
static texenv_t env[UNITS];
static GLuint tex_bound[UNITS];
static int alpha_on;
static GLenum alpha_func = 0x0207;
static GLfloat alpha_ref;
static GLuint array_buf;

// Per texture id: format class (0 RGBA/LA, 1 RGB/L, 2 ALPHA) and GL_GENERATE_MIPMAP.
#define MAX_TEX 8192
static unsigned char tex_fmt[MAX_TEX], tex_mip[MAX_TEX];
// Completeness, which GLES 1.1 and 2 treat differently: an incomplete texture
// switches its unit off in GLES 1.1 but samples as black in GLES 2.
#define TEX_LEVEL0 1 // level 0 has an image
#define TEX_MIPS 2   // more levels uploaded or generated
#define TEX_NOMIP 4  // minification filter without mipmaps
static unsigned char tex_state[MAX_TEX];

static int tex_complete(GLuint t) {
  if (t >= MAX_TEX) return 1;
  unsigned char st = tex_state[t];
  return (st & TEX_LEVEL0) && (st & (TEX_MIPS | TEX_NOMIP));
}

#define GL_MODULATE 0x2100
#define GL_REPLACE 0x1E01
#define GL_DECAL 0x2101
#define GL_ADD 0x0104
#define GL_BLEND 0x0BE2
#define GL_COMBINE 0x8570

static void mat_identity(mat4 *m) {
  memset(m, 0, sizeof *m);
  m->m[0] = m->m[5] = m->m[10] = m->m[15] = 1;
}
static void mat_mul(mat4 *r, const mat4 *a, const GLfloat *b) { // r = a * b (column-major)
  mat4 t;
  for (int c = 0; c < 4; c++)
    for (int rr = 0; rr < 4; rr++) {
      GLfloat s = 0;
      for (int k = 0; k < 4; k++) s += a->m[k * 4 + rr] * b[c * 4 + k];
      t.m[c * 4 + rr] = s;
    }
  *r = t;
}
static mat4 *top(void) { return &stacks[mode][sp[mode]]; }
static void mult(const GLfloat *b) { mat_mul(top(), top(), b); }

// ---- shaders -------------------------------------------------------------------
static const char *vs_src =
  "attribute vec4 a_pos; attribute vec4 a_col; attribute vec4 a_tc0; attribute vec4 a_tc1;\n"
  "uniform mat4 u_mvp; uniform mat4 u_tm0; uniform mat4 u_tm1;\n"
  "varying vec4 v_col; varying vec2 v_tc0; varying vec2 v_tc1;\n"
  "void main() {\n"
  "  gl_Position = u_mvp * a_pos; gl_PointSize = 1.0; v_col = a_col;\n"
  "  vec4 t0 = u_tm0 * a_tc0; v_tc0 = t0.xy / t0.w;\n"
  "  vec4 t1 = u_tm1 * a_tc1; v_tc1 = t1.xy / t1.w;\n"
  "}\n";

// A program per combination of texture units, environments, texture formats
// and alpha test. Keys are zero-filled so they compare with memcmp.
typedef struct { texenv_t env[UNITS]; unsigned char fmt[UNITS]; GLenum afunc; } pkey;
typedef struct { pkey key; GLuint prog; GLint mvp, tm0, tm1, aref; } program;
#define MAX_PROGS 512
static program progs[MAX_PROGS];
static int nprogs;

static GLuint compile(GLenum type, const char *src) {
  GLuint s = r_glCreateShader(type);
  r_glShaderSource(s, 1, &src, NULL);
  r_glCompileShader(s);
  GLint ok = 0;
  r_glGetShaderiv(s, 0x8B81, &ok); // GL_COMPILE_STATUS
  if (!ok) {
    char log[1024] = "";
    r_glGetShaderInfoLog(s, sizeof log, NULL, log);
    LOG("gles1emu: shader error: %s\n%s", log, src);
  }
  return s;
}

static void cat(char *o, size_t n, const char *fmt, ...) {
  size_t l = strlen(o);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(o + l, n - l, fmt, ap);
  va_end(ap);
}

// GL_COMBINE source and operand expressions.
static void src_expr(char *o, size_t n, GLenum src, int unit) {
  if (src == 0x1702) snprintf(o, n, "t%d", unit); // GL_TEXTURE
  else if (src >= 0x84C0 && src < 0x84C0 + UNITS) // GL_TEXTUREn
    snprintf(o, n, "texture2D(u_t%d, v_tc%d)", src - 0x84C0, src - 0x84C0);
  else if (src == 0x8576) snprintf(o, n, "vec4(0.0)"); // GL_CONSTANT (environment colour 0)
  else if (src == 0x8577) snprintf(o, n, "v_col");     // GL_PRIMARY_COLOR
  else snprintf(o, n, "p");                            // GL_PREVIOUS
}
static void arg_expr(char *o, size_t n, GLenum src, GLenum op, int unit, int alpha) {
  char x[64];
  src_expr(x, sizeof x, src, unit);
  if (alpha) snprintf(o, n, op == 0x303 ? "(1.0 - %s.a)" : "%s.a", x);
  else if (op == 0x301) snprintf(o, n, "(1.0 - %s.rgb)", x);
  else if (op == 0x302) snprintf(o, n, "vec3(%s.a)", x);
  else if (op == 0x303) snprintf(o, n, "vec3(1.0 - %s.a)", x);
  else snprintf(o, n, "%s.rgb", x);
}
static void func_expr(char *o, size_t n, GLenum f, const char *a0, const char *a1, const char *a2) {
  switch (f) {
  case GL_REPLACE: snprintf(o, n, "%s", a0); break;
  case GL_ADD: snprintf(o, n, "(%s + %s)", a0, a1); break;
  case 0x8574: snprintf(o, n, "(%s + %s - 0.5)", a0, a1); break;                    // GL_ADD_SIGNED
  case 0x8575: snprintf(o, n, "(%s * %s + %s * (1.0 - %s))", a0, a2, a1, a2); break; // GL_INTERPOLATE
  case 0x84E7: snprintf(o, n, "(%s - %s)", a0, a1); break;                          // GL_SUBTRACT
  case 0x86AE: case 0x86AF: snprintf(o, n, "vec3(4.0 * dot(%s - 0.5, %s - 0.5))", a0, a1); break; // DOT3
  default: snprintf(o, n, "(%s * %s)", a0, a1); break;                                // GL_MODULATE
  }
}

// Fragment code for one texture unit; fmt is the bound texture's format class
// (0 RGBA/LA, 1 RGB/L, 2 ALPHA), which decides the classic environment formulas.
static void stage(char *o, size_t n, int u, const texenv_t *e, int fmt) {
  cat(o, n, "  vec4 t%d = texture2D(u_t%d, v_tc%d);\n", u, u, u);
  switch (e->mode) {
  case GL_REPLACE:
    cat(o, n, fmt == 0 ? "  c = t%d;\n" : fmt == 1 ? "  c.rgb = t%d.rgb;\n" : "  c.a = t%d.a;\n", u);
    break;
  case GL_DECAL:
    if (fmt == 1) cat(o, n, "  c.rgb = t%d.rgb;\n", u);
    else cat(o, n, "  c.rgb = mix(c.rgb, t%d.rgb, t%d.a);\n", u, u);
    break;
  case GL_ADD:
    if (fmt != 2) cat(o, n, "  c.rgb += t%d.rgb;\n", u);
    if (fmt != 1) cat(o, n, "  c.a *= t%d.a;\n", u);
    break;
  case GL_BLEND: // environment colour is (0,0,0,0)
    if (fmt != 2) cat(o, n, "  c.rgb *= 1.0 - t%d.rgb;\n", u);
    if (fmt != 1) cat(o, n, "  c.a *= t%d.a;\n", u);
    break;
  case GL_COMBINE: {
    char a[6][96], rgb[400], al[400];
    for (int i = 0; i < 6; i++) arg_expr(a[i], sizeof a[i], e->src[i], e->op[i], u, i >= 3);
    func_expr(rgb, sizeof rgb, e->crgb, a[0], a[1], a[2]);
    if (e->crgb == 0x86AF) snprintf(al, sizeof al, "d.r"); // DOT3_RGBA writes alpha too
    else func_expr(al, sizeof al, e->calpha == 0x86AE || e->calpha == 0x86AF ? GL_MODULATE : e->calpha,
                   a[3], a[4], a[5]);
    cat(o, n, "  { vec4 p = c; vec3 d = %s;\n    c = clamp(vec4(d * %.1f, %s * %.1f), 0.0, 1.0); }\n", rgb,
        e->srgb, al, e->salpha);
    break;
  }
  default: // GL_MODULATE
    if (fmt != 2) cat(o, n, "  c.rgb *= t%d.rgb;\n", u);
    cat(o, n, "  c.a *= t%d.a;\n", u);
    break;
  }
}

static program *get_program(const pkey *key) {
  for (int i = 0; i < nprogs; i++)
    if (!memcmp(&progs[i].key, key, sizeof *key)) return &progs[i];
  if (nprogs == MAX_PROGS) return &progs[0];
  program *p = &progs[nprogs++];
  p->key = *key;
  char fs[4096] =
    "precision mediump float;\n"
    "uniform sampler2D u_t0; uniform sampler2D u_t1; uniform float u_aref;\n"
    "varying vec4 v_col; varying vec2 v_tc0; varying vec2 v_tc1;\n"
    "void main() {\n  vec4 c = v_col;\n";
  for (int u = 0; u < UNITS; u++)
    if (key->env[u].mode) stage(fs, sizeof fs, u, &key->env[u], key->fmt[u]);
  if (key->afunc) { // alpha test
    static const char *const test[8] = { "false", "c.a < u_aref", "c.a == u_aref", "c.a <= u_aref",
                                         "c.a > u_aref", "c.a != u_aref", "c.a >= u_aref", "true" };
    cat(fs, sizeof fs, "  if (!(%s)) discard;\n", test[key->afunc & 7]);
  }
  cat(fs, sizeof fs, "  gl_FragColor = c;\n}\n");
  GLuint prog = r_glCreateProgram();
  r_glAttachShader(prog, compile(0x8B31, vs_src)); // GL_VERTEX_SHADER
  r_glAttachShader(prog, compile(0x8B30, fs));     // GL_FRAGMENT_SHADER
  r_glBindAttribLocation(prog, 0, "a_pos");
  r_glBindAttribLocation(prog, 1, "a_col");
  r_glBindAttribLocation(prog, 2, "a_tc0");
  r_glBindAttribLocation(prog, 3, "a_tc1");
  r_glLinkProgram(prog);
  GLint ok = 0;
  r_glGetProgramiv(prog, 0x8B82, &ok); // GL_LINK_STATUS
  if (!ok) {
    char log[1024] = "";
    r_glGetProgramInfoLog(prog, sizeof log, NULL, log);
    LOG("gles1emu: link error: %s", log);
  }
  r_glUseProgram(prog);
  r_glUniform1i(r_glGetUniformLocation(prog, "u_t0"), 0);
  r_glUniform1i(r_glGetUniformLocation(prog, "u_t1"), 1);
  p->prog = prog;
  p->mvp = r_glGetUniformLocation(prog, "u_mvp");
  p->tm0 = r_glGetUniformLocation(prog, "u_tm0");
  p->tm1 = r_glGetUniformLocation(prog, "u_tm1");
  p->aref = r_glGetUniformLocation(prog, "u_aref");
  if (g_verbose) {
    const char *body = strstr(fs, "vec4 c = v_col;");
    LOG("gles1emu: program %d:%s", nprogs, body ? body + 15 : fs);
  }
  return p;
}

// ---- draw ----------------------------------------------------------------------
static void attrib(GLuint loc, const array *a, GLboolean norm, const GLfloat *def) {
  if (a->on) {
    r_glBindBuffer(0x8892, a->buf); // GL_ARRAY_BUFFER
    r_glVertexAttribPointer(loc, a->size, a->type, norm, a->stride, a->ptr);
    r_glEnableVertexAttribArray(loc);
  } else {
    r_glDisableVertexAttribArray(loc);
    r_glVertexAttrib4f(loc, def[0], def[1], def[2], def[3]);
  }
}

static void prepare(void) {
  pkey key;
  memset(&key, 0, sizeof key);
  for (int u = 0; u < UNITS; u++) {
    GLuint t = tex_bound[u];
    if (!tex_on[u]) continue;
    if (!tex_complete(t)) {
      note("unit %d: texture %u incomplete (state %d), unit off as in GLES 1.1", u, t,
           t < MAX_TEX ? tex_state[t] : -1);
      continue;
    }
    key.fmt[u] = t < MAX_TEX ? tex_fmt[t] : 0;
    key.env[u].mode = env[u].mode;
    if (env[u].mode == GL_COMBINE) key.env[u] = env[u];
  }
  if (alpha_on && alpha_func != 0x0207) key.afunc = 0x100 | (alpha_func & 7);
  program *p = get_program(&key);
  r_glUseProgram(p->prog);
  mat4 mvp;
  mat_mul(&mvp, &stacks[PROJ][sp[PROJ]], stacks[MV][sp[MV]].m);
  r_glUniformMatrix4fv(p->mvp, 1, 0, mvp.m);
  r_glUniformMatrix4fv(p->tm0, 1, 0, stacks[TEX0][sp[TEX0]].m);
  r_glUniformMatrix4fv(p->tm1, 1, 0, stacks[TEX0 + 1][sp[TEX0 + 1]].m);
  if (p->aref >= 0) r_glUniform1f(p->aref, alpha_ref);
  static const GLfloat tc_def[4] = { 0, 0, 0, 1 };
  attrib(0, &a_vertex, 0, tc_def);
  attrib(1, &a_color, a_color.type == 0x1401 || a_color.type == 0x1400, cur_color); // (U)BYTE: normalized
  attrib(2, &a_tc[0], 0, tc_def);
  attrib(3, &a_tc[1], 0, tc_def);
  r_glBindBuffer(0x8892, array_buf);
}

static void note_draw(GLenum m) {
  note("draw mode %#x vertex %d/%#x color %d/%#x tc0 %d/%#x/%s tc1 %d/%#x/%s", m, a_vertex.size, a_vertex.type,
       a_color.on ? a_color.size : 0, a_color.type, a_tc[0].on ? a_tc[0].size : 0, a_tc[0].type,
       a_tc[0].buf ? "vbo" : "mem", a_tc[1].on ? a_tc[1].size : 0, a_tc[1].type, a_tc[1].buf ? "vbo" : "mem");
}
static void e_glDrawArrays(GLenum m, GLint first, GLsizei n) {
  note_draw(m);
  prepare();
  r_glDrawArrays(m, first, n);
}
static void e_glDrawElements(GLenum m, GLsizei n, GLenum type, const void *idx) {
  note_draw(m);
  note("draw elements type %#x", type);
  prepare();
  r_glDrawElements(m, n, type, idx);
}

// ---- fixed-function entry points ---------------------------------------------------
static void e_glMatrixMode(GLenum m) {
  note("matrix mode %#x unit %d", m, active_unit);
  mode = m == 0x1701 ? PROJ : m == 0x1702 ? TEX0 + active_unit : MV; // GL_PROJECTION, GL_TEXTURE
}
static void e_glLoadIdentity(void) { mat_identity(top()); }
static void e_glPushMatrix(void) {
  if (sp[mode] < DEPTH - 1) { stacks[mode][sp[mode] + 1] = stacks[mode][sp[mode]]; sp[mode]++; }
}
static void e_glPopMatrix(void) { if (sp[mode] > 0) sp[mode]--; }
static void e_glMultMatrixf(const GLfloat *m) { mult(m); }
static void e_glLoadMatrixf(const GLfloat *m) { memcpy(top()->m, m, 64); }
static void e_glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
  GLfloat m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1 };
  mult(m);
}
static void e_glScalef(GLfloat x, GLfloat y, GLfloat z) {
  GLfloat m[16] = { x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1 };
  mult(m);
}
static void e_glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) {
  GLfloat l = sqrtf(x * x + y * y + z * z);
  if (l == 0) return;
  x /= l; y /= l; z /= l;
  GLfloat r = a * (GLfloat)M_PI / 180, c = cosf(r), s = sinf(r), k = 1 - c;
  GLfloat m[16] = { x * x * k + c,     y * x * k + z * s, x * z * k - y * s, 0,
                    x * y * k - z * s, y * y * k + c,     y * z * k + x * s, 0,
                    x * z * k + y * s, y * z * k - x * s, z * z * k + c,     0,
                    0, 0, 0, 1 };
  mult(m);
}
static void e_glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
  GLfloat m[16] = { 2 / (r - l), 0, 0, 0, 0, 2 / (t - b), 0, 0, 0, 0, -2 / (f - n), 0,
                    -(r + l) / (r - l), -(t + b) / (t - b), -(f + n) / (f - n), 1 };
  mult(m);
}
static void e_glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) {
  GLfloat m[16] = { 2 * n / (r - l), 0, 0, 0, 0, 2 * n / (t - b), 0, 0,
                    (r + l) / (r - l), (t + b) / (t - b), -(f + n) / (f - n), -1,
                    0, 0, -2 * f * n / (f - n), 0 };
  mult(m);
}

static void e_glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
  cur_color[0] = r; cur_color[1] = g; cur_color[2] = b; cur_color[3] = a;
}
static void e_glColor4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
  e_glColor4f(r / 255.f, g / 255.f, b / 255.f, a / 255.f);
}

static array *client_array(GLenum cap) {
  switch (cap) {
  case 0x8074: return &a_vertex;          // GL_VERTEX_ARRAY
  case 0x8076: return &a_color;           // GL_COLOR_ARRAY
  case 0x8078: return &a_tc[client_unit]; // GL_TEXTURE_COORD_ARRAY
  }
  return NULL; // normals, point sizes: not used without lighting
}
static void e_glEnableClientState(GLenum cap) { array *a = client_array(cap); if (a) a->on = 1; }
static void e_glDisableClientState(GLenum cap) { array *a = client_array(cap); if (a) a->on = 0; }
static void set_ptr(array *a, GLint size, GLenum type, GLsizei stride, const void *p) {
  a->size = size; a->type = type; a->stride = stride; a->ptr = p; a->buf = array_buf;
}
static void e_glVertexPointer(GLint s, GLenum t, GLsizei st, const void *p) { set_ptr(&a_vertex, s, t, st, p); }
static void e_glColorPointer(GLint s, GLenum t, GLsizei st, const void *p) { set_ptr(&a_color, s, t, st, p); }
static void e_glTexCoordPointer(GLint s, GLenum t, GLsizei st, const void *p) {
  set_ptr(&a_tc[client_unit], s, t, st, p);
}
static void e_glNormalPointer(GLenum t, GLsizei st, const void *p) { (void)t; (void)st; (void)p; }
static void e_glClientActiveTexture(GLenum u) { client_unit = (u - 0x84C0) & (UNITS - 1); }
static void e_glActiveTexture(GLenum u) {
  active_unit = (u - 0x84C0) & (UNITS - 1);
  r_glActiveTexture(u);
}
static void e_glBindBuffer(GLenum target, GLuint b) {
  if (target == 0x8892) array_buf = b;
  r_glBindBuffer(target, b);
}

// Capabilities that only exist in GLES 1.1 are tracked here or dropped.
static int ff_cap(GLenum cap, int on) {
  switch (cap) {
  case 0x0DE1: tex_on[active_unit] = on; return 1; // GL_TEXTURE_2D
  case 0x0BC0: alpha_on = on; return 1;            // GL_ALPHA_TEST
  case 0x0B50: case 0x0B57: case 0x0B60: case 0x0BA1: case 0x803A: // lighting, color material, fog, normalize
  case 0x0B10: case 0x0B20: case 0x809D: case 0x809F: case 0x0BF2: case 0x8861:
    return 1;
  }
  if (cap >= 0x4000 && cap < 0x4008) return 1; // GL_LIGHTi
  if (cap >= 0x3000 && cap < 0x3006) return 1; // GL_CLIP_PLANEi
  return 0;
}
static void e_glEnable(GLenum cap) {
  note("enable %#x unit %d", cap, active_unit);
  if (!ff_cap(cap, 1)) r_glEnable(cap);
}
static void e_glDisable(GLenum cap) { if (!ff_cap(cap, 0)) r_glDisable(cap); }

static void e_glAlphaFunc(GLenum f, GLfloat ref) { alpha_func = f; alpha_ref = ref; }

static void texenv(GLenum target, GLenum pname, GLfloat fv) {
  GLenum v = (GLenum)fv;
  note("texenv %#x %#x = %#x unit %d", target, pname, v, active_unit);
  if (target != 0x2300) return; // GL_TEXTURE_ENV (point sprites are not used)
  texenv_t *e = &env[active_unit];
  if (pname == 0x2200) e->mode = v;                                            // GL_TEXTURE_ENV_MODE
  else if (pname == 0x8571) e->crgb = v;                                       // GL_COMBINE_RGB
  else if (pname == 0x8572) e->calpha = v;                                     // GL_COMBINE_ALPHA
  else if (pname >= 0x8580 && pname <= 0x8582) e->src[pname - 0x8580] = v;     // GL_SRCn_RGB
  else if (pname >= 0x8588 && pname <= 0x858A) e->src[3 + pname - 0x8588] = v; // GL_SRCn_ALPHA
  else if (pname >= 0x8590 && pname <= 0x8592) e->op[pname - 0x8590] = v;      // GL_OPERANDn_RGB
  else if (pname >= 0x8598 && pname <= 0x859A) e->op[3 + pname - 0x8598] = v;  // GL_OPERANDn_ALPHA
  else if (pname == 0x8573) e->srgb = fv;                                      // GL_RGB_SCALE
  else if (pname == 0x0D1C) e->salpha = fv;                                    // GL_ALPHA_SCALE
}
static void e_glTexEnvi(GLenum t, GLenum p, GLint v) { texenv(t, p, (GLfloat)v); }
static void e_glTexEnvf(GLenum t, GLenum p, GLfloat v) { texenv(t, p, v); }

static void e_glBindTexture(GLenum target, GLuint t) {
  if (target == 0x0DE1) tex_bound[active_unit] = t;
  r_glBindTexture(target, t);
}
// Returns 1 when the parameter only exists in GLES 1.1 and was handled here.
static int tex_param(GLenum p, GLint v) {
  note("texparameter %#x = %#x", p, v);
  GLuint t = tex_bound[active_unit];
  if (t >= MAX_TEX) return p == 0x8191;
  if (p == 0x2801) { // GL_TEXTURE_MIN_FILTER: NEAREST / LINEAR need no mipmaps
    if (v == 0x2600 || v == 0x2601) tex_state[t] |= TEX_NOMIP;
    else tex_state[t] &= ~TEX_NOMIP;
  }
  if (p == 0x8191) { // GL_GENERATE_MIPMAP
    tex_mip[t] = v != 0;
    if (v && (tex_state[t] & TEX_LEVEL0)) {
      r_glGenerateMipmap(0x0DE1);
      tex_state[t] |= TEX_MIPS;
    }
    return 1;
  }
  return 0;
}
static void e_glTexParameteri(GLenum target, GLenum p, GLint v) {
  if (!tex_param(p, v)) r_glTexParameteri(target, p, v);
}
static void e_glTexParameterf(GLenum target, GLenum p, GLfloat v) {
  if (!tex_param(p, (GLint)v)) r_glTexParameterf(target, p, v);
}
static void e_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                           GLenum fmt, GLenum type, const void *px) {
  r_glTexImage2D(target, level, ifmt, w, h, border, fmt, type, px);
  GLuint t = tex_bound[active_unit];
  note("teximage level %d %#x/%#x/%#x%s%s", level, ifmt, fmt, type, px ? "" : " (no data)",
       (w & (w - 1)) || (h & (h - 1)) ? " npot" : "");
  if (t >= MAX_TEX || target != 0x0DE1) return;
  if (level == 0) {
    tex_fmt[t] = fmt == 0x1906 ? 2 : (fmt == 0x1907 || fmt == 0x1909) ? 1 : 0; // ALPHA / RGB, LUMINANCE
    tex_state[t] |= TEX_LEVEL0;
    if (tex_mip[t]) {
      r_glGenerateMipmap(target);
      tex_state[t] |= TEX_MIPS;
    }
  } else {
    tex_state[t] |= TEX_MIPS;
  }
}
static void e_glDeleteTextures(GLsizei n, const GLuint *ids) {
  for (int i = 0; i < n; i++)
    if (ids[i] < MAX_TEX) tex_fmt[ids[i]] = tex_mip[ids[i]] = tex_state[ids[i]] = 0;
  r_glDeleteTextures(n, ids);
}

static void e_glHint(GLenum target, GLenum mode_) {
  if (target == 0x8192) r_glHint(target, mode_); // only GL_GENERATE_MIPMAP_HINT exists in GLES 2
}

static void e_glGetIntegerv(GLenum p, GLint *v) {
  switch (p) {
  case 0x84E2: *v = UNITS; return;                                  // GL_MAX_TEXTURE_UNITS
  case 0x0BA0: *v = mode == PROJ ? 0x1701 : mode == MV ? 0x1700 : 0x1702; return; // GL_MATRIX_MODE
  case 0x0D31: case 0x0D36: case 0x0D38: *v = DEPTH; return;        // GL_MAX_*_STACK_DEPTH
  }
  r_glGetIntegerv(p, v);
}

static const char *e_glGetString(GLenum name) {
  static char ext[4096];
  switch (name) {
  case 0x1F02: return "OpenGL ES-CM 1.1"; // GL_VERSION
  case 0x1F03:                            // GL_EXTENSIONS
    if (!ext[0]) {
      const char *e = r_glGetString(name);
      snprintf(ext, sizeof ext, "GL_OES_framebuffer_object GL_OES_matrix_get %s", e ? e : "");
    }
    return ext;
  }
  return r_glGetString(name);
}

// ---- framebuffers: the OES names map to core GLES 2; logged to follow render targets
static void e_glBindFramebuffer(GLenum target, GLuint fb) {
  note("bind framebuffer %u", fb);
  r_glBindFramebuffer(target, fb);
}
static void e_glFramebufferTexture2D(GLenum target, GLenum att, GLenum textarget, GLuint tex, GLint level) {
  r_glFramebufferTexture2D(target, att, textarget, tex, level);
  note("framebuffer texture att %#x tex %u (fmt %d) -> status %#x", att, tex, tex < MAX_TEX ? tex_fmt[tex] : -1,
       r_glCheckFramebufferStatus(target));
}
static void e_glFramebufferRenderbuffer(GLenum target, GLenum att, GLenum rbt, GLuint rb) {
  r_glFramebufferRenderbuffer(target, att, rbt, rb);
  note("framebuffer renderbuffer att %#x rb %u", att, rb);
}

// ---- lookup --------------------------------------------------------------------
static const struct { const char *name; void *fn; } emu[] = {
#define E(n) { #n, (void *)e_##n }
  E(glDrawArrays), E(glDrawElements), E(glMatrixMode), E(glLoadIdentity), E(glPushMatrix),
  E(glPopMatrix), E(glMultMatrixf), E(glLoadMatrixf), E(glTranslatef), E(glScalef), E(glRotatef),
  E(glOrthof), E(glFrustumf), E(glColor4f), E(glColor4ub), E(glEnableClientState),
  E(glDisableClientState), E(glVertexPointer), E(glColorPointer), E(glTexCoordPointer),
  E(glNormalPointer), E(glClientActiveTexture), E(glActiveTexture), E(glBindBuffer), E(glEnable),
  E(glDisable), E(glAlphaFunc), E(glTexEnvi), E(glTexEnvf), E(glBindTexture), E(glTexParameteri),
  E(glTexParameterf), E(glTexImage2D), E(glDeleteTextures), E(glHint), E(glGetIntegerv),
  E(glGetString), E(glBindFramebuffer), E(glFramebufferTexture2D), E(glFramebufferRenderbuffer),
  { "glBindFramebufferOES", (void *)e_glBindFramebuffer },
  { "glFramebufferTexture2DOES", (void *)e_glFramebufferTexture2D },
  { "glFramebufferRenderbufferOES", (void *)e_glFramebufferRenderbuffer },
#undef E
};

int g_gles1emu;
static void *(*real_get)(const char *);

void *gles1emu_proc(const char *name) {
  for (size_t i = 0; i < sizeof emu / sizeof *emu; i++)
    if (!strcmp(emu[i].name, name)) return emu[i].fn;
  // The OES framebuffer entry points are core in GLES 2.
  size_t l = strlen(name);
  if (l > 3 && !strcmp(name + l - 3, "OES")) {
    char core[128];
    snprintf(core, sizeof core, "%.*s", (int)(l - 3), name);
    return real_get(core);
  }
  return NULL;
}

int gles1emu_init(void *(*get)(const char *)) {
  real_get = get;
  int ok = 1;
#define LOAD(ret, name, args) \
  if (!(r_##name = (ret(*) args)get(#name))) { LOG("gles1emu: %s missing", #name); ok = 0; }
  REAL(LOAD)
#undef LOAD
  if (!ok) return -1;
  for (int s = 0; s < NSTACK; s++) mat_identity(&stacks[s][0]);
  for (int u = 0; u < UNITS; u++) {
    texenv_t *e = &env[u];
    e->mode = e->crgb = e->calpha = GL_MODULATE;
    e->src[0] = e->src[3] = 0x1702;                    // GL_TEXTURE
    e->src[1] = e->src[4] = 0x8578;                    // GL_PREVIOUS
    e->src[2] = e->src[5] = 0x8576;                    // GL_CONSTANT
    e->op[0] = e->op[1] = 0x300;                       // GL_SRC_COLOR
    e->op[2] = e->op[3] = e->op[4] = e->op[5] = 0x302; // GL_SRC_ALPHA
    e->srgb = e->salpha = 1;
  }
  g_gles1emu = 1;
  LOG("gles1emu: GLES 1.1 emulated on %s", r_glGetString(0x1F02) ? r_glGetString(0x1F02) : "?");
  return 0;
}
