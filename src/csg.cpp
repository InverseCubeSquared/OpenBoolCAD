#include "csg.h"

#include "manifold/manifold.h"
#include "mesh_repair.h"

/*
 * Conversion
 *
 * Every input goes through mesh_repair first. Manifold only accepts closed
 * solids, and its own error ("not manifold") does not say which defect caused
 * it, so repairing and reporting up front is both more robust and more
 * informative than handing raw geometry over and translating the rejection.
 */

static manifold::Manifold indexed_to_manifold(const IndexedMesh &in) {
    manifold::MeshGL64 gl;
    gl.numProp = 3;
    gl.vertProperties.reserve(in.positions.size() * 3);
    for (size_t i = 0; i < in.positions.size(); ++i) {
        gl.vertProperties.push_back((double)in.positions[i].x);
        gl.vertProperties.push_back((double)in.positions[i].y);
        gl.vertProperties.push_back((double)in.positions[i].z);
    }
    gl.triVerts.reserve(in.tris.size());
    for (size_t i = 0; i < in.tris.size(); ++i) {
        gl.triVerts.push_back((uint64_t)in.tris[i]);
    }
    return manifold::Manifold(gl);
}

/*
 * Collapses anything finer than the grid mesh_repair welds on.
 *
 * Manifold works in double and its own tolerance is finer than ours, so a
 * perfectly good result can carry edges a few nanometres long. They survive the
 * trip out through float, and then the very next mesh_repair welds their
 * endpoints together, drops the triangles that just went degenerate, and hands
 * back a mesh with holes in it - a boolean result that no longer accepts a
 * boolean. Collapsing them here, while there is still a Manifold to do it
 * exactly, is what keeps one operation's output usable as the next one's input.
 *
 * A tolerance this small is three orders of magnitude under any feature anyone
 * models, so nothing visible moves. Where a coarser pass has already run - the
 * thin wall cleanup in csg_merge - this is a no-op.
 */
static manifold::Manifold settle_to_weld_grid(const manifold::Manifold &man) {
    if (man.GetTolerance() >= (double)OBC_WELD_MM) return man;
    return man.SetTolerance((double)OBC_WELD_MM).Simplify((double)OBC_WELD_MM);
}

/* Union-find over the merge vectors, which are "simply a union" per Manifold's
 * own documentation and so can chain. */
static uint32_t merge_root(std::vector<uint32_t> *parent, uint32_t v) {
    while ((*parent)[v] != v) {
        (*parent)[v] = (*parent)[(*parent)[v]];
        v = (*parent)[v];
    }
    return v;
}

static Mesh manifold_to_mesh(const manifold::Manifold &raw) {
    manifold::Manifold man = settle_to_weld_grid(raw);
    manifold::MeshGL64 gl = man.GetMeshGL64();

    size_t vert_count = gl.vertProperties.size() / gl.numProp;

    /*
     * Manifold hands back a *rendering* vertex list, not a topological one: a
     * vertex where two of its input meshes meet comes back duplicated, once per
     * run, and mergeFromVert/mergeToVert say which copies are really the same
     * point. Ignoring them leaves every seam of the boolean looking like an open
     * boundary, and mesh_add_feature_edges draws exactly those - so a bevelled
     * part came back with black outline scribbled across faces that are flat.
     * The geometry was never wrong; the topology handed to us was incomplete.
     */
    std::vector<uint32_t> parent(vert_count);
    for (size_t v = 0; v < vert_count; ++v) parent[v] = (uint32_t)v;

    size_t merges = gl.mergeFromVert.size();
    if (gl.mergeToVert.size() < merges) merges = gl.mergeToVert.size();
    for (size_t k = 0; k < merges; ++k) {
        uint32_t from = (uint32_t)gl.mergeFromVert[k];
        uint32_t to = (uint32_t)gl.mergeToVert[k];
        if (from >= vert_count || to >= vert_count) continue;
        uint32_t ra = merge_root(&parent, from);
        uint32_t rb = merge_root(&parent, to);
        if (ra != rb) parent[ra] = rb;
    }

    IndexedMesh indexed;
    std::vector<uint32_t> slot(vert_count, 0xffffffffu);
    indexed.positions.reserve(vert_count);
    indexed.tris.reserve(gl.triVerts.size());

    for (size_t i = 0; i < gl.triVerts.size(); ++i) {
        uint32_t v = (uint32_t)gl.triVerts[i];
        if (v >= vert_count) continue;
        uint32_t root = merge_root(&parent, v);
        if (slot[root] == 0xffffffffu) {
            size_t o = (size_t)root * gl.numProp;
            slot[root] = (uint32_t)indexed.positions.size();
            indexed.positions.push_back(vec3((float)gl.vertProperties[o + 0],
                                             (float)gl.vertProperties[o + 1],
                                             (float)gl.vertProperties[o + 2]));
        }
        indexed.tris.push_back(slot[root]);
    }

    return mesh_from_indexed(indexed);
}

