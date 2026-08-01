#include "bevel.h"

#include <map>
#include <math.h>

#include "csg.h"
#include "mesh_repair.h"

/*
 * What counts as an edge
 *
 * The same crease the outline is stroked at. Anything shallower is the
 * tessellation of a curved surface, and a fillet on one facet seam of a
 * cylinder is both meaningless and impossible: the cutter is wider than the
 * facet it sits on, so it runs straight past its neighbours.
 */
#define BEVEL_FEATURE_COS 0.94f      // about 20 degrees

/*
 * Faces nearly back to back leave no room for a fillet - the arc centre runs
 * off to infinity as the material closes to a knife edge. Rejected rather than
 * clamped, since there is no bevel of a knife edge that means anything.
 */
#define BEVEL_KNIFE_COS (-0.95f)     // about 18 degrees of material left

/* Two edges continue the same pair of surfaces when both normals still line up
 * this closely across the joint, and the run does not double back. */
#define BEVEL_JOIN_COS 0.5f          // 60 degrees between matching normals
#define BEVEL_TURN_COS 0.17f         // 80 degrees of turn in the chain

/* Topology */

typedef std::pair<uint32_t, uint32_t> EdgeKey;

static EdgeKey edge_key(uint32_t a, uint32_t b) {
    return (a < b) ? std::make_pair(a, b) : std::make_pair(b, a);
}

/* One crease before chaining: the two vertices it runs between, walked in the
 * direction the first of its two faces walks them. */
struct RawEdge {
    uint32_t v0, v1;
    Vec3 na, nb;
    bool convex;
};

struct EdgeFaces {
    Vec3 normal[2];
    uint32_t from, to;  // the direction the first face walks it
    int count;
};

/*
 * Creases of a mesh, from its welded and consistently wound form.
 *
 * Topology comes out of mesh_repair rather than out of a hash of the vertex
 * coordinates. Matching edges by snapping their endpoints to a grid, which is
 * what this used to do, silently loses every pair that straddles a cell
 * boundary - and on a boolean result, whose vertices sit at arbitrary
 * coordinates, that is a good fraction of them. Indices cannot miss.
 */
