#include "render.h"

#include <vector>

#include "gl_compat.h"

/* Palette. Matches the mockup: white stage, pale blue workplane. */
static const float COL_BG[3]        = { 1.00f, 1.00f, 1.00f };
static const float COL_GRID_FINE[3] = { 0.78f, 0.90f, 0.95f };
static const float COL_GRID_COARSE[3] = { 0.60f, 0.82f, 0.90f };
static const float COL_GRID_EDGE[3] = { 0.42f, 0.75f, 0.87f };
static const float COL_POSITIVE[3]  = { 0.68f, 0.74f, 0.80f };
static const float COL_SELECTED[3]  = { 0.40f, 0.66f, 0.92f };
static const float COL_NEGATIVE[3]  = { 0.55f, 0.55f, 0.55f };
static const float COL_OUTLINE[3]   = { 0.05f, 0.05f, 0.05f };
static const float COL_SEL_BOX[3]   = { 0.13f, 0.45f, 0.80f };

#define WORKPLANE_HALF 100.0f

static Vec3 light_dir(void) {
    return vec3_normalized(vec3(-0.35f, -0.45f, 0.82f));
}

/* Geometry drawing */

static void draw_lines(const std::vector<float> &verts, const float col[3], float width) {
    if (verts.empty()) return;
    glColor3fv(col);
    glLineWidth(width);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, &verts[0]);
    glDrawArrays(GL_LINES, 0, (GLsizei)(verts.size() / 3));
    glDisableClientState(GL_VERTEX_ARRAY);
}

/* Vec3 pairs rather than a flat float array: the overlay carries world points,
 * and Vec3 is three tightly packed floats, so the stride does the conversion. */
static void draw_vec3_lines(const std::vector<Vec3> &points, const float col[3], float width) {
    if (points.empty()) return;
    glColor3fv(col);
    glLineWidth(width);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(Vec3), &points[0]);
    glDrawArrays(GL_LINES, 0, (GLsizei)points.size());
    glDisableClientState(GL_VERTEX_ARRAY);
}

static void draw_grid(float snap_grid_mm) {
    std::vector<float> fine, coarse;
    float step = snap_grid_mm > 0.05f ? snap_grid_mm : 1.0f;

    /* Keep the fine grid from turning into a solid wash on large plates. */
    int fine_count = (int)(2.0f * WORKPLANE_HALF / step);
    if (fine_count > 400) step = 2.0f * WORKPLANE_HALF / 400.0f;

    float z = -0.01f; // just under the plate so object bottoms win the depth test
    for (float v = -WORKPLANE_HALF; v <= WORKPLANE_HALF + 0.001f; v += step) {
        bool is_coarse = fmodf(fabsf(v), 10.0f) < 0.001f;
        std::vector<float> *dst = is_coarse ? &coarse : &fine;
        dst->push_back(-WORKPLANE_HALF); dst->push_back(v); dst->push_back(z);
        dst->push_back(WORKPLANE_HALF);  dst->push_back(v); dst->push_back(z);
        dst->push_back(v); dst->push_back(-WORKPLANE_HALF); dst->push_back(z);
        dst->push_back(v); dst->push_back(WORKPLANE_HALF);  dst->push_back(z);
    }

    draw_lines(fine, COL_GRID_FINE, 1.0f);
    draw_lines(coarse, COL_GRID_COARSE, 1.0f);

    std::vector<float> border;
    float c[4][2] = {
        { -WORKPLANE_HALF, -WORKPLANE_HALF }, { WORKPLANE_HALF, -WORKPLANE_HALF },
        { WORKPLANE_HALF, WORKPLANE_HALF },   { -WORKPLANE_HALF, WORKPLANE_HALF }
    };
    for (int i = 0; i < 4; ++i) {
        int j = (i + 1) % 4;
        border.push_back(c[i][0]); border.push_back(c[i][1]); border.push_back(z);
        border.push_back(c[j][0]); border.push_back(c[j][1]); border.push_back(z);
    }
    draw_lines(border, COL_GRID_EDGE, 2.0f);
}

static void draw_mesh_solid(const Mesh &mesh, const float base[3], float alpha) {
    if (mesh.vertices.empty()) return;

    static std::vector<float> colors;
    colors.clear();
    colors.reserve(mesh.vertices.size() * 4);

    Vec3 l = light_dir();
    for (size_t i = 0; i < mesh.normals.size(); ++i) {
        /* Two sided: the sign of the normal must not make a face go black,
         * which also keeps mirrored or negatively scaled meshes readable. */
        float ndl = fabsf(vec3_dot(mesh.normals[i], l));
        float shade = 0.42f + 0.58f * ndl;
        colors.push_back(base[0] * shade);
        colors.push_back(base[1] * shade);
        colors.push_back(base[2] * shade);
        colors.push_back(alpha);
    }

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(Vec3), &mesh.vertices[0]);
    glColorPointer(4, GL_FLOAT, 0, &colors[0]);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)mesh.vertices.size());
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

