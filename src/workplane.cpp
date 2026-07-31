#include "workplane.h"

Workplane workplane_identity(void) {
    Workplane w;
    w.origin = vec3(0.0f, 0.0f, 0.0f);
    w.basis = mat3_identity();
    return w;
}

Workplane workplane_from_normal(Vec3 origin, Vec3 normal) {
    Workplane w;
    w.origin = origin;

    Vec3 z = vec3_normalized(normal);
    if (vec3_length(z) < 0.5f) return workplane_identity(); // degenerate normal

    /* Any reference that is not parallel to the normal will do; switching at
     * 0.9 keeps the cross product well conditioned near the poles. */
    Vec3 reference = (fabsf(z.z) > 0.9f) ? vec3(1.0f, 0.0f, 0.0f) : vec3(0.0f, 0.0f, 1.0f);
    Vec3 x = vec3_normalized(vec3_cross(reference, z));
    Vec3 y = vec3_cross(z, x);

    /* Columns are the plane axes in world space. */
    w.basis.m[0] = x.x; w.basis.m[1] = y.x; w.basis.m[2] = z.x;
    w.basis.m[3] = x.y; w.basis.m[4] = y.y; w.basis.m[5] = z.y;
    w.basis.m[6] = x.z; w.basis.m[7] = y.z; w.basis.m[8] = z.z;
    return w;
}

Vec3 workplane_to_world(const Workplane &w, Vec3 local) {
    return mat3_apply(w.basis, local);
}

Vec3 workplane_from_world(const Workplane &w, Vec3 world) {
    return mat3_apply(mat3_transposed(w.basis), world);
}

Vec3 workplane_axis(const Workplane &w, int axis) {
    if (axis < 0) axis = 0;
    if (axis > 2) axis = 2;
    return mat3_column(w.basis, axis);
}

const char *workplane_mode_name(int mode) {
    switch (mode) {
    case WORKPLANE_OBJECT: return "Object";
    case WORKPLANE_FACE:   return "Face";
    default:               return "Workplate";
    }
}
