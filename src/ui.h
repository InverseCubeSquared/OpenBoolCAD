#ifndef OBC_UI_H
#define OBC_UI_H

#include <string>
#include <vector>

#include "bevel.h"
#include "polyhedron.h"
#include "gear.h"
#include "text3d.h"
#include "import_svg.h"
#include "render.h"
#include "scene.h"

struct App;

/* Drag and drop payload id shared by the bookshelf and the 3D view. */
#define OBC_DND_PRIMITIVE "OBC_PRIMITIVE"
#define OBC_DND_TREE_NODE "OBC_TREE_NODE"

struct BookshelfItem {
    PrimitiveKind kind;
    Polarity polarity;
};

/*
 * File chooser state. Open mode picks an existing file; save mode adds a name
 * field. The directory persists between uses so consecutive saves stay put.
 */
struct FileBrowser {
    bool open;                       // request to open the popup
    bool save_mode;
    std::string dir;
    std::vector<std::string> dirs;
    std::vector<std::string> files;
    std::string chosen;
    char name_buf[256];
    std::string filter;              // extension to show, empty for all
    std::string title;
};

/*
 * What a left drag on the selection does. Move is the default; S and D switch
 * to scale and rotate, and pressing the same key again returns to move.
 */
enum TransformMode {
    XFORM_MOVE = 0,
    XFORM_SCALE,
    XFORM_ROTATE,
    XFORM_BEVEL
};

/*
 * A drag is applied absolutely, not incrementally: the transform each node had
 * when the drag started is kept here and the whole delta is recomputed from the
 * current mouse position every frame. Incremental application accumulates
 * rounding error and makes snapping jitter.
 */
struct DragState {
    bool active;
    TransformMode mode;
    int handle;              // index into scene_handle, or -1 for a body drag
    Vec3 start_plane_point;  // where the press landed on the drag plane
    Bounds start_bounds;
    Vec3 rotate_axis;
    int axis;                // 0/1/2 while dragging a rotation ring
    float start_angle;       // angle in the ring plane when the drag began
    float applied_degrees;   // rotation already written to the scene
    Vec3 applied_factor;     // scale already written to the scene
    Vec3 applied_delta;      // translation already written to the scene
};

/*
 * Rubber band selection: a left drag that starts on empty space sweeps a
 * rectangle and selects every object it touches.
 *
 * Like DragState, this recomputes its whole result from the current pointer
 * position each frame rather than accumulating - "base" is the selection the
 * gesture started from, so shrinking the band releases objects again instead of
 * leaving them stuck selected.
 *
 * "swept" gates on a few pixels of travel. Without it a plain click on empty
 * space near an object would catch that object's bounding box, which extends
 * past the mesh, and select something the user did not point at.
 */
struct RectSelect {
    bool active;
    bool swept;
    float x0, y0;            // press point, viewport local pixels
    float x1, y1;            // pointer now
    std::vector<int> base;
};

/*
 * What the selection looked like before the current editing gesture, kept so
 * the old spot can be marked in the view and the change from it shown as
 * editable counters. Held until the selection changes, so a nudge or a drag can
 * be corrected by typing an exact value afterwards.
 *
 * Move and rotate counters are deltas against this reference; scale counters are
 * absolute sizes read from the selection bounds, since a size in mm is what one
 * actually wants to type.
 */
struct EditReference {
    bool valid;
    Bounds start_bounds;
    Vec3 offset;             // mm moved since the reference was taken
    Vec3 rotation;           // degrees turned about each world axis
    std::vector<int> nodes;  // the selection this reference belongs to
};

struct UiState {
    float left_width;
    float right_width;
    float toolbar_height;

    /*
     * Panes can be folded away for small screens. The widths above stay put
     * while a pane is hidden, so unfolding restores the layout rather than
     * resetting it; ui_left_width / ui_right_width are what the layout should
     * actually use.
     */
    bool show_tree;
    bool show_shelf;

    ViewportRect viewport;   // GL rect of the 3D view, y from the bottom
    bool viewport_hovered;

    int rename_node;         // node being renamed inline, or OBC_NO_NODE
    char rename_buf[64];
    bool rename_focus;       // grab keyboard focus on the first rename frame only

    float snap_grid_mm;
    bool show_about;
    bool show_help;

    TransformMode mode;
    DragState drag;
    RectSelect rect_select;
    int hover_handle;        // scale grip under the cursor, or -1
    int hover_axis;          // rotation ring under the cursor, or -1
    int hover_z_arrow;       // vertical move arrow under the cursor, or -1

    /* Rotation pivot. Shift+click in rotate mode pins it to a point; without
     * one, rotation happens about the selection centre. */
    bool pivot_custom;
    Vec3 pivot_point;

