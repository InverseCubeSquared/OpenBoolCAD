/*
 * Fixed function emulation over GLES 2.0, for the browser build only. See
 * gl_compat.h for what is emulated and what is deliberately approximate.
 */

#define OBC_GL_COMPAT_IMPL
#include "gl_compat.h"

#if defined(__EMSCRIPTEN__)

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

/* Matrices, column major throughout, so glLoadMatrixf is a plain copy and the
 * multiply order matches what the fixed function pipeline documents. */

#define MAT_STACK_DEPTH 32

static void mat4_identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_mul(float *out, const float *a, const float *b) {
    float t[16];
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            t[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] +
                           a[1 * 4 + r] * b[c * 4 + 1] +
                           a[2 * 4 + r] * b[c * 4 + 2] +
                           a[3 * 4 + r] * b[c * 4 + 3];
        }
    }
    memcpy(out, t, sizeof(t));
}

struct MatrixStack {
    float m[MAT_STACK_DEPTH][16];
    int top;
};

static void stack_init(MatrixStack *s) {
    s->top = 0;
    mat4_identity(s->m[0]);
}

/* Right multiply, as every fixed function matrix call does. */
static void stack_apply(MatrixStack *s, const float *rhs) {
    mat4_mul(s->m[s->top], s->m[s->top], rhs);
}

/* State */

struct ArrayRef {
    bool enabled;
    const void *ptr;
    GLint size;
    GLsizei stride;
};

struct CompatState {
    bool ready;

    MatrixStack modelview;
    MatrixStack projection;
    MatrixStack *current;

    ArrayRef vertex;
    ArrayRef color;

    float constant_color[4];
    float point_size;

    bool stipple_on;
    GLint stipple_factor;
    GLushort stipple_pattern;

    GLint viewport[4];

    GLuint program;
    GLint u_mvp;
    GLint u_color;
    GLint u_use_array;
    GLint u_point_size;
    GLuint vbo_pos;
    GLuint vbo_col;
};

static CompatState g;

/* Shader */

static const char *VERTEX_SRC =
    "attribute vec4 a_pos;\n"
    "attribute vec4 a_col;\n"
    "uniform mat4 u_mvp;\n"
    "uniform vec4 u_color;\n"
    "uniform float u_use_array;\n"
    "uniform float u_point_size;\n"
    "varying vec4 v_col;\n"
    "void main() {\n"
    "    gl_Position = u_mvp * a_pos;\n"
    "    v_col = mix(u_color, a_col, u_use_array);\n"
    "    gl_PointSize = u_point_size;\n"
    "}\n";

static const char *FRAGMENT_SRC =
    "precision mediump float;\n"
    "varying vec4 v_col;\n"
    "void main() {\n"
    "    gl_FragColor = v_col;\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);

    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "gl_compat: shader compile failed: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

void obc_gl_init(void) {
    memset(&g, 0, sizeof(g));
    stack_init(&g.modelview);
    stack_init(&g.projection);
    g.current = &g.modelview;
    g.constant_color[0] = g.constant_color[1] = g.constant_color[2] = 1.0f;
    g.constant_color[3] = 1.0f;
    g.point_size = 1.0f;
    g.stipple_factor = 1;
    g.stipple_pattern = 0xFFFF;
    g.vertex.size = 3;
    g.color.size = 4;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, VERTEX_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAGMENT_SRC);
    if (!vs || !fs) return;

    g.program = glCreateProgram();
    glAttachShader(g.program, vs);
    glAttachShader(g.program, fs);
    /* Fixed locations, so a draw never has to look them up. */
    glBindAttribLocation(g.program, 0, "a_pos");
    glBindAttribLocation(g.program, 1, "a_col");
    glLinkProgram(g.program);

    GLint ok = 0;
    glGetProgramiv(g.program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(g.program, sizeof(log), NULL, log);
        fprintf(stderr, "gl_compat: program link failed: %s\n", log);
        glDeleteProgram(g.program);
        g.program = 0;
        return;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);

    g.u_mvp = glGetUniformLocation(g.program, "u_mvp");
    g.u_color = glGetUniformLocation(g.program, "u_color");
    g.u_use_array = glGetUniformLocation(g.program, "u_use_array");
    g.u_point_size = glGetUniformLocation(g.program, "u_point_size");

    glGenBuffers(1, &g.vbo_pos);
    glGenBuffers(1, &g.vbo_col);
    g.ready = true;
}

