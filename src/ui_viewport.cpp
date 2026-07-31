#include "ui.h"

#include <string.h>

#include "app.h"
#include "imgui.h"
#include "undo.h"

/*
 * The 3D view is an ImGui window with no background: the GL scene is drawn
 * underneath into the same rectangle. The window exists so navigation, drops
 * from the bookshelf and the floating controls all go through normal ImGui
 * input handling instead of raw SDL events.
 *
 * The floating controls live in their own windows stacked on top, not in the
 * viewport window. A full region InvisibleButton decides whether it becomes the
 * active item at the point it is submitted, which is before any widget drawn
 * after it exists - so a widget sharing the window with it can be hovered but
 * never clicked. Separate windows get their own input priority and sidestep
 * that entirely.
 */

static const float SNAP_VALUES[] = { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 25.0f };
#define SNAP_COUNT ((int)(sizeof(SNAP_VALUES) / sizeof(SNAP_VALUES[0])))

#define HANDLE_PICK_RADIUS 11.0f
#define RING_PICK_RADIUS 9.0f
#define RING_PICK_STEPS 64
#define ROTATE_MIN_RADIUS_PX 12.0f

static float snap_value(float v, float step) {
    if (step <= 0.0f) return v;
    return floorf(v / step + 0.5f) * step;
}

/* Where a screen position lands on a horizontal plane. Bookshelf drops use this
 * to land a new object on the plate. */
static bool plane_hit(const Camera &c, int vw, int vh, float sx, float sy, float plane_z, Vec3 *out) {
    Vec3 origin, dir;
    camera_screen_ray(c, vw, vh, sx, sy, &origin, &dir);
    if (fabsf(dir.z) < 1e-6f) return false;
    float t = (plane_z - origin.z) / dir.z;
    if (t <= 0.0f) return false;
    *out = vec3_add(origin, vec3_mul(dir, t));
    return true;
}

static bool workplane_hit(const Camera &c, int vw, int vh, float sx, float sy, Vec3 *out) {
    return plane_hit(c, vw, vh, sx, sy, 0.0f, out);
}

/* Intersection with an arbitrary plane through "point" with normal "normal". */
static bool ray_plane(Vec3 origin, Vec3 dir, Vec3 point, Vec3 normal, Vec3 *out) {
    float denom = vec3_dot(dir, normal);
    if (fabsf(denom) < 1e-6f) return false;
    float t = vec3_dot(vec3_sub(point, origin), normal) / denom;
    if (t <= 0.0f) return false;
    *out = vec3_add(origin, vec3_mul(dir, t));
    return true;
}

/* Parameter along an axis line of the point closest to a ray. This is what
 * makes an axis drag track the pointer without a plane to project onto. */
static bool closest_on_axis(Vec3 axis_origin, Vec3 axis_dir, Vec3 ray_o, Vec3 ray_d, float *t) {
    Vec3 w = vec3_sub(axis_origin, ray_o);
    float a = vec3_dot(axis_dir, axis_dir);
    float b = vec3_dot(axis_dir, ray_d);
    float c = vec3_dot(ray_d, ray_d);
    float d = vec3_dot(axis_dir, w);
    float e = vec3_dot(ray_d, w);

    float denom = a * c - b * b;
    if (fabsf(denom) < 1e-9f) return false; // ray parallel to the axis
    *t = (b * e - c * d) / denom;
    return true;
}

/* Picking */

/*
 * The selection box in plane coordinates. Everything below - grips, arrows,
 * rings, drag deltas, counters - is expressed in this frame, so switching the
 * plane switches all of them at once and none of them can disagree.
 */
static Bounds selection_box(const App *app, const Workplane &plane) {
    if (app->scene.selection.empty()) return bounds_empty();
    return scene_selection_bounds_in(&app->scene, mat3_transposed(plane.basis));
}

static int pick_handle(const App *app, const Workplane &plane, ImVec2 region_size,
                       float sx, float sy) {
    Bounds b = selection_box(app, plane);
    if (!b.valid) return -1;

    int best = -1;
    float best_dist = HANDLE_PICK_RADIUS * HANDLE_PICK_RADIUS;
    for (int i = 0; i < OBC_HANDLE_COUNT; ++i) {
        TransformHandle h = scene_handle(b, i);
        Vec3 world = workplane_to_world(plane, h.pos);
        float hx, hy;
        if (!camera_project(app->camera, (int)region_size.x, (int)region_size.y, world, &hx, &hy)) {
            continue;
        }
        float dx = hx - sx;
        float dy = hy - sy;
        float d2 = dx * dx + dy * dy;
        if (d2 <= best_dist) {
            best_dist = d2;
            best = i;
        }
    }
    return best;
}

static int pick_z_arrow(const App *app, const Workplane &plane, ImVec2 region_size,
                        float sx, float sy) {
    Bounds b = selection_box(app, plane);
    if (!b.valid) return -1;

    int which[2] = { OBC_ZARROW_UP, OBC_ZARROW_DOWN };
    int best = -1;
    float best_dist = HANDLE_PICK_RADIUS * HANDLE_PICK_RADIUS;
    for (int i = 0; i < 2; ++i) {
        Vec3 tip = render_z_arrow_tip(b, plane, which[i]);
        float hx, hy;
        if (!camera_project(app->camera, (int)region_size.x, (int)region_size.y, tip, &hx, &hy)) {
            continue;
        }
        float dx = hx - sx;
        float dy = hy - sy;
        float d2 = dx * dx + dy * dy;
        if (d2 <= best_dist) {
            best_dist = d2;
            best = which[i];
        }
    }
    return best;
}

/* Pivot is a world point either way: a pinned one was picked in world space,
 * and the box centre comes back out of the plane. */
static Vec3 rotation_pivot(const App *app, const Workplane &plane, const Bounds &local) {
    if (app->ui.pivot_custom) return app->ui.pivot_point;
    return workplane_to_world(plane, bounds_center(local));
}

/* Rings are hit tested against their projected outline: the nearest sampled
 * point wins, which behaves the same at any orbit angle. */
static int pick_ring(const App *app, const Workplane &plane, ImVec2 region_size,
                     float sx, float sy) {
    Bounds b = selection_box(app, plane);
    if (!b.valid) return -1;

    Vec3 pivot = rotation_pivot(app, plane, b);
    float radius = render_ring_radius(b);

    int best = -1;
    float best_dist = RING_PICK_RADIUS * RING_PICK_RADIUS;
    for (int axis = 0; axis < 3; ++axis) {
        for (int step = 0; step < RING_PICK_STEPS; ++step) {
            Vec3 p = render_ring_point(pivot, radius, plane, axis, step, RING_PICK_STEPS);
            float px, py;
            if (!camera_project(app->camera, (int)region_size.x, (int)region_size.y, p, &px, &py)) {
                continue;
            }
            float dx = px - sx;
            float dy = py - sy;
            float d2 = dx * dx + dy * dy;
            if (d2 <= best_dist) {
                best_dist = d2;
                best = axis;
            }
        }
    }
    return best;
}

/* Align and mirror picking */

#define TOOL_PICK_RADIUS 12.0f

/* Distance from a point to a projected segment, in pixels. */
static float distance_to_segment(const App *app, ImVec2 region_size, Vec3 a, Vec3 b,
                                 float sx, float sy) {
    float ax, ay, bx, by;
    int vw = (int)region_size.x;
    int vh = (int)region_size.y;
    if (!camera_project(app->camera, vw, vh, a, &ax, &ay)) return 1e9f;
    if (!camera_project(app->camera, vw, vh, b, &bx, &by)) return 1e9f;

    float dx = bx - ax;
    float dy = by - ay;
    float len2 = dx * dx + dy * dy;
    float t = 0.0f;
    if (len2 > 1e-6f) {
        t = ((sx - ax) * dx + (sy - ay) * dy) / len2;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
    }
    float px = ax + dx * t;
    float py = ay + dy * t;
    return sqrtf((px - sx) * (px - sx) + (py - sy) * (py - sy));
}

static bool pick_align_line(const App *app, ImVec2 region_size, float sx, float sy,
                            int *out_axis, int *out_slot) {
    if (app->scene.selection.empty()) return false;
    const SceneNode *ref = scene_node(&app->scene, app->scene.selection[0]);
    if (!ref) return false;

    Bounds reference = scene_node_bounds(&app->scene, app->scene.selection[0]);
    Bounds span = scene_selection_bounds(&app->scene);
    if (!reference.valid || !span.valid) return false;

    float best = TOOL_PICK_RADIUS;
    bool found = false;
    for (int axis = 0; axis < 3; ++axis) {
        for (int slot = 0; slot < 3; ++slot) {
            Vec3 a, b;
            render_align_line(reference, span, axis, slot, &a, &b);
            float d = distance_to_segment(app, region_size, a, b, sx, sy);
            if (d <= best) {
                best = d;
                *out_axis = axis;
                *out_slot = slot;
                found = true;
            }
        }
    }
    return found;
}

