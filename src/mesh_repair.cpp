#include "mesh_repair.h"

#include <algorithm>
#include <array>
#include <map>
#include <stdio.h>

/*
 * Vertex welding
 *
 * Tolerance based rather than exact: vertices that should coincide often differ
 * in the last bits because they came from different expressions. A cell keyed
 * lattice at the tolerance plus a scan of the 26 neighbouring cells catches the
 * pairs that straddle a cell boundary, which a plain quantised key misses.
 */
#define WELD_TOLERANCE 1e-4f // mm; far below any dimension that matters in CAD

typedef std::array<int64_t, 3> CellKey;

static CellKey cell_of(Vec3 v, float cell) {
    CellKey k;
    k[0] = (int64_t)floorf(v.x / cell);
    k[1] = (int64_t)floorf(v.y / cell);
    k[2] = (int64_t)floorf(v.z / cell);
    return k;
}

static bool within_tolerance(Vec3 a, Vec3 b) {
    Vec3 d = vec3_sub(a, b);
    return vec3_dot(d, d) <= WELD_TOLERANCE * WELD_TOLERANCE;
}

static uint32_t weld_vertex(std::vector<Vec3> *positions,
                            std::map<CellKey, std::vector<uint32_t> > *grid, Vec3 v) {
    CellKey base = cell_of(v, WELD_TOLERANCE);
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dz = -1; dz <= 1; ++dz) {
                CellKey key = base;
                key[0] += dx; key[1] += dy; key[2] += dz;
                std::map<CellKey, std::vector<uint32_t> >::const_iterator it = grid->find(key);
                if (it == grid->end()) continue;
                for (size_t i = 0; i < it->second.size(); ++i) {
                    uint32_t idx = it->second[i];
                    if (within_tolerance((*positions)[idx], v)) return idx;
                }
            }
        }
    }

    uint32_t idx = (uint32_t)positions->size();
    positions->push_back(v);
    (*grid)[base].push_back(idx);
    return idx;
}

/* Topology */

typedef std::pair<uint32_t, uint32_t> EdgeKey;

static EdgeKey edge_key(uint32_t a, uint32_t b) {
    return (a < b) ? std::make_pair(a, b) : std::make_pair(b, a);
}

struct EdgeUse {
    std::vector<uint32_t> tris;     // triangles touching this edge
    std::vector<bool> forward;      // true when that triangle walks it low -> high
};

static void build_edges(const std::vector<uint32_t> &tris, std::map<EdgeKey, EdgeUse> *edges) {
    for (uint32_t t = 0; t < (uint32_t)(tris.size() / 3); ++t) {
        for (int e = 0; e < 3; ++e) {
            uint32_t v0 = tris[t * 3 + e];
            uint32_t v1 = tris[t * 3 + (e + 1) % 3];
            EdgeUse &use = (*edges)[edge_key(v0, v1)];
            use.tris.push_back(t);
            use.forward.push_back(v0 < v1);
        }
    }
}

static float triangle_area(const std::vector<Vec3> &p, uint32_t a, uint32_t b, uint32_t c) {
    Vec3 n = vec3_cross(vec3_sub(p[b], p[a]), vec3_sub(p[c], p[a]));
    return 0.5f * vec3_length(n);
}

static void flip_triangle(std::vector<uint32_t> *tris, uint32_t t) {
    uint32_t tmp = (*tris)[t * 3 + 1];
    (*tris)[t * 3 + 1] = (*tris)[t * 3 + 2];
    (*tris)[t * 3 + 2] = tmp;
}

/*
 * Winding
 *
 * Two triangles sharing an edge agree only when they traverse it in opposite
 * directions. A breadth first walk over the shared-edge graph rewinds whatever
 * disagrees with the triangle it was reached from; each connected shell is then
 * checked as a whole, because a consistently wound shell can still be inside
 * out, which shows up as a negative signed volume.
 */
