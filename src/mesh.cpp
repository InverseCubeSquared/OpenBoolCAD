#include "mesh.h"

/* Mesh building blocks */

void mesh_clear(Mesh *m) {
    m->vertices.clear();
    m->normals.clear();
    m->edges.clear();
}

void mesh_add_triangle(Mesh *m, Vec3 a, Vec3 b, Vec3 c) {
    Vec3 n = vec3_normalized(vec3_cross(vec3_sub(b, a), vec3_sub(c, a)));
    m->vertices.push_back(a);
    m->vertices.push_back(b);
    m->vertices.push_back(c);
    m->normals.push_back(n);
    m->normals.push_back(n);
    m->normals.push_back(n);
}

static void mesh_add_quad(Mesh *m, Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
    mesh_add_triangle(m, a, b, c);
    mesh_add_triangle(m, a, c, d);
}

void mesh_add_edge(Mesh *m, Vec3 a, Vec3 b) {
    m->edges.push_back(a);
    m->edges.push_back(b);
}

Bounds mesh_bounds(const Mesh &m) {
    Bounds b = bounds_empty();
    for (size_t i = 0; i < m.vertices.size(); ++i) {
        bounds_add_point(&b, m.vertices[i]);
    }
    return b;
}

/* Primitives */

Mesh mesh_make_cube(float size) {
    Mesh m;
    float h = size * 0.5f;
    Vec3 v[8] = {
        vec3(-h, -h, 0.0f), vec3(h, -h, 0.0f), vec3(h, h, 0.0f), vec3(-h, h, 0.0f),
        vec3(-h, -h, size), vec3(h, -h, size), vec3(h, h, size), vec3(-h, h, size)
    };

    mesh_add_quad(&m, v[0], v[3], v[2], v[1]); // bottom
    mesh_add_quad(&m, v[4], v[5], v[6], v[7]); // top
    mesh_add_quad(&m, v[0], v[1], v[5], v[4]); // -y
    mesh_add_quad(&m, v[1], v[2], v[6], v[5]); // +x
    mesh_add_quad(&m, v[2], v[3], v[7], v[6]); // +y
    mesh_add_quad(&m, v[3], v[0], v[4], v[7]); // -x

    for (int i = 0; i < 4; ++i) {
        mesh_add_edge(&m, v[i], v[(i + 1) % 4]);
        mesh_add_edge(&m, v[4 + i], v[4 + (i + 1) % 4]);
        mesh_add_edge(&m, v[i], v[4 + i]);
    }
    return m;
}

/*
 * Ring of unit direction vectors, indexed modulo segments.
 *
 * The seam has to close exactly. Computing the last segment's angle as
 * 2*pi*segments/segments does not reproduce the bit pattern of angle 0
 * (sinf(2*pi) is about -8.7e-8, not 0), which leaves the seam vertices
 * marginally apart and the solid open - and an open mesh is not a solid the
 * boolean stage can use. Indexing one table modulo the count makes shared
 * vertices bit identical instead of merely close.
 */
static std::vector<Vec3> unit_ring(int segments) {
    std::vector<Vec3> ring;
    ring.reserve((size_t)segments);
    for (int i = 0; i < segments; ++i) {
        float a = 2.0f * 3.14159265358979f * (float)i / (float)segments;
        ring.push_back(vec3(cosf(a), sinf(a), 0.0f));
    }
    return ring;
}

Mesh mesh_make_cylinder(float diameter, float height, int segments) {
    Mesh m;
    float r = diameter * 0.5f;
    if (segments < 3) segments = 3;
    std::vector<Vec3> ring = unit_ring(segments);

    for (int i = 0; i < segments; ++i) {
        Vec3 d0 = ring[i];
        Vec3 d1 = ring[(i + 1) % segments];
        Vec3 b0 = vec3(d0.x * r, d0.y * r, 0.0f);
        Vec3 b1 = vec3(d1.x * r, d1.y * r, 0.0f);
        Vec3 t0 = vec3(b0.x, b0.y, height);
        Vec3 t1 = vec3(b1.x, b1.y, height);

        mesh_add_quad(&m, b0, b1, t1, t0);
        mesh_add_triangle(&m, vec3(0.0f, 0.0f, 0.0f), b1, b0);
        mesh_add_triangle(&m, vec3(0.0f, 0.0f, height), t0, t1);

        mesh_add_edge(&m, b0, b1);
        mesh_add_edge(&m, t0, t1);
    }
    return m;
}

