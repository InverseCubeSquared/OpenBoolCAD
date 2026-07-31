/*
 * Headless checks for the geometry pipeline: primitive solidity, the repair
 * pass and the boolean merge. No SDL, no GL, no ImGui, so a failure here points
 * at the geometry and nothing else.
 *
 * Build and run with: make test
 */

#include <stdio.h>
#include <time.h>

#include "bevel.h"
#include "polyhedron.h"
#include "csg.h"
#include "export_stl.h"
#include "gear.h"
#include "text3d.h"
#include "workplane.h"
#include "import_stl.h"
#include "import_svg.h"
#include "mesh_repair.h"
#include "project.h"
#include "scene.h"
#include "undo.h"

static int failures = 0;
static int checks = 0;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void check(bool ok, const char *what) {
    checks += 1;
    if (ok) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        failures += 1;
    }
}

/* Every primitive must come out of the builder already solid: the boolean
 * stage cannot do anything with an open shell. */
static void test_primitives_are_solid(void) {
    printf("primitives are closed solids\n");
    for (int k = 0; k < PRIM_COUNT; ++k) {
        Mesh m = mesh_make_primitive((PrimitiveKind)k);
        IndexedMesh indexed;
        MeshRepairReport r;
        double t0 = now_ms();
        bool solid = mesh_repair(m, &indexed, &r);
        double ms = now_ms() - t0;

        char label[160];
        snprintf(label, sizeof(label), "%s: %d tris, solid=%d, repair=[%s] in %.1f ms",
                 primitive_name((PrimitiveKind)k), r.input_triangles, solid ? 1 : 0,
                 mesh_repair_summary(r).c_str(), ms);
        check(solid, label);

        /* A builder that needs repairing is a bug in the builder. */
        char clean[160];
        snprintf(clean, sizeof(clean), "%s needs no repair",
                 primitive_name((PrimitiveKind)k));
        check(r.removed_degenerate == 0 && r.filled_holes == 0 &&
              r.flipped_triangles == 0 && r.flipped_components == 0 &&
              r.nonmanifold_edges == 0 && r.open_edges == 0, clean);
    }
}

/* Signed volume by the divergence theorem, over the triangle soup. Positive
 * when the winding faces outward, so it checks orientation and size at once. */
static float mesh_volume(const Mesh &m) {
    double v = 0.0;
    for (size_t i = 0; i + 2 < m.vertices.size(); i += 3) {
        Vec3 a = m.vertices[i];
        Vec3 b = m.vertices[i + 1];
        Vec3 c = m.vertices[i + 2];
        v += (double)vec3_dot(a, vec3_cross(b, c)) / 6.0;
    }
    return (float)v;
}

/*
 * Genus from the Euler characteristic. A closed triangle mesh has E = 3F/2, so
 * chi = V - F/2; a solid with one through hole is genus 1. This is what tells
 * a gear whose bore actually went through from one that merely has a circle
 * drawn on its faces.
 */
static int mesh_genus(const IndexedMesh &m) {
    int verts = (int)m.positions.size();
    int faces = (int)(m.tris.size() / 3);
    int chi = verts - faces / 2;
    return (2 - chi) / 2;
}

/* The wedge is half of its own bounding box, and wound outward. */
static void test_wedge(void) {
    printf("wedge\n");

    Mesh w = mesh_make_wedge(20.0f, 20.0f);
    Bounds b = mesh_bounds(w);
    Vec3 size = bounds_size(b);
    check(fabsf(size.x - 20.0f) < 1e-4f && fabsf(size.y - 20.0f) < 1e-4f &&
          fabsf(size.z - 20.0f) < 1e-4f, "fills a 20 mm box");
    check(fabsf(b.min.z) < 1e-4f, "sits on the workplane");

    float volume = mesh_volume(w);
    char label[128];
    snprintf(label, sizeof(label), "volume %.1f mm3 is half the box", volume);
    check(fabsf(volume - 4000.0f) < 1.0f, label);
}

/* The gear generator has to produce a closed solid, at the size its parameters
 * describe, with the bore actually cut through. */
static void test_gear(void) {
    printf("gear generator\n");

    GearParams p;
    gear_params_init(&p);

    Mesh gear;
    std::string error;
    double t0 = now_ms();
    bool built = gear_build(p, &gear, &error);
    double ms = now_ms() - t0;

    char label[200];
    snprintf(label, sizeof(label), "default gear built in %.1f ms%s%s", ms,
             built ? "" : ": ", built ? "" : error.c_str());
    check(built, label);
    if (!built) return;

    IndexedMesh indexed;
    MeshRepairReport r;
    check(mesh_repair(gear, &indexed, &r), "gear is a closed solid");

    /* Pitch diameter is module x teeth, and the tips stand one module past it. */
    Bounds b = mesh_bounds(gear);
    Vec3 size = bounds_size(b);
    float outside = gear_outside_diameter(p);
    snprintf(label, sizeof(label), "outside diameter %.2f mm matches %.2f", size.x, outside);
    check(fabsf(size.x - outside) < 0.25f && fabsf(size.y - outside) < 0.25f, label);

    snprintf(label, sizeof(label), "thickness %.2f mm, standing on the workplane", size.z);
    check(fabsf(size.z - p.thickness_mm) < 1e-3f && fabsf(b.min.z) < 1e-3f, label);

    int genus = mesh_genus(indexed);
    snprintf(label, sizeof(label), "bore is a through hole (genus %d)", genus);
    check(genus == 1, label);

    /* Without a bore it is a plain solid again. */
    GearParams solid_gear = p;
    solid_gear.bore_mm = 0.0f;
    Mesh nobore;
    check(gear_build(solid_gear, &nobore, &error), "gear with no bore builds");
    IndexedMesh nobore_indexed;
    MeshRepairReport nobore_report;
    if (mesh_repair(nobore, &nobore_indexed, &nobore_report)) {
        int g = mesh_genus(nobore_indexed);
        snprintf(label, sizeof(label), "no bore means no hole (genus %d)", g);
        check(g == 0, label);
    } else {
        check(false, "gear with no bore is a closed solid");
    }

    /*
     * Few teeth at a high pressure angle is where the flanks would cross and
     * the contour would self intersect, so the tip has to be capped where they
     * meet. Out of range values must clamp rather than fail.
     */
    GearParams pointed;
    gear_params_init(&pointed);
    pointed.teeth = 2;                 // clamps to 4
    pointed.pressure_angle_deg = 40.0f; // clamps to 35
    pointed.bore_mm = 500.0f;          // clamps to leave a rim
    Mesh tiny;
    if (gear_build(pointed, &tiny, &error)) {
        IndexedMesh ti;
        MeshRepairReport tr;
        check(mesh_repair(tiny, &ti, &tr), "pointed-tooth gear is still a closed solid");
    } else {
        check(false, (std::string("pointed-tooth gear: ") + error).c_str());
    }

    /*
     * The point of a part is that it merges, and sixteen thin teeth are
     * exactly the geometry a boolean trips over. Dropped 1 mm below the block
     * so it cuts a clean pocket rather than leaving the two bottom faces
     * coplanar, which is a different edge case than the one under test.
     */
    Mesh sunk = gear;
    for (size_t i = 0; i < sunk.vertices.size(); ++i) sunk.vertices[i].z -= 1.0f;

    std::vector<Mesh> positives, negatives;
    positives.push_back(mesh_make_cube(40.0f));
    negatives.push_back(sunk);

    Mesh pocket;
    std::string merge_error, merge_note;
    if (csg_merge(positives, negatives, &pocket, &merge_error, &merge_note)) {
        IndexedMesh pi;
        MeshRepairReport pr;
        check(mesh_repair(pocket, &pi, &pr), "block minus gear is a closed solid");
    } else {
        check(false, (std::string("block minus gear: ") + merge_error).c_str());
    }

    /* A big gear must stay well formed, since the root and base circles swap
     * order past about 42 teeth at 20 degrees. */
    GearParams big;
    gear_params_init(&big);
    big.teeth = 120;
    Mesh big_mesh;
    if (gear_build(big, &big_mesh, &error)) {
        IndexedMesh bi;
        MeshRepairReport br;
        check(mesh_repair(big_mesh, &bi, &br), "120 tooth gear is a closed solid");
    } else {
        check(false, (std::string("120 tooth gear: ") + error).c_str());
    }
}