static void fix_winding(const std::vector<Vec3> &positions, std::vector<uint32_t> *tris,
                        MeshRepairReport *report) {
    size_t tri_count = tris->size() / 3;
    if (tri_count == 0) return;

    /* Adjacency is built once, without directions: a flip invalidates stored
     * directions, so the winding is always re-read from tris at compare time. */
    std::map<EdgeKey, std::vector<uint32_t> > tri_of_edge;
    for (uint32_t t = 0; t < (uint32_t)tri_count; ++t) {
        for (int e = 0; e < 3; ++e) {
            EdgeKey key = edge_key((*tris)[t * 3 + e], (*tris)[t * 3 + (e + 1) % 3]);
            tri_of_edge[key].push_back(t);
        }
    }

    std::vector<bool> visited(tri_count, false);

    for (size_t seed = 0; seed < tri_count; ++seed) {
        if (visited[seed]) continue;

        std::vector<uint32_t> component;
        std::vector<uint32_t> queue;
        queue.push_back((uint32_t)seed);
        visited[seed] = true;

        while (!queue.empty()) {
            uint32_t t = queue.back();
            queue.pop_back();
            component.push_back(t);

            for (int e = 0; e < 3; ++e) {
                uint32_t v0 = (*tris)[t * 3 + e];
                uint32_t v1 = (*tris)[t * 3 + (e + 1) % 3];

                std::map<EdgeKey, std::vector<uint32_t> >::const_iterator adj =
                    tri_of_edge.find(edge_key(v0, v1));
                if (adj == tri_of_edge.end()) continue;

                for (size_t i = 0; i < adj->second.size(); ++i) {
                    uint32_t o = adj->second[i];
                    if (o == t || visited[o]) continue;

                    /* Same direction along the shared edge means the neighbour
                     * faces the other way. */
                    for (int oe = 0; oe < 3; ++oe) {
                        uint32_t w0 = (*tris)[o * 3 + oe];
                        uint32_t w1 = (*tris)[o * 3 + (oe + 1) % 3];
                        if (edge_key(v0, v1) != edge_key(w0, w1)) continue;
                        if (w0 == v0 && w1 == v1) {
                            flip_triangle(tris, o);
                            report->flipped_triangles += 1;
                        }
                        break;
                    }
                    visited[o] = true;
                    queue.push_back(o);
                }
            }
        }

        /* Signed volume of this shell about the origin. */
        double volume = 0.0;
        for (size_t i = 0; i < component.size(); ++i) {
            uint32_t t = component[i];
            Vec3 a = positions[(*tris)[t * 3 + 0]];
            Vec3 b = positions[(*tris)[t * 3 + 1]];
            Vec3 c = positions[(*tris)[t * 3 + 2]];
            volume += (double)vec3_dot(a, vec3_cross(b, c)) / 6.0;
        }
        if (volume < 0.0) {
            for (size_t i = 0; i < component.size(); ++i) flip_triangle(tris, component[i]);
            report->flipped_components += 1;
        }
    }
}

/*
 * Hole filling
 *
 * Boundary half-edges are chained into loops and each loop is closed with a fan
 * around its centroid. A fan handles non-planar loops, which ear clipping in 2D
 * does not, and a hole left by a missing face is nearly always small enough for
 * the added centroid to be harmless.
 */