/* Merge */

static const char *manifold_error_text(manifold::Manifold::Error err) {
    switch (err) {
    case manifold::Manifold::Error::NoError:           return "no error";
    case manifold::Manifold::Error::NonFiniteVertex:   return "mesh has a non-finite vertex";
    case manifold::Manifold::Error::NotManifold:       return "mesh is not a closed solid";
    case manifold::Manifold::Error::VertexOutOfBounds: return "vertex index out of bounds";
    case manifold::Manifold::Error::PropertiesWrongLength: return "vertex property length mismatch";
    case manifold::Manifold::Error::MergeVectorsDifferentLengths: return "merge vectors differ in length";
    case manifold::Manifold::Error::MergeIndexOutOfBounds: return "merge index out of bounds";
    case manifold::Manifold::Error::TransformWrongLength: return "transform length mismatch";
    case manifold::Manifold::Error::RunIndexWrongLength:  return "run index length mismatch";
    case manifold::Manifold::Error::FaceIDWrongLength:    return "face id length mismatch";
    case manifold::Manifold::Error::InvalidConstruction:  return "invalid construction";
    default: return "unknown error";
    }
}

/* Accumulates the repair counts of every input into one report. */
static void accumulate(MeshRepairReport *total, const MeshRepairReport &one) {
    total->input_triangles += one.input_triangles;
    total->welded_vertices += one.welded_vertices;
    total->removed_degenerate += one.removed_degenerate;
    total->removed_duplicate += one.removed_duplicate;
    total->flipped_triangles += one.flipped_triangles;
    total->flipped_components += one.flipped_components;
    total->filled_holes += one.filled_holes;
    total->open_edges += one.open_edges;
    total->nonmanifold_edges += one.nonmanifold_edges;
}

static bool collect(const std::vector<Mesh> &meshes, std::vector<manifold::Manifold> *out,
                    MeshRepairReport *repairs, std::string *error) {
    for (size_t i = 0; i < meshes.size(); ++i) {
        if (meshes[i].vertices.empty()) continue;

        IndexedMesh indexed;
        MeshRepairReport report;
        bool solid = mesh_repair(meshes[i], &indexed, &report);
        accumulate(repairs, report);

        if (!solid) {
            if (error) {
                *error = "Merge failed: input is not a solid (" +
                         mesh_repair_summary(report) + ").";
            }
            return false;
        }

        manifold::Manifold m = indexed_to_manifold(indexed);
        manifold::Manifold::Error err = m.Status();
        if (err != manifold::Manifold::Error::NoError) {
            if (error) *error = std::string("Merge failed: ") + manifold_error_text(err);
            return false;
        }
        out->push_back(m);
    }
    return true;
}

/*
 * The union-then-subtract itself, shared by the merge and the plain boolean.
 * Fills "result" and returns false with a reason when an input or the operation
 * did not come out a solid.
 */