static bool pick_mirror_plane(const App *app, ImVec2 region_size, float sx, float sy,
                              int *out_axis) {
    Bounds b = scene_selection_bounds(&app->scene);
    if (!b.valid) return false;

    float best = TOOL_PICK_RADIUS;
    bool found = false;
    for (int axis = 0; axis < 3; ++axis) {
        Vec3 c[4];
        render_mirror_plane(b, axis, c);
        for (int i = 0; i < 4; ++i) {
            float d = distance_to_segment(app, region_size, c[i], c[(i + 1) % 4], sx, sy);
            if (d <= best) {
                best = d;
                *out_axis = axis;
                found = true;
            }
        }
    }
    return found;
}

/* Move reference */

static bool same_selection(const std::vector<int> &a, const std::vector<int> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static void move_ref_clear(App *app) {
    app->ui.edit_ref.valid = false;
    app->ui.edit_ref.offset = vec3(0.0f, 0.0f, 0.0f);
    app->ui.edit_ref.nodes.clear();
    app->ui.counter_edit_axis = -1;
}

/* Captures the pre-move position, unless a reference for this same selection is
 * already open - then the offsets keep accumulating against the original spot. */
static void move_ref_begin(App *app, bool restart) {
    EditReference *r = &app->ui.edit_ref;
    if (r->valid && !restart && same_selection(r->nodes, app->scene.selection)) return;

    r->valid = true;
    /* Kept in plane coordinates, like everything else the gizmos touch. A
     * change of plane drops the reference (see ui_set_workplane_mode), so this
     * can never be read back in a frame it was not taken in. */
    r->start_bounds = selection_box(app, ui_active_workplane(app));
    r->offset = vec3(0.0f, 0.0f, 0.0f);
    r->rotation = vec3(0.0f, 0.0f, 0.0f);
    r->nodes = app->scene.selection;
    app->ui.counter_edit_axis = -1;
}

static void move_ref_add(App *app, Vec3 delta) {
    if (!app->ui.edit_ref.valid) return;
    app->ui.edit_ref.offset = vec3_add(app->ui.edit_ref.offset, delta);
}

/* Drops the reference as soon as it stops describing the current selection. */
static void move_ref_validate(App *app) {
    EditReference *r = &app->ui.edit_ref;
    if (!r->valid) return;
    if (app->scene.selection.empty() || !same_selection(r->nodes, app->scene.selection)) {
        move_ref_clear(app);
    }
}

/* Rubber band selection */

/* How far the pointer must travel before the press counts as a band rather
 * than a click. See the RectSelect comment for why this is not zero. */
#define RECT_SELECT_MIN_PX 4.0f

static void rect_norm(const RectSelect &r, float *x0, float *y0, float *x1, float *y1) {
    *x0 = (r.x0 < r.x1) ? r.x0 : r.x1;
    *x1 = (r.x0 < r.x1) ? r.x1 : r.x0;
    *y0 = (r.y0 < r.y1) ? r.y0 : r.y1;
    *y1 = (r.y0 < r.y1) ? r.y1 : r.y0;
}

/*
 * Segment against an axis aligned rectangle, by parametric clipping. True when
 * any part of the segment lies inside, so a segment wholly inside counts - which
 * is what makes the triangle test below pick up an endpoint in the band without
 * a separate check for it.
 */
static bool segment_hits_rect(float ax, float ay, float bx, float by,
                              float x0, float y0, float x1, float y1) {
    float dx = bx - ax;
    float dy = by - ay;
    float p[4] = { -dx, dx, -dy, dy };
    float q[4] = { ax - x0, x1 - ax, ay - y0, y1 - ay };

    float t0 = 0.0f;
    float t1 = 1.0f;
    for (int i = 0; i < 4; ++i) {
        if (fabsf(p[i]) < 1e-9f) {
            if (q[i] < 0.0f) return false; // parallel to this edge and outside it
            continue;
        }
        float r = q[i] / p[i];
        if (p[i] < 0.0f) {
            if (r > t1) return false;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return false;
            if (r < t1) t1 = r;
        }
    }
    return true;
}

static bool point_in_triangle(float px, float py, float ax, float ay,
                              float bx, float by, float cx, float cy) {
    float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
    float d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
    float d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
    bool neg = (d1 < 0.0f) || (d2 < 0.0f) || (d3 < 0.0f);
    bool pos = (d1 > 0.0f) || (d2 > 0.0f) || (d3 > 0.0f);
    return !(neg && pos); // same side of all three edges
}

/*
 * Does a node's projected surface touch the band?
 *
 * Tested against the triangles rather than the bounding box. The box is the
 * cheaper answer, but its screen extent covers corners the silhouette does not
 * fill, and sweeping a band that visibly clears an object only to have it
 * select is exactly the kind of wrong the user can see. A sphere makes the gap
 * obvious: its box corners are empty space.
 *
 * Cost is one world transform and one projection per vertex per frame, and only
 * while the band is up - the same order as the scene_node_bounds calls the
 * frame already makes. If a large import ever makes that felt, the fix is to
 * cache each node's screen box when the band opens, since neither the objects
 * nor the camera move while the left button is down.
 */
static bool node_touches_rect(const App *app, ImVec2 region_size, int id,
                              float x0, float y0, float x1, float y1) {
    const SceneNode *n = scene_node(&app->scene, id);
    if (!n) return false;

    int vw = (int)region_size.x;
    int vh = (int)region_size.y;
    const std::vector<Vec3> &v = n->mesh.vertices;

    /* Triangle soup: three vertices per triangle, no index buffer. */
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        float px[3], py[3];
        bool projected = true;
        for (int k = 0; k < 3; ++k) {
            Vec3 world = scene_world_point(&app->scene, id, v[i + k]);
            if (!camera_project(app->camera, vw, vh, world, &px[k], &py[k])) {
                projected = false;
                break;
            }
        }
        if (!projected) continue; // crosses the eye plane, so it has no screen extent

        float tx0 = px[0], tx1 = px[0], ty0 = py[0], ty1 = py[0];
        for (int k = 1; k < 3; ++k) {
            if (px[k] < tx0) tx0 = px[k];
            if (px[k] > tx1) tx1 = px[k];
            if (py[k] < ty0) ty0 = py[k];
            if (py[k] > ty1) ty1 = py[k];
        }
        if (tx1 < x0 || x1 < tx0 || ty1 < y0 || y1 < ty0) continue;

        for (int k = 0; k < 3; ++k) {
            int m = (k + 1) % 3;
            if (segment_hits_rect(px[k], py[k], px[m], py[m], x0, y0, x1, y1)) return true;
        }
        /* A band drawn entirely inside one large face still touches it, and no
         * edge of that face comes near the band. */
        if (point_in_triangle((x0 + x1) * 0.5f, (y0 + y1) * 0.5f,
                              px[0], py[0], px[1], py[1], px[2], py[2])) {
            return true;
        }
    }
    return false;
}

static void rect_select_begin(App *app, ImVec2 local, bool extend) {
    RectSelect *r = &app->ui.rect_select;
    r->active = true;
    r->swept = false;
    r->x0 = r->x1 = local.x;
    r->y0 = r->y1 = local.y;
    r->base.clear();

    if (extend) {
        r->base = app->scene.selection;
        return;
    }
    /* Until the pointer moves this is still just a click on nothing, so it
     * clears straight away rather than waiting for the release. */
    scene_select_clear(&app->scene);
    move_ref_clear(app);
}

static void rect_select_update(App *app, ImVec2 region_size, ImVec2 local) {
    RectSelect *r = &app->ui.rect_select;
    r->x1 = local.x;
    r->y1 = local.y;

    if (!r->swept) {
        if (fabsf(r->x1 - r->x0) < RECT_SELECT_MIN_PX &&
            fabsf(r->y1 - r->y0) < RECT_SELECT_MIN_PX) {
            return;
        }
        r->swept = true;
    }

    float bx0, by0, bx1, by1;
    rect_norm(*r, &bx0, &by0, &bx1, &by1);

    /* Recomputed from the base every frame, so narrowing the band deselects. */
    std::vector<int> hits = r->base;
    const Scene &scene = app->scene;
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        int id = (int)i;
        /* Objects only, matching what A selects: a group is picked in the tree,
         * not by sweeping the space its children happen to occupy. */
        if (scene.nodes[i].kind != NODE_OBJECT) continue;
        if (!scene_is_effectively_visible(&scene, id)) continue;

        if (!node_touches_rect(app, region_size, id, bx0, by0, bx1, by1)) continue;
        hits.push_back(id);
    }

    scene_select_set(&app->scene, hits);
}

