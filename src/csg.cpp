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

static Mesh manifold_to_mesh(const manifold::Manifold &man) {
    manifold::MeshGL64 gl = man.GetMeshGL64();

    IndexedMesh indexed;
    size_t vert_count = gl.vertProperties.size() / gl.numProp;
    indexed.positions.reserve(vert_count);
    for (size_t v = 0; v < vert_count; ++v) {
        size_t o = v * gl.numProp;
        indexed.positions.push_back(vec3((float)gl.vertProperties[o + 0],
                                         (float)gl.vertProperties[o + 1],
                                         (float)gl.vertProperties[o + 2]));
    }
    indexed.tris.reserve(gl.triVerts.size());
    for (size_t i = 0; i < gl.triVerts.size(); ++i) {
        indexed.tris.push_back((uint32_t)gl.triVerts[i]);
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

bool csg_merge(const std::vector<Mesh> &positives,
               const std::vector<Mesh> &negatives,
               Mesh *out, std::string *error, std::string *repair_note) {
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

    manifold::Manifold result = manifold::Manifold::BatchBoolean(pos, manifold::OpType::Add);
    if (!neg.empty()) {
        manifold::Manifold cut = manifold::Manifold::BatchBoolean(neg, manifold::OpType::Add);
        result = result - cut;
    }

    manifold::Manifold::Error err = result.Status();
    if (err != manifold::Manifold::Error::NoError) {
        if (error) *error = std::string("Merge failed: ") + manifold_error_text(err);
        return false;
    }

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

    err = result.Status();
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
