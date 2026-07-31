#ifndef OBC_MATH3D_H
#define OBC_MATH3D_H

#include <math.h>

/* Basic 3D math. Z is up, units are millimeters. */

struct Vec2 {
    float x, y;
};

static inline Vec2 vec2(float x, float y) {
    Vec2 v; v.x = x; v.y = y; return v;
}

struct Vec3 {
    float x, y, z;
};

static inline Vec3 vec3(float x, float y, float z) {
    Vec3 v; v.x = x; v.y = y; v.z = z; return v;
}

static inline Vec3 vec3_add(Vec3 a, Vec3 b) { return vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline Vec3 vec3_sub(Vec3 a, Vec3 b) { return vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline Vec3 vec3_mul(Vec3 a, float s) { return vec3(a.x * s, a.y * s, a.z * s); }
static inline Vec3 vec3_scaled(Vec3 a, Vec3 s) { return vec3(a.x * s.x, a.y * s.y, a.z * s.z); }
static inline float vec3_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

static inline Vec3 vec3_cross(Vec3 a, Vec3 b) {
    return vec3(a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x);
}

static inline float vec3_length(Vec3 a) { return sqrtf(vec3_dot(a, a)); }

static inline Vec3 vec3_normalized(Vec3 a) {
    float len = vec3_length(a);
    if (len < 1e-8f) return vec3(0.0f, 0.0f, 0.0f);
    return vec3_mul(a, 1.0f / len);
}

static inline float deg_to_rad(float deg) { return deg * 3.14159265358979f / 180.0f; }
static inline float rad_to_deg(float rad) { return rad * 180.0f / 3.14159265358979f; }

/*
 * 3x3 rotation matrices, row major: m[row * 3 + col], acting as v' = M * v.
 *
 * These exist so a drag can rotate about an arbitrary world axis even though a
 * node stores Euler angles: compose the extra rotation with the node's current
 * one as matrices, then read ZYX angles back out. Adding degrees to one Euler
 * component directly is only correct when the other two are zero.
 */
struct Mat3 {
    float m[9];
};

static inline Mat3 mat3_identity(void) {
    Mat3 r;
    for (int i = 0; i < 9; ++i) r.m[i] = 0.0f;
    r.m[0] = r.m[4] = r.m[8] = 1.0f;
    return r;
}

static inline Mat3 mat3_mul(Mat3 a, Mat3 b) {
    Mat3 r;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) sum += a.m[row * 3 + k] * b.m[k * 3 + col];
            r.m[row * 3 + col] = sum;
        }
    }
    return r;
}

static inline Vec3 mat3_apply(Mat3 a, Vec3 v) {
    return vec3(a.m[0] * v.x + a.m[1] * v.y + a.m[2] * v.z,
                a.m[3] * v.x + a.m[4] * v.y + a.m[5] * v.z,
                a.m[6] * v.x + a.m[7] * v.y + a.m[8] * v.z);
}

/*
 * Column i of a rotation matrix is the image of basis vector i, so for a frame
 * built as local -> world it is that frame's axis expressed in world space.
 */
static inline Vec3 mat3_column(Mat3 a, int index) {
    return vec3(a.m[0 * 3 + index], a.m[1 * 3 + index], a.m[2 * 3 + index]);
}

/* For a rotation the transpose is the inverse, which is what turns a
 * local -> world frame into world -> local. */
static inline Mat3 mat3_transposed(Mat3 a) {
    Mat3 r;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) r.m[row * 3 + col] = a.m[col * 3 + row];
    }
    return r;
}

/* Rotation by "degrees" about a unit axis. */
static inline Mat3 mat3_axis_angle(Vec3 axis, float degrees) {
    Vec3 a = vec3_normalized(axis);
    float rad = deg_to_rad(degrees);
    float c = cosf(rad);
    float s = sinf(rad);
    float t = 1.0f - c;

    Mat3 r;
    r.m[0] = t * a.x * a.x + c;
    r.m[1] = t * a.x * a.y - s * a.z;
    r.m[2] = t * a.x * a.z + s * a.y;
    r.m[3] = t * a.x * a.y + s * a.z;
    r.m[4] = t * a.y * a.y + c;
    r.m[5] = t * a.y * a.z - s * a.x;
    r.m[6] = t * a.x * a.z - s * a.y;
    r.m[7] = t * a.y * a.z + s * a.x;
    r.m[8] = t * a.z * a.z + c;
    return r;
}

/* Matches apply_local / push_node_transform: v' = Rz * Ry * Rx * v. */
static inline Mat3 mat3_from_euler_zyx(Vec3 degrees) {
    Mat3 rx = mat3_axis_angle(vec3(1.0f, 0.0f, 0.0f), degrees.x);
    Mat3 ry = mat3_axis_angle(vec3(0.0f, 1.0f, 0.0f), degrees.y);
    Mat3 rz = mat3_axis_angle(vec3(0.0f, 0.0f, 1.0f), degrees.z);
    return mat3_mul(rz, mat3_mul(ry, rx));
}

static inline Vec3 mat3_to_euler_zyx(Mat3 r) {
    /* Inverse of the product above. m[6] is -sin(pitch). */
    float sy = -r.m[6];
    if (sy > 1.0f) sy = 1.0f;
    if (sy < -1.0f) sy = -1.0f;

    float y = asinf(sy);
    float x, z;
    if (fabsf(sy) > 0.99999f) {
        /* Gimbal lock: roll and yaw become the same rotation, so pin one. */
        x = 0.0f;
        z = atan2f(-r.m[1], r.m[4]);
    } else {
        x = atan2f(r.m[7], r.m[8]);
        z = atan2f(r.m[3], r.m[0]);
    }
    return vec3(rad_to_deg(x), rad_to_deg(y), rad_to_deg(z));
}

/* Axis aligned bounds, used for selection outlines and fit-to-view. */
struct Bounds {
    Vec3 min, max;
    bool valid;
};

static inline Bounds bounds_empty(void) {
    Bounds b;
    b.min = vec3(0.0f, 0.0f, 0.0f);
    b.max = vec3(0.0f, 0.0f, 0.0f);
    b.valid = false;
    return b;
}

static inline void bounds_add_point(Bounds *b, Vec3 p) {
    if (!b->valid) {
        b->min = p;
        b->max = p;
        b->valid = true;
        return;
    }
    if (p.x < b->min.x) b->min.x = p.x;
    if (p.y < b->min.y) b->min.y = p.y;
    if (p.z < b->min.z) b->min.z = p.z;
    if (p.x > b->max.x) b->max.x = p.x;
    if (p.y > b->max.y) b->max.y = p.y;
    if (p.z > b->max.z) b->max.z = p.z;
}

static inline void bounds_merge(Bounds *b, const Bounds &other) {
    if (!other.valid) return;
    bounds_add_point(b, other.min);
    bounds_add_point(b, other.max);
}

static inline Vec3 bounds_center(const Bounds &b) {
    return vec3_mul(vec3_add(b.min, b.max), 0.5f);
}

static inline Vec3 bounds_size(const Bounds &b) {
    return vec3_sub(b.max, b.min);
}

#endif