static void rect_select_end(App *app) {
    RectSelect *r = &app->ui.rect_select;
    if (!r->active) return;

    if (r->swept) {
        int n = (int)app->scene.selection.size();
        char msg[64];
        snprintf(msg, sizeof(msg), (n == 1) ? "Selected %d object." : "Selected %d objects.", n);
        ui_set_status(app, msg, false);
    }

    r->active = false;
    r->swept = false;
    r->base.clear();
}

static void draw_rect_select_band(const App *app, ImVec2 region_min) {
    const RectSelect &r = app->ui.rect_select;
    if (!r.active || !r.swept) return;

    float x0, y0, x1, y1;
    rect_norm(r, &x0, &y0, &x1, &y1);

    /* Drawn on the ImGui layer rather than in GL: it is a screen space overlay,
     * so it needs no projection and costs the renderer nothing. */
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImVec2(region_min.x + x0, region_min.y + y0);
    ImVec2 b = ImVec2(region_min.x + x1, region_min.y + y1);
    dl->AddRectFilled(a, b, IM_COL32(33, 115, 204, 48));  // COL_SEL_BOX, see render.cpp
    dl->AddRect(a, b, IM_COL32(33, 115, 204, 220));
}

/* Drag application */

/*
 * Moves run in the plane, per PLAN.md. The drag plane passes through the point
 * the press landed on and faces along the plane normal, so the selection
 * follows the pointer across a tilted plane exactly as it does across the
 * plate.
 */
static void drag_apply_move(App *app, ImVec2 region_size, ImVec2 local_mouse) {
    DragState *d = &app->ui.drag;
    Workplane plane = ui_active_workplane(app);

    Vec3 origin, dir;
    camera_screen_ray(app->camera, (int)region_size.x, (int)region_size.y,
                      local_mouse.x, local_mouse.y, &origin, &dir);

    Vec3 now;
    if (!ray_plane(origin, dir, d->start_plane_point, workplane_axis(plane, 2), &now)) return;

    Vec3 wanted = workplane_from_world(plane, vec3_sub(now, d->start_plane_point));
    wanted.z = 0.0f; // the drag stays in the plane; the arrows leave it
    wanted.x = snap_value(wanted.x, app->ui.snap_grid_mm);
    wanted.y = snap_value(wanted.y, app->ui.snap_grid_mm);

    Vec3 step = vec3_sub(wanted, d->applied_delta);
    if (fabsf(step.x) < 1e-6f && fabsf(step.y) < 1e-6f) return;

    Vec3 world_step = workplane_to_world(plane, step);
    scene_translate_selection(&app->scene, world_step);
    move_ref_add(app, world_step);
    d->applied_delta = wanted;
}

/* The vertical arrows run along the plane normal, so they lift a part off
 * whatever surface the plane sits on. */
static void drag_apply_z(App *app, ImVec2 region_size, ImVec2 local_mouse) {
    DragState *d = &app->ui.drag;
    Workplane plane = ui_active_workplane(app);
    Vec3 axis = workplane_axis(plane, 2);

    Vec3 origin, dir;
    camera_screen_ray(app->camera, (int)region_size.x, (int)region_size.y,
                      local_mouse.x, local_mouse.y, &origin, &dir);

    float t;
    if (!closest_on_axis(d->start_plane_point, axis, origin, dir, &t)) return;

    float wanted = snap_value(t, app->ui.snap_grid_mm);
    float step = wanted - d->applied_delta.z;
    if (fabsf(step) < 1e-6f) return;

    Vec3 world_step = vec3_mul(axis, step);
    scene_translate_selection(&app->scene, world_step);
    move_ref_add(app, world_step);
    d->applied_delta.z = wanted;
}

static void drag_apply_scale(App *app, ImVec2 region_size, ImVec2 local_mouse) {
    DragState *d = &app->ui.drag;
    Workplane plane = ui_active_workplane(app);

    /* start_bounds is in plane coordinates, so the handle comes out in plane
     * coordinates too and has to be lifted to world before it is projected. */
    TransformHandle h = scene_handle(d->start_bounds, d->handle);
    Vec3 world_anchor = workplane_to_world(plane, h.anchor);
    Vec3 world_pos = workplane_to_world(plane, h.pos);

    /*
     * The factor is measured in screen space along the anchor -> handle
     * direction. Doing it on screen keeps the grip under the cursor from any
     * viewing angle, which unprojecting onto a world axis does not.
     */
    float ax, ay, hx, hy;
    int vw = (int)region_size.x;
    int vh = (int)region_size.y;
    if (!camera_project(app->camera, vw, vh, world_anchor, &ax, &ay)) return;
    if (!camera_project(app->camera, vw, vh, world_pos, &hx, &hy)) return;

    float dirx = hx - ax;
    float diry = hy - ay;
    float len2 = dirx * dirx + diry * diry;
    if (len2 < 1.0f) return; // handle and anchor project to the same pixel

    float t = ((local_mouse.x - ax) * dirx + (local_mouse.y - ay) * diry) / len2;
    if (t < 0.01f) t = 0.01f; // never flip or collapse the volume

    Vec3 size = bounds_size(d->start_bounds);
    Vec3 wanted = vec3(1.0f, 1.0f, 1.0f);
    for (int axis = 0; axis < 3; ++axis) {
        float mask = (axis == 0) ? h.mask.x : (axis == 1) ? h.mask.y : h.mask.z;
        if (mask <= 0.0f) continue;

        float extent = (axis == 0) ? size.x : (axis == 1) ? size.y : size.z;
        float f = t;
        if (extent > 1e-4f) {
            float snapped = snap_value(extent * t, app->ui.snap_grid_mm);
            if (snapped > 1e-4f) f = snapped / extent;
        }
        if (axis == 0) wanted.x = f;
        else if (axis == 1) wanted.y = f;
        else wanted.z = f;
    }

    Vec3 step = vec3(wanted.x / d->applied_factor.x,
                     wanted.y / d->applied_factor.y,
                     wanted.z / d->applied_factor.z);
    if (fabsf(step.x - 1.0f) < 1e-6f && fabsf(step.y - 1.0f) < 1e-6f &&
        fabsf(step.z - 1.0f) < 1e-6f) {
        return;
    }
    scene_scale_selection(&app->scene, world_anchor, step, plane.basis);
    d->applied_factor = wanted;
}

/* Angle of the pointer around the pivot, measured in the ring's own plane -
 * which is one of the workplane's axis planes, not a world one. */
static bool ring_angle(const App *app, const Workplane &plane, ImVec2 region_size,
                       ImVec2 local_mouse, Vec3 pivot, int axis, float *degrees) {
    Vec3 normal = workplane_axis(plane, axis);
    Vec3 origin, dir;
    camera_screen_ray(app->camera, (int)region_size.x, (int)region_size.y,
                      local_mouse.x, local_mouse.y, &origin, &dir);

    Vec3 hit;
    if (!ray_plane(origin, dir, pivot, normal, &hit)) return false;

    /* Measured in plane coordinates, matching how render_ring_point lays the
     * ring out, so the angle and the drawn ring agree. */
    Vec3 rel = workplane_from_world(plane, vec3_sub(hit, pivot));
    float u, v;
    switch (axis) {
    case 0:  u = rel.y; v = rel.z; break;
    case 1:  u = rel.x; v = rel.z; break;
    default: u = rel.x; v = rel.y; break;
    }
    if (fabsf(u) < 1e-6f && fabsf(v) < 1e-6f) return false;
    *degrees = rad_to_deg(atan2f(v, u));
    return true;
}

static void drag_apply_rotate(App *app, ImVec2 region_size, ImVec2 local_mouse) {
    DragState *d = &app->ui.drag;
    Workplane plane = ui_active_workplane(app);

    float angle;
    if (!ring_angle(app, plane, region_size, local_mouse, d->start_plane_point, d->axis, &angle)) {
        return;
    }

    float swept = angle - d->start_angle;
    while (swept > 180.0f) swept -= 360.0f;
    while (swept < -180.0f) swept += 360.0f;

    /* Shift snaps to 15 degrees, the angles parts are actually built on. */
    swept = ImGui::GetIO().KeyShift ? snap_value(swept, 15.0f) : snap_value(swept, 1.0f);

    float step = swept - d->applied_degrees;
    if (fabsf(step) < 1e-4f) return;

    scene_rotate_selection(&app->scene, d->start_plane_point, d->rotate_axis, step);
    d->applied_degrees = swept;

    if (app->ui.edit_ref.valid) {
        Vec3 *acc = &app->ui.edit_ref.rotation;
        if (d->axis == 0) acc->x += step;
        else if (d->axis == 1) acc->y += step;
        else acc->z += step;
    }
}

