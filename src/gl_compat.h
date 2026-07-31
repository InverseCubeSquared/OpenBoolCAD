#ifndef OBC_GL_COMPAT_H
#define OBC_GL_COMPAT_H

/*
 * One GL surface for both builds.
 *
 * The desktop build talks to fixed function GL 1.2 directly. The browser build
 * gets WebGL, which is GLES 2.0: no matrix stack, no client side vertex arrays,
 * no glColor, no line stipple. Rather than fork render.cpp and camera.cpp, this
 * header redirects the handful of fixed function entry points they use onto a
 * small emulation over GLES 2.0, so both files compile unchanged and the 1.2
 * floor in CLAUDE.md stays the single rendering discipline.
 *
 * The emulated subset is exactly what those two files call and nothing more. It
 * is not a general GL 1.x implementation and must not grow into one: a call the
 * renderer needs but that is missing here shows up as a compile error, which is
 * the point - a silent dependency on desktop-only GL would only break in the
 * browser, where nobody is looking.
 *
 * What is emulated exactly: the matrix stacks, vertex and colour arrays, and
 * glColor3fv. What is emulated approximately: glPointSize goes through
 * gl_PointSize, so points are square; glLineWidth is passed through and most
 * browsers clamp it to 1.0, so the width argument reads as a hint. What is
 * dropped: glShadeModel and GL_LINE_SMOOTH, which the WebGL context's own
 * antialiasing covers.
 */

#if defined(__EMSCRIPTEN__)

#include <GLES2/gl2.h>

/* Tokens GLES 2.0 has no use for but the renderer names. Values are the
 * historical GL 1.1 ones, so a mixed build cannot disagree about them. */
#define GL_LIGHTING         0x0B50
#define GL_LINE_STIPPLE     0x0B24
#define GL_LINE_SMOOTH      0x0B20
#define GL_LINE_SMOOTH_HINT 0x0C52
#define GL_NICEST           0x1102
#define GL_FLAT             0x1D00
#define GL_SMOOTH           0x1D01
#define GL_MODELVIEW        0x1700
#define GL_PROJECTION       0x1701
#define GL_VERTEX_ARRAY     0x8074
#define GL_COLOR_ARRAY      0x8076

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the shader and the streaming buffers. Must run with a current
 * context; every other call here is inert until it has. */
void obc_gl_init(void);
void obc_gl_shutdown(void);

void obc_gl_matrix_mode(GLenum mode);
void obc_gl_load_identity(void);
void obc_gl_load_matrixf(const GLfloat *m);
void obc_gl_push_matrix(void);
void obc_gl_pop_matrix(void);
void obc_gl_translatef(GLfloat x, GLfloat y, GLfloat z);
void obc_gl_rotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void obc_gl_scalef(GLfloat x, GLfloat y, GLfloat z);
void obc_gl_ortho(float l, float r, float b, float t, float n, float f);
void obc_gl_frustum(float l, float r, float b, float t, float n, float f);

void obc_gl_enable_client_state(GLenum array);
void obc_gl_disable_client_state(GLenum array);
void obc_gl_vertex_pointer(GLint size, GLenum type, GLsizei stride, const void *ptr);
void obc_gl_color_pointer(GLint size, GLenum type, GLsizei stride, const void *ptr);
void obc_gl_color3fv(const GLfloat *rgb);

void obc_gl_point_size(GLfloat size);
void obc_gl_line_stipple(GLint factor, GLushort pattern);
void obc_gl_shade_model(GLenum mode);
void obc_gl_hint(GLenum target, GLenum mode);
void obc_gl_enable(GLenum cap);
void obc_gl_disable(GLenum cap);
void obc_gl_viewport(GLint x, GLint y, GLsizei w, GLsizei h);
void obc_gl_draw_arrays(GLenum mode, GLint first, GLsizei count);
void obc_gl_read_pixels(GLint x, GLint y, GLsizei w, GLsizei h,
                        GLenum format, GLenum type, void *pixels);

#ifdef __cplusplus
}
#endif

/* gl_compat.cpp needs the real entry points, so it defines this before
 * including us. Everything else gets the redirect. */
#ifndef OBC_GL_COMPAT_IMPL

#define glMatrixMode        obc_gl_matrix_mode
#define glLoadIdentity      obc_gl_load_identity
#define glLoadMatrixf       obc_gl_load_matrixf
#define glPushMatrix        obc_gl_push_matrix
#define glPopMatrix         obc_gl_pop_matrix
#define glTranslatef        obc_gl_translatef
#define glRotatef           obc_gl_rotatef
#define glScalef            obc_gl_scalef
#define glOrtho             obc_gl_ortho
#define glFrustum           obc_gl_frustum
#define glEnableClientState obc_gl_enable_client_state
#define glDisableClientState obc_gl_disable_client_state
#define glVertexPointer     obc_gl_vertex_pointer
#define glColorPointer      obc_gl_color_pointer
#define glColor3fv          obc_gl_color3fv
#define glPointSize         obc_gl_point_size
#define glLineStipple       obc_gl_line_stipple
#define glShadeModel        obc_gl_shade_model
#define glHint              obc_gl_hint
#define glEnable            obc_gl_enable
#define glDisable           obc_gl_disable
#define glViewport          obc_gl_viewport
#define glDrawArrays        obc_gl_draw_arrays
#define glReadPixels        obc_gl_read_pixels

#endif /* OBC_GL_COMPAT_IMPL */

#else /* desktop */

#include <GL/gl.h>

static inline void obc_gl_init(void) {}
static inline void obc_gl_shutdown(void) {}

#endif

#endif