void obc_gl_shutdown(void) {
    if (!g.ready) return;
    glDeleteBuffers(1, &g.vbo_pos);
    glDeleteBuffers(1, &g.vbo_col);
    glDeleteProgram(g.program);
    memset(&g, 0, sizeof(g));
}

/* Matrix stack */

void obc_gl_matrix_mode(GLenum mode) {
    g.current = (mode == GL_PROJECTION) ? &g.projection : &g.modelview;
}

void obc_gl_load_identity(void) {
    mat4_identity(g.current->m[g.current->top]);
}

void obc_gl_load_matrixf(const GLfloat *m) {
    memcpy(g.current->m[g.current->top], m, 16 * sizeof(float));
}

void obc_gl_push_matrix(void) {
    MatrixStack *s = g.current;
    if (s->top + 1 >= MAT_STACK_DEPTH) return; // deep scene trees clamp, not crash
    memcpy(s->m[s->top + 1], s->m[s->top], 16 * sizeof(float));
    s->top++;
}

void obc_gl_pop_matrix(void) {
    if (g.current->top > 0) g.current->top--;
}

void obc_gl_translatef(GLfloat x, GLfloat y, GLfloat z) {
    float m[16];
    mat4_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
    stack_apply(g.current, m);
}

void obc_gl_rotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    float len = sqrtf(x * x + y * y + z * z);
    if (len < 1e-8f) return;
    x /= len; y /= len; z /= len;

    float a = angle * 3.14159265358979f / 180.0f;
    float c = cosf(a);
    float s = sinf(a);
    float ic = 1.0f - c;

    float m[16];
    mat4_identity(m);
    m[0]  = x * x * ic + c;
    m[1]  = y * x * ic + z * s;
    m[2]  = z * x * ic - y * s;
    m[4]  = x * y * ic - z * s;
    m[5]  = y * y * ic + c;
    m[6]  = z * y * ic + x * s;
    m[8]  = x * z * ic + y * s;
    m[9]  = y * z * ic - x * s;
    m[10] = z * z * ic + c;
    stack_apply(g.current, m);
}

void obc_gl_scalef(GLfloat x, GLfloat y, GLfloat z) {
    float m[16];
    mat4_identity(m);
    m[0] = x; m[5] = y; m[10] = z;
    stack_apply(g.current, m);
}

void obc_gl_ortho(float l, float r, float b, float t, float n, float f) {
    if (l == r || b == t || n == f) return;
    float m[16];
    mat4_identity(m);
    m[0]  = 2.0f / (r - l);
    m[5]  = 2.0f / (t - b);
    m[10] = -2.0f / (f - n);
    m[12] = -(r + l) / (r - l);
    m[13] = -(t + b) / (t - b);
    m[14] = -(f + n) / (f - n);
    stack_apply(g.current, m);
}

void obc_gl_frustum(float l, float r, float b, float t, float n, float f) {
    if (l == r || b == t || n == f || n <= 0.0f) return;
    float m[16];
    memset(m, 0, sizeof(m));
    m[0]  = 2.0f * n / (r - l);
    m[5]  = 2.0f * n / (t - b);
    m[8]  = (r + l) / (r - l);
    m[9]  = (t + b) / (t - b);
    m[10] = -(f + n) / (f - n);
    m[11] = -1.0f;
    m[14] = -2.0f * f * n / (f - n);
    stack_apply(g.current, m);
}

/* Vertex arrays */

void obc_gl_enable_client_state(GLenum array) {
    if (array == GL_VERTEX_ARRAY) g.vertex.enabled = true;
    else if (array == GL_COLOR_ARRAY) g.color.enabled = true;
}

void obc_gl_disable_client_state(GLenum array) {
    if (array == GL_VERTEX_ARRAY) g.vertex.enabled = false;
    else if (array == GL_COLOR_ARRAY) g.color.enabled = false;
}

void obc_gl_vertex_pointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    (void)type; // the renderer only ever hands over floats
    g.vertex.size = size;
    g.vertex.stride = stride;
    g.vertex.ptr = ptr;
}

void obc_gl_color_pointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    (void)type;
    g.color.size = size;
    g.color.stride = stride;
    g.color.ptr = ptr;
}

void obc_gl_color3fv(const GLfloat *rgb) {
    g.constant_color[0] = rgb[0];
    g.constant_color[1] = rgb[1];
    g.constant_color[2] = rgb[2];
    g.constant_color[3] = 1.0f;
}

