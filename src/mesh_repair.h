#ifndef OBC_MESH_REPAIR_H
#define OBC_MESH_REPAIR_H

#include <stdint.h>
#include <string>
#include <vector>

#include "mesh.h"

/*
 * Turns a triangle soup into a closed, consistently wound, indexed solid.
 *
 * Every boolean operation runs through this first: Manifold rejects anything
 * that is not a closed solid, and the failure it reports ("not manifold") says
 * nothing about which defect caused it. Imported STLs are the other consumer -
 * they routinely arrive with unwelded seams, flipped faces and small holes.
 *
 * The pass is deliberately conservative. It fixes defects that have one
 * unambiguous answer and reports the rest instead of guessing.
 */

/*
 * How close two vertices have to be before this pass treats them as one.
 *
 * Far below any dimension that matters in CAD, but not below what a boolean
 * backend working in double precision can produce, which is why csg.cpp knows
 * about it too: a result carrying edges shorter than this welds shut here, and
 * the triangles those edges belonged to collapse and get dropped, tearing a
 * hole in a mesh that was closed when it arrived. The two have to agree.
 */
#define OBC_WELD_MM 1e-4f

struct IndexedMesh {
    std::vector<Vec3> positions;
    std::vector<uint32_t> tris; // 3 indices per triangle
};

struct MeshRepairReport {
    int input_triangles;
    int welded_vertices;      // duplicate positions collapsed
    int removed_degenerate;   // zero area or repeated corners
    int removed_duplicate;    // same triangle submitted twice
    int flipped_triangles;    // rewound to agree with their neighbours
    int flipped_components;   // whole shells that were inside out
    int filled_holes;         // boundary loops closed with a fan
    int open_edges;           // still bordered by one face after filling
    int nonmanifold_edges;    // shared by more than two faces, not repairable here
    bool is_solid;
};

/*
 * Returns true when the result is a closed solid. Even when it returns false
 * the output is still the best repair achieved, so a caller can choose to
 * report and continue.
 */
bool mesh_repair(const Mesh &in, IndexedMesh *out, MeshRepairReport *report);

/* Human readable summary of what changed; empty when nothing was touched. */
std::string mesh_repair_summary(const MeshRepairReport &report);

/* Back to a renderable triangle soup, with feature edges rebuilt from the
 * dihedral angle. */
Mesh mesh_from_indexed(const IndexedMesh &in);

/* Feature edge extraction, shared with the boolean result path. */
void mesh_add_feature_edges(Mesh *out, const std::vector<Vec3> &positions,
                            const std::vector<uint32_t> &tris);

#endif