/*
 * Editing planes, per PLAN.md. The payoff of an object-relative plane is that
 * a rotated part measures its own size rather than the size of the world box
 * around it, so that is what these check.
 */
static void test_workplane(void) {
    printf("editing plane\n");

    /* A frame built from a normal has to be orthonormal, or every box drawn in
     * it would be sheared. */
    Workplane p = workplane_from_normal(vec3(0.0f, 0.0f, 0.0f),
                                        vec3_normalized(vec3(1.0f, 2.0f, 3.0f)));
    Vec3 ax = workplane_axis(p, 0);
    Vec3 ay = workplane_axis(p, 1);
    Vec3 az = workplane_axis(p, 2);
    check(fabsf(vec3_length(ax) - 1.0f) < 1e-4f && fabsf(vec3_length(ay) - 1.0f) < 1e-4f &&
          fabsf(vec3_length(az) - 1.0f) < 1e-4f, "face plane axes are unit length");
    check(fabsf(vec3_dot(ax, ay)) < 1e-4f && fabsf(vec3_dot(ay, az)) < 1e-4f &&
          fabsf(vec3_dot(ax, az)) < 1e-4f, "face plane axes are perpendicular");
    check(fabsf(vec3_dot(az, vec3_normalized(vec3(1.0f, 2.0f, 3.0f))) - 1.0f) < 1e-4f,
          "face plane Z is the face normal");

    Vec3 sample = vec3(3.0f, -7.0f, 11.0f);
    Vec3 round_trip = workplane_to_world(p, workplane_from_world(p, sample));
    check(vec3_length(vec3_sub(round_trip, sample)) < 1e-3f, "plane coordinates round trip");

    /* A cube turned 45 degrees about Z. */
    Scene s;
    scene_new_empty(&s);
    int root = s.roots.empty() ? OBC_NO_NODE : s.roots[0];
    int id = scene_add_object(&s, root, "Cube", mesh_make_cube(20.0f), POLARITY_POSITIVE);
    SceneNode *n = scene_node(&s, id);
    n->rotation = vec3(0.0f, 0.0f, 45.0f);
    scene_select_only(&s, id);

    Bounds world = scene_selection_bounds(&s);
    Vec3 world_size = bounds_size(world);
    char label[160];
    snprintf(label, sizeof(label), "world box of a turned cube is %.2f wide, not 20", world_size.x);
    check(world_size.x > 28.0f && world_size.x < 28.6f, label);

    Mat3 object = scene_world_rotation(&s, id);
    Bounds own = scene_selection_bounds_in(&s, mat3_transposed(object));
    Vec3 own_size = bounds_size(own);
    snprintf(label, sizeof(label), "object plane box is %.2f x %.2f x %.2f",
             own_size.x, own_size.y, own_size.z);
    check(fabsf(own_size.x - 20.0f) < 1e-3f && fabsf(own_size.y - 20.0f) < 1e-3f &&
          fabsf(own_size.z - 20.0f) < 1e-3f, label);

    /*
     * Scaling in that plane is the case the world-axis version got wrong: it
     * has to stretch the part along its own X, leaving the other two alone.
     */
    Vec3 anchor = mat3_apply(object, own.min);
    scene_scale_selection(&s, anchor, vec3(2.0f, 1.0f, 1.0f), object);

    Bounds scaled = scene_selection_bounds_in(&s, mat3_transposed(object));
    Vec3 scaled_size = bounds_size(scaled);
    snprintf(label, sizeof(label), "scaled in its own plane to %.2f x %.2f x %.2f",
             scaled_size.x, scaled_size.y, scaled_size.z);
    check(fabsf(scaled_size.x - 40.0f) < 1e-2f && fabsf(scaled_size.y - 20.0f) < 1e-2f &&
          fabsf(scaled_size.z - 20.0f) < 1e-2f, label);

    /* The anchored corner must not have moved, or the part would drift off
     * whatever it was sitting on. */
    Vec3 anchor_now = mat3_apply(object, scaled.min);
    check(vec3_length(vec3_sub(anchor_now, anchor)) < 1e-2f, "the anchored corner stayed put");

    /* SHIFT+P needs a face normal, and it has to point back at the ray. */
    Scene flat;
    scene_new_empty(&flat);
    int flat_root = flat.roots.empty() ? OBC_NO_NODE : flat.roots[0];
    scene_add_object(&flat, flat_root, "Cube", mesh_make_cube(20.0f), POLARITY_POSITIVE);

    float distance = 0.0f;
    Vec3 normal;
    int hit = scene_pick_surface(&flat, vec3(0.0f, 0.0f, 100.0f), vec3(0.0f, 0.0f, -1.0f),
                                 &distance, &normal);
    check(hit != OBC_NO_NODE, "ray from above hits the cube");
    snprintf(label, sizeof(label), "top face normal is (%.2f, %.2f, %.2f)",
             normal.x, normal.y, normal.z);
    check(fabsf(normal.z - 1.0f) < 1e-3f, label);
    check(fabsf(distance - 80.0f) < 1e-3f, "hit distance lands on the top face");

    /* SHIFT+P centres the plane on the face, so an off centre hit still has to
     * come back with the middle of the face. */
    Vec3 off_centre = vec3(7.0f, -6.0f, 20.0f);
    Vec3 middle = scene_face_center(&flat, hit, off_centre, vec3(0.0f, 0.0f, 1.0f));
    snprintf(label, sizeof(label), "face centre is (%.2f, %.2f, %.2f) from an off centre hit",
             middle.x, middle.y, middle.z);
    check(fabsf(middle.x) < 1e-3f && fabsf(middle.y) < 1e-3f &&
          fabsf(middle.z - 20.0f) < 1e-3f, label);

    /* A side face must give its own centre, not the top's. */
    Vec3 side = scene_face_center(&flat, hit, vec3(10.0f, 3.0f, 4.0f), vec3(1.0f, 0.0f, 0.0f));
    snprintf(label, sizeof(label), "side face centre is (%.2f, %.2f, %.2f)",
             side.x, side.y, side.z);
    check(fabsf(side.x - 10.0f) < 1e-3f && fabsf(side.y) < 1e-3f &&
          fabsf(side.z - 10.0f) < 1e-3f, label);
}

/*
 * A cut that lands almost flush leaves a paper-thin wall. The merge has to
 * drop those without touching a wall anyone actually modelled.
 */