/* Drag lifecycle */

static void drag_begin(App *app, ImVec2 region_size, ImVec2 local_mouse,
                       TransformMode mode, int handle, int axis) {
    DragState *d = &app->ui.drag;
    Workplane plane = ui_active_workplane(app);
    Bounds b = selection_box(app, plane);
    if (!b.valid) return;

    /* One history entry per gesture, taken before the first change. */
    const char *label = (mode == XFORM_SCALE) ? "Scale" : (mode == XFORM_ROTATE) ? "Rotate" : "Move";
    undo_record(&app->undo, app->scene, label);

    memset(d, 0, sizeof(*d));
    d->active = true;
    d->mode = mode;
    d->handle = handle;
    d->axis = axis;
    d->start_bounds = b;
    d->applied_factor = vec3(1.0f, 1.0f, 1.0f);
    d->applied_delta = vec3(0.0f, 0.0f, 0.0f);

    if (mode == XFORM_ROTATE) {
        d->rotate_axis = workplane_axis(plane, axis); // a plane axis, not a world one
        d->start_plane_point = rotation_pivot(app, plane, b); // pivot, not a plane hit
        move_ref_begin(app, false);
        float angle = 0.0f;
        ring_angle(app, plane, region_size, local_mouse, d->start_plane_point, axis, &angle);
        d->start_angle = angle;
        return;
    }

    if (handle == OBC_ZARROW_UP || handle == OBC_ZARROW_DOWN) {
        /* The axis line runs through the arrow tip along the plane normal, and
         * applied_delta.z is measured along it, so the offset starts wherever
         * the grab landed. */
        Vec3 tip = render_z_arrow_tip(b, plane, handle);
        Vec3 axis_dir = workplane_axis(plane, 2);
        Vec3 origin, dir;
        camera_screen_ray(app->camera, (int)region_size.x, (int)region_size.y,
                          local_mouse.x, local_mouse.y, &origin, &dir);
        float t = 0.0f;
        closest_on_axis(tip, axis_dir, origin, dir, &t);
        d->start_plane_point = vec3_add(tip, vec3_mul(axis_dir, t));
        move_ref_begin(app, true);
        return;
    }

    /* Body drag: the plane through the selection centre, facing along the
     * plane normal. */
    Vec3 center = workplane_to_world(plane, bounds_center(b));
    Vec3 origin, dir;
    camera_screen_ray(app->camera, (int)region_size.x, (int)region_size.y,
                      local_mouse.x, local_mouse.y, &origin, &dir);

    Vec3 hit;
    if (ray_plane(origin, dir, center, workplane_axis(plane, 2), &hit)) {
        d->start_plane_point = hit;
    } else {
        d->start_plane_point = center;
    }
    if (mode == XFORM_MOVE) move_ref_begin(app, true);
}

static void drag_end(App *app) {
    DragState *d = &app->ui.drag;
    if (!d->active) return;

    char msg[128];
    if (d->mode == XFORM_ROTATE && fabsf(d->applied_degrees) > 1e-4f) {
        const char *axis_name = (d->axis == 0) ? "X" : (d->axis == 1) ? "Y" : "Z";
        snprintf(msg, sizeof(msg), "Rotated %.1f degrees about %s.", d->applied_degrees, axis_name);
        ui_set_status(app, msg, false);
    } else if (d->mode == XFORM_SCALE) {
        Vec3 size = bounds_size(scene_selection_bounds(&app->scene));
        snprintf(msg, sizeof(msg), "Size %.2f x %.2f x %.2f mm.", size.x, size.y, size.z);
        ui_set_status(app, msg, false);
    }

    memset(d, 0, sizeof(*d));
    d->handle = -1;
}

/* Navigation and picking */

