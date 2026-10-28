/**
 * @file gles.c
 * @brief OpenGL ES 1.1 pipeline translation over vitaGL.
 * @details The game renders into a virtual 320x480 portrait renderbuffer created on iOS by EAGLView
 *          (with internal projection rotation to landscape). Here that renderbuffer is an FBO with a texture
 *          of the same size: viewport, scissor, and glCopyTexImage2D remain in iOS coordinate space untouched.
 *          During presentation (gles_present), the FBO is drawn rotated and scaled onto the native 960x544 screen.
 *
 *          Float gl* entry points map directly: the loader and vitaGL (SOFTFP_ABI=1) use the iOS softfp ABI.
 *          GL_FIXED vertex arrays are converted to float immediately before draw calls.
 */
#include <stdlib.h>
#include <string.h>
#include <vitaGL.h>

#include "gles.h"
#include "hle.h"
#include "objc.h"
#include "patch.h"
#include "imports.h"
#include "log.h"

#define FB_W 320
#define FB_H 480
#define SCR_W 960
#define SCR_H 544
#ifndef SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE
#define SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE 0x8C6000
#endif
// Fullscreen without stretching: the 480x320 game would fill 816x544 at the Vita's height.
// Drawing into the window framebuffer compresses clip space along the screen-horizontal axis by
// WIDE_K (= 816/960) and the blit then fills all 960 px: the 3D view gains field of view on both
// sides, while everything the game lays out in 480x320 (HUD, menus) keeps its original size,
// centered in those 816 px, which is also where input.c maps touches.
#define DST_H SCR_H
#define DST_W SCR_W
#define DST_X 0
#define WIDE_K (816.0f / 960.0f)

#define GL_FRAMEBUFFER_COMPLETE_OES 0x8CD5
#define GL_RENDERBUFFER_WIDTH_OES 0x8D42
#define GL_RENDERBUFFER_HEIGHT_OES 0x8D43

static GLuint fbo, fbo_tex; // window framebuffer (ours until the game creates its own) and its color texture

// Client array state, tracked because (1) vitaGL rejects some GLES1 array types (GL_BYTE
// vertices/texcoords, GL_FIXED colors; we also convert GL_FIXED vertex/texcoord/normal as before):
// on GL_INVALID_ENUM it keeps the PREVIOUS pointer and the draw uses another mesh's data;
// (2) gles_present must re-apply every pointer with the buffer object it was declared with.
typedef struct {
    GLint size;
    GLenum type;
    GLsizei stride;
    const void *ptr;
    GLuint vbo; // GL_ARRAY_BUFFER bound when the pointer was set (ptr is then an offset)
    int convert;
} array_ptr;
enum { A_VTX, A_TC0, A_TC1, A_COL, A_NRM, A_COUNT };
static array_ptr arr[A_COUNT];
static GLuint array_vbo;
static uint32_t enabled; // bit per A_* kind, from glEnable/DisableClientState
static int client_unit; // glClientActiveTexture - GL_TEXTURE0
static GLint env_mode = GL_MODULATE;
static GLfixed color[4] = {0x10000, 0x10000, 0x10000, 0x10000};

static int needs_convert(int kind, GLenum type) {
    switch (kind) {
    case A_COL:
        return type == GL_FIXED;
    case A_NRM:
        return type == GL_FIXED;
    default: // vertex / texcoord: vitaGL takes GL_FLOAT and GL_SHORT
        return type == GL_FIXED || type == GL_BYTE;
    }
}

// vitaGL buffer names are pointers to its vbo struct, whose first field is the CPU-visible data
// pointer (read directly: glUnmapNamedBuffer would mark it unused by the GPU).
static const uint8_t *array_source(const array_ptr *a) {
    if (!a->vbo)
        return a->ptr;
    const uint8_t *base = *(const uint8_t **)(uintptr_t)a->vbo;
    return base ? base + (uintptr_t)a->ptr : NULL;
}

