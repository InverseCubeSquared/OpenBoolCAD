#ifndef OBC_CAMERA_H
#define OBC_CAMERA_H

#include "math3d.h"

/*
 * Orbit camera. "distance" doubles as the scene scale that scroll and
 * PgUp/PgDn drive; there is no separate zoom factor.
 */
struct Camera {
    Vec3 target;
    float yaw;      // degrees around Z
    float pitch;    // degrees above the XY plane
    float distance; // mm from target
    bool orthographic;
};

void camera_init(Camera *c);
void camera_home(Camera *c);
void camera_orbit(Camera *c, float dyaw, float dpitch);

/* Points the camera at the target from the given world direction, which is
 * what clicking a face of the orientation cube asks for. */
void camera_look_from(Camera *c, Vec3 direction);
void camera_zoom(Camera *c, float steps);
void camera_pan(Camera *c, float dx, float dy, int viewport_h);
void camera_frame_bounds(Camera *c, const Bounds &b);

Vec3 camera_eye(const Camera &c);
Vec3 camera_right(const Camera &c);
Vec3 camera_up(const Camera &c);

/* Loads projection and modelview onto the fixed function matrix stack. */
void camera_apply_gl(const Camera &c, int viewport_w, int viewport_h);

/* Screen ray for picking. Screen coords are pixels inside the viewport,
 * y measured from the top. */
void camera_screen_ray(const Camera &c, int viewport_w, int viewport_h,
                       float sx, float sy, Vec3 *origin, Vec3 *dir);

/* World -> viewport pixels, the inverse of the above. Returns false when the
 * point sits behind the camera, where the projection is meaningless. Used to
 * hit test the scale handles in screen space. */
bool camera_project(const Camera &c, int viewport_w, int viewport_h, Vec3 world,
                    float *sx, float *sy);

#endif
