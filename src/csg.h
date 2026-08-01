#ifndef OBC_CSG_H
#define OBC_CSG_H

#include <string.h>
#include <string>
#include <vector>
#include "mesh.h"

/*
 * Boolean volume operations, backed by Manifold (third_party/manifold).
 *
 * Manifold's types stay inside csg.cpp on purpose: the rest of the codebase
 * only ever sees Mesh, so the backend can be swapped without touching the
 * scene or the renderer.
 *
 * Input meshes must be in world space. Our meshes are triangle soups with
 * duplicated vertices, which is not a manifold representation, so vertices are
 * welded on the way in and feature edges are rebuilt on the way out.
 */

/*
 * Walls left thinner than this are dropped by the merge.
 *
 * A cut that lands almost flush with a surface leaves a paper-thin shell: not
 * something anyone modelled, and at this scale not something any process could
 * make either. It survives as a sliver that upsets later booleans and shows up
 * in an exported STL as self-intersecting junk, so the merge removes it rather
 * than passing it on.
 */
#define OBC_MIN_WALL_MM 0.01

/*
 * The merge order from PLAN.md: union the positives, union the negatives, then
 * cut the negative volume out of the positive one.
 *
 * Every input runs through mesh_repair first (see mesh_repair.h), so seams,
 * flipped faces and small holes are corrected rather than rejected. Returns
 * false and fills "error" when an input still is not a solid. "repair_note", if
 * given, receives a summary of what the repair pass changed.
 */
bool csg_merge(const std::vector<Mesh> &positives,
               const std::vector<Mesh> &negatives,
               Mesh *out, std::string *error, std::string *repair_note);

/*
 * The same union-then-subtract as csg_merge, without the thin wall pass.
 *
 * For an operation whose own detail is finer than OBC_MIN_WALL_MM. A fillet is
 * the case that matters: at a 0.4 mm radius and six segments the facet chords
 * are about 14 microns, against a 10 micron wall tolerance, so the cleanup eats
 * the arc and hands back something closer to a chamfer than the bevel that was
 * asked for. Debris removal belongs to the merge the user asked for, not to
 * every boolean the editor runs on its own account.
 */
bool csg_boolean(const std::vector<Mesh> &positives,
                 const std::vector<Mesh> &negatives,
                 Mesh *out, std::string *error);

/* Repairs a mesh in place, returning true when it came out a closed solid. */
bool csg_make_solid(Mesh *mesh, std::string *note);

/*
 * Repairs a mesh and passes it once through Manifold, which rebuilds its
 * topology: the tidy-up to run after an operation that leaves slivers or near
 * coincident faces behind, such as a bevel whose cutters overlapped.
 *
 * Deliberately *not* csg_merge with one input. That would re-run the thin wall
 * tolerance, and applying it twice compounds: measured on a 2 mm fillet at 16
 * segments, the facet chords are about 0.0024 mm, so a second pass collapses
 * the arc and turns the round back into something closer to a chamfer. One
 * pass leaves real curvature alone; two do not.
 */
bool csg_settle(Mesh *mesh, std::string *error);

/*
 * Extrudes closed 2D contours (in mm, on the workplane) upward into a solid.
 *
 * Contour orientation is normalised here: nesting depth decides which contours
 * are islands and which are holes, so callers can hand over SVG subpaths in
 * whatever winding the artwork happened to use. The triangulation comes from
 * Manifold, so the result is a closed solid or an error.
 */
bool csg_extrude(const std::vector<std::vector<Vec2> > &contours, float height,
                 Mesh *out, std::string *error);

/*
 * Convex hull of a point cloud.
 *
 * This is what builds the polyhedra: the vertices of a Platonic solid or of a
 * prism are a couple of lines of coordinates, and the hull works out the faces.
 * Writing face tables by hand instead would mean twelve pentagons for the
 * dodecahedron alone, every one of them a chance to get a winding wrong.
 */
bool csg_hull(const std::vector<Vec3> &points, Mesh *out, std::string *error);

#endif