static float *conv_buf[A_COUNT];
static int conv_cap[A_COUNT];

static const float *convert_array(int kind, int n) {
    const array_ptr *a = &arr[kind];
    const uint8_t *src = array_source(a);
    if (!src)
        return NULL;
    int need = n * a->size;
    if (need > conv_cap[kind]) {
        conv_buf[kind] = realloc(conv_buf[kind], need * sizeof(float));
        conv_cap[kind] = need;
    }
    int esz = a->type == GL_BYTE ? 1 : 4;
    int stride = a->stride ? a->stride : a->size * esz;
    float *dst = conv_buf[kind];
    for (int i = 0; i < n; i++, src += stride)
        for (int c = 0; c < a->size; c++)
            dst[i * a->size + c] = a->type == GL_BYTE ? (float)((const int8_t *)src)[c]
                                                      : ((const int32_t *)src)[c] * (1.0f / 65536.0f);
    return dst;
}

// Passes one array to vitaGL as the game declared it (native types) or as converted floats.
static void set_native(int kind, GLint size, GLenum type, GLsizei stride, const void *ptr) {
    if (kind == A_TC0 || kind == A_TC1)
        glClientActiveTexture(GL_TEXTURE0 + kind - A_TC0);
    switch (kind) {
    case A_VTX:
        glVertexPointer(size, type, stride, ptr);
        break;
    case A_COL:
        glColorPointer(size, type, stride, ptr);
        break;
    case A_NRM:
        glNormalPointer(type, stride, ptr);
        break;
    default:
        glTexCoordPointer(size, type, stride, ptr);
        break;
    }
    if (kind == A_TC0 || kind == A_TC1)
        glClientActiveTexture(GL_TEXTURE0 + client_unit);
}

static void declare(int kind, GLint size, GLenum type, GLsizei stride, const void *ptr) {
    static uint32_t seen[64];
    static int nseen;
    uint32_t key = kind << 24 | (type & 0xFFFF) << 8 | size << 4 | (array_vbo != 0);
    int known = 0;
    for (int i = 0; i < nseen; i++)
        known |= seen[i] == key;
    if (!known && nseen < 64) {
        seen[nseen++] = key;
        l_info("GL array %d: size %d type 0x%04X stride %d %s%s", kind, size, type, stride,
               array_vbo ? "vbo" : "client", needs_convert(kind, type) ? " (converted)" : "");
    }
    arr[kind] = (array_ptr){size, type, stride, ptr, array_vbo, needs_convert(kind, type)};
    if (!arr[kind].convert)
        set_native(kind, size, type, stride, ptr);
}

// Before a draw: converted arrays are uploaded as client floats for vertices [0, n).
static void prepare_draw(int n) {
    int any = 0;
    for (int k = 0; k < A_COUNT; k++)
        if (arr[k].size && arr[k].convert && enabled & 1u << k) {
            const float *f = convert_array(k, n);
            if (!f)
                continue;
            if (!any++)
                glBindBuffer(GL_ARRAY_BUFFER, 0);
            set_native(k, arr[k].size, GL_FLOAT, 0, f);
        }
    if (any && array_vbo)
        glBindBuffer(GL_ARRAY_BUFFER, array_vbo);
}

static int any_convert(void) {
    for (int k = 0; k < A_COUNT; k++)
        if (arr[k].size && arr[k].convert && enabled & 1u << k)
            return 1;
    return 0;
}

// Re-applies every native pointer with its own buffer binding (after gles_present's blit).
static void reapply_arrays(void) {
    for (int k = 0; k < A_COUNT; k++)
        if (arr[k].size && !arr[k].convert) {
            glBindBuffer(GL_ARRAY_BUFFER, arr[k].vbo);
            set_native(k, arr[k].size, arr[k].type, arr[k].stride, arr[k].ptr);
        }
    glBindBuffer(GL_ARRAY_BUFFER, array_vbo);
}