static void collect_raw(const Mesh &mesh, std::vector<Vec3> *positions,
                        std::vector<RawEdge> *out) {
    IndexedMesh idx;
    MeshRepairReport report;
    mesh_repair(mesh, &idx, &report);
    *positions = idx.positions;

    std::map<EdgeKey, EdgeFaces> edges;
    for (size_t t = 0; t + 2 < idx.tris.size(); t += 3) {
        uint32_t tri[3] = { idx.tris[t + 0], idx.tris[t + 1], idx.tris[t + 2] };
        Vec3 n = vec3_normalized(vec3_cross(
            vec3_sub(idx.positions[tri[1]], idx.positions[tri[0]]),
            vec3_sub(idx.positions[tri[2]], idx.positions[tri[0]])));
        if (vec3_length(n) < 0.5f) continue; // degenerate

        for (int e = 0; e < 3; ++e) {
            uint32_t a = tri[e];
            uint32_t b = tri[(e + 1) % 3];
            EdgeKey key = edge_key(a, b);

            std::map<EdgeKey, EdgeFaces>::iterator it = edges.find(key);
            if (it == edges.end()) {
                EdgeFaces f;
                f.normal[0] = n;
                f.from = a;
                f.to = b;
                f.count = 1;
                edges[key] = f;
            } else if (it->second.count == 1) {
                it->second.normal[1] = n;
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

        float d = vec3_dot(f.normal[0], f.normal[1]);
        if (d > BEVEL_FEATURE_COS) continue; // too flat to be an edge
        if (d < BEVEL_KNIFE_COS) continue;   // knife edge, no room for an arc

        RawEdge e;
        e.v0 = f.from;
        e.v1 = f.to;
        e.na = f.normal[0];
        e.nb = f.normal[1];
        /*
         * Convex when the two normals turn the same way the first face walks
         * the edge. Reading it off the winding does not care how thin or skewed
         * the triangles are, which the old "does the far vertex lie behind the
         * other face's plane" test very much did.
         */
        Vec3 dir = vec3_sub((*positions)[f.to], (*positions)[f.from]);
        e.convex = vec3_dot(vec3_cross(f.normal[0], f.normal[1]), dir) > 0.0f;
        out->push_back(e);
    }
}

/* Chaining */

/*
 * How well two creases continue the same two surfaces, and whether the second
 * one's normals have to be swapped to line up with the first's. A rim vertex of
 * a cylinder has three creases meeting at it; this is what tells the rim from
 * the vertical seam that crosses it.
 */
static float pair_quality(const RawEdge &a, const RawEdge &b, bool *swap) {
    float straight = vec3_dot(a.na, b.na);
    float other = vec3_dot(a.nb, b.nb);
    if (other < straight) straight = other;

    float crossed = vec3_dot(a.na, b.nb);
    other = vec3_dot(a.nb, b.na);
    if (other < crossed) crossed = other;

    if (crossed > straight) {
        if (swap) *swap = true;
        return crossed;
    }
    if (swap) *swap = false;
    return straight;
}

/* The crease that carries on from "e" through vertex "v", or -1. */
static int continuation(const std::vector<Vec3> &pos, const std::vector<RawEdge> &edges,
                        const std::vector<std::vector<int> > &at_vertex,
                        int e, uint32_t v, bool *swap_out) {
    const RawEdge &me = edges[e];
    uint32_t behind = (me.v0 == v) ? me.v1 : me.v0;
    Vec3 din = vec3_normalized(vec3_sub(pos[v], pos[behind]));

    int best = -1;
    float best_quality = BEVEL_JOIN_COS;
    bool best_swap = false;

    const std::vector<int> &list = at_vertex[v];
    for (size_t i = 0; i < list.size(); ++i) {
        int o = list[i];
        if (o == e) continue;

        const RawEdge &them = edges[o];
        if (them.convex != me.convex) continue;

        uint32_t ahead = (them.v0 == v) ? them.v1 : them.v0;
        Vec3 dout = vec3_normalized(vec3_sub(pos[ahead], pos[v]));
        if (vec3_dot(din, dout) < BEVEL_TURN_COS) continue;

        bool swap = false;
        float q = pair_quality(me, them, &swap);
        if (q <= best_quality) continue;

        best_quality = q;
        best = o;
        best_swap = swap;
    }

    if (swap_out) *swap_out = best_swap;
    return best;
}

struct Neighbour {
    int edge;   // -1 for none
    bool swap;
};

void bevel_collect_edges(const Mesh &mesh, std::vector<BevelEdge> *out) {
    out->clear();

    std::vector<Vec3> pos;
    std::vector<RawEdge> raw;
    collect_raw(mesh, &pos, &raw);
    if (raw.empty()) return;

    std::vector<std::vector<int> > at_vertex(pos.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        at_vertex[raw[i].v0].push_back((int)i);
        at_vertex[raw[i].v1].push_back((int)i);
    }

    /*
     * Links are resolved before any walking and only kept when both sides
     * agree. A one sided link would let a chain run into a junction and out of
     * an arm that does not consider itself part of it, which is how a rim ends
     * up swallowing the seam that crosses it.
     */
    std::vector<Neighbour> link0(raw.size()), link1(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        for (int end = 0; end < 2; ++end) {
            uint32_t v = end == 0 ? raw[i].v0 : raw[i].v1;
            bool swap = false;
            int other = continuation(pos, raw, at_vertex, (int)i, v, &swap);
            if (other >= 0) {
                bool back = false;
                if (continuation(pos, raw, at_vertex, other, v, &back) != (int)i) other = -1;
            }
            Neighbour n;
            n.edge = other;
            n.swap = swap;
            if (end == 0) link0[i] = n; else link1[i] = n;
        }
    }

    /* Walk each run, starting from an end where there is one so an open chain
     * comes out in order; whatever is left over is a closed ring. */
    std::vector<char> used(raw.size(), 0);

    for (int pass = 0; pass < 2; ++pass) {
        for (size_t seed = 0; seed < raw.size(); ++seed) {
            if (used[seed]) continue;

            bool from_v0 = link0[seed].edge < 0;
            bool from_v1 = link1[seed].edge < 0;
            if (pass == 0 && !from_v0 && !from_v1) continue; // ring, second pass

            /* Enter at the free end so an open run comes out in order; a ring
             * has no free end and can be entered anywhere. */
            uint32_t enter = from_v0 ? raw[seed].v0 : (from_v1 ? raw[seed].v1 : raw[seed].v0);

            std::vector<uint32_t> verts;
            std::vector<int> steps;
            std::vector<char> flipped;

            int e = (int)seed;
            char flip = 0;
            uint32_t v = enter;
            bool closed = false;

            verts.push_back(v);
            for (;;) {
                used[e] = 1;
                steps.push_back(e);
                flipped.push_back(flip);

                uint32_t next_v = (raw[e].v0 == v) ? raw[e].v1 : raw[e].v0;
                verts.push_back(next_v);

                const Neighbour &link = (raw[e].v0 == next_v) ? link0[e] : link1[e];
                if (link.edge < 0) break;
                if (link.edge == (int)seed) { closed = true; break; }
                if (used[link.edge]) break;

                flip = (char)(flip ^ (link.swap ? 1 : 0));
                e = link.edge;
                v = next_v;
            }
            if (closed) verts.pop_back(); // the ring's last point is its first

            if (steps.empty()) continue;

            BevelEdge chain;
            chain.closed = closed;
            chain.convex = raw[steps[0]].convex;
            chain.points.reserve(verts.size());
            for (size_t i = 0; i < verts.size(); ++i) chain.points.push_back(pos[verts[i]]);

            /* One true normal pair per segment, in the chain's own orientation:
             * "swapped" records where a link met its neighbour the other way
             * round, and following it is what keeps the two surfaces from
             * changing places halfway along a rim. */
            chain.normal_a.resize(steps.size());
            chain.normal_b.resize(steps.size());
            for (size_t i = 0; i < steps.size(); ++i) {
                const RawEdge &r = raw[steps[i]];
                bool sw = flipped[i] != 0;
                chain.normal_a[i] = sw ? r.nb : r.na;
                chain.normal_b[i] = sw ? r.na : r.nb;
            }

            out->push_back(chain);
        }
    }
}

/* Geometry of one cutter */

/*
 * The cross section of the material a bevel takes away, in the plane across the
 * edge: the corner point, the two points where the arc meets the faces, and the
 * arc between them.
 *
 * The fillet touches both faces, so its centre is r from each; with "a" the
 * angle between the outward normals that distance is r / cos(a/2). At a right
 * angle it is r * sqrt(2), the familiar answer for a cube edge. A concave edge
 * puts the centre on the other side of the corner, out in the notch, which is
 * the whole difference between cutting a corner and filling one.
 */
static bool build_section(Vec3 v, Vec3 na, Vec3 nb, bool convex, float radius,
                          int segments, float nudge, std::vector<Vec3> *out) {
    float nd = vec3_dot(na, nb);
    if (nd > 1.0f) nd = 1.0f;
    if (nd < -1.0f) nd = -1.0f;

    float c = cosf(0.5f * acosf(nd));
    if (c < 1e-3f) return false; // faces back to back

    Vec3 bisector = vec3_normalized(vec3_add(na, nb));
    if (vec3_length(bisector) < 0.5f) return false;

    float side = convex ? -1.0f : 1.0f;
    Vec3 centre = vec3_add(v, vec3_mul(bisector, side * radius / c));
    Vec3 t0 = vec3_sub(centre, vec3_mul(na, side * radius));
    Vec3 t1 = vec3_sub(centre, vec3_mul(nb, side * radius));

    /*
     * The corner is nudged off the surface it came from - outward for a cut,
     * into the material for a fill. Left exactly on the edge, the cutter's two
     * flanks lie in the two face planes along the whole length of it, and a
     * boolean between coincident faces is the case that resolves worst. A few
     * microns is enough to separate them, and the sliver the nudge adds is on
     * the side the operation throws away.
     */
    Vec3 corner = vec3_sub(v, vec3_mul(bisector, side * nudge));

    out->clear();
    out->push_back(corner);
    out->push_back(t0);

    if (segments > 1) {
        Vec3 r0 = vec3_sub(t0, centre);
        Vec3 r1 = vec3_sub(t1, centre);
        Vec3 axis = vec3_normalized(vec3_cross(r0, r1));
        if (vec3_length(axis) < 0.5f) return false;

        float cosine = vec3_dot(vec3_normalized(r0), vec3_normalized(r1));
        if (cosine > 1.0f) cosine = 1.0f;
        if (cosine < -1.0f) cosine = -1.0f;
        float sweep = acosf(cosine);

        for (int s = 1; s < segments; ++s) {
            Mat3 rot = mat3_axis_angle(axis, rad_to_deg(sweep * (float)s / (float)segments));
            out->push_back(vec3_add(centre, mat3_apply(rot, r0)));
        }
    }
    out->push_back(t1);
    return true;
}

/*
 * Stretches a section across a joint onto the plane that bisects the turn.
 *
 * Without it the sweep pinches wherever the run turns, and leaves a wedge of
 * material standing between one segment's fillet and the next. The stretch runs
 * along the direction the run turns towards, which is exactly the mitre a
 * picture frame is cut with, and it is exact for a section square to the run.
 */
static void mitre_section(std::vector<Vec3> *section, Vec3 v, Vec3 din, Vec3 dout) {
    float cosine = vec3_dot(din, dout);
    if (cosine > 0.999999f) return; // straight through, nothing to stretch
    if (cosine < -1.0f) cosine = -1.0f;

    float c = cosf(0.5f * acosf(cosine));
    if (c < 0.2f) return; // too sharp to mitre; the joint stays blunt

    Vec3 turn = vec3_normalized(vec3_sub(dout, din));
    if (vec3_length(turn) < 0.5f) return;

    float k = 1.0f / c - 1.0f;
    for (size_t i = 0; i < section->size(); ++i) {
        float along = vec3_dot(vec3_sub((*section)[i], v), turn);
        (*section)[i] = vec3_add((*section)[i], vec3_mul(turn, along * k));
    }
}

/* Sections are collected through this so a joint that turned out to be no joint
 * at all does not leave two identical rings and a ribbon of zero area quads
 * between them, which mesh_repair would drop and leave a hole where they were. */
static void push_section(std::vector<std::vector<Vec3> > *list, const std::vector<Vec3> &s) {
    if (!list->empty()) {
        const std::vector<Vec3> &last = list->back();
        bool same = last.size() == s.size();
        for (size_t i = 0; same && i < s.size(); ++i) {
            if (vec3_length(vec3_sub(last[i], s[i])) > OBC_WELD_MM) same = false;
        }
        if (same) return;
    }
    list->push_back(s);
}

/*
 * Signed volume, used only to decide which way round the shell came out.
 *
 * Measured from a point on the shell itself and summed in double. Taking the
 * triple product of the raw coordinates instead is the obvious way to write it
 * and it does not work: a cutter sitting at the far corner of a 20 mm part has
 * terms in the thousands cancelling down to a result near one, which single
 * precision cannot do. The answer that comes out is noise, and its *sign* is
 * noise, so the shell gets turned inside out at random.
 */
static double shell_volume(const Mesh &m) {
    if (m.vertices.empty()) return 0.0;

    Vec3 origin = m.vertices[0];
    double sum = 0.0;
    for (size_t t = 0; t + 2 < m.vertices.size(); t += 3) {
        double ax = m.vertices[t + 0].x - origin.x;
        double ay = m.vertices[t + 0].y - origin.y;
        double az = m.vertices[t + 0].z - origin.z;
        double bx = m.vertices[t + 1].x - origin.x;
        double by = m.vertices[t + 1].y - origin.y;
        double bz = m.vertices[t + 1].z - origin.z;
        double cx = m.vertices[t + 2].x - origin.x;
        double cy = m.vertices[t + 2].y - origin.y;
        double cz = m.vertices[t + 2].z - origin.z;
        sum += (ax * (by * cz - bz * cy) +
                ay * (bz * cx - bx * cz) +
                az * (bx * cy - by * cx)) / 6.0;
    }
    return sum;
}

/*
 * Emits a triangle unless two of its corners are close enough that the repair
 * pass would weld them.
 *
 * A sweep pinches wherever two sections share a point, and they do: rounding a
 * joint turns the section about the crease the two faces meet along, and the
 * point where the arc lands on that crease is common to both sides of the turn.
 * The quad there is really a triangle. Emitting it as a quad anyway leaves a
 * zero area sliver that mesh_repair drops, which opens a hole in a surface that
 * was closed - which is exactly the failure this whole exercise is about.
 */
static void add_triangle(Mesh *out, Vec3 a, Vec3 b, Vec3 c) {
    if (vec3_length(vec3_sub(a, b)) <= OBC_WELD_MM) return;
    if (vec3_length(vec3_sub(b, c)) <= OBC_WELD_MM) return;
    if (vec3_length(vec3_sub(c, a)) <= OBC_WELD_MM) return;
    mesh_add_triangle(out, a, b, c);
}

/* Skins a run of equal sized sections and closes the ends, then turns the shell
 * the right way out. */
static void loft(const std::vector<std::vector<Vec3> > &sections, bool closed, Mesh *out) {
    size_t count = sections.size();
    if (count < 2) return;
    size_t m = sections[0].size();

    size_t spans = closed ? count : count - 1;
    for (size_t i = 0; i < spans; ++i) {
        const std::vector<Vec3> &a = sections[i];
        const std::vector<Vec3> &b = sections[(i + 1) % count];
        for (size_t k = 0; k < m; ++k) {
            size_t j = (k + 1) % m;
            add_triangle(out, a[k], b[k], b[j]);
            add_triangle(out, a[k], b[j], a[j]);
        }
    }

    if (!closed) {
        /*
         * Fanned from the corner, which every other point of the section is
         * visible from: the shape is a corner with a disc taken out of it.
         *
         * Each cap takes the winding that agrees with the side quads above -
         * the first cap the section's own order, the last cap the reverse.
         * Wound the other way round the caps disagree with the sides, and then
         * no single flip of the shell can make the whole thing face outward.
         */
        const std::vector<Vec3> &first = sections[0];
        const std::vector<Vec3> &last = sections[count - 1];
        for (size_t k = 1; k + 1 < m; ++k) {
            add_triangle(out, first[0], first[k], first[k + 1]);
            add_triangle(out, last[0], last[k + 1], last[k]);
        }
    }

    if (shell_volume(*out) < 0.0) {
        for (size_t t = 0; t + 2 < out->vertices.size(); t += 3) {
            Vec3 tmp = out->vertices[t + 1];
            out->vertices[t + 1] = out->vertices[t + 2];
            out->vertices[t + 2] = tmp;
            for (int k = 0; k < 3; ++k) {
                out->normals[t + k] = vec3_mul(out->normals[t + k], -1.0f);
            }
        }
    }
}

/*
 * One cutter for one chain: the cross section swept along the whole run, closed
 * off at both ends unless the run is a ring.
 *
 * Sweeping the chain rather than each of its segments is the point of the whole
 * exercise. Two segments of one rim share their section exactly, so the sweep
 * across them is a single watertight surface with nothing for a boolean to
 * resolve. A cutter per segment instead leaves each pair overlapping in a film
 * a few microns thick, and unioning a rim's worth of those is what used to come
 * back as a mesh with a few hundred open edges in it.
 *
 * Each segment carries its own two face normals from end to end, so the fillet
 * along it is the exact cylinder tangent to both of its faces. What happens at
 * a joint depends on which way the run turns:
 *
 * - Turning away from the cutter, the sections separate, and the gap between
 *   them is filled by rotating one into the other about the joint. That sweep
 *   is the torus a real fillet has at a corner, and it is what stops a peak of
 *   material standing where two bevels meet.
 * - Turning into the cutter - going round the inside of a bore, say - the
 *   sections overlap instead, and one mitred section serves both segments.
 */
static bool build_cutter(const BevelEdge &edge, float radius, int segments, Mesh *out) {
    size_t n = edge.points.size();
    if (n < 2) return false;

    size_t nseg = edge.closed ? n : n - 1;
    if (edge.normal_a.size() < nseg || edge.normal_b.size() < nseg) return false;

    std::vector<Vec3> dir(nseg);
    for (size_t i = 0; i < nseg; ++i) {
        dir[i] = vec3_normalized(vec3_sub(edge.points[(i + 1) % n], edge.points[i]));
        if (vec3_length(dir[i]) < 0.5f) return false; // repeated point
    }

    float nudge = radius * 0.02f + 1e-3f;
    std::vector<std::vector<Vec3> > sections;
    std::vector<Vec3> s;

    if (!edge.closed) {
        if (!build_section(edge.points[0], edge.normal_a[0], edge.normal_b[0],
                           edge.convex, radius, segments, nudge, &s)) return false;
        push_section(&sections, s);
    }

    for (size_t i = 0; i < nseg; ++i) {
        Vec3 v = edge.points[(i + 1) % n];

        if (!edge.closed && i + 1 == nseg) {
            if (!build_section(v, edge.normal_a[i], edge.normal_b[i], edge.convex,
                               radius, segments, nudge, &s)) return false;
            push_section(&sections, s);
            break;
        }

        size_t j = (i + 1) % nseg;
        Vec3 a0 = edge.normal_a[i], a1 = edge.normal_a[j];
        Vec3 b0 = edge.normal_b[i], b1 = edge.normal_b[j];

        /*
         * One section serves both segments: built from the average of their two
         * normals, then stretched onto the plane that bisects the turn.
         *
         * Turning the section from one segment's normals round to the other's
         * is the exact answer, and it is not worth having. It pinches the
         * section against the crease the two faces meet along - the point where
         * the arc lands on that crease belongs to both sides of the turn - and
         * the ribbon of triangles beside the pinch comes out microns wide.
         * Measured, the exact version bought 0.2% of the removed volume on an
         * eight sided rim, and cost bevelling every edge of a gear, which
         * stopped resolving at all. Averaging leaves a joint a little over-cut
         * and leaves every boolean downstream working.
         */
        if (!build_section(v, vec3_normalized(vec3_add(a0, a1)),
                           vec3_normalized(vec3_add(b0, b1)), edge.convex,
                           radius, segments, nudge, &s)) return false;
        mitre_section(&s, v, dir[i], dir[j]);
        push_section(&sections, s);
    }

    if (sections.size() < 2) return false;

    /*
     * Open ends run a hair past the last point, so the cap does not land
     * exactly on the face the edge finishes against - the coplanar case again.
     * Small on purpose: past the end of a chain the cutter is nicking a face
     * nobody asked to bevel.
     */
    if (!edge.closed) {
        float pad = radius * 0.02f + 1e-3f;
        std::vector<Vec3> &first = sections[0];
        std::vector<Vec3> &last = sections[sections.size() - 1];
        for (size_t k = 0; k < first.size(); ++k) {
            first[k] = vec3_sub(first[k], vec3_mul(dir[0], pad));
            last[k] = vec3_add(last[k], vec3_mul(dir[nseg - 1], pad));
        }
    }

    loft(sections, edge.closed, out);
    return true;
}

/*
 * Why there is no corner piece, and no rounded joint
 *
 * Both looked necessary and neither survived measurement.
 *
 * Two runs cannot be chained unless both their surfaces carry on across the
 * joint, and at a box corner they do not - the top edge running one way and the
 * top edge running the other share only the top face. It looks as though their
 * two cutters must leave something standing between them, and the fix looks
 * like a third cutter at the vertex sweeping one run's section round to the
 * other's. That cutter removes nothing: at corners of 90, 108 and 120 degrees
 * it changed the result by under a thousandth of a cubic millimetre, and
 * filling an inside corner came out the same. A fillet cylinder already reaches
 * far enough past its own edge that its neighbour has nothing left to take.
 *
 * The same sweep inside a run, turning the section through a crease instead of
 * averaging across it, is exact where averaging is not - and it pinches the
 * section against the crease and leaves microns-wide slivers beside the pinch.
 * It bought 0.2% of the removed volume on an eight sided rim and lost a gear
 * with every edge bevelled, which stopped resolving at all.
 *
 * So if something does appear to be standing at a corner, check the *outline*
 * before the geometry. Seam vertices that came back from the boolean unmerged
 * used to draw black creases across faces that were perfectly flat, which reads
 * exactly like leftover material and is not (see manifold_to_mesh in csg.cpp).
 */

/* Limits */

/* How far along each face the arc reaches: r * tan(a/2), with "a" the angle
 * between the outward normals. A shallow crease throws it a long way. */
static float reach_factor(const BevelEdge &edge) {
    float worst = 0.0f;
    for (size_t i = 0; i < edge.normal_a.size(); ++i) {
        float nd = vec3_dot(edge.normal_a[i], edge.normal_b[i]);
        if (nd > 1.0f) nd = 1.0f;
        if (nd < -1.0f) nd = -1.0f;
        float half = 0.5f * acosf(nd);
        float c = cosf(half);
        if (c < 1e-3f) return 1e3f;
        float t = sinf(half) / c;
        if (t > worst) worst = t;
    }
    if (worst < 1.0f) worst = 1.0f;
    return worst;
}

static float chain_length(const BevelEdge &edge) {
    float total = 0.0f;
    size_t steps = edge.closed ? edge.points.size() : edge.points.size() - 1;
    for (size_t i = 0; i < steps; ++i) {
        total += vec3_length(vec3_sub(edge.points[(i + 1) % edge.points.size()],
                                      edge.points[i]));
    }
    return total;
}

float bevel_max_radius(const Mesh &mesh, const std::vector<BevelEdge> &edges,
                       const std::vector<int> &chosen) {
    Bounds b = mesh_bounds(mesh);
    Vec3 size = bounds_size(b);
    float smallest = size.x;
    if (size.y < smallest) smallest = size.y;
    if (size.z < smallest) smallest = size.z;

    float limit = smallest * 0.45f;

    for (size_t i = 0; i < chosen.size(); ++i) {
        int index = chosen[i];
        if (index < 0 || index >= (int)edges.size()) continue;
        const BevelEdge &e = edges[index];
        if (e.points.size() < 2) continue;

        /* The arc has to stay on the faces the edge sits between, so a crease
         * shallower than a right angle gets proportionally less radius. */
        float reach = smallest * 0.45f / reach_factor(e);
        if (reach < limit) limit = reach;

        /* A short run cannot carry a fillet wider than itself. A ring has no
         * ends, so nothing to run off. */
        if (!e.closed) {
            float along = chain_length(e) * 0.45f;
            if (along < limit) limit = along;
        }
    }

    if (limit < 1e-3f) limit = 1e-3f;
    return limit;
}

/* Applying */

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

    /* Clamped here as well as in the dialog, so no caller can reach the
     * boolean with a radius that has nowhere to go. */
    float limit = bevel_max_radius(mesh, edges, chosen);
    if (radius > limit) radius = limit;

    /*
     * Convex edges cut away and concave ones fill in, so they cannot go through
     * the same side of the merge. Both lists are unioned before they meet the
     * solid, which is what lets two bevelled edges sharing a vertex resolve
     * against each other instead of fighting.
     */
    std::vector<Mesh> positives, negatives;
    positives.push_back(mesh);

    int built = 0;
    for (size_t i = 0; i < chosen.size(); ++i) {
        int index = chosen[i];
        if (index < 0 || index >= (int)edges.size()) continue;

        Mesh cutter;
        if (!build_cutter(edges[index], radius, segments, &cutter)) continue;
        if (cutter.vertices.empty()) continue;

        if (edges[index].convex) negatives.push_back(cutter);
        else positives.push_back(cutter);
        built += 1;
    }

    if (built == 0) {
        if (error) *error = "None of those edges could be bevelled.";
        return false;
    }

    /*
     * Deliberately not csg_merge: that ends with the thin wall pass, and a
     * fillet's facet chords are the same size as the walls it is meant to drop.
     * A 0.4 mm radius at six segments has chords of about 14 microns against a
     * 10 micron tolerance, so the pass eats the arc and hands back something
     * closer to a chamfer than the bevel that was asked for.
     */
    Mesh cut;
    if (!csg_boolean(positives, negatives, &cut, error)) return false;

    /*
     * A settling pass over the result. Cutters that met at a vertex leave
     * slivers and near coincident faces behind, and those are what make the
     * *next* operation on this part fail, several edits away from anything
     * mentioning bevels.
     *
     * Its failure is the bevel's failure. A part that looks bevelled on screen
     * but refuses every boolean afterwards is the worst of the three outcomes -
     * worse than an error, and much worse than an unchanged part, because
     * nothing on screen says which edit broke it.
     */
    if (!csg_settle(&cut, error)) return false;

    *out = cut;
    return true;
}