static void viewport_handle_navigation(App *app, ImVec2 region_min, ImVec2 region_size) {
    ImGuiIO &io = ImGui::GetIO();

    ImGui::SetCursorScreenPos(region_min);
    ImGui::InvisibleButton("##stage", region_size,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);

    bool hovered = ImGui::IsItemHovered();
    app->ui.viewport_hovered = hovered;

    if (ImGui::IsItemActive()) {
        if (io.MouseDown[ImGuiMouseButton_Right]) {
            camera_orbit(&app->camera, -io.MouseDelta.x * 0.4f, io.MouseDelta.y * 0.4f);
        } else if (io.MouseDown[ImGuiMouseButton_Middle]) {
            camera_pan(&app->camera, io.MouseDelta.x, io.MouseDelta.y, (int)region_size.y);
        }
    }

    ImVec2 local = ImVec2(io.MousePos.x - region_min.x, io.MousePos.y - region_min.y);
    int vw = (int)region_size.x;
    int vh = (int)region_size.y;

    /* Whatever grip is under the cursor, for the hover highlight and to decide
     * what a press starts. Only the active mode's grips are live. */
    /*
     * The orientation cube takes the click before anything else: it sits over
     * the top left of the stage, and picking behind it would both select and
     * turn the view.
     */
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        Vec3 look;
        if (render_orientation_cube_pick(app->camera, local.x, local.y, &look)) {
            camera_look_from(&app->camera, look);

            const char *name = (look.x > 0.5f) ? "Right" : (look.x < -0.5f) ? "Left"
                             : (look.y > 0.5f) ? "Back"  : (look.y < -0.5f) ? "Front"
                             : (look.z > 0.5f) ? "Top"   : "Bottom";
            char msg[64];
            snprintf(msg, sizeof(msg), "%s view.", name);
            ui_set_status(app, msg, false);
            return;
        }
    }

    Workplane plane = ui_active_workplane(app);

    /* Selecting a different object in the tree while the mode is up re-collects
     * its edges, which is how PLAN's "select one in the tree if already in
     * bevel mode" works out. */
    if (app->ui.mode == XFORM_BEVEL) ui_bevel_refresh(app);

    app->ui.hover_handle = -1;
    app->ui.hover_axis = -1;
    int hover_z = -1;
    if (hovered && !app->ui.drag.active && !app->ui.plane_pick_active &&
        !app->scene.selection.empty()) {
        if (app->ui.mode == XFORM_SCALE) {
            app->ui.hover_handle = pick_handle(app, plane, region_size, local.x, local.y);
        } else if (app->ui.mode == XFORM_ROTATE) {
            app->ui.hover_axis = pick_ring(app, plane, region_size, local.x, local.y);
        } else {
            hover_z = pick_z_arrow(app, plane, region_size, local.x, local.y);
        }
    }
    app->ui.hover_z_arrow = hover_z;

    /*
     * SHIFT+P armed the plane picker, so the next click lands the workplane on
     * a face instead of selecting. It takes the button before any gizmo for the
     * same reason align and mirror do: two meanings for one click cannot be
     * told apart.
     */
    if (app->ui.plane_pick_active) {
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            Vec3 origin, dir;
            camera_screen_ray(app->camera, vw, vh, local.x, local.y, &origin, &dir);

            float distance = 0.0f;
            Vec3 normal;
            int hit = scene_pick_surface(&app->scene, origin, dir, &distance, &normal);
            if (hit == OBC_NO_NODE) {
                ui_set_status(app, "No face there. Click a surface, or Esc to cancel.", true);
            } else {
                Vec3 point = vec3_add(origin, vec3_mul(dir, distance));
                if (app->ui.plane_pick_center) {
                    point = scene_face_center(&app->scene, hit, point, normal);
                }
                app->ui.face_plane = workplane_from_normal(point, normal);
                ui_set_workplane_mode(app, WORKPLANE_FACE);
            }
        }
        return; // nothing else may act on the pointer while the picker is armed
    }

    /* Align and mirror take over the left button while they are up, so a click
     * commits the highlighted plane instead of reselecting. */
    app->ui.align_hover_axis = -1;
    app->ui.align_hover_slot = -1;
    app->ui.mirror_hover_axis = -1;

    if (app->ui.align_active && !app->scene.selection.empty()) {
        int axis, slot;
        if (hovered && pick_align_line(app, region_size, local.x, local.y, &axis, &slot)) {
            app->ui.align_hover_axis = axis;
            app->ui.align_hover_slot = slot;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                undo_record(&app->undo, app->scene, "Align");
                scene_align_selection(&app->scene, app->scene.selection[0], axis, slot);

                const char *axis_name = (axis == 0) ? "X" : (axis == 1) ? "Y" : "Z";
                const char *slot_name = (slot == ALIGN_SLOT_MIN) ? "low edge"
                    : (slot == ALIGN_SLOT_MAX) ? "high edge" : "centre";
                char msg[96];
                snprintf(msg, sizeof(msg), "Aligned %s to the %s.", axis_name, slot_name);
                ui_set_status(app, msg, false);
                return;
            }
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return; // absorb stray clicks
    }

    if (app->ui.mirror_active && !app->scene.selection.empty()) {
        int axis;
        if (hovered && pick_mirror_plane(app, region_size, local.x, local.y, &axis)) {
            app->ui.mirror_hover_axis = axis;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                Bounds b = scene_selection_bounds(&app->scene);
                Vec3 c = bounds_center(b);
                float plane = (axis == 0) ? c.x : (axis == 1) ? c.y : c.z;

                undo_record(&app->undo, app->scene, "Mirror");
                scene_mirror_selection(&app->scene, axis, plane);

                const char *axis_name = (axis == 0) ? "X" : (axis == 1) ? "Y" : "Z";
                char msg[96];
                snprintf(msg, sizeof(msg), "Mirrored across %s.", axis_name);
                ui_set_status(app, msg, false);
                return;
            }
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    }

    /*
     * Bevel mode owns both buttons: left adds an edge, right drops one. It has
     * to come before picking and before the orbit handler, or a click would
     * both choose an edge and change the selection under it.
     */
    if (app->ui.mode == XFORM_BEVEL && app->ui.bevel_node != OBC_NO_NODE) {
        if (hovered && !app->ui.drag.active) {
            ui_bevel_update_hover(app, region_size.x, region_size.y, local.x, local.y);
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            ui_bevel_pick(app, region_size.x, region_size.y, local.x, local.y, true);
            return;
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ui_bevel_pick(app, region_size.x, region_size.y, local.x, local.y, false);
            return;
        }
    }

    bool extend = io.KeyShift || io.KeyCtrl;

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        /*
         * Shift+click in rotate mode pins the pivot instead of extending the
         * selection: the point clicked on the model wins, otherwise the point
         * on the workplane under the cursor.
         */
        if (app->ui.mode == XFORM_ROTATE && io.KeyShift && !app->scene.selection.empty()) {
            Vec3 origin, dir;
            camera_screen_ray(app->camera, vw, vh, local.x, local.y, &origin, &dir);
            float distance = 0.0f;
            Vec3 point;
            bool have = false;
            if (scene_pick(&app->scene, origin, dir, &distance) != OBC_NO_NODE) {
                point = vec3_add(origin, vec3_mul(dir, distance));
                have = true;
            } else if (workplane_hit(app->camera, vw, vh, local.x, local.y, &point)) {
                have = true;
            }
            if (have) {
                app->ui.pivot_custom = true;
                app->ui.pivot_point = point;
                ui_set_status(app, "Rotation origin pinned. Esc restores the centre.", false);
            }
            return;
        }

        int handle = -1;
        int axis = -1;
        TransformMode mode = app->ui.mode;
        if (!app->scene.selection.empty()) {
            if (mode == XFORM_SCALE) {
                handle = pick_handle(app, plane, region_size, local.x, local.y);
            } else if (mode == XFORM_ROTATE) {
                axis = pick_ring(app, plane, region_size, local.x, local.y);
            } else {
                handle = pick_z_arrow(app, plane, region_size, local.x, local.y);
            }
        }

        if (handle >= 0 || axis >= 0) {
            drag_begin(app, region_size, local, mode, handle, axis);
            return;
        }

        Vec3 origin, dir;
        camera_screen_ray(app->camera, vw, vh, local.x, local.y, &origin, &dir);
        int hit = scene_pick(&app->scene, origin, dir, NULL);

        if (hit == OBC_NO_NODE) {
            /* Nothing under the cursor, so the press opens a rubber band. It is
             * indistinguishable from a plain click until the pointer moves. */
            rect_select_begin(app, local, extend);
        } else if (extend) {
            scene_select_toggle(&app->scene, hit);
        } else {
            /* Clicking a node already in the selection keeps the whole
             * selection, so several objects can be dragged together. */
            if (!scene_is_selected(&app->scene, hit)) scene_select_only(&app->scene, hit);
            /* Body drags move; in scale and rotate mode the grips are the only
             * way to transform, so a body press just selects. */
            if (app->ui.mode == XFORM_MOVE) {
                drag_begin(app, region_size, local, XFORM_MOVE, -1, -1);
            }
        }
    }

    if (app->ui.drag.active && io.MouseDown[ImGuiMouseButton_Left]) {
        switch (app->ui.drag.mode) {
        case XFORM_SCALE:  drag_apply_scale(app, region_size, local); break;
        case XFORM_ROTATE: drag_apply_rotate(app, region_size, local); break;
        default:
            if (app->ui.drag.handle == OBC_ZARROW_UP || app->ui.drag.handle == OBC_ZARROW_DOWN) {
                drag_apply_z(app, region_size, local);
            } else {
                drag_apply_move(app, region_size, local);
            }
            break;
        }
    }

    if (app->ui.drag.active && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) drag_end(app);

    /* Not gated on "hovered": a band started inside the view keeps tracking the
     * pointer once it leaves, and must still end wherever the button comes up. */
    if (app->ui.rect_select.active && io.MouseDown[ImGuiMouseButton_Left]) {
        rect_select_update(app, region_size, local);
    }
    if (app->ui.rect_select.active && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        rect_select_end(app);
    }

    if (hovered && io.MouseWheel != 0.0f) camera_zoom(&app->camera, io.MouseWheel);
}

/* Must run directly after the stage InvisibleButton: the drop target binds to
 * the last submitted item. */
static void viewport_handle_drop(App *app, ImVec2 region_min, ImVec2 region_size) {
    if (!ImGui::BeginDragDropTarget()) return;

    const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(OBC_DND_PRIMITIVE);
    if (payload && payload->DataSize == (int)sizeof(BookshelfItem)) {
        BookshelfItem item = *(const BookshelfItem *)payload->Data;

        /* Captured before the add, which replaces the selection an object
         * plane is derived from. */
        Workplane plane = ui_active_workplane(app);
        ui_action_add_primitive(app, item.kind, item.polarity);

        ImVec2 mouse = ImGui::GetIO().MousePos;
        Vec3 origin, dir;
        camera_screen_ray(app->camera, (int)region_size.x, (int)region_size.y,
                          mouse.x - region_min.x, mouse.y - region_min.y, &origin, &dir);

        /* A drop lands where the cursor meets the active plane, snapped in that
         * plane's own axes - so dropping onto a sloped face puts the object on
         * the slope instead of on the plate underneath it. */
        Vec3 hit;
        if (!app->scene.selection.empty() &&
            ray_plane(origin, dir, plane.origin, workplane_axis(plane, 2), &hit)) {
            Vec3 local = workplane_from_world(plane, vec3_sub(hit, plane.origin));
            local.x = snap_value(local.x, app->ui.snap_grid_mm);
            local.y = snap_value(local.y, app->ui.snap_grid_mm);
            local.z = 0.0f;

            Vec3 world = vec3_add(plane.origin, workplane_to_world(plane, local));
            ui_place_on_workplane(app, app->scene.selection.back(), world);
        }
    }
    ImGui::EndDragDropTarget();
}

/*
 * Which way one grid step goes for a given direction on screen.
 *
 * The arrow keys move by what the user sees rather than by where the scene's
 * axes happen to point, so Right keeps moving things rightwards as the view
 * orbits. It still resolves to a whole plane axis rather than a free screen
 * direction: the snap grid only means anything along the plane's own axes, and
 * a nudge that drifts off it would be unusable.
 */
static Vec3 screen_step_axis(const App *app, const Workplane &plane, ImVec2 region_size,
                             float want_x, float want_y) {
    Bounds b = selection_box(app, plane);
    if (!b.valid) return vec3(0.0f, 0.0f, 0.0f);

    int vw = (int)region_size.x;
    int vh = (int)region_size.y;
    Vec3 centre = workplane_to_world(plane, bounds_center(b));

    float cx, cy;
    if (!camera_project(app->camera, vw, vh, centre, &cx, &cy)) return vec3(0.0f, 0.0f, 0.0f);

    Vec3 best = vec3(0.0f, 0.0f, 0.0f);
    float best_dot = -2.0f;

    for (int axis = 0; axis < 2; ++axis) {
        for (int sign = -1; sign <= 1; sign += 2) {
            Vec3 local = vec3(axis == 0 ? (float)sign : 0.0f,
                              axis == 1 ? (float)sign : 0.0f, 0.0f);
            /* A short probe along the axis, projected, gives its screen
             * direction from where the selection actually sits. */
            Vec3 probe = vec3_add(centre, workplane_to_world(plane, local));
            float px, py;
            if (!camera_project(app->camera, vw, vh, probe, &px, &py)) continue;

            float dx = px - cx;
            float dy = py - cy;
            float len = sqrtf(dx * dx + dy * dy);
            if (len < 1e-4f) continue; // edge on, no direction to speak of

            float d = (dx / len) * want_x + (dy / len) * want_y;
            if (d > best_dot) {
                best_dot = d;
                best = local;
            }
        }
    }
    return best;
}