    /*
     * Editing plane, per PLAN.md. Only the face plane is stored: the world and
     * object planes are derived from the world axes and from the selection, so
     * keeping a copy would only let it go stale. See ui_active_workplane().
     */
    int workplane_mode;       // WORKPLANE_*
    Workplane face_plane;     // valid while workplane_mode is WORKPLANE_FACE
    bool plane_pick_active;   // SHIFT+P or CTRL+P armed, waiting for a face click
    /* SHIFT+P centres the plane on the face, CTRL+P leaves it where the cursor
     * landed. The centre is what you want for placing something on a face; the
     * exact point is what you want for working at a spot on one. */
    bool plane_pick_center;

    /*
     * Align and mirror are modal tools rather than one-shot commands: the lines
     * stay up so several axes can be applied, with Esc to leave.
     */
    bool align_active;
    bool mirror_active;
    int align_hover_axis;
    int align_hover_slot;
    int mirror_hover_axis;

    EditReference edit_ref;
    int counter_edit_axis;    // axis whose offset is being typed, or -1
    float counter_edit_value;

    /* Transient one-line feedback, shown at the bottom of the 3D view.
     * Merge failures surface here instead of being swallowed. */
    std::string status;
    bool status_is_error;

    /*
     * File chooser. One browser serves Open, Save As and Export; file_action
     * says what to do with the path it returns. project_path is remembered so a
     * plain Save after a Save As does not ask again.
     */
    FileBrowser browser;
    int file_action;          // FILE_PROMPT_* below, or 0 for none
    std::string project_path;

    /* SVG import asks how to turn the artwork into volume before the file is
     * read, so the options outlive the browser. */
    SvgImportOptions svg_options;
    bool svg_options_open;
    std::string svg_pending_path;

    /*
     * Tessellation for the next round primitive placed. Right clicking a
     * cylinder, sphere or cone on the shelf opens the menu; the choice sticks,
     * so a run of parts can be placed at one resolution.
     */
    PrimitiveResolution resolution;
    int resolution_menu_kind;  // PrimitiveKind whose menu is open, or -1
    Polarity resolution_menu_polarity;

    /* Gear generator. The dialog edits these in place and only builds on
     * Create, so a half typed number never reaches the scene. They persist, so
     * a second gear of the same module starts from the first one's numbers. */
    GearParams gear_params;
    bool gear_open;

    /* Text generator, same shape of thing: parameters live here, geometry is
     * built on Create. */
    TextParams text_params;
    bool text_open;

    /* N-sided solid generator. */
    PolyhedronParams poly_params;
    bool poly_open;

    /*
     * Clipboard. A standalone fragment rather than a list of ids: ids are
     * indices into the scene's node vector, so a copy would go stale the moment
     * anything was deleted, and pasting into a *different* project has to work
     * too.
     */
    std::vector<SceneNode> clipboard;

    /*
     * Bevel tool. The edge list belongs to one object and is held in that
     * object's own space, so moving or turning the part does not invalidate it
     * - only editing its geometry does, which is what bevel_node guards.
     */
    int bevel_node;                  // node the edges were collected from
    std::vector<BevelEdge> bevel_edges;
    std::vector<int> bevel_selected;
    int bevel_hover;                 // edge under the cursor, or -1
    bool bevel_menu_open;
    float bevel_radius;
    int bevel_segments;

    /* Set when the next frame should grab the 3D view for the project preview,
     * which can only be read back before the buffer swap. */
    bool want_thumbnail;
};

#define FILE_PROMPT_NONE 0
#define FILE_PROMPT_OPEN 1
#define FILE_PROMPT_SAVE 2
#define FILE_PROMPT_EXPORT 3
#define FILE_PROMPT_IMPORT_STL 4
#define FILE_PROMPT_IMPORT_SVG 5
/*
 * Save As is its own action, not Save with a flag. In the browser a plain Save
 * writes back through the file handle the last save left behind, and Save As
 * has to ask for a new one - so the two cannot share a code path that decides
 * for itself whether to prompt.
 */
#define FILE_PROMPT_SAVE_AS 6

void ui_init(UiState *ui);
void ui_draw(App *app);

/* Panels, one per file. */
void ui_draw_menubar(App *app);
void ui_draw_toolbar(App *app);
void ui_draw_tree(App *app);
void ui_draw_bookshelf(App *app);
void ui_draw_viewport(App *app);

/* Pane widths after folding, which is what every pane must lay itself out
 * against - reading left_width directly would leave a gap where a hidden
 * pane used to be. */
float ui_left_width(const App *app);
float ui_right_width(const App *app);
/* The key list and usage notes, in ui_help.cpp. */
void ui_draw_help(App *app);

/* Shared actions, so the menu bar, the toolbar and the shortcuts all go
 * through one implementation. */
void ui_action_add_primitive(App *app, PrimitiveKind kind, Polarity polarity);

/*
 * Sits a freshly created object on the active editing plane at a world point:
 * turned so the plane's normal is the object's up, which is what makes a
 * primitive land flat on a sloped face rather than upright through it.
 *
 * The rotation is left alone while the workplate is active, where an object
 * added into a turned group has always inherited that group's rotation.
 */
