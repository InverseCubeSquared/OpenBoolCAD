#include "camera.h"

#include "gl_compat.h"

#define OBC_FOV_DEG 40.0f
#define OBC_NEAR_MIN 1.0f

void camera_init(Camera *c) {
    camera_home(c);
    c->orthographic = false;
}

void camera_home(Camera *c) {
    c->target = vec3(0.0f, 0.0f, 0.0f);
    c->yaw = -55.0f;
    c->pitch = 28.0f;
    c->distance = 260.0f;
}

void camera_orbit(Camera *c, float dyaw, float dpitch) {
    c->yaw += dyaw;
    c->pitch += dpitch;
    if (c->pitch > 89.0f) c->pitch = 89.0f;
    if (c->pitch < -89.0f) c->pitch = -89.0f;
    while (c->yaw > 360.0f) c->yaw -= 360.0f;
    while (c->yaw < -360.0f) c->yaw += 360.0f;
}

void camera_look_from(Camera *c, Vec3 direction) {
    Vec3 d = vec3_normalized(direction);
    if (vec3_length(d) < 0.5f) return;

    c->yaw = rad_to_deg(atan2f(d.y, d.x));
    /* Straight down the Z axis is where yaw stops meaning anything, so it stops
     * one degree short - the same clamp orbiting uses. */
    float pitch = rad_to_deg(asinf(d.z < -1.0f ? -1.0f : (d.z > 1.0f ? 1.0f : d.z)));
    if (pitch > 89.0f) pitch = 89.0f;
    if (pitch < -89.0f) pitch = -89.0f;
    c->pitch = pitch;
}

void camera_zoom(Camera *c, float steps) {
    c->distance *= powf(0.9f, steps);
    if (c->distance < 5.0f) c->distance = 5.0f;
    if (c->distance > 20000.0f) c->distance = 20000.0f;
}

void camera_pan(Camera *c, float dx, float dy, int viewport_h) {
    if (viewport_h <= 0) return;
    /* One pixel of drag maps to one pixel of world at the target plane. */
    float world_per_pixel = 2.0f * c->distance * tanf(deg_to_rad(OBC_FOV_DEG) * 0.5f) / (float)viewport_h;
    Vec3 right = camera_right(*c);
    Vec3 up = camera_up(*c);
    c->target = vec3_sub(c->target, vec3_mul(right, dx * world_per_pixel));
    c->target = vec3_add(c->target, vec3_mul(up, dy * world_per_pixel));
}

void camera_frame_bounds(Camera *c, const Bounds &b) {
    if (!b.valid) {
        camera_home(c);
        return;
    }
    c->target = bounds_center(b);
    Vec3 size = bounds_size(b);
    float radius = 0.5f * vec3_length(size);
    if (radius < 5.0f) radius = 5.0f;
    c->distance = radius / tanf(deg_to_rad(OBC_FOV_DEG) * 0.5f) * 1.6f;
}

Vec3 camera_eye(const Camera &c) {
    float cy = cosf(deg_to_rad(c.yaw));
    float sy = sinf(deg_to_rad(c.yaw));
    float cp = cosf(deg_to_rad(c.pitch));
    float sp = sinf(deg_to_rad(c.pitch));
    Vec3 offset = vec3(cy * cp, sy * cp, sp);
    return vec3_add(c.target, vec3_mul(offset, c.distance));
}

Vec3 camera_right(const Camera &c) {
    Vec3 forward = vec3_normalized(vec3_sub(c.target, camera_eye(c)));
    return vec3_normalized(vec3_cross(forward, vec3(0.0f, 0.0f, 1.0f)));
}

Vec3 camera_up(const Camera &c) {
    Vec3 forward = vec3_normalized(vec3_sub(c.target, camera_eye(c)));
    return vec3_normalized(vec3_cross(camera_right(c), forward));
}

/* Fixed function matrix setup, GL 1.2 safe: glFrustum / glOrtho plus a
 * hand rolled look-at, so no GLU dependency. */
