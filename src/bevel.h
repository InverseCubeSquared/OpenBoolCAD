#ifndef OBC_BEVEL_H
#define OBC_BEVEL_H

#include <string>
#include <vector>

#include "mesh.h"

/*
 * Edge bevelling: rounds or chamfers chosen edges of a solid.
 *
 * Manifold has no bevel, so this is CSG. For each edge a cutter is built whose
 * cross section is exactly the material the bevel takes away - the corner, the
 * two points where the arc meets the faces, and the arc between them - swept
 * along the edge. Subtracting the union of those cutters is the bevel.
 *
 * A concave edge works the other way round: the same cross section sits outside
 * the material, and adding it fills the notch. Which case an edge is in is
 * decided from the two faces that meet at it, not asked of the user.
 *
 * A bevellable edge is a whole *chain*, not one triangle edge. A cylinder rim
 * arrives as one ring of thirty-two segments, and a face an earlier boolean cut
 * in two arrives as one straight run; each becomes a single swept cutter with
 * mitred joints. That is the difference between a bevel that survives the next
 * boolean and one that does not. One cutter per triangle edge leaves each
 * neighbour overlapping the last in a sliver a few microns thick, and the union
 * of a hundred of those is a mesh no repair pass can put back together.
 */

struct BevelEdge {
    /* The polyline the edge runs along, in the mesh's own space: two points for
     * a plain straight edge, more where it follows a rim. */
    std::vector<Vec3> points;
    /*
     * The two surfaces that meet along it, one entry per *segment* rather than
     * per point - a segment lies in two definite faces, a point between two
     * segments lies in four. Keeping the true pair per segment is what lets the
     * sweep round each joint instead of averaging across it, which pinched the
     * cutter and left a peak of material standing at every corner.
     */
    std::vector<Vec3> normal_a;
    std::vector<Vec3> normal_b;
    bool closed;    // a ring: the last point joins the first
    bool convex;    // false where the bevel has to add rather than remove
};

/*
 * The bevellable edges of a mesh, as chains.
 *
 * An edge is a crease of about twenty degrees or more - the same threshold the
 * black outline is stroked with - shared by exactly two faces. A shallower
 * crease is the tessellation of a curved surface: bevelling one facet seam of a
 * cylinder is not something anyone means, and it cannot work either, since the
 * cutter comes out wider than the facet. An edge with one face is the boundary
 * of an open shell and one with more is non-manifold; neither has a dihedral
 * angle to bevel.
 */
void bevel_collect_edges(const Mesh &mesh, std::vector<BevelEdge> *out);

/* Largest radius the given edges can take before the cutter eats past the
 * faces it sits between. Used to clamp the dialog rather than fail late;
 * bevel_apply clamps its own copy too, so no caller can skip it. */
float bevel_max_radius(const Mesh &mesh, const std::vector<BevelEdge> &edges,
                       const std::vector<int> &chosen);

/*
 * Applies the bevel. "chosen" indexes into the edge list from
 * bevel_collect_edges. Segments is how many facets the arc gets; one gives a
 * chamfer, which is why the dialog has no separate chamfer switch.
 *
 * Returns false and leaves "out" alone when the result would not be a closed
 * solid, so a bevel that went wrong cannot reach the scene: a part that looks
 * bevelled but refuses every later boolean is worse than one that never changed.
 */
bool bevel_apply(const Mesh &mesh, const std::vector<BevelEdge> &edges,
                 const std::vector<int> &chosen, float radius, int segments,
                 Mesh *out, std::string *error);

#endif
