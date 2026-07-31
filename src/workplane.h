#ifndef OBC_WORKPLANE_H
#define OBC_WORKPLANE_H

#include "math3d.h"

/*
 * The editing plane, per PLAN.md: moves, the selection outline and the scale
 * grips are all expressed relative to it rather than to the world axes.
 *
 * Three modes, cycled with P and SHIFT+P:
 *
 *   WORLD   the workplate. Identity basis, so everything behaves as it did
 *           before workplanes existed.
 *   OBJECT  the first selected object's own orientation, recomputed from the
 *           selection every frame rather than snapshotted - PLAN.md's "a
 *           rotated object will store its plane as a rotational value" is the
 *           node's rotation, so there is nothing separate to store.
 *   FACE    pinned by SHIFT+P and then clicking a face. This one *is* stored,
 *           since the face that defined it may later move or be deselected.
 *
 * The frame is a pure rotation of world space. "origin" says where to draw the
 * plane and where a face-pinned plane sits; it deliberately takes no part in
 * the coordinate transform, so a point's plane coordinates do not shift when
 * the plane slides along itself, and box extents stay comparable between
 * modes.
 */

enum WorkplaneMode {
    WORKPLANE_WORLD = 0,
    WORKPLANE_OBJECT,
    WORKPLANE_FACE
};

struct Workplane {
    Vec3 origin;
    Mat3 basis; // plane axes -> world; orthonormal, so the inverse is the transpose
};

Workplane workplane_identity(void);

/* Builds a frame whose Z is the given normal. The other two axes are free, so
 * they are picked to be stable rather than to mean anything. */
Workplane workplane_from_normal(Vec3 origin, Vec3 normal);

/* Rotation only, see the note above about origin. */
Vec3 workplane_to_world(const Workplane &w, Vec3 local);
Vec3 workplane_from_world(const Workplane &w, Vec3 world);

/* World direction of plane axis 0/1/2. Axis 2 is the plane normal. */
Vec3 workplane_axis(const Workplane &w, int axis);

const char *workplane_mode_name(int mode);

#endif