/* Per draw state */

void obc_gl_point_size(GLfloat size) {
    g.point_size = size;
}

void obc_gl_line_stipple(GLint factor, GLushort pattern) {
    g.stipple_factor = factor > 0 ? factor : 1;
    g.stipple_pattern = pattern;
}

void obc_gl_shade_model(GLenum mode) {
    /* Shading is computed on the CPU into a colour array, so flat versus smooth
     * never reaches the pipeline. */
    (void)mode;
}

void obc_gl_hint(GLenum target, GLenum mode) {
    /* GLES 2.0 only accepts GL_GENERATE_MIPMAP_HINT; passing the line smooth
     * hint through would raise GL_INVALID_ENUM every frame. */
    (void)target;
    (void)mode;
}

void obc_gl_enable(GLenum cap) {
    if (cap == GL_LINE_STIPPLE) { g.stipple_on = true; return; }
    if (cap == GL_LIGHTING || cap == GL_LINE_SMOOTH) return;
    glEnable(cap);
}

void obc_gl_disable(GLenum cap) {
    if (cap == GL_LINE_STIPPLE) { g.stipple_on = false; return; }
    if (cap == GL_LIGHTING || cap == GL_LINE_SMOOTH) return;
    glDisable(cap);
}

void obc_gl_viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    g.viewport[0] = x;
    g.viewport[1] = y;
    g.viewport[2] = w;
    g.viewport[3] = h;
    glViewport(x, y, w, h);
}

/* Drawing */

static GLsizei array_step(const ArrayRef &a) {
    return a.stride ? a.stride : (GLsizei)(a.size * (GLint)sizeof(float));
}

static void bind_stream(GLuint vbo, GLuint index, const ArrayRef &a,
                        GLint first, GLsizei count) {
    GLsizei step = array_step(a);
    const unsigned char *base = (const unsigned char *)a.ptr + (size_t)first * (size_t)step;

    /* WebGL has no client side arrays, so every draw streams its vertices
     * through a scratch buffer. */
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)count * step, base, GL_STREAM_DRAW);
    glEnableVertexAttribArray(index);
    glVertexAttribPointer(index, a.size, GL_FLOAT, GL_FALSE, step, (const void *)0);
}

static void begin_draw(const float *mvp, bool use_color_array) {
    glUseProgram(g.program);
    glUniformMatrix4fv(g.u_mvp, 1, GL_FALSE, mvp);
    glUniform4fv(g.u_color, 1, g.constant_color);
    glUniform1f(g.u_use_array, use_color_array ? 1.0f : 0.0f);
    glUniform1f(g.u_point_size, g.point_size);
}