static bool combine(const std::vector<Mesh> &positives,
                    const std::vector<Mesh> &negatives,
                    manifold::Manifold *result, std::string *error,
                    std::string *repair_note) {
    if (positives.empty()) {
        if (error) *error = "Merge needs at least one positive volume.";
        return false;
    }

    MeshRepairReport repairs;
    memset(&repairs, 0, sizeof(repairs));

    std::vector<manifold::Manifold> pos, neg;
    bool ok = collect(positives, &pos, &repairs, error) &&
              collect(negatives, &neg, &repairs, error);
    if (repair_note) *repair_note = mesh_repair_summary(repairs);
    if (!ok) return false;

    if (pos.empty()) {
        if (error) *error = "Merge needs at least one positive volume.";
        return false;
    }

    manifold::Manifold r = manifold::Manifold::BatchBoolean(pos, manifold::OpType::Add);
    if (!neg.empty()) {
        manifold::Manifold cut = manifold::Manifold::BatchBoolean(neg, manifold::OpType::Add);
        r = r - cut;
    }

    manifold::Manifold::Error err = r.Status();
    if (err != manifold::Manifold::Error::NoError) {
        if (error) *error = std::string("Merge failed: ") + manifold_error_text(err);
        return false;
    }

    *result = r;
    return true;
}

bool csg_boolean(const std::vector<Mesh> &positives,
                 const std::vector<Mesh> &negatives,
                 Mesh *out, std::string *error) {
    manifold::Manifold result;
    if (!combine(positives, negatives, &result, error, NULL)) return false;

    if (result.IsEmpty()) {
        if (error) *error = "The operation produced an empty volume.";
        return false;
    }

    *out = manifold_to_mesh(result);
    return true;
}

bool csg_merge(const std::vector<Mesh> &positives,
               const std::vector<Mesh> &negatives,
               Mesh *out, std::string *error, std::string *repair_note) {
    manifold::Manifold result;
    if (!combine(positives, negatives, &result, error, repair_note)) return false;

    /*
     * Drop leftover walls thinner than OBC_MIN_WALL_MM.
     *
     * Manifold's tolerance is what does the work: raising it collapses the
     * short edges that span a wall that thin, which zips the wall shut and
     * leaves nothing behind. A wall thicker than the tolerance keeps every one
     * of its triangles, so this is not a decimation pass - measured on a
     * faceted sphere it changed neither the triangle count nor the volume.
     *
     * The alternative, a morphological opening through MinkowskiDifference and
     * MinkowskiSum, is geometrically the more honest answer and gives the same
     * result here, but it ran three orders of magnitude slower on a mesh of a
     * couple of thousand triangles. Not worth it for debris cleanup.
     */
    double volume_before = result.Volume();
    bool had_volume = !result.IsEmpty();

    result = result.SetTolerance(OBC_MIN_WALL_MM).Simplify(OBC_MIN_WALL_MM);

    manifold::Manifold::Error err = result.Status();
    if (err != manifold::Manifold::Error::NoError) {
        if (error) *error = std::string("Thin wall cleanup failed: ") + manifold_error_text(err);
        return false;
    }

    if (result.IsEmpty()) {
        if (error) {
            /* Worth telling apart: a merge that cancelled out is a modelling
             * mistake, one that was all slivers is a near miss. */
            *error = had_volume
                ? "Merge left nothing but walls thinner than 0.01 mm."
                : "Merge produced an empty volume.";
        }
        return false;
    }

    if (repair_note) {
        double removed = volume_before - result.Volume();
        if (removed > 1e-7) {
            char note[96];
            snprintf(note, sizeof(note), "removed %.4g mm3 of walls under %g mm",
                     removed, OBC_MIN_WALL_MM);
            if (!repair_note->empty()) *repair_note += ", ";
            *repair_note += note;
        }
    }

    *out = manifold_to_mesh(result);
    return true;
}

/* Extrusion */

static float contour_area(const std::vector<Vec2> &c) {
    float sum = 0.0f;
    for (size_t i = 0; i < c.size(); ++i) {
        const Vec2 &a = c[i];
        const Vec2 &b = c[(i + 1) % c.size()];
        sum += a.x * b.y - b.x * a.y;
    }
    return sum * 0.5f;
}

static bool point_in_contour(const std::vector<Vec2> &c, Vec2 p) {
    bool inside = false;
    for (size_t i = 0, j = c.size() - 1; i < c.size(); j = i++) {
        bool crosses = (c[i].y > p.y) != (c[j].y > p.y);
        if (!crosses) continue;
        float x = (c[j].x - c[i].x) * (p.y - c[i].y) / (c[j].y - c[i].y) + c[i].x;
        if (p.x < x) inside = !inside;
    }
    return inside;
}