static void test_thin_walls(void) {
    printf("thin wall removal\n");

    /* A 20 mm cube cut by the same cube offset sideways: what survives is a
     * slab exactly as thick as the offset. */
    Mesh block = mesh_make_cube(20.0f);

    struct Case { float thickness; bool survives; };
    Case cases[] = { { 0.005f, false }, { 0.002f, false }, { 1.0f, true }, { 0.5f, true } };

    for (int i = 0; i < 4; ++i) {
        Mesh shifted = mesh_make_cube(20.0f);
        for (size_t v = 0; v < shifted.vertices.size(); ++v) {
            shifted.vertices[v].x += cases[i].thickness;
        }

        std::vector<Mesh> pos, neg;
        pos.push_back(block);
        neg.push_back(shifted);

        Mesh result;
        std::string error, note;
        bool merged = csg_merge(pos, neg, &result, &error, &note);

        char label[220];
        if (cases[i].survives) {
            Bounds b = merged ? mesh_bounds(result) : bounds_empty();
            float thick = merged ? bounds_size(b).x : 0.0f;
            snprintf(label, sizeof(label), "a %g mm wall survives (measured %.4f mm)",
                     cases[i].thickness, thick);
            check(merged && fabsf(thick - cases[i].thickness) < 1e-3f, label);
        } else {
            snprintf(label, sizeof(label), "a %g mm wall is dropped (%s)",
                     cases[i].thickness, merged ? "it survived" : error.c_str());
            check(!merged, label);
        }
    }

    /*
     * The case that matters: a solid body with a thin fin standing on it, where
     * the body has to come through and only the fin goes.
     *
     * The block spans x -10..10, z 0..20. Cutting with the same block shifted
     * 0.005 mm in -x and 5 mm up leaves a full width base below z = 5, plus a
     * 0.005 x 20 x 15 fin at x 9.995..10 standing on it.
     */
    Mesh body = mesh_make_cube(20.0f);
    Mesh cutter = mesh_make_cube(20.0f);
    for (size_t v = 0; v < cutter.vertices.size(); ++v) {
        cutter.vertices[v].x -= 0.005f;
        cutter.vertices[v].z += 5.0f;
    }

    std::vector<Mesh> pos2, neg2;
    pos2.push_back(body);
    neg2.push_back(cutter);

    Mesh mixed;
    std::string error2, note2;
    bool ok = csg_merge(pos2, neg2, &mixed, &error2, &note2);
    check(ok, ok ? "body with a fin still merges" : error2.c_str());

    if (ok) {
        Bounds b = mesh_bounds(mixed);
        Vec3 size = bounds_size(b);
        char label[220];

        snprintf(label, sizeof(label), "the base is kept in full, %.3f x %.3f mm",
                 size.x, size.y);
        check(fabsf(size.x - 20.0f) < 0.02f && fabsf(size.y - 20.0f) < 0.02f, label);

        snprintf(label, sizeof(label), "the fin is gone, height is %.3f mm not 20", size.z);
        check(size.z < 5.1f, label);

        snprintf(label, sizeof(label), "cleanup is reported: \"%s\"", note2.c_str());
        check(note2.find("walls under") != std::string::npos, label);

        IndexedMesh indexed;
        MeshRepairReport r;
        check(mesh_repair(mixed, &indexed, &r), "the cleaned result is still a closed solid");
    }
}

/* Mean x of the points near the top, minus the mean near the bottom: how far
 * the tops of the letters sit right of their feet. */