static void fill_holes(std::vector<Vec3> *positions, std::vector<uint32_t> *tris,
                       MeshRepairReport *report) {
    std::map<EdgeKey, EdgeUse> edges;
    build_edges(*tris, &edges);

    /* Directed boundary half-edges, keyed by their start vertex. */
    std::map<uint32_t, std::vector<uint32_t> > next;
    int boundary_count = 0;
    std::map<EdgeKey, EdgeUse>::const_iterator it;
    for (it = edges.begin(); it != edges.end(); ++it) {
        if (it->second.tris.size() != 1) continue;
        bool forward = it->second.forward[0];
        uint32_t from = forward ? it->first.first : it->first.second;
        uint32_t to = forward ? it->first.second : it->first.first;
        next[from].push_back(to);
        boundary_count += 1;
    }
    if (boundary_count == 0) return;

    std::map<uint32_t, std::vector<uint32_t> >::iterator start;
    for (start = next.begin(); start != next.end(); ++start) {
        while (!start->second.empty()) {
            uint32_t first = start->first;
            uint32_t v = first;

            std::vector<uint32_t> loop;
            loop.push_back(v);

            bool closed = false;
            for (int guard = 0; guard < boundary_count + 1; ++guard) {
                std::map<uint32_t, std::vector<uint32_t> >::iterator step = next.find(v);
                if (step == next.end() || step->second.empty()) break;

                uint32_t w = step->second.back();
                step->second.pop_back();

                if (w == first) {
                    closed = true;
                    break;
                }
                loop.push_back(w);
                v = w;
            }

            if (!closed || loop.size() < 3) continue;

            Vec3 centroid = vec3(0.0f, 0.0f, 0.0f);
            for (size_t i = 0; i < loop.size(); ++i) {
                centroid = vec3_add(centroid, (*positions)[loop[i]]);
            }
            centroid = vec3_mul(centroid, 1.0f / (float)loop.size());

            uint32_t c = (uint32_t)positions->size();
            positions->push_back(centroid);

            /* The existing face walks v0 -> v1, so the patch must walk v1 -> v0
             * for the two to agree. */
            for (size_t i = 0; i < loop.size(); ++i) {
                uint32_t v0 = loop[i];
                uint32_t v1 = loop[(i + 1) % loop.size()];
                tris->push_back(v1);
                tris->push_back(v0);
                tris->push_back(c);
            }
            report->filled_holes += 1;
        }
    }
}

/* Repair */

bool mesh_repair(const Mesh &in, IndexedMesh *out, MeshRepairReport *report) {
    MeshRepairReport r;
    r.input_triangles = (int)(in.vertices.size() / 3);
    r.welded_vertices = 0;
    r.removed_degenerate = 0;
    r.removed_duplicate = 0;
    r.flipped_triangles = 0;
    r.flipped_components = 0;
    r.filled_holes = 0;
    r.open_edges = 0;
    r.nonmanifold_edges = 0;
    r.is_solid = false;

    out->positions.clear();
    out->tris.clear();

    /* Weld */
    std::map<CellKey, std::vector<uint32_t> > grid;
    std::map<std::array<uint32_t, 3>, int> seen;

    for (size_t i = 0; i + 2 < in.vertices.size(); i += 3) {
        uint32_t idx[3];
        for (int k = 0; k < 3; ++k) {
            idx[k] = weld_vertex(&out->positions, &grid, in.vertices[i + k]);
        }

        if (idx[0] == idx[1] || idx[1] == idx[2] || idx[0] == idx[2]) {
            r.removed_degenerate += 1;
            continue;
        }
        if (triangle_area(out->positions, idx[0], idx[1], idx[2]) < 1e-12f) {
            r.removed_degenerate += 1;
            continue;
        }

        std::array<uint32_t, 3> canon = { idx[0], idx[1], idx[2] };
        std::sort(canon.begin(), canon.end());
        if (seen.find(canon) != seen.end()) {
            r.removed_duplicate += 1;
            continue;
        }
        seen[canon] = 1;

        out->tris.push_back(idx[0]);
        out->tris.push_back(idx[1]);
        out->tris.push_back(idx[2]);
    }

    int input_vertices = (int)(in.vertices.size());
    r.welded_vertices = input_vertices - (int)out->positions.size();
    if (r.welded_vertices < 0) r.welded_vertices = 0;

    fix_winding(out->positions, &out->tris, &r);
    fill_holes(&out->positions, &out->tris, &r);

    /* Final verdict */
    std::map<EdgeKey, EdgeUse> edges;
    build_edges(out->tris, &edges);
    std::map<EdgeKey, EdgeUse>::const_iterator it;
    for (it = edges.begin(); it != edges.end(); ++it) {
        size_t count = it->second.tris.size();
        if (count == 1) r.open_edges += 1;
        else if (count > 2) r.nonmanifold_edges += 1;
    }

    r.is_solid = (r.open_edges == 0 && r.nonmanifold_edges == 0 && !out->tris.empty());
    if (report) *report = r;
    return r.is_solid;
}

