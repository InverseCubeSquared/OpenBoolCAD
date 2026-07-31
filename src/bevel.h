#ifndef OBC_BEVEL_H
#define OBC_BEVEL_H

#include <string>
#include <vector>

#include "mesh.h"

/*
 * Edge bevelling: rounds or chamfers chosen edges of a solid.
 *
 * Manifold has no bevel, so this is done with CSG. For each edge a cutter is
 * built whose cross section is exactly the material the bevel takes away - the
 * corner, the two tangent points, and the arc between them - extruded along the
 * edge. Subtracting the union of those cutters is the bevel.
 *
 * A concave edge works the other way round: the same cross section sits outside
 * the material, and adding it fills the notch. Which case an edge is in is
 * decided from the two faces that meet at it, not asked of the user.
 *
 * The cutters are handed to csg_merge, so the result is a closed solid or a
 * reported error, and edges that meet at a vertex resolve against each other
 * because their cutters are unioned before the subtraction.
 */

struct BevelEdge {
    Vec3 a, b;      // world space endpoints
    Vec3 normal_a;  // the two faces that meet here
    Vec3 normal_b;
    bool convex;    // false where the bevel has to add rather than remove
};

/*
 * The bevellable edges of a mesh: the feature edges the outline is stroked
 * from, deduplicated, and only those with exactly two adjacent faces. An edge
 * with one face is a boundary of an open shell and an edge with more is
 * non-manifold; neither has a dihedral angle to bevel.
 */
void bevel_collect_edges(const Mesh &mesh, std::vector<BevelEdge> *out);

/* Largest radius the given edges can take before the cutter eats past the
 * faces it sits between. Used to clamp the dialog rather than fail late. */
float bevel_max_radius(const Mesh &mesh, const std::vector<BevelEdge> &edges,
                       const std::vector<int> &chosen);

/*
 * Applies the bevel. "chosen" indexes into the edge list from
 * bevel_collect_edges. Segments is how many facets the arc gets; one gives a
 * chamfer, which is why the dialog has no separate chamfer switch.
 */
bool bevel_apply(const Mesh &mesh, const std::vector<BevelEdge> &edges,
                 const std::vector<int> &chosen, float radius, int segments,
                 Mesh *out, std::string *error);

#endif