static void viewport_handle_keys(App *app, ImVec2 region_size) {
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantTextInput) return;

    /* Ctrl+Z and Ctrl+Y route through ui_action_undo / ui_action_redo, which
     * unmerge a selected merged object and otherwise step the global history.
     * They must not call unmerge directly, or undo is unreachable by keyboard. */
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false)) ui_action_merge_selection(app);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) ui_action_undo(app);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) ui_action_redo(app);

    /* The File menu has always advertised these three; they are bound here so
     * the label and the key agree. Ctrl+S cannot reach scale mode below, since
     * that is past the Ctrl guard. */
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) ui_action_copy(app);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) ui_action_paste(app);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) ui_action_duplicate(app);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) ui_action_new(app);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) ui_prompt_path(app, FILE_PROMPT_OPEN);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) ui_action_save_current(app);
    /* CTRL+P is the "where I clicked" half of plane setting, so it has to be
     * handled here, above the guard that drops every Ctrl combination. */
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P, false)) ui_begin_plane_pick(app, false);

    if (io.KeyCtrl) return; // no unmodified shortcut should fire with Ctrl held

    /* Toggles, so the same key puts the help away again. */
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) app->ui.show_help = !app->ui.show_help;

    if (ImGui::IsKeyPressed(ImGuiKey_A, false)) scene_select_all_visible(&app->scene);
    if (ImGui::IsKeyPressed(ImGuiKey_I, false)) scene_invert_selection_polarity(&app->scene);

    /* Transform modes. Both keys toggle, so the same key returns to move. */
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) ui_set_mode(app, XFORM_SCALE);
    if (ImGui::IsKeyPressed(ImGuiKey_D, false)) ui_set_mode(app, XFORM_ROTATE);
    if (ImGui::IsKeyPressed(ImGuiKey_B, false)) {
        /* Shift+B opens the menu while already in the mode; plain B toggles it. */
        if (io.KeyShift && app->ui.mode == XFORM_BEVEL) app->ui.bevel_menu_open = true;
        else ui_set_mode(app, XFORM_BEVEL);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_L, false)) ui_action_align(app);
    if (ImGui::IsKeyPressed(ImGuiKey_M, false)) ui_action_mirror(app);

    /* Pane folding, for when the toolbar itself is what ran out of room. */
    if (ImGui::IsKeyPressed(ImGuiKey_T, false)) app->ui.show_tree = !app->ui.show_tree;
    if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) app->ui.show_shelf = !app->ui.show_shelf;

    /* Editing plane: P toggles plate and object, SHIFT+P centres a face plane. */
    if (ImGui::IsKeyPressed(ImGuiKey_P, false)) {
        if (io.KeyShift) ui_begin_plane_pick(app, true);
        else ui_toggle_workplane(app);
    }

    /* Snap grid stepping, so the scale can be changed without the mouse. */
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true)) ui_step_snap_grid(app, -1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) ui_step_snap_grid(app, 1);

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (app->ui.plane_pick_active) {
            app->ui.plane_pick_active = false;
            ui_set_status(app, "Plane setting cancelled.", false);
        } else if (app->ui.align_active || app->ui.mirror_active) {
            app->ui.align_active = false;
            app->ui.mirror_active = false;
            ui_set_status(app, "", false);
        } else if (app->ui.pivot_custom) {
            app->ui.pivot_custom = false;
            ui_set_status(app, "Rotation origin back to the centre.", false);
        } else if (app->ui.mode != XFORM_MOVE) {
            ui_set_mode(app, XFORM_MOVE);
        } else {
            scene_select_clear(&app->scene);
            move_ref_clear(app);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        ui_action_delete_selection(app);
        move_ref_clear(app);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true)) camera_zoom(&app->camera, 1.0f);
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true)) camera_zoom(&app->camera, -1.0f);

    /*
     * Arrow keys nudge by one grid step along whichever plane axis points that
     * way on screen. Plane relative as PLAN.md asks, but chosen by the view, so
     * the keys keep matching the picture after an orbit.
     */
    Workplane nudge_plane = ui_active_workplane(app);
    float step = app->ui.snap_grid_mm;
    Vec3 delta = vec3(0.0f, 0.0f, 0.0f);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
        delta = vec3_add(delta, screen_step_axis(app, nudge_plane, region_size, -1.0f, 0.0f));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
        delta = vec3_add(delta, screen_step_axis(app, nudge_plane, region_size, 1.0f, 0.0f));
    }
    /* Screen y grows downward, so "up" is negative. */
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
        delta = vec3_add(delta, screen_step_axis(app, nudge_plane, region_size, 0.0f, -1.0f));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
        delta = vec3_add(delta, screen_step_axis(app, nudge_plane, region_size, 0.0f, 1.0f));
    }
    delta = vec3_mul(delta, step);

    if ((delta.x != 0.0f || delta.y != 0.0f) && !app->scene.selection.empty()) {
        /* Nudges accumulate against one reference so the counters keep showing
         * the total offset rather than the last keypress, and one history entry
         * covers the whole run of keypresses. */
        if (!app->ui.edit_ref.valid) undo_record(&app->undo, app->scene, "Nudge");
        move_ref_begin(app, false);

        Vec3 world_delta = workplane_to_world(nudge_plane, delta);
        scene_translate_selection(&app->scene, world_delta);
        move_ref_add(app, world_delta);
    }
}

/* Floating controls, each in its own window so they actually receive clicks */

static void overlay_begin(const char *id, ImVec2 pos, bool backdrop) {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus;

    ImGui::SetNextWindowPos(pos);
    if (backdrop) {
        /* Numbers sitting on top of the plate need something behind them. */
        ImGui::SetNextWindowBgAlpha(0.88f);
    } else {
        ImGui::SetNextWindowBgAlpha(0.0f);
        flags |= ImGuiWindowFlags_NoBackground;
    }
    ImGui::Begin(id, NULL, flags);
}