static int state_kind(GLenum cap) {
    return cap == GL_VERTEX_ARRAY ? A_VTX : cap == GL_COLOR_ARRAY ? A_COL : cap == GL_NORMAL_ARRAY ? A_NRM
         : cap == GL_TEXTURE_COORD_ARRAY ? A_TC0 + client_unit : -1;
}
static void g_glEnableClientState(GLenum cap) {
    int k = state_kind(cap);
    if (k >= 0)
        enabled |= 1u << k;
    glEnableClientState(cap);
}
static void g_glDisableClientState(GLenum cap) {
    int k = state_kind(cap);
    if (k >= 0)
        enabled &= ~(1u << k);
    glDisableClientState(cap);
}

static void g_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    declare(A_VTX, size, type, stride, ptr);
}
static void g_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    declare(A_TC0 + client_unit, size, type, stride, ptr);
}
static void g_glColorPointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    declare(A_COL, size, type, stride, ptr);
}
static void g_glNormalPointer(GLenum type, GLsizei stride, const void *ptr) {
    declare(A_NRM, 3, type, stride, ptr);
}

static void g_glClientActiveTexture(GLenum unit) {
    client_unit = (unit - GL_TEXTURE0) & 1;
    glClientActiveTexture(unit);
}

static uint32_t gles_draw_calls;
static int wide_begin(void);
static void wide_end(int on);

uint32_t gles_get_draw_calls(void) {
    return gles_draw_calls;
}

static void g_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    gles_draw_calls++;
    if (any_convert())
        prepare_draw(first + count);
    int w = wide_begin();
    glDrawArrays(mode, first, count);
    wide_end(w);
}

static void g_glGetPointerv(GLenum pname, void **out) {
    if (pname == GL_VERTEX_ARRAY_POINTER)
        *out = (void *)arr[A_VTX].ptr;
    else if (pname == GL_TEXTURE_COORD_ARRAY_POINTER)
        *out = (void *)arr[A_TC0 + client_unit].ptr;
    else if (pname == GL_COLOR_ARRAY_POINTER)
        *out = (void *)arr[A_COL].ptr;
    else if (pname == GL_NORMAL_ARRAY_POINTER)
        *out = (void *)arr[A_NRM].ptr;
    else
        glGetPointerv(pname, out);
}

// ---------------------------------------------------------------- *x state

static void g_glColor4x(GLfixed r, GLfixed g, GLfixed b, GLfixed a) {
    color[0] = r, color[1] = g, color[2] = b, color[3] = a;
    glColor4x(r, g, b, a);
}

static void g_glTexEnvi(GLenum target, GLenum pname, GLint v) {
    if (pname == GL_TEXTURE_ENV_MODE)
        env_mode = v;
    glTexEnvi(target, pname, v);
}

static void g_glTexEnvxv(GLenum target, GLenum pname, const GLfixed *v) {
    if (pname == GL_TEXTURE_ENV_MODE) // enum value, not fixed point
        return g_glTexEnvi(target, pname, v[0]);
    glTexEnvxv(target, pname, (GLfixed *)v);
}

// Texture parameters used by the engine are enums (filters, wrap modes): pass unscaled.
static void g_glTexParameterx(GLenum target, GLenum pname, GLfixed v) { glTexParameteri(target, pname, v); }

static void g_glGetFixedv(GLenum pname, GLfixed *out) {
    float f[16];
    int n = 1;
    if (pname == GL_MODELVIEW_MATRIX || pname == GL_PROJECTION_MATRIX || pname == GL_TEXTURE_MATRIX)
        n = 16;
    else if (pname == GL_CURRENT_COLOR) {
        memcpy(out, color, sizeof(color));
        return;
    }
    glGetFloatv(pname, f);
    for (int i = 0; i < n; i++)
        out[i] = (GLfixed)(f[i] * 65536.0f);
}