static void draw_mesh_outline(const Mesh &mesh) {
    if (mesh.edges.empty()) return;
    glColor3fv(COL_OUTLINE);
    glLineWidth(1.0f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(Vec3), &mesh.edges[0]);
    glDrawArrays(GL_LINES, 0, (GLsizei)mesh.edges.size());
    glDisableClientState(GL_VERTEX_ARRAY);
}

static void push_node_transform(const SceneNode &n) {
    glPushMatrix();
    glTranslatef(n.position.x, n.position.y, n.position.z);
    glRotatef(n.rotation.z, 0.0f, 0.0f, 1.0f);
    glRotatef(n.rotation.y, 0.0f, 1.0f, 0.0f);
    glRotatef(n.rotation.x, 1.0f, 0.0f, 0.0f);
    glScalef(n.scale.x, n.scale.y, n.scale.z);
}

/* Selection state is inherited: selecting a group highlights its contents. */
static void draw_subtree(const Scene &scene, int id, Polarity pass, bool inherited_selection) {
    const SceneNode *n = scene_node(&scene, id);
    if (!n || !n->visible) return;

    bool selected = inherited_selection || scene_is_selected(&scene, id);

    push_node_transform(*n);

    if (n->kind == NODE_OBJECT && n->polarity == pass) {
        if (pass == POLARITY_POSITIVE) {
            draw_mesh_solid(n->mesh, selected ? COL_SELECTED : COL_POSITIVE, 1.0f);
            draw_mesh_outline(n->mesh);
        } else {
            /* Negative volumes read as transparent grey and must not occlude
             * what sits behind them, so depth writes stay off. */
            glEnable(GL_BLEND);
            glDepthMask(GL_FALSE);
            draw_mesh_solid(n->mesh, selected ? COL_SELECTED : COL_NEGATIVE, 0.35f);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            draw_mesh_outline(n->mesh);
        }
    }

    /* A merged node's children are its history; only the result mesh draws. */
    if (!n->merged) {
        for (size_t i = 0; i < n->children.size(); ++i) {
            draw_subtree(scene, n->children[i], pass, selected);
        }
    }

    glPopMatrix();
}

/* Thumbnail capture */