bool csg_extrude(const std::vector<std::vector<Vec2> > &contours, float height,
                 Mesh *out, std::string *error) {
    if (contours.empty()) {
        if (error) *error = "Nothing to extrude.";
        return false;
    }
    if (height <= 0.0f) {
        if (error) *error = "Extrusion height must be positive.";
        return false;
    }

    manifold::Polygons polys;
    for (size_t i = 0; i < contours.size(); ++i) {
        const std::vector<Vec2> &c = contours[i];
        if (c.size() < 3) continue;

        /* Nesting parity decides island versus hole: a contour inside an odd
         * number of others is a hole. Manifold wants islands counter clockwise
         * and holes clockwise. */
        int depth = 0;
        for (size_t k = 0; k < contours.size(); ++k) {
            if (k == i || contours[k].size() < 3) continue;
            if (point_in_contour(contours[k], c[0])) depth += 1;
        }
        bool is_hole = (depth % 2) == 1;
        bool ccw = contour_area(c) > 0.0f;
        bool reverse = is_hole ? ccw : !ccw;

        manifold::SimplePolygon poly;
        poly.reserve(c.size());
        for (size_t k = 0; k < c.size(); ++k) {
            const Vec2 &v = reverse ? c[c.size() - 1 - k] : c[k];
            poly.push_back(manifold::vec2((double)v.x, (double)v.y));
        }
        polys.push_back(poly);
    }

    if (polys.empty()) {
        if (error) *error = "No contour had enough points to extrude.";
        return false;
    }

    manifold::Manifold solid = manifold::Manifold::Extrude(polys, (double)height);
    manifold::Manifold::Error err = solid.Status();
    if (err != manifold::Manifold::Error::NoError) {
        if (error) *error = std::string("Extrude failed: ") + manifold_error_text(err);
        return false;
    }
    if (solid.IsEmpty()) {
        if (error) *error = "Extrusion produced an empty volume.";
        return false;
    }

    *out = manifold_to_mesh(solid);
    return true;
}

bool csg_settle(Mesh *mesh, std::string *error) {
    IndexedMesh indexed;
    MeshRepairReport report;
    if (!mesh_repair(*mesh, &indexed, &report)) {
        if (error) *error = "Not a solid after the operation (" +
                            mesh_repair_summary(report) + ").";
        return false;
    }

    manifold::Manifold m = indexed_to_manifold(indexed);
    manifold::Manifold::Error err = m.Status();
    if (err != manifold::Manifold::Error::NoError) {
        if (error) *error = std::string("Cleanup failed: ") + manifold_error_text(err);
        return false;
    }
    if (m.IsEmpty()) {
        if (error) *error = "Cleanup left nothing.";
        return false;
    }

    *mesh = manifold_to_mesh(m);
    return true;
}

bool csg_hull(const std::vector<Vec3> &points, Mesh *out, std::string *error) {
    if (points.size() < 4) {
        if (error) *error = "A solid needs at least four points.";
        return false;
    }

    std::vector<manifold::vec3> pts;
    pts.reserve(points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        pts.push_back(manifold::vec3((double)points[i].x, (double)points[i].y,
                                     (double)points[i].z));
    }

    manifold::Manifold solid = manifold::Manifold::Hull(pts);
    manifold::Manifold::Error err = solid.Status();
    if (err != manifold::Manifold::Error::NoError) {
        if (error) *error = std::string("Hull failed: ") + manifold_error_text(err);
        return false;
    }
    if (solid.IsEmpty()) {
        /* Every point on one plane has no volume to hull. */
        if (error) *error = "Those points are flat, so they enclose nothing.";
        return false;
    }

    *out = manifold_to_mesh(solid);
    return true;
}

bool csg_make_solid(Mesh *mesh, std::string *note) {
    IndexedMesh indexed;
    MeshRepairReport report;
    bool solid = mesh_repair(*mesh, &indexed, &report);
    if (note) *note = mesh_repair_summary(report);
    *mesh = mesh_from_indexed(indexed);
    return solid;
}