static void end_draw(void) {
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void current_mvp(float *out) {
    mat4_mul(out, g.projection.m[g.projection.top], g.modelview.m[g.modelview.top]);
}

static void read_vertex(GLint index, float *out) {
    GLsizei step = array_step(g.vertex);
    const float *v = (const float *)((const unsigned char *)g.vertex.ptr +
                                     (size_t)index * (size_t)step);
    out[0] = v[0];
    out[1] = v[1];
    out[2] = (g.vertex.size >= 3) ? v[2] : 0.0f;
    out[3] = (g.vertex.size >= 4) ? v[3] : 1.0f;
}

/*
 * Line stipple, cut on the CPU.
 *
 * The dashes are measured in screen space, as glLineStipple defines them, so
 * each endpoint is transformed to clip space here, divided through, and the
 * dashes emitted as normalised device coordinates drawn with an identity
 * matrix. Cutting them in object space instead would make the dash length drift
 * with perspective, which is exactly what the ghost and align guides must not
 * do. Depth is interpolated linearly in NDC rather than perspective correctly,
 * which is invisible: every stippled draw in the renderer runs with depth
 * testing off.
 */
#define STIPPLE_MAX_BITS 4096

static void draw_stippled_lines(GLint first, GLsizei count) {
    float mvp[16];
    current_mvp(mvp);

    float vw = (float)g.viewport[2];
    float vh = (float)g.viewport[3];
    if (vw <= 0.0f || vh <= 0.0f) return;

    static std::vector<float> ndc;
    ndc.clear();

    for (GLsizei i = 0; i + 1 < count; i += 2) {
        float p0[4], p1[4];
        read_vertex(first + i, p0);
        read_vertex(first + i + 1, p1);

        float c0[4], c1[4];
        for (int r = 0; r < 4; ++r) {
            c0[r] = mvp[r] * p0[0] + mvp[4 + r] * p0[1] + mvp[8 + r] * p0[2] + mvp[12 + r] * p0[3];
            c1[r] = mvp[r] * p1[0] + mvp[4 + r] * p1[1] + mvp[8 + r] * p1[2] + mvp[12 + r] * p1[3];
        }
        /* A segment crossing the eye plane has no screen length to measure;
         * it is off screen anyway. */
        if (c0[3] <= 1e-6f || c1[3] <= 1e-6f) continue;

        float n0[3], n1[3];
        for (int r = 0; r < 3; ++r) {
            n0[r] = c0[r] / c0[3];
            n1[r] = c1[r] / c1[3];
        }

        float dx = (n1[0] - n0[0]) * 0.5f * vw;
        float dy = (n1[1] - n0[1]) * 0.5f * vh;
        float length_px = sqrtf(dx * dx + dy * dy);
        if (length_px < 1e-3f) continue;

        float bit_px = (float)g.stipple_factor;
        int bits = (int)(length_px / bit_px) + 1;

        /* Past this the line is so long that the dashes are sub-pixel; drawing
         * it solid reads better than emitting a hundred thousand vertices. */
        if (bits > STIPPLE_MAX_BITS) {
            for (int r = 0; r < 3; ++r) ndc.push_back(n0[r]);
            ndc.push_back(1.0f);
            for (int r = 0; r < 3; ++r) ndc.push_back(n1[r]);
            ndc.push_back(1.0f);
            continue;
        }

        for (int b = 0; b < bits; ++b) {
            if (!((g.stipple_pattern >> (b % 16)) & 1)) continue;

            float t0 = (float)b * bit_px / length_px;
            float t1 = (float)(b + 1) * bit_px / length_px;
            if (t0 >= 1.0f) break;
            if (t1 > 1.0f) t1 = 1.0f;

            for (int r = 0; r < 3; ++r) ndc.push_back(n0[r] + (n1[r] - n0[r]) * t0);
            ndc.push_back(1.0f);
            for (int r = 0; r < 3; ++r) ndc.push_back(n0[r] + (n1[r] - n0[r]) * t1);
            ndc.push_back(1.0f);
        }
    }

    if (ndc.empty()) return;

    float identity[16];
    mat4_identity(identity);
    begin_draw(identity, false);

    glBindBuffer(GL_ARRAY_BUFFER, g.vbo_pos);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(ndc.size() * sizeof(float)), &ndc[0], GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * (GLsizei)sizeof(float), (const void *)0);

    glDrawArrays(GL_LINES, 0, (GLsizei)(ndc.size() / 4));
    end_draw();
}

void obc_gl_draw_arrays(GLenum mode, GLint first, GLsizei count) {
    if (!g.ready || count <= 0) return;
    if (!g.vertex.enabled || !g.vertex.ptr) return;

    if (g.stipple_on && mode == GL_LINES) {
        draw_stippled_lines(first, count);
        return;
    }

    float mvp[16];
    current_mvp(mvp);

    bool use_colors = g.color.enabled && g.color.ptr != NULL;
    begin_draw(mvp, use_colors);

    bind_stream(g.vbo_pos, 0, g.vertex, first, count);
    if (use_colors) bind_stream(g.vbo_col, 1, g.color, first, count);

    glDrawArrays(mode, 0, count);
    end_draw();
}

/*
 * GLES 2.0 only guarantees RGBA readback, so an RGB request is served by
 * reading RGBA and dropping the alpha. The thumbnail path asks for RGB.
 */
void obc_gl_read_pixels(GLint x, GLint y, GLsizei w, GLsizei h,
                        GLenum format, GLenum type, void *pixels) {
    if (format != GL_RGB || type != GL_UNSIGNED_BYTE || w <= 0 || h <= 0) {
        glReadPixels(x, y, w, h, format, type, pixels);
        return;
    }

    std::vector<unsigned char> rgba((size_t)w * (size_t)h * 4);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, &rgba[0]);

    unsigned char *out = (unsigned char *)pixels;
    size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; ++i) {
        out[i * 3 + 0] = rgba[i * 4 + 0];
        out[i * 3 + 1] = rgba[i * 4 + 1];
        out[i * 3 + 2] = rgba[i * 4 + 2];
    }
}

#endif /* __EMSCRIPTEN__ */
