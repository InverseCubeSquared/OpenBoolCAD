#include "bevel.h"

#include <map>
#include <math.h>

#include "csg.h"

/* Vertices are matched on a grid, the same way the repair pass welds them: a
 * triangle soup carries no shared vertices, so two corners of adjacent faces
 * are only ever nearly equal. */
#define WELD_GRID 1e-4f

struct VertexKey {
    long x, y, z;
    bool operator<(const VertexKey &o) const {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        return z < o.z;
    }
};

static VertexKey key_of(Vec3 p) {
    VertexKey k;
    k.x = (long)floorf(p.x / WELD_GRID + 0.5f);
    k.y = (long)floorf(p.y / WELD_GRID + 0.5f);
    k.z = (long)floorf(p.z / WELD_GRID + 0.5f);
    return k;
}

struct EdgeKey {
    VertexKey a, b;
    bool operator<(const EdgeKey &o) const {
        if (a < o.a) return true;
        if (o.a < a) return false;
        return b < o.b;
    }
};

static EdgeKey edge_key(Vec3 p, Vec3 q) {
    VertexKey ka = key_of(p);
    VertexKey kb = key_of(q);
    EdgeKey e;
    /* Ordered, so the same edge from either triangle hashes the same. */
    if (kb < ka) { e.a = kb; e.b = ka; }
    else { e.a = ka; e.b = kb; }
    return e;
}

struct EdgeFaces {
    Vec3 a, b;
    Vec3 normal[2];
    Vec3 opposite[2]; // the vertex of each face that is not on the edge
    int count;
};

void bevel_collect_edges(const Mesh &mesh, std::vector<BevelEdge> *out) {
    out->clear();

    /* Every triangle contributes its three edges, so an interior edge is seen
     * exactly twice and its two faces land together. */
    std::map<EdgeKey, EdgeFaces> edges;
    for (size_t t = 0; t + 2 < mesh.vertices.size(); t += 3) {
        Vec3 v[3] = { mesh.vertices[t], mesh.vertices[t + 1], mesh.vertices[t + 2] };
        Vec3 n = vec3_normalized(vec3_cross(vec3_sub(v[1], v[0]), vec3_sub(v[2], v[0])));
        if (vec3_length(n) < 0.5f) continue; // degenerate

        for (int i = 0; i < 3; ++i) {
            int j = (i + 1) % 3;
            int k = (i + 2) % 3;
            EdgeKey key = edge_key(v[i], v[j]);

            std::map<EdgeKey, EdgeFaces>::iterator it = edges.find(key);
            if (it == edges.end()) {
                EdgeFaces f;
                f.a = v[i];
                f.b = v[j];
                f.normal[0] = n;
                f.opposite[0] = v[k];
                f.count = 1;
                edges[key] = f;
            } else if (it->second.count == 1) {
                it->second.normal[1] = n;
                it->second.opposite[1] = v[k];
                it->second.count = 2;
            } else {
                it->second.count += 1; // non-manifold, dropped below
            }
        }
    }

    for (std::map<EdgeKey, EdgeFaces>::const_iterator it = edges.begin();
         it != edges.end(); ++it) {
        const EdgeFaces &f = it->second;
        if (f.count != 2) continue;

        /* Flat across the edge means there is nothing to bevel. The same
         * threshold the outline uses to decide a feature edge would do; this
         * one only has to reject faces that are genuinely coplanar. */
        float d = vec3_dot(f.normal[0], f.normal[1]);
        if (d > 0.9995f) continue;

        BevelEdge e;
        e.a = f.a;
        e.b = f.b;
        e.normal_a = f.normal[0];
        e.normal_b = f.normal[1];
        /*
         * Convex when each face's far vertex lies behind the other face's
         * plane: that is what "the material is on the inside of both" means,
         * and it decides whether the bevel removes or fills.
         */
        e.convex = vec3_dot(f.normal[1], vec3_sub(f.opposite[0], f.a)) < 0.0f;
        out->push_back(e);
    }
}

/* Geometry of one cutter */

/*
 * How far the fillet centre sits from the edge, along the bisector.
 *
 * The fillet touches both faces, so its centre is r from each; with a the
 * angle between the outward normals, that distance is r / cos(a/2). At a right
 * angle it is r * sqrt(2), which is the familiar answer for a cube edge.
 */
static float centre_offset(float radius, float normal_dot) {
    if (normal_dot > 1.0f) normal_dot = 1.0f;
    if (normal_dot < -1.0f) normal_dot = -1.0f;
    float half = 0.5f * acosf(normal_dot);
    float c = cosf(half);
    if (c < 1e-3f) return radius * 1000.0f; // faces nearly back to back
    return radius / c;
}

float bevel_max_radius(const Mesh &mesh, const std::vector<BevelEdge> &edges,
                       const std::vector<int> &chosen) {
    /* The cutter must not reach past the faces it sits between, so the limit is
     * set by the shortest edge and by the overall size of the part. */
    Bounds b = mesh_bounds(mesh);
    Vec3 size = bounds_size(b);
    float smallest = size.x;
    if (size.y < smallest) smallest = size.y;
    if (size.z < smallest) smallest = size.z;
    float limit = smallest * 0.45f;

    for (size_t i = 0; i < chosen.size(); ++i) {
        int index = chosen[i];
        if (index < 0 || index >= (int)edges.size()) continue;
        float length = vec3_length(vec3_sub(edges[index].b, edges[index].a));
        if (length * 0.45f < limit) limit = length * 0.45f;
    }
    if (limit < 1e-3f) limit = 1e-3f;
    return limit;
}

/*
 * The cutter for one edge: the cross section of the material the bevel takes
 * away, extruded along the edge.
 *
 * In the plane across the edge that cross section is the corner point, the two
 * points where the arc meets the faces, and the arc between them. Extruding it
 * needs no boolean of its own, which keeps one bevel to one merge however many
 * edges were chosen.
 */