void camera_apply_gl(const Camera &c, int viewport_w, int viewport_h) {
    if (viewport_w <= 0 || viewport_h <= 0) return;
    float aspect = (float)viewport_w / (float)viewport_h;

    float far_plane = c.distance * 10.0f + 1000.0f;
    float near_plane = c.distance * 0.02f;
    if (near_plane < OBC_NEAR_MIN) near_plane = OBC_NEAR_MIN;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    if (c.orthographic) {
        float half_h = c.distance * tanf(deg_to_rad(OBC_FOV_DEG) * 0.5f);
        float half_w = half_h * aspect;
        glOrtho(-half_w, half_w, -half_h, half_h, -far_plane, far_plane);
    } else {
        float top = near_plane * tanf(deg_to_rad(OBC_FOV_DEG) * 0.5f);
        float right = top * aspect;
        glFrustum(-right, right, -top, top, near_plane, far_plane);
    }

    Vec3 eye = camera_eye(c);
    Vec3 forward = vec3_normalized(vec3_sub(c.target, eye));
    Vec3 right = camera_right(c);
    Vec3 up = camera_up(c);

    float view[16] = {
        right.x, up.x, -forward.x, 0.0f,
        right.y, up.y, -forward.y, 0.0f,
        right.z, up.z, -forward.z, 0.0f,
        -vec3_dot(right, eye), -vec3_dot(up, eye), vec3_dot(forward, eye), 1.0f
    };

    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(view);
}

bool camera_project(const Camera &c, int viewport_w, int viewport_h, Vec3 world,
                    float *sx, float *sy) {
    if (viewport_w <= 0 || viewport_h <= 0) return false;

    float aspect = (float)viewport_w / (float)viewport_h;
    float tan_half = tanf(deg_to_rad(OBC_FOV_DEG) * 0.5f);

    Vec3 eye = camera_eye(c);
    Vec3 forward = vec3_normalized(vec3_sub(c.target, eye));
    Vec3 right = camera_right(c);
    Vec3 up = camera_up(c);

    Vec3 rel = vec3_sub(world, eye);
    float vx = vec3_dot(rel, right);
    float vy = vec3_dot(rel, up);
    float vz = vec3_dot(rel, forward);

    float ndc_x, ndc_y;
    if (c.orthographic) {
        float half_h = c.distance * tan_half;
        ndc_x = vx / (half_h * aspect);
        ndc_y = vy / half_h;
    } else {
        if (vz <= 1e-4f) return false; // behind the eye
        ndc_x = (vx / vz) / (tan_half * aspect);
        ndc_y = (vy / vz) / tan_half;
    }

    *sx = (ndc_x + 1.0f) * 0.5f * (float)viewport_w;
    *sy = (1.0f - ndc_y) * 0.5f * (float)viewport_h;
    return true;
}

void camera_screen_ray(const Camera &c, int viewport_w, int viewport_h,
                       float sx, float sy, Vec3 *origin, Vec3 *dir) {
    if (viewport_w <= 0 || viewport_h <= 0) {
        *origin = camera_eye(c);
        *dir = vec3_normalized(vec3_sub(c.target, *origin));
        return;
    }

    float aspect = (float)viewport_w / (float)viewport_h;
    float ndc_x = (2.0f * sx / (float)viewport_w) - 1.0f;
    float ndc_y = 1.0f - (2.0f * sy / (float)viewport_h);
    float tan_half = tanf(deg_to_rad(OBC_FOV_DEG) * 0.5f);

    Vec3 eye = camera_eye(c);
    Vec3 forward = vec3_normalized(vec3_sub(c.target, eye));
    Vec3 right = camera_right(c);
    Vec3 up = camera_up(c);

    if (c.orthographic) {
        float half_h = c.distance * tan_half;
        Vec3 offset = vec3_add(vec3_mul(right, ndc_x * half_h * aspect), vec3_mul(up, ndc_y * half_h));
        *origin = vec3_add(eye, offset);
        *dir = forward;
        return;
    }

    Vec3 d = forward;
    d = vec3_add(d, vec3_mul(right, ndc_x * tan_half * aspect));
    d = vec3_add(d, vec3_mul(up, ndc_y * tan_half));
    *origin = eye;
    *dir = vec3_normalized(d);
}
