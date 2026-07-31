#ifndef OBC_POLYHEDRON_H
#define OBC_POLYHEDRON_H

#include <string>
#include <vector>

#include "mesh.h"

/*
 * N-sided solid generator.
 *
 * Two kinds of thing live here, and the difference is worth keeping straight.
 * The five Platonic solids are *named*: a regular polyhedron only exists with
 * 4, 6, 8, 12 or 20 faces, so those are presets rather than a number to type.
 * The rest are *families* built around an n-gon, where the side count is free
 * and the face count follows from it.
 *
 * Every one of them is a convex hull of a short list of vertices, so the faces
 * and their winding come from Manifold rather than from a table written out by
 * hand. That is what keeps a dodecahedron to four lines of coordinates.
 */

enum PolyhedronKind {
    POLY_TETRAHEDRON = 0,
    POLY_HEXAHEDRON,
    POLY_OCTAHEDRON,
    POLY_DODECAHEDRON,
    POLY_ICOSAHEDRON,
    POLY_PRISM,
    POLY_ANTIPRISM,
    POLY_BIPYRAMID,
    POLY_PYRAMID,
    POLY_COUNT
};

struct PolyhedronParams {
    int kind;
    int sides;        // the n-gon the families are built on; ignored by the presets
    float size_mm;    // across the widest point
    float height_mm;  // families only
    bool negative;
};

void polyhedron_params_init(PolyhedronParams *p);
void polyhedron_params_clamp(PolyhedronParams *p);

const char *polyhedron_kind_name(int kind);
/* False for the five presets, whose side count is fixed by their geometry. */
bool polyhedron_kind_takes_sides(int kind);
/* False for the presets, which are regular and so have no free height. */
bool polyhedron_kind_takes_height(int kind);

/* How many faces the current parameters produce, for the dialog to show - the
 * whole point of the tool being "n-sided". */
int polyhedron_face_count(const PolyhedronParams &p);

/* Built centred in x and y and standing on the workplane, like the fixed
 * primitives. Returns false with a reason when the numbers make no solid. */
bool polyhedron_build(const PolyhedronParams &params, Mesh *out, std::string *error);

#endif
