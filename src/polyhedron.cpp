#include "polyhedron.h"

#include <math.h>

#include "csg.h"

#define POLY_PI 3.14159265358979f

void polyhedron_params_init(PolyhedronParams *p) {
    p->kind = POLY_ICOSAHEDRON;
    p->sides = 6;
    p->size_mm = OBC_PRIMITIVE_SIZE;
    p->height_mm = OBC_PRIMITIVE_SIZE;
    p->negative = false;
}

void polyhedron_params_clamp(PolyhedronParams *p) {
    if (p->kind < 0 || p->kind >= POLY_COUNT) p->kind = POLY_ICOSAHEDRON;
    /* Three is the fewest an n-gon can have; past a few hundred the facets are
     * smaller than anything is going to be made at. */
    if (p->sides < 3) p->sides = 3;
    if (p->sides > 256) p->sides = 256;
    if (p->size_mm < 0.1f) p->size_mm = 0.1f;
    if (p->size_mm > 1000.0f) p->size_mm = 1000.0f;
    if (p->height_mm < 0.1f) p->height_mm = 0.1f;
    if (p->height_mm > 1000.0f) p->height_mm = 1000.0f;
}

const char *polyhedron_kind_name(int kind) {
    switch (kind) {
    case POLY_TETRAHEDRON:  return "Tetrahedron";
    case POLY_HEXAHEDRON:   return "Hexahedron";
    case POLY_OCTAHEDRON:   return "Octahedron";
    case POLY_DODECAHEDRON: return "Dodecahedron";
    case POLY_PRISM:        return "Prism";
    case POLY_ANTIPRISM:    return "Antiprism";
    case POLY_BIPYRAMID:    return "Bipyramid";
    case POLY_PYRAMID:      return "Pyramid";
    default:                return "Icosahedron";
    }
}

bool polyhedron_kind_takes_sides(int kind) {
    return kind == POLY_PRISM || kind == POLY_ANTIPRISM ||
           kind == POLY_BIPYRAMID || kind == POLY_PYRAMID;
}

bool polyhedron_kind_takes_height(int kind) {
    return polyhedron_kind_takes_sides(kind);
}

int polyhedron_face_count(const PolyhedronParams &params) {
    PolyhedronParams p = params;
    polyhedron_params_clamp(&p);

    switch (p.kind) {
    case POLY_TETRAHEDRON:  return 4;
    case POLY_HEXAHEDRON:   return 6;
    case POLY_OCTAHEDRON:   return 8;
    case POLY_DODECAHEDRON: return 12;
    case POLY_ICOSAHEDRON:  return 20;
    case POLY_PRISM:        return p.sides + 2;      // the sides plus two caps
    case POLY_ANTIPRISM:    return 2 * p.sides + 2;  // triangles between the caps
    case POLY_BIPYRAMID:    return 2 * p.sides;      // two cones of triangles
    default:                return p.sides + 1;      // pyramid: sides plus a base
    }
}

/* Vertices */

static void push(std::vector<Vec3> *out, float x, float y, float z) {
    out->push_back(vec3(x, y, z));
}

/* The eight cube corners, shared by the cube and the dodecahedron. */
static void push_cube_corners(std::vector<Vec3> *out, float a) {
    for (int i = 0; i < 8; ++i) {
        push(out, (i & 1) ? a : -a, (i & 2) ? a : -a, (i & 4) ? a : -a);
    }
}

static void platonic_vertices(int kind, std::vector<Vec3> *out) {
    /* The golden ratio, which is what makes the last two solids regular. */
    const float phi = 1.61803398874989f;
    const float inv = 1.0f / phi;

    switch (kind) {
    case POLY_TETRAHEDRON:
        /* Four alternating corners of a cube. */
        push(out,  1.0f,  1.0f,  1.0f);
        push(out,  1.0f, -1.0f, -1.0f);
        push(out, -1.0f,  1.0f, -1.0f);
        push(out, -1.0f, -1.0f,  1.0f);
        break;

    case POLY_HEXAHEDRON:
        push_cube_corners(out, 1.0f);
        break;

    case POLY_OCTAHEDRON:
        push(out,  1.0f, 0.0f, 0.0f); push(out, -1.0f, 0.0f, 0.0f);
        push(out, 0.0f,  1.0f, 0.0f); push(out, 0.0f, -1.0f, 0.0f);
        push(out, 0.0f, 0.0f,  1.0f); push(out, 0.0f, 0.0f, -1.0f);
        break;

    case POLY_DODECAHEDRON:
        /* A cube with three golden rectangles threaded through it. */
        push_cube_corners(out, 1.0f);
        for (int sx = -1; sx <= 1; sx += 2) {
            for (int sy = -1; sy <= 1; sy += 2) {
                push(out, 0.0f, (float)sx * inv, (float)sy * phi);
                push(out, (float)sx * inv, (float)sy * phi, 0.0f);
                push(out, (float)sx * phi, 0.0f, (float)sy * inv);
            }
        }
        break;

    default: /* icosahedron: three orthogonal golden rectangles */
        for (int sx = -1; sx <= 1; sx += 2) {
            for (int sy = -1; sy <= 1; sy += 2) {
                push(out, 0.0f, (float)sx, (float)sy * phi);
                push(out, (float)sx, (float)sy * phi, 0.0f);
                push(out, (float)sx * phi, 0.0f, (float)sy);
            }
        }
        break;
    }
}