/* Both poles are emitted as exact points: sinf(pi) is not 0, so deriving them
 * from the polar angle would scatter the pole into "segments" distinct
 * vertices and tear the mesh open. */
static Vec3 sphere_vertex(const std::vector<Vec3> &ring, int seg, int row, int rows, float r) {
    if (row == 0) return vec3(0.0f, 0.0f, 2.0f * r);
    if (row == rows) return vec3(0.0f, 0.0f, 0.0f);

    float p = 3.14159265358979f * (float)row / (float)rows;
    float radial = sinf(p) * r;
    Vec3 d = ring[seg % (int)ring.size()];
    return vec3(d.x * radial, d.y * radial, r + cosf(p) * r);
}

Mesh mesh_make_sphere(float diameter, int segments, int rings) {
    Mesh m;
    float r = diameter * 0.5f;
    if (segments < 3) segments = 3;
    if (rings < 2) rings = 2;
    std::vector<Vec3> ring = unit_ring(segments);

    /* Sits on the workplane, so the center is lifted by one radius. */
    for (int row = 0; row < rings; ++row) {
        for (int seg = 0; seg < segments; ++seg) {
            int s0 = seg;
            int s1 = (seg + 1) % segments;

            Vec3 a = sphere_vertex(ring, s0, row, rings, r);
            Vec3 b = sphere_vertex(ring, s0, row + 1, rings, r);
            Vec3 c = sphere_vertex(ring, s1, row + 1, rings, r);
            Vec3 d = sphere_vertex(ring, s1, row, rings, r);

            if (row == 0) {
                mesh_add_triangle(&m, a, b, c);
            } else if (row == rings - 1) {
                mesh_add_triangle(&m, a, b, d);
            } else {
                mesh_add_quad(&m, a, b, c, d);
            }
        }
    }
    return m; // smooth body, no feature edges
}

Mesh mesh_make_cone(float diameter, float height, int segments) {
    Mesh m;
    float r = diameter * 0.5f;
    if (segments < 3) segments = 3;
    Vec3 apex = vec3(0.0f, 0.0f, height);
    std::vector<Vec3> ring = unit_ring(segments);

    for (int i = 0; i < segments; ++i) {
        Vec3 d0 = ring[i];
        Vec3 d1 = ring[(i + 1) % segments];
        Vec3 b0 = vec3(d0.x * r, d0.y * r, 0.0f);
        Vec3 b1 = vec3(d1.x * r, d1.y * r, 0.0f);

        mesh_add_triangle(&m, b0, b1, apex);
        mesh_add_triangle(&m, vec3(0.0f, 0.0f, 0.0f), b1, b0);
        mesh_add_edge(&m, b0, b1);
    }
    return m;
}

Mesh mesh_make_pyramid(float size, float height) {
    Mesh m;
    float h = size * 0.5f;
    Vec3 b[4] = {
        vec3(-h, -h, 0.0f), vec3(h, -h, 0.0f), vec3(h, h, 0.0f), vec3(-h, h, 0.0f)
    };
    Vec3 apex = vec3(0.0f, 0.0f, height);

    mesh_add_quad(&m, b[0], b[3], b[2], b[1]);
    for (int i = 0; i < 4; ++i) {
        Vec3 p0 = b[i];
        Vec3 p1 = b[(i + 1) % 4];
        mesh_add_triangle(&m, p0, p1, apex);
        mesh_add_edge(&m, p0, p1);
        mesh_add_edge(&m, p0, apex);
    }
    return m;
}

/*
 * Triangular prism standing on the workplane: the cross section in XZ is a
 * right triangle, full height at -x and zero at +x, extruded along y. Every
 * vertex is exact, so the seam trap the round primitives have does not apply.
 */