static void build_cutter(const BevelEdge &edge, float radius, int segments, Mesh *out) {
    Vec3 dir = vec3_normalized(vec3_sub(edge.b, edge.a));
    if (vec3_length(dir) < 0.5f) return;

    /* Outward for a convex edge, into the notch for a concave one; flipping it
     * is the whole difference between cutting a corner and filling one. */
    Vec3 bisector = vec3_normalized(vec3_add(edge.normal_a, edge.normal_b));
    if (vec3_length(bisector) < 0.5f) return; // faces back to back
    if (!edge.convex) bisector = vec3_mul(bisector, -1.0f);

    float nd = vec3_dot(edge.normal_a, edge.normal_b);
    float offset = centre_offset(radius, nd);

    /* Centre of the arc, on the material side for a convex edge. */
    Vec3 centre = vec3_add(edge.a, vec3_mul(bisector, edge.convex ? -offset : offset));

    /* Where the arc meets each face. */
    Vec3 sign = edge.convex ? vec3(1.0f, 1.0f, 1.0f) : vec3(-1.0f, -1.0f, -1.0f);
    Vec3 t0 = vec3_add(centre, vec3_mul(vec3_scaled(edge.normal_a, sign), radius));
    Vec3 t1 = vec3_add(centre, vec3_mul(vec3_scaled(edge.normal_b, sign), radius));

    /* Cross section, in order round the shape: the corner, out along one face,
     * round the arc, back along the other. */
    std::vector<Vec3> section;
    section.push_back(edge.a);
    section.push_back(t0);

    if (segments > 1) {
        Vec3 r0 = vec3_sub(t0, centre);
        Vec3 r1 = vec3_sub(t1, centre);
        float sweep = acosf(vec3_dot(vec3_normalized(r0), vec3_normalized(r1)));
        Vec3 axis = vec3_normalized(vec3_cross(r0, r1));
        if (vec3_length(axis) > 0.5f) {
            for (int s = 1; s < segments; ++s) {
                float t = sweep * (float)s / (float)segments;
                Mat3 rot = mat3_axis_angle(axis, rad_to_deg(t));
                section.push_back(vec3_add(centre, mat3_apply(rot, r0)));
            }
        }
    }
    section.push_back(t1);

    /*
     * Extended a hair past both ends so the cut does not leave a film of
     * material where the cutter face lands exactly on the solid's, which is the
     * coplanar case booleans are worst at.
     */
    float pad = radius * 0.01f + 1e-3f;
    Vec3 start = vec3_sub(edge.a, vec3_mul(dir, pad));
    Vec3 end = vec3_add(edge.b, vec3_mul(dir, pad));
    Vec3 along = vec3_sub(end, start);
    Vec3 base = vec3_sub(start, edge.a);

    size_t n = section.size();
    std::vector<Vec3> lo(n), hi(n);
    for (size_t i = 0; i < n; ++i) {
        lo[i] = vec3_add(section[i], base);
        hi[i] = vec3_add(lo[i], along);
    }

    /* Winding is not worth agonising over: mesh_repair rewinds a shell that
     * comes out inside-out before the boolean sees it. */
    for (size_t i = 1; i + 1 < n; ++i) {
        mesh_add_triangle(out, lo[0], lo[i], lo[i + 1]);
        mesh_add_triangle(out, hi[0], hi[i + 1], hi[i]);
    }
    for (size_t i = 0; i < n; ++i) {
        size_t j = (i + 1) % n;
        mesh_add_triangle(out, lo[i], hi[i], hi[j]);
        mesh_add_triangle(out, lo[i], hi[j], lo[j]);
    }
}

bool bevel_apply(const Mesh &mesh, const std::vector<BevelEdge> &edges,
                 const std::vector<int> &chosen, float radius, int segments,
                 Mesh *out, std::string *error) {
    if (chosen.empty()) {
        if (error) *error = "Select at least one edge to bevel.";
        return false;
    }
    if (radius <= 1e-4f) {
        if (error) *error = "Bevel amount must be greater than zero.";
        return false;
    }
    if (segments < 1) segments = 1;
    if (segments > 64) segments = 64;

    /*
     * Convex edges cut away and concave ones fill in, so they cannot go through
     * the same side of the merge. Both lists are unioned before they meet the
     * solid, which is what lets two bevelled edges sharing a vertex resolve
     * against each other instead of fighting.
     */
    std::vector<Mesh> positives, negatives;
    positives.push_back(mesh);

    for (size_t i = 0; i < chosen.size(); ++i) {
        int index = chosen[i];
        if (index < 0 || index >= (int)edges.size()) continue;

        Mesh cutter;
        build_cutter(edges[index], radius, segments, &cutter);
        if (cutter.vertices.empty()) continue;

        if (edges[index].convex) negatives.push_back(cutter);
        else positives.push_back(cutter);
    }

    if (negatives.empty() && positives.size() == 1) {
        if (error) *error = "None of those edges could be bevelled.";
        return false;
    }

    std::string note;
    Mesh cut;
    if (!csg_merge(positives, negatives, &cut, error, &note)) return false;

    /*
     * A fix pass over the result. Cutters that met at a vertex leave slivers
     * and near coincident faces behind, which are what makes the *next*
     * operation on this part fail; settling them here means the error, if
     * there is one, still has the word "bevel" attached rather than turning up
     * several edits later.
     */
    std::string settle_error;
    if (!csg_settle(&cut, &settle_error)) {
        /* The bevel itself worked, so keep it rather than throwing the result
         * away because the tidy-up could not improve on it. */
        if (error) *error = settle_error;
    }
    *out = cut;
    return true;
}