static void family_vertices(int kind, int sides, std::vector<Vec3> *out) {
    float half = 0.5f;
    float step = 2.0f * POLY_PI / (float)sides;

    if (kind == POLY_BIPYRAMID || kind == POLY_PYRAMID) {
        /* The n-gon sits at the waist of a bipyramid and at the foot of a
         * pyramid, so only the apexes differ. */
        float ring_z = (kind == POLY_PYRAMID) ? -half : 0.0f;
        for (int i = 0; i < sides; ++i) {
            float a = step * (float)i;
            push(out, cosf(a), sinf(a), ring_z);
        }
        push(out, 0.0f, 0.0f, half * (kind == POLY_PYRAMID ? 1.0f : 1.0f));
        if (kind == POLY_BIPYRAMID) push(out, 0.0f, 0.0f, -half);
        return;
    }

    /* Prism and antiprism: two rings. The antiprism turns the top one by half
     * a step, which is what replaces its rectangles with triangles. */
    float twist = (kind == POLY_ANTIPRISM) ? step * 0.5f : 0.0f;
    for (int i = 0; i < sides; ++i) {
        float a = step * (float)i;
        push(out, cosf(a), sinf(a), -half);
        push(out, cosf(a + twist), sinf(a + twist), half);
    }
}

bool polyhedron_build(const PolyhedronParams &params, Mesh *out, std::string *error) {
    PolyhedronParams p = params;
    polyhedron_params_clamp(&p);

    std::vector<Vec3> points;
    if (polyhedron_kind_takes_sides(p.kind)) family_vertices(p.kind, p.sides, &points);
    else platonic_vertices(p.kind, &points);

    if (points.size() < 4) {
        if (error) *error = "That shape has too few vertices to enclose a volume.";
        return false;
    }

    /*
     * Scaled to the requested size across, which for the presets means the
     * widest span in any direction rather than an edge length - two solids at
     * the same setting should look the same size.
     */
    Bounds b = bounds_empty();
    for (size_t i = 0; i < points.size(); ++i) bounds_add_point(&b, points[i]);
    Vec3 span = bounds_size(b);

    float widest = span.x > span.y ? span.x : span.y;
    if (span.z > widest) widest = span.z;
    if (widest < 1e-6f) {
        if (error) *error = "That shape came out flat.";
        return false;
    }

    float scale_xy = p.size_mm / widest;
    float scale_z = scale_xy;
    if (polyhedron_kind_takes_height(p.kind)) {
        /* The families get their height from its own field, so the ring is
         * sized across and the axis is sized separately. */
        float ring = span.x > span.y ? span.x : span.y;
        scale_xy = p.size_mm / ring;
        scale_z = p.height_mm / span.z;
    }

    for (size_t i = 0; i < points.size(); ++i) {
        points[i].x *= scale_xy;
        points[i].y *= scale_xy;
        points[i].z *= scale_z;
    }

    if (!csg_hull(points, out, error)) return false;

    /* Centred in x and y and standing on the plate, like every primitive. */
    Bounds solid = mesh_bounds(*out);
    Vec3 centre = bounds_center(solid);
    for (size_t i = 0; i < out->vertices.size(); ++i) {
        out->vertices[i].x -= centre.x;
        out->vertices[i].y -= centre.y;
        out->vertices[i].z -= solid.min.z;
    }
    for (size_t i = 0; i < out->edges.size(); ++i) {
        out->edges[i].x -= centre.x;
        out->edges[i].y -= centre.y;
        out->edges[i].z -= solid.min.z;
    }
    return true;
}