static void g_glGetLightxv(GLenum light, GLenum pname, GLfixed *out) { memset(out, 0, 4 * sizeof(GLfixed)); }

// ---------------------------------------------------------------- EAGLView framebuffer emulation

// OES framebuffers/renderbuffers are real vitaGL objects (render-to-texture keeps working). The
// one exception is the renderbuffer backed by the CAEAGLLayer ([EAGLContext renderbufferStorage:
// fromDrawable:]): attaching it as a color buffer attaches fbo_tex instead, which gles_present blits.
static GLuint bound_rb, drawable_rb;
static GLuint depth_fbs[64]; // framebuffers whose depth attachment was already forwarded
static int ndepth_fbs;

void gles_bind_drawable(void) {
    drawable_rb = bound_rb;
    l_info("EAGL drawable renderbuffer = %u (%ux%u)", drawable_rb, FB_W, FB_H);
}

static void g_glBindRenderbufferOES(GLenum target, GLuint rb) {
    bound_rb = rb;
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
}
static void g_glFramebufferRenderbufferOES(GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb) {
    if (rb && rb == drawable_rb && attachment == GL_COLOR_ATTACHMENT0) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint *)&fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex, 0);
        l_info("EAGL window framebuffer = %u", fbo);
        return;
    }
    // vitaGL renderbuffers hold no depth memory: every depth attach DISCARDS the framebuffer's own
    // depth buffer and a fresh, uninitialized one is created later. LibEffects (cutscene effects)
    // re-attaches depth every frame, which corrupted the scene's depth mid-frame. Since the attached
    // renderbuffer cannot change which memory is used anyway, only the first depth attach of each
    // framebuffer is forwarded.
    if (attachment == GL_DEPTH_ATTACHMENT || attachment == GL_STENCIL_ATTACHMENT) {
        GLint cur;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &cur);
        for (int i = 0; i < ndepth_fbs; i++)
            if (depth_fbs[i] == (GLuint)cur)
                return;
        if (rb && ndepth_fbs < 64)
            depth_fbs[ndepth_fbs++] = cur;
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, GL_RENDERBUFFER, rb);
}
static void g_glDeleteFramebuffersOES(GLsizei n, const GLuint *ids) {
    for (int k = 0; k < n; k++)
        for (int i = 0; i < ndepth_fbs; i++)
            if (depth_fbs[i] == ids[k])
                depth_fbs[i--] = depth_fbs[--ndepth_fbs];
    glDeleteFramebuffers(n, ids);
}

// ---------------------------------------------------------------- widescreen (see WIDE_K)

static GLuint bound_fb;
static GLint scissor_box[4];
static int scissor_wide = -1; // state the real scissor box was last set for

static void g_glBindFramebufferOES(GLenum target, GLuint fb) {
    bound_fb = fb;
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
}
static int wide_target(void) { return fbo && bound_fb == fbo; }

// Screen-horizontal = framebuffer Y (the 320x480 framebuffer is portrait): Y spans are scaled
// around the framebuffer center.
static void wide_span(GLint *y, GLsizei *h) {
    float c = FB_H / 2.0f;
    *y = (GLint)(c + (*y - c) * WIDE_K);
    *h = (GLsizei)(*h * WIDE_K + 0.5f);
}
static void apply_scissor(void) {
    int w = wide_target();
    if (w == scissor_wide)
        return;
    GLint x = scissor_box[0], y = scissor_box[1];
    GLsizei sw = scissor_box[2], sh = scissor_box[3];
    if (w)
        wide_span(&y, &sh);
    glScissor(x, y, sw, sh);
    scissor_wide = w;
}
static void g_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    scissor_box[0] = x, scissor_box[1] = y, scissor_box[2] = w, scissor_box[3] = h;
    scissor_wide = -1;
    apply_scissor();
}
static void g_glClear(GLbitfield mask) {
    apply_scissor();
    glClear(mask);
}
static void g_glCopyTexImage2D(GLenum t, GLint l, GLenum f, GLint x, GLint y, GLsizei w, GLsizei h, GLint b) {
    if (wide_target())
        wide_span(&y, &h);
    glCopyTexImage2D(t, l, f, x, y, w, h, b);
}
static void g_glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) {
    if (wide_target())
        wide_span(&y, &h);
    glCopyTexSubImage2D(t, l, xo, yo, x, y, w, h);
}