std::string mesh_repair_summary(const MeshRepairReport &r) {
    char buf[256];
    std::string out;

    if (r.welded_vertices > 0) {
        snprintf(buf, sizeof(buf), "welded %d vertices", r.welded_vertices);
        out += buf;
    }
    if (r.removed_degenerate > 0) {
        snprintf(buf, sizeof(buf), "%sdropped %d degenerate triangles",
                 out.empty() ? "" : ", ", r.removed_degenerate);
        out += buf;
    }
    if (r.removed_duplicate > 0) {
        snprintf(buf, sizeof(buf), "%sdropped %d duplicate triangles",
                 out.empty() ? "" : ", ", r.removed_duplicate);
        out += buf;
    }
    if (r.flipped_triangles > 0) {
        snprintf(buf, sizeof(buf), "%srewound %d triangles",
                 out.empty() ? "" : ", ", r.flipped_triangles);
        out += buf;
    }
    if (r.flipped_components > 0) {
        snprintf(buf, sizeof(buf), "%sflipped %d inside out shells",
                 out.empty() ? "" : ", ", r.flipped_components);
        out += buf;
    }
    if (r.filled_holes > 0) {
        snprintf(buf, sizeof(buf), "%sclosed %d holes", out.empty() ? "" : ", ", r.filled_holes);
        out += buf;
    }
    if (r.open_edges > 0) {
        snprintf(buf, sizeof(buf), "%s%d open edges remain", out.empty() ? "" : ", ", r.open_edges);
        out += buf;
    }
    if (r.nonmanifold_edges > 0) {
        snprintf(buf, sizeof(buf), "%s%d non-manifold edges remain",
                 out.empty() ? "" : ", ", r.nonmanifold_edges);
        out += buf;
    }
    return out;
}

/* Conversion back to a renderable soup */

#define FEATURE_ANGLE_COS 0.94f // about 20 degrees

struct FeatureEdge {
    int count;
    Vec3 normal;
    bool feature;
};

void mesh_add_feature_edges(Mesh *out, const std::vector<Vec3> &positions,
                            const std::vector<uint32_t> &tris) {
    std::map<EdgeKey, FeatureEdge> edges;

    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        uint32_t a = tris[t + 0];
        uint32_t b = tris[t + 1];
        uint32_t c = tris[t + 2];
        Vec3 n = vec3_normalized(vec3_cross(vec3_sub(positions[b], positions[a]),
                                            vec3_sub(positions[c], positions[a])));
        uint32_t tri[3] = { a, b, c };
        for (int e = 0; e < 3; ++e) {
            EdgeKey key = edge_key(tri[e], tri[(e + 1) % 3]);
            std::map<EdgeKey, FeatureEdge>::iterator it = edges.find(key);
            if (it == edges.end()) {
                FeatureEdge info;
                info.count = 1;
                info.normal = n;
                info.feature = false;
                edges[key] = info;
            } else {
                it->second.count += 1;
                if (vec3_dot(it->second.normal, n) < FEATURE_ANGLE_COS) it->second.feature = true;
            }
        }
    }

    std::map<EdgeKey, FeatureEdge>::const_iterator it;
    for (it = edges.begin(); it != edges.end(); ++it) {
        /* One face is an open boundary, more than two is non-manifold; both are
         * worth seeing, as is any sharp crease. */
        if (it->second.count == 2 && !it->second.feature) continue;
        mesh_add_edge(out, positions[it->first.first], positions[it->first.second]);
    }
}

Mesh mesh_from_indexed(const IndexedMesh &in) {
    Mesh out;
    for (size_t i = 0; i + 2 < in.tris.size(); i += 3) {
        mesh_add_triangle(&out, in.positions[in.tris[i + 0]],
                                in.positions[in.tris[i + 1]],
                                in.positions[in.tris[i + 2]]);
    }
    mesh_add_feature_edges(&out, in.positions, in.tris);
    return out;
}