Mesh mesh_make_wedge(float size, float height) {
    Mesh m;
    float h = size * 0.5f;

    Vec3 b0 = vec3(-h, -h, 0.0f);
    Vec3 b1 = vec3(h, -h, 0.0f);
    Vec3 b2 = vec3(h, h, 0.0f);
    Vec3 b3 = vec3(-h, h, 0.0f);
    Vec3 t0 = vec3(-h, -h, height);
    Vec3 t3 = vec3(-h, h, height);

    mesh_add_quad(&m, b0, b3, b2, b1); // bottom
    mesh_add_quad(&m, b0, t0, t3, b3); // back wall at -x
    mesh_add_quad(&m, b1, b2, t3, t0); // the slope
    mesh_add_triangle(&m, b0, b1, t0); // -y end
    mesh_add_triangle(&m, b3, t3, b2); // +y end

    mesh_add_edge(&m, b0, b1);
    mesh_add_edge(&m, b1, b2);
    mesh_add_edge(&m, b2, b3);
    mesh_add_edge(&m, b3, b0);
    mesh_add_edge(&m, b0, t0);
    mesh_add_edge(&m, b3, t3);
    mesh_add_edge(&m, t0, t3);
    mesh_add_edge(&m, b1, t0);
    mesh_add_edge(&m, b2, t3);
    return m;
}

/* Resolution */

void primitive_resolution_init(PrimitiveResolution *r) {
    r->cylinder_segments = 48;
    r->cone_segments = 48;
    r->sphere_segments = 32;
    r->sphere_rings = 16;
}

void primitive_resolution_clamp(PrimitiveResolution *r) {
    /* Three is the fewest that closes a ring; the ceiling is where the boolean
     * cost stops being worth the extra smoothness. */
    if (r->cylinder_segments < 3) r->cylinder_segments = 3;
    if (r->cylinder_segments > 512) r->cylinder_segments = 512;
    if (r->cone_segments < 3) r->cone_segments = 3;
    if (r->cone_segments > 512) r->cone_segments = 512;
    if (r->sphere_segments < 3) r->sphere_segments = 3;
    if (r->sphere_segments > 256) r->sphere_segments = 256;
    if (r->sphere_rings < 2) r->sphere_rings = 2;
    if (r->sphere_rings > 128) r->sphere_rings = 128;
}

bool primitive_has_resolution(PrimitiveKind kind) {
    return kind == PRIM_CYLINDER || kind == PRIM_SPHERE || kind == PRIM_CONE;
}

int primitive_triangle_count(PrimitiveKind kind, const PrimitiveResolution &r) {
    switch (kind) {
    /* Side quad plus a cap fan triangle at each end. */
    case PRIM_CYLINDER: return r.cylinder_segments * 4;
    /* Side triangle plus a base fan triangle. */
    case PRIM_CONE:     return r.cone_segments * 2;
    /* Quads everywhere but the two pole rows, which are single triangles. */
    case PRIM_SPHERE:   return r.sphere_segments * (2 * r.sphere_rings - 2);
    case PRIM_CUBE:     return 12;
    case PRIM_PYRAMID:  return 6;
    case PRIM_WEDGE:    return 8;
    default:            return 0;
    }
}

Mesh mesh_make_primitive_at(PrimitiveKind kind, const PrimitiveResolution &res) {
    PrimitiveResolution r = res;
    primitive_resolution_clamp(&r);

    float s = OBC_PRIMITIVE_SIZE;
    switch (kind) {
    case PRIM_CUBE:     return mesh_make_cube(s);
    case PRIM_CYLINDER: return mesh_make_cylinder(s, s, r.cylinder_segments);
    case PRIM_SPHERE:   return mesh_make_sphere(s, r.sphere_segments, r.sphere_rings);
    case PRIM_CONE:     return mesh_make_cone(s, s, r.cone_segments);
    case PRIM_PYRAMID:  return mesh_make_pyramid(s, s);
    case PRIM_WEDGE:    return mesh_make_wedge(s, s);
    default:            return mesh_make_cube(s);
    }
}

Mesh mesh_make_primitive(PrimitiveKind kind) {
    PrimitiveResolution r;
    primitive_resolution_init(&r);
    return mesh_make_primitive_at(kind, r);
}

const char *primitive_name(PrimitiveKind kind) {
    switch (kind) {
    case PRIM_CUBE:     return "Cube";
    case PRIM_CYLINDER: return "Cylinder";
    case PRIM_SPHERE:   return "Sphere";
    case PRIM_CONE:     return "Cone";
    case PRIM_PYRAMID:  return "Pyramid";
    case PRIM_WEDGE:    return "Wedge";
    default:            return "Unknown";
    }
}
