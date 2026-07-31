#ifndef OBC_RENDER_H
#define OBC_RENDER_H

#include <vector>

#include "camera.h"
#include "project.h"
#include "scene.h"
#include "workplane.h"

/*
 * All drawing is fixed function GL, kept inside the 1.2 feature set: client
 * side vertex arrays, the matrix stack, no shaders and no FBOs. Shading is
 * computed on the CPU per triangle so GL_LIGHTING state never enters the
 * picture.
 */

struct ViewportRect {
    int x, y, w, h; // y measured from the bottom, as GL wants it
};

/* Vertical move arrows, addressed like scale grips but numbered clear of them. */
#define OBC_ZARROW_UP 100
#define OBC_ZARROW_DOWN 101

/*
 * Editing furniture drawn over the scene. What is shown depends on the active
 * transform mode, so the view only ever offers the grips that currently do
 * something. The picking code in ui_viewport reads the same geometry helpers
 * below, so what is drawn and what responds cannot diverge.
 */
struct RenderOverlay {
    /*
     * The active editing plane. The selection box, its grips, the ghost, the
     * preview boxes and the rotation rings are all expressed in its
     * coordinates, so "plane" plus a Bounds is an oriented box rather than an
     * axis aligned one. Align and mirror are the exception and stay in world
     * space; their own bounds below are world bounds.
     */
    Workplane plane;
    bool show_plane;      // draw the plane itself, i.e. it is not the workplate

    bool show_selection;
    Bounds selection;     // plane coordinates

    bool show_ghost;      // where a moved selection came from
    Bounds ghost;         // plane coordinates

    bool show_scale_grips;
    int hover_handle;

    bool show_z_arrows;
    int hover_z_arrow;

    /*
     * Align and mirror overlays. Align draws three grabbable lines per axis at
     * the reference's low edge, centre and high edge; mirror draws one plane
     * per axis through the selection. Both preview the result as dashed boxes
     * before anything is committed.
     */
    bool show_align;
    Bounds align_reference;   // the object everything else aligns to
    Bounds align_span;        // whole selection, for how far the lines reach
    int align_hover_axis;
    int align_hover_slot;

    bool show_mirror;
    Bounds mirror_bounds;
    int mirror_hover_axis;

    std::vector<Bounds> preview; // where the selection would end up, plane coordinates

    /* Bevel tool: every bevellable edge of the object, and which of them are
     * chosen. World space, since they are drawn as they sit in the scene. */
    bool show_bevel;
    std::vector<Vec3> bevel_lines;
    std::vector<Vec3> bevel_chosen;
    std::vector<Vec3> bevel_hover;

    bool show_rings;
    Vec3 pivot;
    bool pivot_custom;
    float ring_radius;
    int hover_axis;
    int active_axis;
};

void render_overlay_init(RenderOverlay *o);

void render_frame(const Scene &scene, const Camera &camera, ViewportRect vp,
                  float snap_grid_mm, const RenderOverlay &overlay);

/* Reads the rendered 3D view back for the project file's preview image, scaled
 * down to at most max_width. Must be called while the frame is still in the
 * back buffer, i.e. before the swap. */
void render_capture_thumbnail(ViewportRect vp, int max_width, Thumbnail *out);

/*
 * Gizmo geometry, shared with the picker so what is drawn and what responds
 * cannot diverge. Every one of these takes plane coordinates and returns a
 * world point.
 */
Vec3 render_box_corner(const Bounds &local, const Workplane &plane, int index);
Vec3 render_ring_point(Vec3 pivot, float radius, const Workplane &plane,
                       int axis, int step, int steps);
float render_ring_radius(const Bounds &b);
Vec3 render_z_arrow_tip(const Bounds &local, const Workplane &plane, int which);

/* Endpoints of one align line, and the corners of one mirror plane. Both are
 * world space: align and mirror work on world axes, not on the plane. */
void render_align_line(const Bounds &reference, const Bounds &span, int axis, int slot,
                       Vec3 *from, Vec3 *to);
void render_mirror_plane(const Bounds &b, int axis, Vec3 corners[4]);

/* Small orientation cube drawn in the corner of the 3D view. */
void render_orientation_cube(const Camera &camera, ViewportRect vp);

/* Where that cube sits, in viewport-local pixels with y from the top, so the
 * picker and the renderer agree about the rectangle. */
#define OBC_CUBE_MARGIN 12
#define OBC_CUBE_SIZE 72

/*
 * Which face of the orientation cube a click landed on, as the world direction
 * to look from, or false when the click missed a face that is turned away.
 * "local" is in viewport pixels, y from the top.
 */
bool render_orientation_cube_pick(const Camera &camera, float local_x, float local_y,
                                  Vec3 *out_direction);

#endif