static void draw_nav_controls(App *app, ImVec2 region_min) {
    overlay_begin("##nav", ImVec2(region_min.x + 16.0f, region_min.y + 100.0f), false);

    const float btn = 34.0f;
    struct { const char *label; const char *tip; } buttons[] = {
        { "H", "Home view" },
        { "F", "Fit selection" },
        { "+", "Zoom in" },
        { "-", "Zoom out" },
        { "O", "Toggle orthographic / perspective" }
    };
    for (int i = 0; i < 5; ++i) {
        ImGui::PushID(i);
        if (ImGui::Button(buttons[i].label, ImVec2(btn, btn))) {
            switch (i) {
            case 0: camera_home(&app->camera); break;
            case 1: ui_action_frame_selection(app); break;
            case 2: camera_zoom(&app->camera, 1.5f); break;
            case 3: camera_zoom(&app->camera, -1.5f); break;
            case 4: app->camera.orthographic = !app->camera.orthographic; break;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", buttons[i].tip);
        ImGui::PopID();
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    /* Transform mode strip. The keys are the primary route; these make the
     * current mode visible. */
    struct { const char *label; TransformMode mode; const char *tip; } modes[] = {
        { "M", XFORM_MOVE,   "Move mode: drag the body, or the arrows for Z" },
        { "S", XFORM_SCALE,  "Scale mode (S): drag a corner or face grip" },
        { "D", XFORM_ROTATE, "Rotate mode (D): drag a ring, shift+click to pin the origin" },
        { "B", XFORM_BEVEL,  "Bevel mode (B): click edges, shift+B to set the amount" }
    };
    for (int i = 0; i < 4; ++i) {
        ImGui::PushID(100 + i);
        bool active = (app->ui.mode == modes[i].mode);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(modes[i].label, ImVec2(btn, btn))) ui_set_mode(app, modes[i].mode);
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", modes[i].tip);
        ImGui::PopID();
    }

    ImGui::End();
}

static void draw_snap_controls(App *app, ImVec2 region_min, ImVec2 region_size) {
    overlay_begin("##snap", ImVec2(region_min.x + region_size.x - 250.0f,
                                   region_min.y + region_size.y - 44.0f), false);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Snap Grid");
    ImGui::SameLine();

    char label[32];
    snprintf(label, sizeof(label), "%g mm", app->ui.snap_grid_mm);
    ImGui::SetNextItemWidth(96.0f);
    if (ImGui::BeginCombo("##snap_combo", label)) {
        for (int i = 0; i < SNAP_COUNT; ++i) {
            char entry[32];
            snprintf(entry, sizeof(entry), "%g mm", SNAP_VALUES[i]);
            if (ImGui::Selectable(entry, SNAP_VALUES[i] == app->ui.snap_grid_mm)) {
                ui_set_snap_grid(app, SNAP_VALUES[i]);
            }
        }
        ImGui::Separator();

        /* Any value, not just the presets: real parts are not always on a
         * round grid. */
        static float custom = 1.0f;
        ImGui::SetNextItemWidth(70.0f);
        ImGui::InputFloat("##custom", &custom, 0.0f, 0.0f, "%.3f");
        ImGui::SameLine();
        if (ImGui::Button("Set")) {
            ui_set_snap_grid(app, custom);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Snap step for moves, nudges and scaling.\n[ and ] step through the presets.");

    ImGui::End();
}

/*
 * Value counters, one row per mode, anchored under the selection.
 *
 * Every mode gets the same treatment: read the current numbers back, and let any
 * of them be clicked and typed. Dragging is for roughing a shape out; typing is
 * the only way to hit an exact dimension, so both routes exist everywhere rather
 * than only for moves.
 *
 * Move and rotate show the change since the reference was taken; scale shows
 * absolute sizes in mm, because that is the number one actually wants to set.
 */
static void counter_apply(App *app, int axis, float wanted) {
    EditReference *r = &app->ui.edit_ref;
    Workplane plane = ui_active_workplane(app);
    Bounds sel = selection_box(app, plane);

    switch (app->ui.mode) {
    case XFORM_SCALE: {
        /* Typed sizes grow away from the box's low corner, matching the anchor a
         * corner grip drag uses, so an object on the plate stays on it. */
        if (!sel.valid) return;
        Vec3 size = bounds_size(sel);
        float current = (axis == 0) ? size.x : (axis == 1) ? size.y : size.z;
        if (current < 1e-4f || wanted < 1e-4f) return;

        Vec3 factor = vec3(1.0f, 1.0f, 1.0f);
        float f = wanted / current;
        if (axis == 0) factor.x = f;
        else if (axis == 1) factor.y = f;
        else factor.z = f;
        scene_scale_selection(&app->scene, workplane_to_world(plane, sel.min), factor, plane.basis);
        break;
    }
    case XFORM_ROTATE: {
        if (!r->valid) return;
        float current = (axis == 0) ? r->rotation.x : (axis == 1) ? r->rotation.y : r->rotation.z;
        float step = wanted - current;
        if (fabsf(step) < 1e-4f) return;

        Vec3 pivot = rotation_pivot(app, plane, sel);
        scene_rotate_selection(&app->scene, pivot, workplane_axis(plane, axis), step);
        if (axis == 0) r->rotation.x = wanted;
        else if (axis == 1) r->rotation.y = wanted;
        else r->rotation.z = wanted;
        break;
    }
    default: {
        if (!r->valid) return;
        /* The stored offset is a world vector; the counters read and write it
         * in plane coordinates, so it survives a change of plane by simply
         * re-expressing itself. */
        Vec3 offset = workplane_from_world(plane, r->offset);
        float current = (axis == 0) ? offset.x : (axis == 1) ? offset.y : offset.z;

        Vec3 delta = vec3(0.0f, 0.0f, 0.0f);
        if (axis == 0) delta.x = wanted - current;
        else if (axis == 1) delta.y = wanted - current;
        else delta.z = wanted - current;

        Vec3 world_delta = workplane_to_world(plane, delta);
        scene_translate_selection(&app->scene, world_delta);
        move_ref_add(app, world_delta);
        break;
    }
    }
}

static void counter_reset(App *app) {
    EditReference *r = &app->ui.edit_ref;
    if (!r->valid) return;

    if (app->ui.mode == XFORM_ROTATE) {
        Workplane plane = ui_active_workplane(app);
        Bounds sel = selection_box(app, plane);
        Vec3 pivot = rotation_pivot(app, plane, sel);
        /* Undone in reverse order, since rotations do not commute. */
        scene_rotate_selection(&app->scene, pivot, workplane_axis(plane, 2), -r->rotation.z);
        scene_rotate_selection(&app->scene, pivot, workplane_axis(plane, 1), -r->rotation.y);
        scene_rotate_selection(&app->scene, pivot, workplane_axis(plane, 0), -r->rotation.x);
        r->rotation = vec3(0.0f, 0.0f, 0.0f);
        return;
    }
    if (app->ui.mode == XFORM_MOVE) {
        scene_translate_selection(&app->scene, vec3_mul(r->offset, -1.0f));
        r->offset = vec3(0.0f, 0.0f, 0.0f);
    }
}

static void draw_edit_counters(App *app, ImVec2 region_min, ImVec2 region_size) {
    if (app->scene.selection.empty()) return;
    /* Nothing has been edited yet and the selection is still changing under the
     * band, so the row would only flicker. */
    if (app->ui.rect_select.active) return;

    /* Captured here rather than only on a drag, so a selection made in the tree
     * can be adjusted by typing straight away. */
    move_ref_begin(app, false);

    Workplane plane = ui_active_workplane(app);
    Bounds sel = selection_box(app, plane);
    if (!sel.valid) return;

    Vec3 centre = bounds_center(sel);
    Vec3 anchor = workplane_to_world(plane, vec3(centre.x, centre.y, sel.min.z));
    float sx, sy;
    if (!camera_project(app->camera, (int)region_size.x, (int)region_size.y, anchor, &sx, &sy)) {
        return;
    }

    float width = 360.0f;
    float px = region_min.x + sx - width * 0.5f;
    float py = region_min.y + sy + 24.0f;
    if (px < region_min.x + 8.0f) px = region_min.x + 8.0f;
    if (px + width > region_min.x + region_size.x - 8.0f) {
        px = region_min.x + region_size.x - width - 8.0f;
    }
    if (py > region_min.y + region_size.y - 40.0f) py = region_min.y + region_size.y - 40.0f;

    overlay_begin("##counters", ImVec2(px, py), true);

    Vec3 size = bounds_size(sel);
    const EditReference &r = app->ui.edit_ref;

    float value[3];
    const char *format;
    const char *tip;
    switch (app->ui.mode) {
    case XFORM_SCALE:
        value[0] = size.x; value[1] = size.y; value[2] = size.z;
        format = "%s %.2f";
        tip = "Size in mm. Click to type an exact dimension;\nit grows from the low corner.";
        break;
    case XFORM_ROTATE:
        value[0] = r.rotation.x; value[1] = r.rotation.y; value[2] = r.rotation.z;
        format = "%s %+.1f";
        tip = "Degrees turned about this axis.\nClick to type an exact angle.";
        break;
    default: {
        /* Shown in plane coordinates, since that is the frame the move
         * happened in. */
        Vec3 offset = workplane_from_world(plane, r.offset);
        value[0] = offset.x; value[1] = offset.y; value[2] = offset.z;
        format = "%s %+.2f";
        tip = "Offset in mm from the marked location, along the active plane.\n"
              "Click to type an exact value.";
        break;
    }
    }

    const char *axis_label[3] = { "X", "Y", "Z" };
    for (int axis = 0; axis < 3; ++axis) {
        ImGui::PushID(axis);
        char text[48];
        snprintf(text, sizeof(text), format, axis_label[axis], value[axis]);
        if (ImGui::Button(text)) {
            app->ui.counter_edit_axis = axis;
            app->ui.counter_edit_value = value[axis];
            ImGui::OpenPopup("##counter_edit");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);

        if (ImGui::BeginPopup("##counter_edit")) {
            ImGui::TextUnformatted(app->ui.mode == XFORM_ROTATE ? "Degrees" : "Millimeters");
            /* Focused on open so the value can be typed straight away. */
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(110.0f);
            bool commit = ImGui::InputFloat("##value", &app->ui.counter_edit_value, 0.0f, 0.0f,
                                            "%.3f", ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button("Apply") || commit) {
                if (app->ui.counter_edit_axis >= 0) {
                    counter_apply(app, app->ui.counter_edit_axis, app->ui.counter_edit_value);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        ImGui::SameLine();
    }

    /* Nothing to undo for scale: its counters are absolute sizes, not a delta. */
    bool can_reset = (app->ui.mode != XFORM_SCALE);
    if (!can_reset) ImGui::BeginDisabled();
    if (ImGui::Button("Reset")) counter_reset(app);
    if (!can_reset) ImGui::EndDisabled();
    if (can_reset && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Undo the change since the marked state.");
    }

    ImGui::End();
}

static void draw_status_line(App *app, ImVec2 region_min, ImVec2 region_size) {
    if (app->ui.status.empty()) return;

    overlay_begin("##status", ImVec2(region_min.x + 16.0f,
                                     region_min.y + region_size.y - 44.0f), false);
    ImVec4 col = app->ui.status_is_error ? ImVec4(0.72f, 0.15f, 0.15f, 1.0f)
                                         : ImVec4(0.25f, 0.45f, 0.25f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(app->ui.status.c_str());
    ImGui::PopStyleColor();
    ImGui::End();
}

/* Frame */

void ui_build_overlay(const App *app, RenderOverlay *out) {
    render_overlay_init(out);

    const UiState &ui = app->ui;

    /*
     * Align and mirror work on world axes, so while either is up the overlay
     * falls back to the world frame rather than mixing two frames on screen.
     * Their own bounds below are world bounds for the same reason.
     */
    bool world_tool = (ui.align_active || ui.mirror_active);
    Workplane plane = world_tool ? workplane_identity() : ui_active_workplane(app);
    out->plane = plane;
    out->show_plane = (!world_tool && ui.workplane_mode != WORKPLANE_WORLD);

    /* The old spot is only worth marking once the object has actually left it,
     * otherwise the ghost sits exactly on the selection box. */
    Vec3 off = ui.edit_ref.offset;
    bool moved = (fabsf(off.x) > 1e-4f || fabsf(off.y) > 1e-4f || fabsf(off.z) > 1e-4f);
    if (!world_tool && ui.edit_ref.valid && ui.edit_ref.start_bounds.valid && moved) {
        out->show_ghost = true;
        out->ghost = ui.edit_ref.start_bounds;
    }
    if (app->scene.selection.empty()) return;

    Bounds sel = scene_selection_bounds_in(&app->scene, mat3_transposed(plane.basis));
    if (!sel.valid) return;

    out->show_selection = true;
    out->selection = sel;

    /* Align and mirror replace the transform gizmos while they are up: showing
     * both at once would put two sets of grabbable things in the same space. */
    if (ui.align_active) {
        out->show_align = true;
        out->align_reference = scene_node_bounds(&app->scene, app->scene.selection[0]);
        out->align_span = sel;
        out->align_hover_axis = ui.align_hover_axis;
        out->align_hover_slot = ui.align_hover_slot;

        /* Preview: where each node would land if this line were clicked. */
        if (ui.align_hover_axis >= 0 && out->align_reference.valid) {
            float target = scene_align_target(out->align_reference, ui.align_hover_axis,
                                              ui.align_hover_slot);
            for (size_t i = 0; i < app->scene.selection.size(); ++i) {
                int id = app->scene.selection[i];
                if (id == app->scene.selection[0]) continue;

                Bounds b = scene_node_bounds(&app->scene, id);
                if (!b.valid) continue;

                Vec3 v = (ui.align_hover_slot == ALIGN_SLOT_MIN) ? b.min
                       : (ui.align_hover_slot == ALIGN_SLOT_MAX) ? b.max : bounds_center(b);
                float current = (ui.align_hover_axis == 0) ? v.x
                              : (ui.align_hover_axis == 1) ? v.y : v.z;
                float shift = target - current;

                Vec3 delta = vec3(ui.align_hover_axis == 0 ? shift : 0.0f,
                                  ui.align_hover_axis == 1 ? shift : 0.0f,
                                  ui.align_hover_axis == 2 ? shift : 0.0f);
                b.min = vec3_add(b.min, delta);
                b.max = vec3_add(b.max, delta);
                out->preview.push_back(b);
            }
        }
        return;
    }

    if (ui.mirror_active) {
        out->show_mirror = true;
        out->mirror_bounds = sel;
        out->mirror_hover_axis = ui.mirror_hover_axis;

        if (ui.mirror_hover_axis >= 0) {
            int axis = ui.mirror_hover_axis;
            Vec3 c = bounds_center(sel);
            float plane = (axis == 0) ? c.x : (axis == 1) ? c.y : c.z;

            for (size_t i = 0; i < app->scene.selection.size(); ++i) {
                Bounds b = scene_node_bounds(&app->scene, app->scene.selection[i]);
                if (!b.valid) continue;

                /* Reflecting swaps min and max on that axis. */
                float lo = (axis == 0) ? b.min.x : (axis == 1) ? b.min.y : b.min.z;
                float hi = (axis == 0) ? b.max.x : (axis == 1) ? b.max.y : b.max.z;
                float new_lo = 2.0f * plane - hi;
                float new_hi = 2.0f * plane - lo;
                if (axis == 0) { b.min.x = new_lo; b.max.x = new_hi; }
                else if (axis == 1) { b.min.y = new_lo; b.max.y = new_hi; }
                else { b.min.z = new_lo; b.max.z = new_hi; }
                out->preview.push_back(b);
            }
        }
        return;
    }

    /* The selection box still draws, which is the feedback that matters; the
     * grips would only flicker as the band sweeps, and none of them can be
     * grabbed while the button is already down. */
    if (ui.rect_select.active) return;

    if (ui.mode == XFORM_BEVEL && ui.bevel_node != OBC_NO_NODE) {
        out->show_bevel = true;
        for (size_t i = 0; i < ui.bevel_edges.size(); ++i) {
            Vec3 a = scene_world_point(&app->scene, ui.bevel_node, ui.bevel_edges[i].a);
            Vec3 b = scene_world_point(&app->scene, ui.bevel_node, ui.bevel_edges[i].b);

            bool chosen = false;
            for (size_t k = 0; k < ui.bevel_selected.size(); ++k) {
                if (ui.bevel_selected[k] == (int)i) { chosen = true; break; }
            }
            std::vector<Vec3> *dst = chosen ? &out->bevel_chosen
                                   : ((int)i == ui.bevel_hover ? &out->bevel_hover
                                                               : &out->bevel_lines);
            dst->push_back(a);
            dst->push_back(b);
        }
        return; // the transform grips would only clutter the edge set
    }

    switch (ui.mode) {
    case XFORM_SCALE:
        out->show_scale_grips = true;
        out->hover_handle = ui.hover_handle;
        break;
    case XFORM_ROTATE:
        out->show_rings = true;
        /* World point: a pinned pivot was picked in world space, and the box
         * centre has to come back out of the plane. */
        out->pivot = ui.pivot_custom ? ui.pivot_point
                                     : workplane_to_world(plane, bounds_center(sel));
        out->pivot_custom = ui.pivot_custom;
        out->ring_radius = render_ring_radius(sel);
        out->hover_axis = ui.hover_axis;
        out->active_axis = (ui.drag.active && ui.drag.mode == XFORM_ROTATE) ? ui.drag.axis : -1;
        break;
    default:
        out->show_z_arrows = true;
        out->hover_z_arrow = ui.hover_z_arrow;
        break;
    }
}

void ui_draw_viewport(App *app) {
    float menu_h = ImGui::GetFrameHeight();
    float top = menu_h + app->ui.toolbar_height;
    /* Folded panes give their width back to the view rather than leaving a
     * gap, which is the whole point of folding them on a small screen. */
    float left = ui_left_width(app);
    float width = (float)app->window_w - left - ui_right_width(app);
    float height = (float)app->window_h - top;
    if (width < 1.0f) width = 1.0f;

    /* GL viewport rect, y measured from the bottom of the window. */
    app->ui.viewport.x = (int)left;
    app->ui.viewport.y = 0;
    app->ui.viewport.w = (int)width;
    app->ui.viewport.h = (int)height;

    move_ref_validate(app);

    ImGui::SetNextWindowPos(ImVec2(left, top));
    ImGui::SetNextWindowSize(ImVec2(width, height));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImVec2 region_min = ImVec2(left, top);
    ImVec2 region_size = ImVec2(width, height);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("##viewport", NULL, flags)) {
        viewport_handle_navigation(app, region_min, region_size);
        viewport_handle_drop(app, region_min, region_size);
        /* After the drop target, which has to bind to the stage button. Drawing
         * takes no input, so its position in the window does not matter. */
        draw_rect_select_band(app, region_min);
    }
    ImGui::End();
    ImGui::PopStyleVar();

    /* Controls come after, in their own windows, so they sit above the stage
     * and get first claim on the mouse. */
    draw_nav_controls(app, region_min);
    draw_snap_controls(app, region_min, region_size);
    draw_edit_counters(app, region_min, region_size);
    draw_status_line(app, region_min, region_size);

    viewport_handle_keys(app, region_size);
}