void ui_place_on_workplane(App *app, int id, Vec3 world_point);
/* Shape generators: parameters first, geometry on commit. */
void ui_action_add_gear(App *app);
void ui_draw_gear_options(App *app);
void ui_action_add_text(App *app);
void ui_draw_text_options(App *app);
void ui_action_add_polyhedron(App *app);
void ui_draw_polyhedron_options(App *app);

/* Bevel tool, in ui_bevel.cpp. */
void ui_bevel_refresh(App *app);
void ui_bevel_pick(App *app, float region_w, float region_h, float sx, float sy, bool add);
void ui_bevel_update_hover(App *app, float region_w, float region_h, float sx, float sy);
void ui_draw_bevel_options(App *app);
void ui_action_apply_bevel(App *app);
void ui_action_delete_selection(App *app);
void ui_action_copy(App *app);
void ui_action_paste(App *app);
void ui_action_duplicate(App *app);
void ui_action_repair_selection(App *app);
bool ui_can_repair(const App *app);
bool ui_can_paste(const App *app);
void ui_action_group_selection(App *app);
void ui_action_frame_selection(App *app);
void ui_action_merge_selection(App *app);
void ui_action_align(App *app);
void ui_action_mirror(App *app);
void ui_action_undo(App *app);
void ui_action_redo(App *app);
bool ui_can_align(const App *app);
void ui_action_unmerge(App *app);
void ui_action_remerge(App *app);

void ui_set_status(App *app, const char *text, bool is_error);
void ui_set_mode(App *app, TransformMode mode);
const char *ui_mode_name(TransformMode mode);

/* Snap grid drives moves, nudges, scaling and the fine grid spacing. */
void ui_set_snap_grid(App *app, float mm);
void ui_step_snap_grid(App *app, int direction);

/* Collects the editing furniture the renderer should draw this frame. */
void ui_build_overlay(const App *app, RenderOverlay *out);

/*
 * The plane every editing operation is measured in. Derived rather than
 * stored, so an object-relative plane follows its object without anything
 * having to notice that the object moved.
 */
Workplane ui_active_workplane(const App *app);
void ui_set_workplane_mode(App *app, int mode);
void ui_toggle_workplane(App *app);
void ui_begin_plane_pick(App *app, bool center_on_face);

/* Project and export actions. Save with no known path prompts for one. */
void ui_action_new(App *app);
void ui_action_open(App *app, const char *path);
void ui_action_save(App *app, const char *path);
void ui_action_save_current(App *app);
void ui_action_export_stl(App *app, const char *path);
void ui_action_import_stl(App *app, const char *path);
void ui_action_import_svg(App *app, const char *path);
void ui_draw_svg_options(App *app);
void ui_prompt_path(App *app, int prompt);
void ui_draw_file_prompt(App *app);
/* Turns a chosen path into one of the FILE_PROMPT_* actions. */
void ui_dispatch_path(App *app, int action, const char *path);
bool ui_can_export(const App *app);

/* File browser, in ui_filebrowser.cpp. */
void ui_file_browser_init(FileBrowser *fb);
void ui_file_browser_request(FileBrowser *fb, const char *title, const char *filter,
                             bool save_mode, const char *suggested_name);
bool ui_file_browser_draw(FileBrowser *fb, std::string *out_path);

/* Which of the merge actions apply to the current selection. */
bool ui_can_merge(const App *app);
bool ui_can_unmerge(const App *app);
bool ui_can_remerge(const App *app);

/* Bookshelf thumbnails are drawn, not textured, so the same routine can mark
 * a primitive in the bookshelf and in a drag preview. Takes a screen position
 * so it never touches the ImGui cursor. */
void ui_draw_primitive_glyph(PrimitiveKind kind, Polarity polarity, float size,
                             float screen_x, float screen_y);

/* Generators are not PrimitiveKinds, so they bring their own glyph. */
void ui_draw_gear_glyph(float size, float screen_x, float screen_y, bool negative);
void ui_draw_text_glyph(float size, float screen_x, float screen_y, bool negative);
void ui_draw_polyhedron_glyph(float size, float screen_x, float screen_y, bool negative);

/*
 * Draws closed 2D contours filled and outlined, scaled to fit a square of the
 * given size. Generators preview the very contours they extrude, so what the
 * dialog shows and what Create builds cannot drift apart.
 */
void ui_draw_contour_preview(const std::vector<std::vector<Vec2> > &contours, float size);

/*
 * The same idea for a solid: draws the mesh a generator is about to create,
 * shaded, from a fixed three-quarter view. Painter sorted rather than depth
 * tested, since ImGui has no depth buffer to lean on.
 */
void ui_draw_mesh_preview(const Mesh &mesh, float size);

#endif