void render_capture_thumbnail(ViewportRect vp, int max_width, Thumbnail *out) {
    out->width = 0;
    out->height = 0;
    out->rgb.clear();
    if (vp.w <= 0 || vp.h <= 0 || max_width <= 0) return;

    std::vector<unsigned char> full((size_t)vp.w * (size_t)vp.h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(vp.x, vp.y, vp.w, vp.h, GL_RGB, GL_UNSIGNED_BYTE, &full[0]);

    /* Point sampled box scale: a preview does not need better, and this keeps
     * the save path free of any image library. */
    int scale = (vp.w + max_width - 1) / max_width;
    if (scale < 1) scale = 1;
    int tw = vp.w / scale;
    int th = vp.h / scale;
    if (tw <= 0 || th <= 0) return;

    out->width = tw;
    out->height = th;
    out->rgb.resize((size_t)tw * (size_t)th * 3);
    for (int y = 0; y < th; ++y) {
        /* glReadPixels returns rows bottom up; the thumbnail is stored top down. */
        int src_y = vp.h - 1 - (y * scale);
        if (src_y < 0) src_y = 0;
        for (int x = 0; x < tw; ++x) {
            size_t src = ((size_t)src_y * (size_t)vp.w + (size_t)(x * scale)) * 3;
            size_t dst = ((size_t)y * (size_t)tw + (size_t)x) * 3;
            out->rgb[dst + 0] = full[src + 0];
            out->rgb[dst + 1] = full[src + 1];
            out->rgb[dst + 2] = full[src + 2];
        }
    }
}

/* Gizmo geometry */

void render_overlay_init(RenderOverlay *o) {
    o->plane = workplane_identity();
    o->show_plane = false;
    o->show_selection = false;
    o->selection = bounds_empty();
    o->show_ghost = false;
    o->ghost = bounds_empty();
    o->show_scale_grips = false;
    o->hover_handle = -1;
    o->show_z_arrows = false;
    o->hover_z_arrow = -1;
    o->show_align = false;
    o->align_reference = bounds_empty();
    o->align_span = bounds_empty();
    o->align_hover_axis = -1;
    o->align_hover_slot = -1;
    o->show_mirror = false;
    o->mirror_bounds = bounds_empty();
    o->mirror_hover_axis = -1;
    o->preview.clear();
    o->show_bevel = false;
    o->bevel_lines.clear();
    o->bevel_chosen.clear();
    o->bevel_hover.clear();
    o->show_rings = false;
    o->pivot = vec3(0.0f, 0.0f, 0.0f);
    o->pivot_custom = false;
    o->ring_radius = 0.0f;
    o->hover_axis = -1;
    o->active_axis = -1;
}

float render_ring_radius(const Bounds &b) {
    if (!b.valid) return 10.0f;
    Vec3 s = bounds_size(b);
    float longest = s.x;
    if (s.y > longest) longest = s.y;
    if (s.z > longest) longest = s.z;
    float r = longest * 0.75f;
    return (r < 5.0f) ? 5.0f : r;
}

/*
 * Corner of an oriented box. Index bits are x, y, z, matching how scene_handle
 * numbers its corner grips, so a handle index and a corner index agree.
 */
Vec3 render_box_corner(const Bounds &local, const Workplane &plane, int index) {
    Vec3 p = vec3((index & 1) ? local.max.x : local.min.x,
                  (index & 2) ? local.max.y : local.min.y,
                  (index & 4) ? local.max.z : local.min.z);
    return workplane_to_world(plane, p);
}

/* The eight corners in the order the edge tables below expect: bottom face
 * first, going round, then the top face above it. */
static void box_corners(const Bounds &local, const Workplane &plane, Vec3 out[8]) {
    static const int order[8] = { 0, 1, 3, 2, 4, 5, 7, 6 };
    for (int i = 0; i < 8; ++i) out[i] = render_box_corner(local, plane, order[i]);
}

Vec3 render_ring_point(Vec3 pivot, float radius, const Workplane &plane,
                       int axis, int step, int steps) {
    if (steps <= 0) steps = 1;
    float a = 2.0f * 3.14159265358979f * (float)step / (float)steps;
    float c = cosf(a) * radius;
    float s = sinf(a) * radius;

    /* Rings lie in the plane's own axis planes, so rotating about "Z" turns
     * about the plane normal rather than about world up. */
    Vec3 local;
    switch (axis) {
    case 0:  local = vec3(0.0f, c, s); break;
    case 1:  local = vec3(c, 0.0f, s); break;
    default: local = vec3(c, s, 0.0f); break;
    }
    return vec3_add(pivot, workplane_to_world(plane, local));
}

/* Tip of the vertical move arrow, which is also its grab point. */
#define ZARROW_GAP 6.0f
#define ZARROW_LENGTH 14.0f

Vec3 render_z_arrow_tip(const Bounds &local, const Workplane &plane, int which) {
    if (!local.valid) return vec3(0.0f, 0.0f, 0.0f);
    Vec3 c = bounds_center(local);
    float z = (which == OBC_ZARROW_UP) ? local.max.z + ZARROW_GAP + ZARROW_LENGTH
                                       : local.min.z - ZARROW_GAP - ZARROW_LENGTH;
    return workplane_to_world(plane, vec3(c.x, c.y, z));
}

/* The arrows run along the plane normal, so they lift a part off whatever
 * surface the plane is sitting on rather than always along world up. */
static void draw_z_arrow(const Bounds &local, const Workplane &plane, int which, bool hovered) {
    Vec3 c = bounds_center(local);
    float base_z = (which == OBC_ZARROW_UP) ? local.max.z + ZARROW_GAP
                                            : local.min.z - ZARROW_GAP;
    Vec3 base = workplane_to_world(plane, vec3(c.x, c.y, base_z));
    Vec3 tip = render_z_arrow_tip(local, plane, which);
    float dir = (which == OBC_ZARROW_UP) ? 1.0f : -1.0f;

    std::vector<float> lines;
    lines.push_back(base.x); lines.push_back(base.y); lines.push_back(base.z);
    lines.push_back(tip.x);  lines.push_back(tip.y);  lines.push_back(tip.z);

    /* Four barbs, so the head reads as an arrow from any orbit angle. */
    float head = ZARROW_LENGTH * 0.4f;
    float wing = head * 0.5f;
    float offsets[4][2] = { { wing, 0.0f }, { -wing, 0.0f }, { 0.0f, wing }, { 0.0f, -wing } };
    float tip_z = (which == OBC_ZARROW_UP) ? local.max.z + ZARROW_GAP + ZARROW_LENGTH
                                           : local.min.z - ZARROW_GAP - ZARROW_LENGTH;
    for (int i = 0; i < 4; ++i) {
        Vec3 barb = workplane_to_world(plane, vec3(c.x + offsets[i][0], c.y + offsets[i][1],
                                                   tip_z - dir * head));
        lines.push_back(tip.x); lines.push_back(tip.y); lines.push_back(tip.z);
        lines.push_back(barb.x); lines.push_back(barb.y); lines.push_back(barb.z);
    }

    static const float col[3] = { 0.13f, 0.45f, 0.80f };
    static const float col_hot[3] = { 0.95f, 0.62f, 0.10f };
    draw_lines(lines, hovered ? col_hot : col, hovered ? 3.0f : 2.0f);

    std::vector<float> pt;
    pt.push_back(tip.x); pt.push_back(tip.y); pt.push_back(tip.z);
    glColor3fv(hovered ? col_hot : col);
    glPointSize(hovered ? 10.0f : 7.0f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, &pt[0]);
    glDrawArrays(GL_POINTS, 0, 1);
    glDisableClientState(GL_VERTEX_ARRAY);
}

static void draw_rotation_rings(const RenderOverlay &o) {
    static const float axis_col[3][3] = {
        { 0.85f, 0.25f, 0.25f }, // X
        { 0.20f, 0.65f, 0.30f }, // Y
        { 0.20f, 0.45f, 0.90f }  // Z
    };
    static const float hot[3] = { 0.95f, 0.62f, 0.10f };
    const int steps = 64;

    for (int axis = 0; axis < 3; ++axis) {
        std::vector<float> lines;
        for (int i = 0; i < steps; ++i) {
            Vec3 a = render_ring_point(o.pivot, o.ring_radius, o.plane, axis, i, steps);
            Vec3 b = render_ring_point(o.pivot, o.ring_radius, o.plane, axis, i + 1, steps);
            lines.push_back(a.x); lines.push_back(a.y); lines.push_back(a.z);
            lines.push_back(b.x); lines.push_back(b.y); lines.push_back(b.z);
        }
        bool lit = (axis == o.hover_axis || axis == o.active_axis);
        draw_lines(lines, lit ? hot : axis_col[axis], lit ? 3.0f : 1.6f);
    }

    /* Pivot marker, yellow once it has been pinned somewhere by hand. */
    std::vector<float> pt;
    pt.push_back(o.pivot.x); pt.push_back(o.pivot.y); pt.push_back(o.pivot.z);
    static const float pinned[3] = { 0.95f, 0.85f, 0.15f };
    static const float centre[3] = { 0.35f, 0.35f, 0.40f };
    glColor3fv(o.pivot_custom ? pinned : centre);
    glPointSize(o.pivot_custom ? 11.0f : 8.0f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, &pt[0]);
    glDrawArrays(GL_POINTS, 0, 1);
    glDisableClientState(GL_VERTEX_ARRAY);
}

/* Align and mirror gizmos */

static const float AXIS_COLOUR[3][3] = {
    { 0.85f, 0.25f, 0.25f }, // X
    { 0.20f, 0.65f, 0.30f }, // Y
    { 0.20f, 0.45f, 0.90f }  // Z
};
static const float AXIS_HOT[3] = { 0.95f, 0.62f, 0.10f };

/* Align lines are green on every axis: the axis is already obvious from the
 * direction the line runs, and one colour keeps them reading as one tool. */
static const float ALIGN_COLOUR[3] = { 0.15f, 0.70f, 0.30f };
static const float ALIGN_HOT[3] = { 0.35f, 1.00f, 0.45f };

#define ALIGN_LINE_MARGIN 8.0f

void render_align_line(const Bounds &reference, const Bounds &span, int axis, int slot,
                       Vec3 *from, Vec3 *to) {
    float at = scene_align_target(reference, axis, slot);

    /* Each line lies in the plane it would align to and runs through the middle
     * of the selection, so it reads as passing through the objects rather than
     * sitting on the plate under them. */
    Vec3 centre = bounds_center(span);
    Vec3 lo = span.min;
    Vec3 hi = span.max;
    lo.x -= ALIGN_LINE_MARGIN; lo.y -= ALIGN_LINE_MARGIN;
    hi.x += ALIGN_LINE_MARGIN; hi.y += ALIGN_LINE_MARGIN;

    switch (axis) {
    case 0: // plane of constant X, running along Y through the middle
        *from = vec3(at, lo.y, centre.z);
        *to = vec3(at, hi.y, centre.z);
        break;
    case 1: // plane of constant Y, running along X through the middle
        *from = vec3(lo.x, at, centre.z);
        *to = vec3(hi.x, at, centre.z);
        break;
    default: // plane of constant Z, running along X through the middle
        *from = vec3(lo.x, centre.y, at);
        *to = vec3(hi.x, centre.y, at);
        break;
    }
}

void render_mirror_plane(const Bounds &b, int axis, Vec3 corners[4]) {
    Vec3 c = bounds_center(b);
    Vec3 lo = b.min;
    Vec3 hi = b.max;
    float m = ALIGN_LINE_MARGIN;

    switch (axis) {
    case 0:
        corners[0] = vec3(c.x, lo.y - m, lo.z - m);
        corners[1] = vec3(c.x, hi.y + m, lo.z - m);
        corners[2] = vec3(c.x, hi.y + m, hi.z + m);
        corners[3] = vec3(c.x, lo.y - m, hi.z + m);
        break;
    case 1:
        corners[0] = vec3(lo.x - m, c.y, lo.z - m);
        corners[1] = vec3(hi.x + m, c.y, lo.z - m);
        corners[2] = vec3(hi.x + m, c.y, hi.z + m);
        corners[3] = vec3(lo.x - m, c.y, hi.z + m);
        break;
    default:
        corners[0] = vec3(lo.x - m, lo.y - m, c.z);
        corners[1] = vec3(hi.x + m, lo.y - m, c.z);
        corners[2] = vec3(hi.x + m, hi.y + m, c.z);
        corners[3] = vec3(lo.x - m, hi.y + m, c.z);
        break;
    }
}

static void draw_align_lines(const RenderOverlay &o) {
    if (!o.align_reference.valid || !o.align_span.valid) return;

    for (int axis = 0; axis < 3; ++axis) {
        for (int slot = 0; slot < 3; ++slot) {
            Vec3 a, b;
            render_align_line(o.align_reference, o.align_span, axis, slot, &a, &b);

            std::vector<float> line;
            line.push_back(a.x); line.push_back(a.y); line.push_back(a.z);
            line.push_back(b.x); line.push_back(b.y); line.push_back(b.z);

            bool lit = (axis == o.align_hover_axis && slot == o.align_hover_slot);
            /* The centre line is dashed so the three slots stay tellable apart
             * when they nearly coincide on a small object. */
            if (slot == ALIGN_SLOT_CENTER) {
                glEnable(GL_LINE_STIPPLE);
                glLineStipple(2, 0x0F0F);
            }
            draw_lines(line, lit ? ALIGN_HOT : ALIGN_COLOUR, lit ? 4.0f : 2.0f);
            if (slot == ALIGN_SLOT_CENTER) glDisable(GL_LINE_STIPPLE);

            /* A dot at each end marks where to grab. */
            std::vector<float> pts;
            pts.push_back(a.x); pts.push_back(a.y); pts.push_back(a.z);
            pts.push_back(b.x); pts.push_back(b.y); pts.push_back(b.z);
            glColor3fv(lit ? ALIGN_HOT : ALIGN_COLOUR);
            glPointSize(lit ? 11.0f : 7.0f);
            glEnableClientState(GL_VERTEX_ARRAY);
            glVertexPointer(3, GL_FLOAT, 0, &pts[0]);
            glDrawArrays(GL_POINTS, 0, 2);
            glDisableClientState(GL_VERTEX_ARRAY);
        }
    }
}

static void draw_mirror_planes(const RenderOverlay &o) {
    if (!o.mirror_bounds.valid) return;

    for (int axis = 0; axis < 3; ++axis) {
        Vec3 c[4];
        render_mirror_plane(o.mirror_bounds, axis, c);
        bool lit = (axis == o.mirror_hover_axis);

        std::vector<float> outline;
        for (int i = 0; i < 4; ++i) {
            Vec3 a = c[i];
            Vec3 b = c[(i + 1) % 4];
            outline.push_back(a.x); outline.push_back(a.y); outline.push_back(a.z);
            outline.push_back(b.x); outline.push_back(b.y); outline.push_back(b.z);
        }
        /* Both diagonals, so the quad reads as a plane rather than a frame. */
        outline.push_back(c[0].x); outline.push_back(c[0].y); outline.push_back(c[0].z);
        outline.push_back(c[2].x); outline.push_back(c[2].y); outline.push_back(c[2].z);
        outline.push_back(c[1].x); outline.push_back(c[1].y); outline.push_back(c[1].z);
        outline.push_back(c[3].x); outline.push_back(c[3].y); outline.push_back(c[3].z);

        if (!lit) {
            glEnable(GL_LINE_STIPPLE);
            glLineStipple(2, 0x5555);
        }
        draw_lines(outline, lit ? AXIS_HOT : AXIS_COLOUR[axis], lit ? 3.5f : 1.6f);
        if (!lit) glDisable(GL_LINE_STIPPLE);
    }
}

/* The twelve edges of a box, over the corner order box_corners produces. */
static const int BOX_EDGES[12][2] = {
    {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7}
};

static void append_box_edges(std::vector<float> *lines, const Bounds &local,
                             const Workplane &plane) {
    if (!local.valid) return;
    Vec3 v[8];
    box_corners(local, plane, v);
    for (int i = 0; i < 12; ++i) {
        for (int j = 0; j < 2; ++j) {
            Vec3 p = v[BOX_EDGES[i][j]];
            lines->push_back(p.x); lines->push_back(p.y); lines->push_back(p.z);
        }
    }
}

/* Where the selection would land, shown before anything is committed. */
static void draw_preview(const std::vector<Bounds> &boxes, const Workplane &plane) {
    static const float col[3] = { 0.95f, 0.62f, 0.10f };

    std::vector<float> lines;
    for (size_t k = 0; k < boxes.size(); ++k) append_box_edges(&lines, boxes[k], plane);
    if (lines.empty()) return;

    glEnable(GL_LINE_STIPPLE);
    glLineStipple(1, 0x00FF);
    draw_lines(lines, col, 2.0f);
    glDisable(GL_LINE_STIPPLE);
}

/* Dashed outline of where a moved selection started, plus a line to where it
 * is now. glLineStipple is fixed function and fine at the 1.2 target. */
static void draw_ghost(const Bounds &ghost, const Bounds &current, const Workplane &plane) {
    if (!ghost.valid) return;

    std::vector<float> lines;
    append_box_edges(&lines, ghost, plane);
    if (current.valid) {
        Vec3 a = workplane_to_world(plane, bounds_center(ghost));
        Vec3 b = workplane_to_world(plane, bounds_center(current));
        lines.push_back(a.x); lines.push_back(a.y); lines.push_back(a.z);
        lines.push_back(b.x); lines.push_back(b.y); lines.push_back(b.z);
    }

    static const float col[3] = { 0.55f, 0.60f, 0.66f };
    glEnable(GL_LINE_STIPPLE);
    glLineStipple(2, 0x3333);
    draw_lines(lines, col, 1.5f);
    glDisable(GL_LINE_STIPPLE);
}

static void draw_selection_box(const Bounds &local, const Workplane &plane) {
    if (!local.valid) return;
    std::vector<float> lines;
    append_box_edges(&lines, local, plane);
    draw_lines(lines, COL_SEL_BOX, 1.5f);
}

/* Grips come from scene_handle so the markers drawn are exactly the ones the
 * picker responds to. The hovered one is drawn larger and warmer. */
static void draw_scale_grips(const Bounds &local, const Workplane &plane, int hover) {
    if (!local.valid) return;

    std::vector<float> pts, hot_pts;
    for (int i = 0; i < OBC_HANDLE_COUNT; ++i) {
        /* scene_handle works in plane coordinates now, so the grip has to come
         * back out to world before it can be drawn. */
        TransformHandle h = scene_handle(local, i);
        Vec3 p = workplane_to_world(plane, h.pos);
        std::vector<float> *dst = (i == hover) ? &hot_pts : &pts;
        dst->push_back(p.x); dst->push_back(p.y); dst->push_back(p.z);
    }

    if (!pts.empty()) {
        glColor3fv(COL_SEL_BOX);
        glPointSize(7.0f);
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(3, GL_FLOAT, 0, &pts[0]);
        glDrawArrays(GL_POINTS, 0, (GLsizei)(pts.size() / 3));
        glDisableClientState(GL_VERTEX_ARRAY);
    }
    if (!hot_pts.empty()) {
        static const float hot[3] = { 0.95f, 0.62f, 0.10f };
        glColor3fv(hot);
        glPointSize(11.0f);
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(3, GL_FLOAT, 0, &hot_pts[0]);
        glDrawArrays(GL_POINTS, 0, (GLsizei)(hot_pts.size() / 3));
        glDisableClientState(GL_VERTEX_ARRAY);
    }
}

/*
 * The active editing plane, drawn only when it is not the workplate - the
 * plate already draws itself as the grid, and a second patch on top of it
 * would just z-fight. A small ruled square with its two in-plane axis
 * directions picked out, so which way X and Y run is visible before dragging
 * anything.
 */
#define PLANE_PATCH_HALF 30.0f
#define PLANE_PATCH_STEPS 6

static void draw_workplane(const Workplane &plane) {
    static const float col[3] = { 0.35f, 0.62f, 0.72f };
    static const float col_x[3] = { 0.85f, 0.30f, 0.30f };
    static const float col_y[3] = { 0.25f, 0.70f, 0.35f };

    std::vector<float> grid;
    float step = 2.0f * PLANE_PATCH_HALF / (float)PLANE_PATCH_STEPS;
    for (int i = 0; i <= PLANE_PATCH_STEPS; ++i) {
        float t = -PLANE_PATCH_HALF + step * (float)i;
        Vec3 a = vec3_add(plane.origin, workplane_to_world(plane, vec3(t, -PLANE_PATCH_HALF, 0.0f)));
        Vec3 b = vec3_add(plane.origin, workplane_to_world(plane, vec3(t, PLANE_PATCH_HALF, 0.0f)));
        Vec3 c = vec3_add(plane.origin, workplane_to_world(plane, vec3(-PLANE_PATCH_HALF, t, 0.0f)));
        Vec3 d = vec3_add(plane.origin, workplane_to_world(plane, vec3(PLANE_PATCH_HALF, t, 0.0f)));
        grid.push_back(a.x); grid.push_back(a.y); grid.push_back(a.z);
        grid.push_back(b.x); grid.push_back(b.y); grid.push_back(b.z);
        grid.push_back(c.x); grid.push_back(c.y); grid.push_back(c.z);
        grid.push_back(d.x); grid.push_back(d.y); grid.push_back(d.z);
    }
    glEnable(GL_LINE_STIPPLE);
    glLineStipple(2, 0x5555);
    draw_lines(grid, col, 1.0f);
    glDisable(GL_LINE_STIPPLE);

    for (int axis = 0; axis < 2; ++axis) {
        Vec3 dir = workplane_axis(plane, axis);
        Vec3 tip = vec3_add(plane.origin, vec3_mul(dir, PLANE_PATCH_HALF));
        std::vector<float> line;
        line.push_back(plane.origin.x); line.push_back(plane.origin.y); line.push_back(plane.origin.z);
        line.push_back(tip.x); line.push_back(tip.y); line.push_back(tip.z);
        draw_lines(line, axis == 0 ? col_x : col_y, 2.5f);
    }
}

/* Frame */

void render_frame(const Scene &scene, const Camera &camera, ViewportRect vp,
                  float snap_grid_mm, const RenderOverlay &overlay) {
    if (vp.w <= 0 || vp.h <= 0) return;

    glEnable(GL_SCISSOR_TEST);
    glViewport(vp.x, vp.y, vp.w, vp.h);
    glScissor(vp.x, vp.y, vp.w, vp.h);

    glClearColor(COL_BG[0], COL_BG[1], COL_BG[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glShadeModel(GL_FLAT);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);

    camera_apply_gl(camera, vp.w, vp.h);

    draw_grid(snap_grid_mm);

    for (size_t i = 0; i < scene.roots.size(); ++i) {
        draw_subtree(scene, scene.roots[i], POLARITY_POSITIVE, false);
    }
    for (size_t i = 0; i < scene.roots.size(); ++i) {
        draw_subtree(scene, scene.roots[i], POLARITY_NEGATIVE, false);
    }

    /* Editing furniture draws without depth testing so a grip is never buried
     * inside the very object it belongs to. */
    glDisable(GL_DEPTH_TEST);
    if (overlay.show_plane) draw_workplane(overlay.plane);

    const Bounds &sel = overlay.selection;
    if (overlay.show_ghost) draw_ghost(overlay.ghost, sel, overlay.plane);
    if (overlay.show_selection && sel.valid) {
        draw_selection_box(sel, overlay.plane);
        if (overlay.show_scale_grips) draw_scale_grips(sel, overlay.plane, overlay.hover_handle);
        if (overlay.show_z_arrows) {
            draw_z_arrow(sel, overlay.plane, OBC_ZARROW_UP,
                         overlay.hover_z_arrow == OBC_ZARROW_UP);
            draw_z_arrow(sel, overlay.plane, OBC_ZARROW_DOWN,
                         overlay.hover_z_arrow == OBC_ZARROW_DOWN);
        }
    }
    if (overlay.show_bevel) {
        /* Candidates thin and cool, chosen ones thick and warm, so which edges
         * are in the set reads at a glance. */
        static const float col_edge[3] = { 0.20f, 0.55f, 0.85f };
        static const float col_chosen[3] = { 0.95f, 0.62f, 0.10f };
        static const float col_hover[3] = { 0.20f, 0.80f, 0.95f };
        draw_vec3_lines(overlay.bevel_lines, col_edge, 1.5f);
        draw_vec3_lines(overlay.bevel_hover, col_hover, 3.0f);
        draw_vec3_lines(overlay.bevel_chosen, col_chosen, 4.0f);
    }
    if (overlay.show_rings) draw_rotation_rings(overlay);
    if (overlay.show_align) draw_align_lines(overlay);
    if (overlay.show_mirror) draw_mirror_planes(overlay);
    if (!overlay.preview.empty()) draw_preview(overlay.preview, overlay.plane);
    glEnable(GL_DEPTH_TEST);

    render_orientation_cube(camera, vp);

    glDisable(GL_SCISSOR_TEST);
}

/*
 * The camera the cube is drawn with: the scene camera's orientation, but
 * looking at the origin from a fixed distance and always orthographic. The
 * picker builds the same one, so a face is hit exactly where it is drawn.
 */
static Camera orientation_cube_camera(const Camera &camera) {
    Camera c = camera;
    c.target = vec3(0.0f, 0.0f, 0.0f);
    c.distance = 3.2f;
    c.orthographic = true;
    return c;
}

bool render_orientation_cube_pick(const Camera &camera, float local_x, float local_y,
                                  Vec3 *out_direction) {
    float x = local_x - (float)OBC_CUBE_MARGIN;
    float y = local_y - (float)OBC_CUBE_MARGIN;
    if (x < 0.0f || y < 0.0f || x > (float)OBC_CUBE_SIZE || y > (float)OBC_CUBE_SIZE) {
        return false;
    }

    Camera c = orientation_cube_camera(camera);
    Vec3 forward = vec3_normalized(vec3_sub(c.target, camera_eye(c)));

    static const Vec3 FACES[6] = {
        { 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f }
    };

    bool found = false;
    float best = 0.0f;
    for (int i = 0; i < 6; ++i) {
        /* Only faces turned towards the viewer can be clicked; the ones behind
         * project into the same square and would steal the hit. */
        if (vec3_dot(FACES[i], forward) > -0.05f) continue;

        float fx, fy;
        if (!camera_project(c, OBC_CUBE_SIZE, OBC_CUBE_SIZE,
                            vec3_mul(FACES[i], 0.5f), &fx, &fy)) {
            continue;
        }
        float dx = fx - x;
        float dy = fy - y;
        float d2 = dx * dx + dy * dy;
        if (!found || d2 < best) {
            best = d2;
            found = true;
            *out_direction = FACES[i];
        }
    }
    return found;
}

void render_orientation_cube(const Camera &camera, ViewportRect vp) {
    const int size = OBC_CUBE_SIZE;
    const int margin = OBC_CUBE_MARGIN;
    if (vp.w < size + margin || vp.h < size + margin) return;

    ViewportRect cube_vp;
    cube_vp.x = vp.x + margin;
    cube_vp.y = vp.y + vp.h - size - margin;
    cube_vp.w = size;
    cube_vp.h = size;

    glViewport(cube_vp.x, cube_vp.y, cube_vp.w, cube_vp.h);
    glScissor(cube_vp.x, cube_vp.y, cube_vp.w, cube_vp.h);
    glClear(GL_DEPTH_BUFFER_BIT);

    Camera c = orientation_cube_camera(camera);
    camera_apply_gl(c, cube_vp.w, cube_vp.h);

    /* Faces get distinct greys so the current orientation is readable even
     * without face labels; labelled faces come with the view cube widget. */
    Mesh cube = mesh_make_cube(1.0f);
    glPushMatrix();
    glTranslatef(0.0f, 0.0f, -0.5f);
    static const float col[3] = { 0.85f, 0.86f, 0.88f };
    draw_mesh_solid(cube, col, 1.0f);
    draw_mesh_outline(cube);
    glPopMatrix();

    glViewport(vp.x, vp.y, vp.w, vp.h);
    glScissor(vp.x, vp.y, vp.w, vp.h);
}