// Around each draw into the window framebuffer: projection P -> S * P, S scaling clip Y by WIDE_K
// (row 1 of the column-major matrix). Restored right after, so the game's matrices never change.
static int wide_begin(void) {
    apply_scissor();
    if (!wide_target())
        return 0;
    GLint mode;
    float p[16];
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glGetFloatv(GL_PROJECTION_MATRIX, p);
    p[1] *= WIDE_K, p[5] *= WIDE_K, p[9] *= WIDE_K, p[13] *= WIDE_K;
    glLoadMatrixf(p);
    glMatrixMode(mode);
    return 1;
}
static void wide_end(int on) {
    if (!on)
        return;
    GLint mode;
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(mode);
}
static void g_glGetRenderbufferParameterivOES(GLenum target, GLenum pname, GLint *v) {
    if (bound_rb && bound_rb == drawable_rb)
        *v = pname == GL_RENDERBUFFER_WIDTH_OES ? FB_W : pname == GL_RENDERBUFFER_HEIGHT_OES ? FB_H : 0;
    else
        *v = 0; // ponytail: vitaGL has no glGetRenderbufferParameteriv; only the drawable is queried by EAGLView
}
static GLuint element_vbo;
static void g_glBindBuffer(GLenum target, GLuint b) {
    if (target == GL_ARRAY_BUFFER)
        array_vbo = b;
    else if (target == GL_ELEMENT_ARRAY_BUFFER)
        element_vbo = b;
    glBindBuffer(target, b);
}

// Converted arrays need the highest referenced index (indices may live in a buffer object).
static void g_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx) {
    gles_draw_calls++;
    if (any_convert()) {
        const uint8_t *ix = idx;
        if (element_vbo) {
            const uint8_t *base = *(const uint8_t **)(uintptr_t)element_vbo;
            ix = base ? base + (uintptr_t)idx : NULL;
        }
        int n = 0;
        for (int i = 0; ix && i < count; i++) {
            int v = type == GL_UNSIGNED_BYTE ? ix[i] : ((const uint16_t *)ix)[i];
            if (v >= n)
                n = v + 1;
        }
        prepare_draw(n);
    }
    int w = wide_begin();
    glDrawElements(mode, count, type, idx);
    wide_end(w);
}

static void g_glLightf(GLenum light, GLenum pname, GLfloat v) { glLightfv(light, pname, &v); }
static void g_glNoop(void) {}

// ---------------------------------------------------------------- init / present

