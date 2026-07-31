#ifndef OBC_MESH_H
#define OBC_MESH_H

#include <vector>
#include "math3d.h"

/*
 * Triangle soup with flat normals. Vertices are stored three per triangle so
 * every face can carry its own normal without an index buffer; the boolean
 * stage later needs to split and re-stitch faces, which indexed meshes make
 * awkward.
 *
 * "edges" holds the feature edges (sharp silhouette candidates) as vertex
 * pairs. Those are what gets stroked as the thin black outline.
 */
struct Mesh {
    std::vector<Vec3> vertices;
    std::vector<Vec3> normals;
    std::vector<Vec3> edges;
};

enum PrimitiveKind {
    PRIM_CUBE = 0,
    PRIM_CYLINDER,
    PRIM_SPHERE,
    PRIM_CONE,
    PRIM_PYRAMID,
    PRIM_WEDGE,
    PRIM_COUNT
};

/* Default footprint for every primitive, in millimeters. */
#define OBC_PRIMITIVE_SIZE 20.0f

/*
 * How finely the round primitives are tessellated. Only they have a choice to
 * make; the flat faced ones ignore it.
 *
 * It matters beyond looks: segment count is what a boolean has to chew on, and
 * a sphere at 128 x 64 costs a merge far more than one at 16 x 8. Chosen per
 * placement rather than globally, so a rough draft and a finished part can sit
 * in the same scene.
 */
struct PrimitiveResolution {
    int cylinder_segments;
    int cone_segments;
    int sphere_segments;
    int sphere_rings;
};

void primitive_resolution_init(PrimitiveResolution *r);
void primitive_resolution_clamp(PrimitiveResolution *r);

/* True for the primitives whose resolution can be chosen at all. */
bool primitive_has_resolution(PrimitiveKind kind);

/* Triangle count the current settings would produce, for the menu to show. */
int primitive_triangle_count(PrimitiveKind kind, const PrimitiveResolution &r);

void mesh_clear(Mesh *m);
void mesh_add_triangle(Mesh *m, Vec3 a, Vec3 b, Vec3 c);
void mesh_add_edge(Mesh *m, Vec3 a, Vec3 b);
Bounds mesh_bounds(const Mesh &m);

/*
 * Builders. Every primitive is centered on the origin in X/Y and sits on the
 * workplane (z = 0 .. size), matching how a part is placed on a print bed.
 */
Mesh mesh_make_cube(float size);
Mesh mesh_make_cylinder(float diameter, float height, int segments);
Mesh mesh_make_sphere(float diameter, int segments, int rings);
Mesh mesh_make_cone(float diameter, float height, int segments);
Mesh mesh_make_pyramid(float size, float height);
/* Ramp: square footprint, full height along -x, falling to zero at +x. */
Mesh mesh_make_wedge(float size, float height);

Mesh mesh_make_primitive(PrimitiveKind kind);
Mesh mesh_make_primitive_at(PrimitiveKind kind, const PrimitiveResolution &r);
const char *primitive_name(PrimitiveKind kind);

#endif