static float contour_lean(const std::vector<std::vector<Vec2> > &contours) {
    float min_y = 0.0f, max_y = 0.0f;
    bool any = false;
    for (size_t g = 0; g < contours.size(); ++g) {
        for (size_t v = 0; v < contours[g].size(); ++v) {
            float y = contours[g][v].y;
            if (!any) { min_y = max_y = y; any = true; continue; }
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    if (!any || max_y - min_y < 1e-6f) return 0.0f;

    float high_cut = min_y + 0.7f * (max_y - min_y);
    float low_cut = min_y + 0.3f * (max_y - min_y);
    double high_sum = 0.0, low_sum = 0.0;
    int high_n = 0, low_n = 0;
    for (size_t g = 0; g < contours.size(); ++g) {
        for (size_t v = 0; v < contours[g].size(); ++v) {
            Vec2 p = contours[g][v];
            if (p.y > high_cut) { high_sum += p.x; high_n += 1; }
            else if (p.y < low_cut) { low_sum += p.x; low_n += 1; }
        }
    }
    if (!high_n || !low_n) return 0.0f;
    return (float)(high_sum / high_n - low_sum / low_n);
}

static float contour_height(const std::vector<std::vector<Vec2> > &contours) {
    float min_y = 0.0f, max_y = 0.0f;
    bool any = false;
    for (size_t g = 0; g < contours.size(); ++g) {
        for (size_t v = 0; v < contours[g].size(); ++v) {
            float y = contours[g][v].y;
            if (!any) { min_y = max_y = y; any = true; continue; }
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    return any ? (max_y - min_y) : 0.0f;
}

/* Extruded text: real glyph outlines, closed solids, counters as holes. */
static void test_text(void) {
    printf("text generator\n");

    TextParams p;
    text_params_init(&p);
    snprintf(p.text, sizeof(p.text), "%s", "Ao");

    std::vector<std::vector<Vec2> > contours;
    std::string error;
    bool ok = text_contours(p, &contours, &error);
    char label[200];
    snprintf(label, sizeof(label), "\"Ao\" gives %d contours%s%s", (int)contours.size(),
             ok ? "" : ": ", ok ? "" : error.c_str());
    /* A needs an outline plus its counter, o needs two: four in all. */
    check(ok && contours.size() == 4, label);

    Mesh mesh;
    ok = text_build(p, &mesh, &error);
    check(ok, ok ? "text extrudes" : error.c_str());
    if (ok) {
        IndexedMesh indexed;
        MeshRepairReport r;
        check(mesh_repair(mesh, &indexed, &r), "extruded text is a closed solid");

        Bounds b = mesh_bounds(mesh);
        Vec3 size = bounds_size(b);
        snprintf(label, sizeof(label), "cap height %.2f mm for a 10 mm setting", size.y);
        check(size.y > 6.0f && size.y < 11.0f, label);
        snprintf(label, sizeof(label), "depth %.2f mm", size.z);
        check(fabsf(size.z - p.depth_mm) < 1e-3f, label);

        /*
         * Genus is a per component idea and "Ao" is two separate letters, so
         * the Euler characteristic is what generalises: two components with one
         * counter each give (2-2) + (2-2) = 0.
         */
        int verts = (int)indexed.positions.size();
        int faces = (int)(indexed.tris.size() / 3);
        int chi = verts - faces / 2;
        snprintf(label, sizeof(label), "both counters are real holes (chi %d)", chi);
        check(chi == 0, label);
    }

    /* Every family has to produce something, or the dialog offers dead ends. */
    for (int family = 0; family < TEXT_FONT_COUNT; ++family) {
        TextParams f;
        text_params_init(&f);
        f.family = family;
        snprintf(f.text, sizeof(f.text), "%s", "Ag");

        std::vector<std::vector<Vec2> > c;
        std::string e;
        snprintf(label, sizeof(label), "%s has outlines%s%s", text_font_name(family),
                 text_contours(f, &c, &e) ? "" : ": ", e.c_str());
        check(!c.empty(), label);
    }

    /*
     * Italic is a shear. Comparing overall width would not catch it: for a word
     * whose leftmost and rightmost extremes are both near the cap line, a shear
     * moves them by the same amount and the width barely changes. What it does
     * do is push the top of the letters right of their feet, so that is what is
     * measured.
     */
    TextParams upright, slanted;
    text_params_init(&upright);
    slanted = upright;
    slanted.italic = true;

    std::vector<std::vector<Vec2> > a, b2;
    std::string ea, eb;
    if (text_contours(upright, &a, &ea) && text_contours(slanted, &b2, &eb)) {
        float lean_a = contour_lean(a);
        float lean_b = contour_lean(b2);
        snprintf(label, sizeof(label), "italic leans the tops right (%.2f mm vs %.2f mm)",
                 lean_b, lean_a);
        check(lean_b > lean_a + 0.5f, label);

        float tall_a = contour_height(a);
        float tall_b = contour_height(b2);
        snprintf(label, sizeof(label), "italic keeps the height (%.2f vs %.2f)",
                 tall_b, tall_a);
        check(fabsf(tall_b - tall_a) < 1e-3f, label);
    } else {
        check(false, "italic comparison could not be built");
    }

    /* Empty text is a refusal, not an empty solid. */
    TextParams blank;
    text_params_init(&blank);
    blank.text[0] = 0;
    Mesh nothing;
    std::string blank_error;
    check(!text_build(blank, &nothing, &blank_error), "empty text is refused");
}

/* Bevelling: the volume has to go down by the right amount, the result has to
 * stay a solid, and a concave edge has to fill rather than cut. */
static void test_bevel(void) {
    printf("bevel tool\n");

    Mesh cube = mesh_make_cube(20.0f);
    std::vector<BevelEdge> edges;
    bevel_collect_edges(cube, &edges);

    char label[220];
    snprintf(label, sizeof(label), "a cube offers %d bevellable edges", (int)edges.size());
    check(edges.size() == 12, label);

    int convex = 0;
    for (size_t i = 0; i < edges.size(); ++i) if (edges[i].convex) convex += 1;
    snprintf(label, sizeof(label), "all %d of them are convex", convex);
    check(convex == 12, label);

    /* One edge, rounded. A quarter cylinder of radius r and length L is
     * replaced by nothing, so the volume drops by (1 - pi/4) * r^2 * L. */
    float r = 2.0f;
    std::vector<int> one;
    one.push_back(0);

    Mesh rounded;
    std::string error;
    bool ok = bevel_apply(cube, edges, one, r, 16, &rounded, &error);
    check(ok, ok ? "one edge bevels" : error.c_str());

    if (ok) {
        IndexedMesh indexed;
        MeshRepairReport report;
        check(mesh_repair(rounded, &indexed, &report), "the bevelled cube is a closed solid");

        float before = mesh_volume(cube);
        float after = mesh_volume(rounded);
        float expected = (1.0f - 3.14159265f / 4.0f) * r * r * 20.0f;
        snprintf(label, sizeof(label), "removed %.2f mm3, expected about %.2f",
                 before - after, expected);
        check(fabsf((before - after) - expected) < 0.6f, label);

        Bounds b = mesh_bounds(rounded);
        Vec3 size = bounds_size(b);
        snprintf(label, sizeof(label), "the cube is still 20 mm across (%.2f x %.2f x %.2f)",
                 size.x, size.y, size.z);
        check(fabsf(size.x - 20.0f) < 1e-2f && fabsf(size.y - 20.0f) < 1e-2f &&
              fabsf(size.z - 20.0f) < 1e-2f, label);
    }

    /* One segment is a chamfer: a right prism, so exactly half the corner box. */
    Mesh chamfered;
    if (bevel_apply(cube, edges, one, r, 1, &chamfered, &error)) {
        float removed = mesh_volume(cube) - mesh_volume(chamfered);
        float expected = 0.5f * r * r * 20.0f;
        snprintf(label, sizeof(label), "a one segment bevel is a chamfer (%.2f vs %.2f)",
                 removed, expected);
        check(fabsf(removed - expected) < 0.4f, label);
    } else {
        check(false, (std::string("chamfer: ") + error).c_str());
    }

    /* Every edge at once, which is where cutters meeting at a vertex have to
     * resolve against each other rather than tear the solid. */
    std::vector<int> all;
    for (size_t i = 0; i < edges.size(); ++i) all.push_back((int)i);

    Mesh full;
    double t0 = now_ms();
    ok = bevel_apply(cube, edges, all, 1.5f, 8, &full, &error);
    double ms = now_ms() - t0;
    snprintf(label, sizeof(label), "all twelve edges bevel in %.0f ms%s%s", ms,
             ok ? "" : ": ", ok ? "" : error.c_str());
    check(ok, label);
    if (ok) {
        IndexedMesh fi;
        MeshRepairReport fr;
        check(mesh_repair(full, &fi, &fr), "the fully bevelled cube is a closed solid");
        check(mesh_volume(full) < mesh_volume(cube), "it lost volume rather than gaining it");
    }

    /*
     * A concave edge has to fill, not cut. An L shape made of two boxes has one
     * inside corner; bevelling it must add material.
     */
    std::vector<Mesh> pos;
    Mesh arm = mesh_make_cube(20.0f);
    for (size_t v = 0; v < arm.vertices.size(); ++v) {
        arm.vertices[v].x += 20.0f;
        arm.vertices[v].z -= 10.0f;
    }
    pos.push_back(mesh_make_cube(20.0f));
    pos.push_back(arm);

    Mesh shape;
    std::string merge_error, note;
    if (csg_merge(pos, std::vector<Mesh>(), &shape, &merge_error, &note)) {
        std::vector<BevelEdge> l_edges;
        bevel_collect_edges(shape, &l_edges);

        int concave = 0;
        for (size_t i = 0; i < l_edges.size(); ++i) if (!l_edges[i].convex) concave += 1;
        snprintf(label, sizeof(label), "the L shape has %d concave edges", concave);
        check(concave > 0, label);

        std::vector<int> inside;
        for (size_t i = 0; i < l_edges.size(); ++i) {
            if (!l_edges[i].convex) inside.push_back((int)i);
        }

        Mesh filled;
        if (bevel_apply(shape, l_edges, inside, 1.5f, 8, &filled, &error)) {
            IndexedMesh li;
            MeshRepairReport lr;
            check(mesh_repair(filled, &li, &lr), "the filleted L is a closed solid");
            snprintf(label, sizeof(label), "a concave edge gained %.2f mm3",
                     mesh_volume(filled) - mesh_volume(shape));
            check(mesh_volume(filled) > mesh_volume(shape) + 0.1f, label);
        } else {
            check(false, (std::string("concave bevel: ") + error).c_str());
        }
    } else {
        check(false, "the L shape could not be built");
    }

    /* Nothing selected is a refusal, not an empty result. */
    Mesh none_out;
    std::vector<int> none;
    check(!bevel_apply(cube, edges, none, 1.0f, 8, &none_out, &error),
          "bevelling nothing is refused");
}

/* Every polyhedron has to come out a closed solid with the face count the
 * dialog promises, at the size that was asked for. */
static void test_polyhedron(void) {
    printf("n-sided generator\n");

    struct Expect { int kind; int faces; };
    Expect presets[] = {
        { POLY_TETRAHEDRON, 4 }, { POLY_HEXAHEDRON, 6 }, { POLY_OCTAHEDRON, 8 },
        { POLY_DODECAHEDRON, 12 }, { POLY_ICOSAHEDRON, 20 }
    };

    for (int i = 0; i < 5; ++i) {
        PolyhedronParams p;
        polyhedron_params_init(&p);
        p.kind = presets[i].kind;
        p.size_mm = 20.0f;

        Mesh m;
        std::string error;
        char label[220];
        if (!polyhedron_build(p, &m, &error)) {
            snprintf(label, sizeof(label), "%s: %s", polyhedron_kind_name(p.kind),
                     error.c_str());
            check(false, label);
            continue;
        }

        IndexedMesh indexed;
        MeshRepairReport r;
        bool solid = mesh_repair(m, &indexed, &r);

        Bounds b = mesh_bounds(m);
        Vec3 size = bounds_size(b);
        float widest = size.x > size.y ? size.x : size.y;
        if (size.z > widest) widest = size.z;

        snprintf(label, sizeof(label), "%s is a %d face solid, %.2f mm across, on the plate",
                 polyhedron_kind_name(p.kind), polyhedron_face_count(p), widest);
        check(solid && fabsf(widest - 20.0f) < 0.05f && fabsf(b.min.z) < 1e-3f, label);

        /*
         * The hull triangulates every face, so counting distinct plane normals
         * is what recovers the face count a user would count by eye. This is
         * the check that a dodecahedron really has twelve pentagons rather
         * than thirty-six triangles arranged hopefully.
         */
        std::vector<Vec3> normals;
        for (size_t t = 0; t + 2 < m.vertices.size(); t += 3) {
            Vec3 n = vec3_normalized(vec3_cross(vec3_sub(m.vertices[t + 1], m.vertices[t]),
                                                vec3_sub(m.vertices[t + 2], m.vertices[t])));
            bool seen = false;
            for (size_t k = 0; k < normals.size(); ++k) {
                if (vec3_dot(normals[k], n) > 0.999f) { seen = true; break; }
            }
            if (!seen) normals.push_back(n);
        }
        snprintf(label, sizeof(label), "%s has %d distinct faces, expected %d",
                 polyhedron_kind_name(p.kind), (int)normals.size(), presets[i].faces);
        check((int)normals.size() == presets[i].faces, label);
    }

    /* The families, where the side count is the point. */
    struct Family { int kind; int sides; int faces; };
    Family families[] = {
        { POLY_PRISM, 6, 8 }, { POLY_PRISM, 20, 22 },
        { POLY_ANTIPRISM, 5, 12 }, { POLY_BIPYRAMID, 8, 16 }, { POLY_PYRAMID, 7, 8 }
    };

    for (int i = 0; i < 5; ++i) {
        PolyhedronParams p;
        polyhedron_params_init(&p);
        p.kind = families[i].kind;
        p.sides = families[i].sides;
        p.size_mm = 20.0f;
        p.height_mm = 12.0f;

        Mesh m;
        std::string error;
        char label[220];
        if (!polyhedron_build(p, &m, &error)) {
            check(false, (std::string(polyhedron_kind_name(p.kind)) + ": " + error).c_str());
            continue;
        }

        IndexedMesh indexed;
        MeshRepairReport r;
        bool solid = mesh_repair(m, &indexed, &r);

        Bounds b = mesh_bounds(m);
        Vec3 size = bounds_size(b);
        snprintf(label, sizeof(label), "%d sided %s: %d faces, %.1f mm tall, solid=%d",
                 p.sides, polyhedron_kind_name(p.kind), polyhedron_face_count(p),
                 size.z, solid ? 1 : 0);
        check(solid && polyhedron_face_count(p) == families[i].faces &&
              fabsf(size.z - 12.0f) < 0.05f, label);
    }

    /* Out of range values clamp rather than producing nothing. */
    PolyhedronParams odd;
    polyhedron_params_init(&odd);
    odd.kind = POLY_PRISM;
    odd.sides = 1;        // clamps to 3
    odd.size_mm = -5.0f;  // clamps up
    Mesh clamped;
    std::string error;
    check(polyhedron_build(odd, &clamped, &error),
          error.empty() ? "out of range parameters still build" : error.c_str());
}

/* Copy and paste have to produce independent deep copies, keeping the tree
 * shape and not aliasing the originals. */
static void test_copy_paste(void) {
    printf("copy and paste\n");

    Scene s;
    scene_new_empty(&s);
    int root = s.roots.empty() ? OBC_NO_NODE : s.roots[0];

    int group = scene_add_group(&s, root, "Assembly");
    int a = scene_add_object(&s, group, "Cube", mesh_make_cube(20.0f), POLARITY_POSITIVE);
    scene_add_object(&s, group, "Hole", mesh_make_cylinder(10.0f, 30.0f, 24), POLARITY_NEGATIVE);

    SceneNode *an = scene_node(&s, a);
    an->position = vec3(5.0f, 0.0f, 0.0f);

    std::vector<int> pick;
    pick.push_back(group);

    std::vector<SceneNode> fragment;
    scene_copy_subtrees(&s, pick, &fragment);

    char label[200];
    snprintf(label, sizeof(label), "a group of two copies as %d nodes", (int)fragment.size());
    check(fragment.size() == 3, label);

    size_t before = s.nodes.size();
    std::vector<int> pasted;
    scene_paste_subtrees(&s, root, fragment, &pasted);

    check(pasted.size() == 1, "the paste has one root");
    snprintf(label, sizeof(label), "three new nodes exist (%d)", (int)(s.nodes.size() - before));
    check(s.nodes.size() - before == 3, label);

    if (!pasted.empty()) {
        const SceneNode *copy = scene_node(&s, pasted[0]);
        check(copy && copy->kind == NODE_GROUP && copy->children.size() == 2,
              "the copied group kept its two children");

        /* The copy must be its own geometry, not a second name for the first. */
        if (copy && copy->children.size() == 2) {
            const SceneNode *child = scene_node(&s, copy->children[0]);
            check(child && child->id != a, "the child is a new node, not the original");
            check(child && !child->mesh.vertices.empty(), "the mesh came with it");
            check(child && fabsf(child->position.x - 5.0f) < 1e-4f,
                  "the transform came with it");

            /* Editing the copy must leave the original alone. */
            SceneNode *editable = scene_node(&s, copy->children[0]);
            editable->position.x = 99.0f;
            const SceneNode *original = scene_node(&s, a);
            check(original && fabsf(original->position.x - 5.0f) < 1e-4f,
                  "editing the copy does not touch the original");
        }
    }

    /*
     * Every node has to appear exactly once in the tree. scene_new_node
     * registers what it makes as a root, so a paste that re-parents without
     * clearing that entry leaves the node in two places at once - it then
     * renders twice, the second time without its parent's transform, which is
     * a copy left behind wherever the parent used to be.
     */
    for (size_t i = 0; i < s.nodes.size(); ++i) {
        if (s.nodes[i].kind == NODE_DELETED) continue;
        int id = (int)i;

        int appearances = 0;
        for (size_t r = 0; r < s.roots.size(); ++r) {
            if (s.roots[r] == id) appearances += 1;
        }
        for (size_t n = 0; n < s.nodes.size(); ++n) {
            if (s.nodes[n].kind == NODE_DELETED) continue;
            for (size_t c = 0; c < s.nodes[n].children.size(); ++c) {
                if (s.nodes[n].children[c] == id) appearances += 1;
            }
        }

        snprintf(label, sizeof(label), "node %d \"%s\" appears once in the tree, not %d",
                 id, s.nodes[i].name.c_str(), appearances);
        check(appearances == 1, label);
    }

    /* And a pasted child has to agree with its parent about the link. */
    if (!pasted.empty()) {
        const SceneNode *copy = scene_node(&s, pasted[0]);
        bool consistent = true;
        for (size_t c = 0; copy && c < copy->children.size(); ++c) {
            const SceneNode *child = scene_node(&s, copy->children[c]);
            if (!child || child->parent != copy->id) consistent = false;
        }
        check(consistent, "each pasted child points back at its parent");
    }

    /* Selecting a parent and its own child must not copy the child twice. */
    std::vector<int> overlapping;
    overlapping.push_back(group);
    overlapping.push_back(a);

    std::vector<SceneNode> nested;
    scene_copy_subtrees(&s, overlapping, &nested);
    snprintf(label, sizeof(label), "a nested selection still copies %d nodes, not more",
             (int)nested.size());
    check(nested.size() == 3, label);
}

/* The repair pass has to fix what it claims to fix. */
static void test_repair_fixes_defects(void) {
    printf("repair pass\n");

    /* A cube missing one face is an open shell that hole filling should close. */
    Mesh holed = mesh_make_cube(20.0f);
    holed.vertices.resize(holed.vertices.size() - 6); // drop two triangles = one face
    holed.normals.resize(holed.normals.size() - 6);
    IndexedMesh indexed;
    MeshRepairReport r;
    bool solid = mesh_repair(holed, &indexed, &r);
    check(solid, "cube with a missing face is closed again");
    check(r.filled_holes == 1, "exactly one hole reported");

    /* An inside out cube should be detected by signed volume and flipped. */
    Mesh flipped = mesh_make_cube(20.0f);
    for (size_t i = 0; i + 2 < flipped.vertices.size(); i += 3) {
        Vec3 tmp = flipped.vertices[i + 1];
        flipped.vertices[i + 1] = flipped.vertices[i + 2];
        flipped.vertices[i + 2] = tmp;
    }
    MeshRepairReport r2;
    bool solid2 = mesh_repair(flipped, &indexed, &r2);
    check(solid2, "inside out cube is still solid");
    check(r2.flipped_components == 1, "inside out shell was flipped");

    /* Duplicated triangles make edges non-manifold and must be dropped. */
    Mesh doubled = mesh_make_cube(20.0f);
    /* Snapshot the counts first: push_back into the same vector the loop bound
     * reads from never terminates. */
    size_t vertex_count = doubled.vertices.size();
    size_t normal_count = doubled.normals.size();
    for (size_t i = 0; i < vertex_count; ++i) doubled.vertices.push_back(doubled.vertices[i]);
    for (size_t i = 0; i < normal_count; ++i) doubled.normals.push_back(doubled.normals[i]);
    MeshRepairReport r3;
    bool solid3 = mesh_repair(doubled, &indexed, &r3);
    check(solid3, "cube with every triangle duplicated is solid");
    check(r3.removed_duplicate == 12, "12 duplicate triangles dropped");
}

/* Union and difference through the real merge entry point. */
static void test_merge(void) {
    printf("boolean merge\n");

    Mesh cube = mesh_make_cube(20.0f);
    Mesh hole = mesh_make_cylinder(10.0f, 40.0f, 48);

    std::vector<Mesh> pos, neg;
    pos.push_back(cube);
    neg.push_back(hole);

    std::string error, repair;
    Mesh out;
    double t0 = now_ms();
    bool ok = csg_merge(pos, neg, &out, &error, &repair);
    double ms = now_ms() - t0;

    char label[200];
    snprintf(label, sizeof(label), "cube minus cylinder: %s (%.1f ms, %d tris)",
             ok ? "ok" : error.c_str(), ms, (int)(out.vertices.size() / 3));
    check(ok, label);

    if (ok) {
        IndexedMesh indexed;
        MeshRepairReport r;
        check(mesh_repair(out, &indexed, &r), "merge result is itself a closed solid");
        check(!out.edges.empty(), "merge result has outline edges");
    }

    /* Two overlapping cubes unioned must not report a failure. */
    Mesh a = mesh_make_cube(20.0f);
    Mesh b = mesh_make_cube(20.0f);
    for (size_t i = 0; i < b.vertices.size(); ++i) b.vertices[i].x += 10.0f;
    std::vector<Mesh> two, none;
    two.push_back(a);
    two.push_back(b);
    Mesh unioned;
    check(csg_merge(two, none, &unioned, &error, &repair), "two overlapping cubes union");
}

/* The scene level merge, including history and the unmerge round trip. */
static void test_scene_merge_roundtrip(void) {
    printf("scene merge and unmerge\n");

    Scene s;
    scene_seed_demo(&s);

    /* Object 1 (cube) and SubSubObject1 (negative cylinder inside it). */
    int cube = OBC_NO_NODE, hole = OBC_NO_NODE;
    for (size_t i = 0; i < s.nodes.size(); ++i) {
        if (s.nodes[i].name == "Object 1") cube = (int)i;
        if (s.nodes[i].name == "SubSubObject1") hole = (int)i;
    }
    check(cube != OBC_NO_NODE && hole != OBC_NO_NODE, "demo scene has the expected nodes");

    s.selection.clear();
    s.selection.push_back(cube);
    s.selection.push_back(hole);

    std::string error, repair;
    double t0 = now_ms();
    bool ok = scene_merge_selection(&s, &error, &repair);
    double ms = now_ms() - t0;

    char label[200];
    snprintf(label, sizeof(label), "merge selection: %s (%.1f ms)", ok ? "ok" : error.c_str(), ms);
    check(ok, label);
    if (!ok) return;

    int merged = s.selection.empty() ? OBC_NO_NODE : s.selection[0];
    check(scene_node_is_merged(&s, merged), "result node is marked merged");

    const SceneNode *n = scene_node(&s, merged);
    check(n && n->children.size() == 2, "both sources kept as history");
    check(!scene_is_effectively_visible(&s, cube), "history is not selectable");

    check(scene_unmerge(&s, merged, &error), "unmerge");
    check(!scene_node_is_merged(&s, merged), "no longer merged");
    check(scene_is_effectively_visible(&s, cube), "history is selectable again");

    check(scene_remerge(&s, merged, &error, &repair), "remerge");
    check(scene_node_is_merged(&s, merged), "merged again");
}

/* Project files: a round trip has to preserve the tree, ids, meshes and the
 * merge history, since ids are what history refers to. */
static void test_project_roundtrip(void) {
    printf("project save and load\n");

    Scene s;
    scene_seed_demo(&s);

    /* Merge first, so the saved file carries a merged node plus its history. */
    int cube = OBC_NO_NODE, hole = OBC_NO_NODE;
    for (size_t i = 0; i < s.nodes.size(); ++i) {
        if (s.nodes[i].name == "Object 1") cube = (int)i;
        if (s.nodes[i].name == "SubSubObject1") hole = (int)i;
    }
    s.selection.clear();
    s.selection.push_back(cube);
    s.selection.push_back(hole);

    std::string error, repair;
    check(scene_merge_selection(&s, &error, &repair), "merge before saving");
    int merged = s.selection.empty() ? OBC_NO_NODE : s.selection[0];

    Camera cam;
    camera_init(&cam);
    cam.yaw = 12.5f;
    cam.distance = 321.0f;

    Thumbnail thumb;
    thumb.width = 4;
    thumb.height = 2;
    thumb.rgb.assign((size_t)(4 * 2 * 3), 0x40);

    const char *path = "/tmp/obc_test_project.obc";
    check(project_save(path, s, cam, thumb, &error), "save project");

    Scene loaded;
    Camera loaded_cam;
    camera_init(&loaded_cam);
    check(project_load(path, &loaded, &loaded_cam, &error), "load project");

    check(loaded.nodes.size() == s.nodes.size(), "node count preserved");
    check(loaded.roots.size() == s.roots.size(), "root count preserved");

    const SceneNode *a = scene_node(&s, merged);
    const SceneNode *b = scene_node(&loaded, merged);
    check(a && b, "merged node survives with the same id");
    if (a && b) {
        check(b->merged, "still marked merged");
        check(b->children.size() == a->children.size(), "history children preserved");
        check(b->mesh.vertices.size() == a->mesh.vertices.size(), "result mesh preserved");
        check(b->name == a->name, "name preserved");
    }

    /* Loaded meshes must be usable by the boolean stage, i.e. still solid. */
    if (b) {
        IndexedMesh indexed;
        MeshRepairReport r;
        check(mesh_repair(b->mesh, &indexed, &r), "loaded mesh is still a closed solid");
    }

    check(fabsf(loaded_cam.yaw - 12.5f) < 1e-3f && fabsf(loaded_cam.distance - 321.0f) < 1e-3f,
          "camera restored");

    /* History still works after a reload: that is the point of storing it. */
    check(scene_unmerge(&loaded, merged, &error), "unmerge after reload");
    check(scene_remerge(&loaded, merged, &error, &repair), "remerge after reload");

    Thumbnail back;
    check(project_read_thumbnail(path, &back, &error), "read thumbnail alone");
    check(back.width == 4 && back.height == 2 && back.rgb.size() == 24, "thumbnail preserved");

    /* A file that is not ours must be refused rather than misparsed. */
    FILE *junk = fopen("/tmp/obc_test_junk.obc", "wb");
    if (junk) {
        fwrite("not a project at all", 1, 20, junk);
        fclose(junk);
        Scene ignored;
        check(!project_load("/tmp/obc_test_junk.obc", &ignored, NULL, &error),
              "non-project file is rejected");
    }
}

/* STL export, including a group exported in merged state. */
static void test_stl_export(void) {
    printf("STL export\n");

    Scene s;
    scene_seed_demo(&s);

    int group = OBC_NO_NODE, cube = OBC_NO_NODE;
    for (size_t i = 0; i < s.nodes.size(); ++i) {
        if (s.nodes[i].name == "Group 1") group = (int)i;
        if (s.nodes[i].name == "Object 1") cube = (int)i;
    }

    std::string error, note;
    const char *path = "/tmp/obc_test_export.stl";

    std::vector<int> one;
    one.push_back(cube);
    check(export_stl(path, s, one, &error, &note), "export a single object");

    /* Header is 84 bytes and each triangle is 50, so the size pins the count. */
    FILE *f = fopen(path, "rb");
    long size = 0;
    unsigned char header[84];
    size_t got = 0;
    if (f) {
        got = fread(header, 1, sizeof(header), f);
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fclose(f);
    }
    check(got == sizeof(header), "STL header written");
    uint32_t tris = got == sizeof(header)
        ? (uint32_t)header[80] | ((uint32_t)header[81] << 8) |
          ((uint32_t)header[82] << 16) | ((uint32_t)header[83] << 24)
        : 0;
    check(tris == 12, "cube exports as 12 triangles");
    check(size == 84 + (long)tris * 50, "file size matches the triangle count");

    std::vector<int> as_group;
    as_group.push_back(group);
    check(export_stl(path, s, as_group, &error, &note), "export a group in merged state");

    std::vector<int> none;
    check(!export_stl(path, s, none, &error, &note), "empty selection is refused");
}

/* STL import, of a file this program wrote: export then import must round trip. */
static void test_stl_import(void) {
    printf("STL import\n");

    Scene s;
    scene_seed_demo(&s);
    int cube = OBC_NO_NODE;
    for (size_t i = 0; i < s.nodes.size(); ++i) {
        if (s.nodes[i].name == "Object 1") cube = (int)i;
    }

    std::string error, note;
    const char *path = "/tmp/obc_test_roundtrip.stl";
    std::vector<int> one;
    one.push_back(cube);
    check(export_stl(path, s, one, &error, &note), "export a cube for reimport");

    Mesh loaded;
    check(import_stl(path, &loaded, &error, &note), "import the binary STL back");
    check(loaded.vertices.size() == 36, "12 triangles round tripped");

    IndexedMesh indexed;
    MeshRepairReport r;
    check(mesh_repair(loaded, &indexed, &r), "imported mesh is a closed solid");
    check(indexed.positions.size() == 8, "welded back to 8 cube corners");
    check(!loaded.edges.empty(), "outline edges were derived");

    /* ASCII STL of a single triangle: the parser must not need binary. */
    FILE *f = fopen("/tmp/obc_test_ascii.stl", "wb");
    if (f) {
        fprintf(f, "solid test\n"
                   "facet normal 0 0 1\n outer loop\n"
                   "  vertex 0 0 0\n  vertex 10 0 0\n  vertex 0 10 0\n"
                   " endloop\nendfacet\nendsolid test\n");
        fclose(f);
    }
    Mesh ascii;
    check(import_stl("/tmp/obc_test_ascii.stl", &ascii, &error, &note), "import an ASCII STL");
    /* Not a triangle count: a lone open triangle is not a solid, so the repair
     * pass fan-fills its boundary. What matters is that the coordinates parsed. */
    Bounds ab = mesh_bounds(ascii);
    check(ab.valid && fabsf(ab.min.x) < 1e-4f && fabsf(ab.max.x - 10.0f) < 1e-4f &&
          fabsf(ab.max.y - 10.0f) < 1e-4f && fabsf(ab.max.z) < 1e-4f,
          "ASCII coordinates parsed into the right corner positions");

    Mesh missing;
    check(!import_stl("/tmp/obc_definitely_missing.stl", &missing, &error, &note),
          "a missing file is an error");
}

/* SVG import in both modes. */
static void test_svg_import(void) {
    printf("SVG import\n");

    /* 100x100 mm document: an outer square with a square hole, plus two shapes
     * with different greys for the height map. */
    FILE *f = fopen("/tmp/obc_test.svg", "wb");
    if (f) {
        fprintf(f,
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100mm\" height=\"100mm\""
            " viewBox=\"0 0 100 100\">\n"
            "  <path d=\"M 10 10 L 50 10 L 50 50 L 10 50 Z"
            " M 20 20 L 20 40 L 40 40 L 40 20 Z\" fill=\"#000000\"/>\n"
            "  <rect x=\"60\" y=\"10\" width=\"30\" height=\"30\" fill=\"#808080\"/>\n"
            "  <circle cx=\"30\" cy=\"75\" r=\"15\" fill=\"#ffffff\"/>\n"
            "</svg>\n");
        fclose(f);
    }

    SvgImportOptions options;
    svg_import_options_init(&options);
    check(options.height_mm == 10.0f, "outline mode defaults to 10 mm");

    SvgImportResult outline;
    std::string error, note;
    check(import_svg("/tmp/obc_test.svg", options, &outline, &error, &note),
          "import as outline");
    check(outline.meshes.size() == 1, "outline mode makes one solid");

    /* The document is 100 mm across and the artwork spans 10..90. */
    char dims[128];
    snprintf(dims, sizeof(dims), "artwork measures %.1f x %.1f mm",
             outline.width_mm, outline.height_mm);
    check(outline.width_mm > 75.0f && outline.width_mm < 85.0f, dims);

    if (!outline.meshes.empty()) {
        IndexedMesh indexed;
        MeshRepairReport r;
        check(mesh_repair(outline.meshes[0], &indexed, &r), "extruded outline is a closed solid");

        Bounds b = mesh_bounds(outline.meshes[0]);
        char zs[96];
        snprintf(zs, sizeof(zs), "extruded to %.2f mm", b.max.z - b.min.z);
        check(fabsf((b.max.z - b.min.z) - 10.0f) < 0.01f, zs);

        /* The hole has to survive: a solid square would have more volume than
         * the ring, so check the triangle count reflects an inner wall. */
        check(indexed.tris.size() / 3 >= 16, "hole kept its own walls");
    }

    SvgImportResult relief;
    options.mode = SVG_IMPORT_HEIGHTMAP;
    options.height_mm = 5.0f;
    options.base_mm = 1.0f;
    check(import_svg("/tmp/obc_test.svg", options, &relief, &error, &note),
          "import as height map");
    check(relief.meshes.size() == 3, "three greys become three bands");

    /* Black is tallest by default, white flattest. */
    float tallest = 0.0f, flattest = 1e9f;
    for (size_t i = 0; i < relief.meshes.size(); ++i) {
        Bounds b = mesh_bounds(relief.meshes[i]);
        float h = b.max.z - b.min.z;
        if (h > tallest) tallest = h;
        if (h < flattest) flattest = h;
    }
    char band[96];
    snprintf(band, sizeof(band), "bands span %.2f to %.2f mm", flattest, tallest);
    check(fabsf(tallest - 5.0f) < 0.01f && fabsf(flattest - 1.0f) < 0.01f, band);

    options.invert = true;
    SvgImportResult inverted;
    check(import_svg("/tmp/obc_test.svg", options, &inverted, &error, &note),
          "height map inverts");

    SvgImportResult ignored;
    check(!import_svg("/tmp/obc_definitely_missing.svg", options, &ignored, &error, &note),
          "a missing SVG is an error");
}

/* Align, mirror and the global undo stack. */
static void test_align_mirror_undo(void) {
    printf("align, mirror and undo\n");

    Scene s;
    scene_init(&s);
    int group = scene_add_group(&s, OBC_NO_NODE, "Main");

    /* Reference cube at the origin, a second cube offset on every axis. */
    int a = scene_add_object(&s, group, "A", mesh_make_cube(20.0f), POLARITY_POSITIVE);
    int b = scene_add_object(&s, group, "B", mesh_make_cube(10.0f), POLARITY_POSITIVE);
    scene_node(&s, b)->position = vec3(50.0f, 30.0f, 5.0f);

    s.selection.clear();
    s.selection.push_back(a); // first selected is the reference
    s.selection.push_back(b);

    Bounds ref = scene_node_bounds(&s, a);
    check(ref.valid, "reference has bounds");

    /* Align X to the low edge: B's minimum x must meet A's. */
    scene_align_selection(&s, a, 0, ALIGN_SLOT_MIN);
    Bounds moved = scene_node_bounds(&s, b);
    char msg[128];
    snprintf(msg, sizeof(msg), "X low edges meet (%.2f vs %.2f)", moved.min.x, ref.min.x);
    check(fabsf(moved.min.x - ref.min.x) < 1e-3f, msg);
    check(fabsf(moved.min.y - 25.0f) < 1e-3f, "other axes are untouched");

    /* The reference itself must never move. */
    Bounds ref_after = scene_node_bounds(&s, a);
    check(fabsf(ref_after.min.x - ref.min.x) < 1e-6f &&
          fabsf(ref_after.min.y - ref.min.y) < 1e-6f, "reference stays put");

    /* Centre on Y, then high edge on Z. */
    scene_align_selection(&s, a, 1, ALIGN_SLOT_CENTER);
    moved = scene_node_bounds(&s, b);
    check(fabsf(bounds_center(moved).y - bounds_center(ref).y) < 1e-3f, "Y centres meet");

    scene_align_selection(&s, a, 2, ALIGN_SLOT_MAX);
    moved = scene_node_bounds(&s, b);
    check(fabsf(moved.max.z - ref.max.z) < 1e-3f, "Z high edges meet");

    /* Mirror across X through the selection centre: the pair swaps sides. */
    Bounds before = scene_selection_bounds(&s);
    float plane = bounds_center(before).x;
    Bounds b_before = scene_node_bounds(&s, b);
    scene_mirror_selection(&s, 0, plane);
    Bounds b_after = scene_node_bounds(&s, b);

    snprintf(msg, sizeof(msg), "mirrored X spans %.2f..%.2f from %.2f..%.2f",
             b_after.min.x, b_after.max.x, b_before.min.x, b_before.max.x);
    check(fabsf(b_after.min.x - (2.0f * plane - b_before.max.x)) < 1e-3f, msg);
    check(fabsf((b_after.max.x - b_after.min.x) - (b_before.max.x - b_before.min.x)) < 1e-3f,
          "mirroring preserves size");
    check(scene_node(&s, b)->scale.x < 0.0f, "mirrored node has a negated axis");

    /* A mirrored volume must still be usable by the boolean stage. */
    std::vector<Mesh> pos, neg;
    scene_collect_meshes(&s, b, OBC_NO_NODE, false, &pos, &neg);
    check(pos.size() == 1, "mirrored node still contributes one volume");
    if (!pos.empty()) {
        IndexedMesh indexed;
        MeshRepairReport r;
        check(mesh_repair(pos[0], &indexed, &r), "mirrored mesh is still a closed solid");
        check(r.flipped_components == 1, "repair notices the inverted winding");
    }

    /* Undo stack: record, mutate, step back, step forward. */
    UndoStack undo;
    undo_init(&undo);
    check(!undo_can_step_back(&undo), "nothing to undo at the start");

    Vec3 original = scene_node(&s, b)->position;
    undo_record(&undo, s, "Move");
    scene_node(&s, b)->position = vec3(123.0f, 456.0f, 7.0f);
    check(undo_can_step_back(&undo), "an action can be undone");

    std::string label;
    check(undo_step_back(&undo, &s, &label), "step back");
    check(label == "Move", "label round trips");
    check(fabsf(scene_node(&s, b)->position.x - original.x) < 1e-6f, "position restored");
    check(undo_can_step_forward(&undo), "redo is available after an undo");

    check(undo_step_forward(&undo, &s, &label), "step forward");
    check(fabsf(scene_node(&s, b)->position.x - 123.0f) < 1e-6f, "redo reapplies");

    /* A fresh action must drop the redo branch. */
    undo_record(&undo, s, "Delete");
    check(!undo_can_step_forward(&undo), "a new action clears the redo branch");

    /* Undo has to restore the whole tree, not just transforms. */
    size_t before_count = s.roots.size();
    undo_record(&undo, s, "Add");
    scene_add_group(&s, OBC_NO_NODE, "Extra");
    check(s.roots.size() == before_count + 1, "group added");
    check(undo_step_back(&undo, &s, &label), "undo the add");
    check(s.roots.size() == before_count, "tree structure restored");
}

int main(void) {
    test_primitives_are_solid();
    test_wedge();
    test_gear();
    test_workplane();
    test_thin_walls();
    test_text();
    test_bevel();
    test_polyhedron();
    test_copy_paste();
    test_repair_fixes_defects();
    test_merge();
    test_scene_merge_roundtrip();
    test_project_roundtrip();
    test_stl_export();
    test_stl_import();
    test_svg_import();
    test_align_mirror_undo();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