void gles_init(void) {
    vglUseTripleBuffering(GL_TRUE);
    vglInitWithCustomThreshold(0, SCR_W, SCR_H, 32 * 1024 * 1024, 16 * 1024 * 1024, 8 * 1024 * 1024,
                               SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE, SCE_GXM_MULTISAMPLE_NONE);
    glGenTextures(1, &fbo_tex);
    glBindTexture(GL_TEXTURE_2D, fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FB_W, FB_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glViewport(0, 0, FB_W, FB_H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    l_info("vitaGL: FBO %ux%u -> screen %ux%u at x=%d", FB_W, FB_H, DST_W, DST_H, DST_X);
}

// Blits the 320x480 framebuffer to the screen. The game rotates its own rendering according to
// its device orientation (CTouchPad::s_currentDeviceOrientation): rotate180 = orientation 1.
// Every fixed-function state that could alter the blit (lighting, fog, culling, a second texture
// unit, the texture matrix, write masks...) is saved, neutralized and restored: the game leaves
// them enabled across frames (in-engine cutscenes use fog/lighting).
void gles_present(int rotate180) {
    static const GLenum caps[] = {GL_BLEND,      GL_ALPHA_TEST, GL_SCISSOR_TEST,   GL_DEPTH_TEST,
                                  GL_CULL_FACE,  GL_LIGHTING,   GL_FOG,            GL_STENCIL_TEST,
                                  GL_CLIP_PLANE0, GL_TEXTURE_2D}; // vitaGL can't query GL_COLOR_MATERIAL
#define NCAPS (sizeof(caps) / sizeof(caps[0]))
    GLboolean on[NCAPS], tca, va, ca, na, tex1_on, cmask[4], dmask;
    GLint tex, mode, active, vp[4], bound_fb;
    for (unsigned i = 0; i < NCAPS; i++)
        on[i] = glIsEnabled(caps[i]);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE1);
    tex1_on = glIsEnabled(GL_TEXTURE_2D);
    glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    tca = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
    va = glIsEnabled(GL_VERTEX_ARRAY);
    ca = glIsEnabled(GL_COLOR_ARRAY);
    na = glIsEnabled(GL_NORMAL_ARRAY);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound_fb);
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
    GLfloat clear[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0); // the blit quad uses client arrays
    glViewport(0, 0, SCR_W, SCR_H);
    for (unsigned i = 0; i < NCAPS - 1; i++)
        glDisable(caps[i]);
    glEnable(GL_TEXTURE_2D);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glBindTexture(GL_TEXTURE_2D, fbo_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(0, SCR_W, SCR_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glClearColor(0, 0, 0, 1); // side bars: black, not the game's clear color
    glClear(GL_COLOR_BUFFER_BIT);
    // Screen corners (top-left, top-right, bottom-left, bottom-right) -> vertical FBO texture.
    // Orientation 3: the top edge of the framebuffer is the left edge of the screen; 1 is the
    // same rotated 180 degrees. Must stay consistent with to_view() in uikit.c.
    static const float uv3[] = {1, 1, 1, 0, 0, 1, 0, 0}, uv1[] = {0, 0, 0, 1, 1, 0, 1, 1};
    const float xy[] = {DST_X, 0, DST_X + DST_W, 0, DST_X, SCR_H, DST_X + DST_W, SCR_H};
    glVertexPointer(2, GL_FLOAT, 0, xy);
    glTexCoordPointer(2, GL_FLOAT, 0, rotate180 ? uv1 : uv3);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    patch_overlay(); // loader OSD on the 960x544 screen (scissor + clear only)
    vglSwapBuffers(ime_active()); // GL_TRUE while system dialog is active

    // Restore guest OpenGL state.
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(mode);
    glBindFramebuffer(GL_FRAMEBUFFER, bound_fb);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
    glDepthMask(dmask);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, env_mode);
    for (unsigned i = 0; i < NCAPS; i++)
        on[i] ? glEnable(caps[i]) : glDisable(caps[i]);
    if (tex1_on) {
        glActiveTexture(GL_TEXTURE1);
        glEnable(GL_TEXTURE_2D);
    }
    if (!va)
        glDisableClientState(GL_VERTEX_ARRAY);
    if (!tca)
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if (ca)
        glEnableClientState(GL_COLOR_ARRAY);
    if (na)
        glEnableClientState(GL_NORMAL_ARRAY);
    glColor4x(color[0], color[1], color[2], color[3]);
    reapply_arrays();
    glClientActiveTexture(GL_TEXTURE0 + client_unit);
    glActiveTexture(active);
#undef NCAPS
}

// ---------------------------------------------------------------- import table

#define F(name, fn) {"_" #name, (uintptr_t)(fn)}
#define D(name) {"_" #name, (uintptr_t)(name)}

const import_entry gles_imports[] = {
    D(glActiveTexture), D(glBindTexture), D(glBlendFunc), F(glClear, g_glClear), F(glCopyTexImage2D, g_glCopyTexImage2D),
    F(glCopyTexSubImage2D, g_glCopyTexSubImage2D), D(glDeleteTextures), D(glDepthMask), D(glDisable), F(glDisableClientState, g_glDisableClientState),
    D(glEnable), F(glEnableClientState, g_glEnableClientState), D(glGenTextures), D(glGetIntegerv), D(glIsEnabled),
    D(glLoadIdentity), D(glLoadMatrixx), D(glMatrixMode), D(glOrthox), D(glPopMatrix), D(glPushMatrix),
    D(glRotatex), D(glScalex), F(glScissor, g_glScissor), D(glShadeModel), D(glTexImage2D), D(glTexParameteri),
    D(glTexSubImage2D), D(glTranslatex), D(glViewport),
    F(glColor4x, g_glColor4x), F(glTexEnvi, g_glTexEnvi), F(glTexEnvxv, g_glTexEnvxv),
    F(glTexParameterx, g_glTexParameterx), F(glGetFixedv, g_glGetFixedv), F(glGetLightxv, g_glGetLightxv),
    F(glVertexPointer, g_glVertexPointer), F(glTexCoordPointer, g_glTexCoordPointer),
    F(glClientActiveTexture, g_glClientActiveTexture), F(glDrawArrays, g_glDrawArrays),
    F(glGetPointerv, g_glGetPointerv),
    F(glGenFramebuffersOES, glGenFramebuffers), F(glGenRenderbuffersOES, glGenRenderbuffers),
    F(glBindFramebufferOES, g_glBindFramebufferOES), F(glBindRenderbufferOES, g_glBindRenderbufferOES),
    F(glDeleteFramebuffersOES, g_glDeleteFramebuffersOES), F(glDeleteRenderbuffersOES, glDeleteRenderbuffers),
    F(glRenderbufferStorageOES, glRenderbufferStorage),
    F(glFramebufferRenderbufferOES, g_glFramebufferRenderbufferOES),
    F(glFramebufferTexture2DOES, glFramebufferTexture2D),
    F(glCheckFramebufferStatusOES, glCheckFramebufferStatus),
    F(glGetRenderbufferParameterivOES, g_glGetRenderbufferParameterivOES),
    F(glGetFramebufferAttachmentParameterivOES, glGetFramebufferAttachmentParameteriv),
    // Float entry points: loader and vitaGL (SOFTFP_ABI=1) share the iOS softfp convention.
    D(glAlphaFunc), D(glClearColor), D(glClearDepthf), D(glClipPlanef), D(glColor4f), D(glColor4ub),
    D(glColorMask), F(glColorPointer, g_glColorPointer), D(glCompressedTexImage2D), D(glCullFace), D(glDepthFunc),
    D(glDepthRangef), D(glFogf), D(glFogfv), D(glFrontFace), D(glGetBooleanv), D(glGetError),
    D(glGetFloatv), D(glGetString), D(glHint), D(glLightModelfv), D(glLightfv), D(glLineWidth),
    D(glLoadMatrixf), D(glMaterialf), D(glMaterialfv), D(glMultMatrixf), D(glNormal3f),
    F(glNormalPointer, g_glNormalPointer), D(glPixelStorei), D(glPointSize), D(glPolygonOffset), D(glReadPixels),
    D(glRotatef), D(glScalef), D(glStencilFunc), D(glStencilMask), D(glStencilOp), D(glTexEnvf),
    D(glTexEnvfv), D(glTexEnvx), D(glTexParameterf), D(glTranslatef), D(glGenBuffers),
    D(glDeleteBuffers), D(glBufferData), D(glBufferSubData), F(glBindBuffer, g_glBindBuffer),
    F(glDrawElements, g_glDrawElements), F(glLightf, g_glLightf), F(glPointParameterf, g_glNoop),
};
const int gles_imports_count = sizeof(gles_imports) / sizeof(gles_imports[0]);
