#include "ui.h"

#include <string.h>

#include "app.h"
#include "csg.h"
#include "export_stl.h"
#include "imgui.h"
#include "import_stl.h"
#include "undo.h"
#include "import_svg.h"
#include "platform.h"
#include "project.h"

/* Layout */

void ui_init(UiState *ui) {
    ui->left_width = 340.0f;
    ui->right_width = 260.0f;
    ui->toolbar_height = 44.0f;
    ui->show_tree = true;
    ui->show_shelf = true;
    ui->viewport.x = 0;
    ui->viewport.y = 0;
    ui->viewport.w = 0;
    ui->viewport.h = 0;
    ui->viewport_hovered = false;
    ui->rename_node = OBC_NO_NODE;
    ui->rename_buf[0] = 0;
    ui->rename_focus = false;
    ui->snap_grid_mm = 1.0f;
    ui->show_about = false;
    ui->show_help = false;
    ui->status.clear();
    ui->status_is_error = false;

    ui->mode = XFORM_MOVE;
    ui->hover_handle = -1;
    ui->hover_axis = -1;
    ui->hover_z_arrow = -1;
    ui->pivot_custom = false;
    ui->pivot_point = vec3(0.0f, 0.0f, 0.0f);
    ui->workplane_mode = WORKPLANE_WORLD;
    ui->face_plane = workplane_identity();
    ui->plane_pick_active = false;
    ui->plane_pick_center = true;
    memset(&ui->drag, 0, sizeof(ui->drag));
    ui->drag.handle = -1;

    ui->rect_select.active = false;
    ui->rect_select.swept = false;
    ui->rect_select.x0 = ui->rect_select.y0 = 0.0f;
    ui->rect_select.x1 = ui->rect_select.y1 = 0.0f;
    ui->rect_select.base.clear();

    ui->edit_ref.valid = false;
    ui->edit_ref.start_bounds = bounds_empty();
    ui->edit_ref.offset = vec3(0.0f, 0.0f, 0.0f);
    ui->edit_ref.rotation = vec3(0.0f, 0.0f, 0.0f);
    ui->edit_ref.nodes.clear();
    ui->counter_edit_axis = -1;
    ui->counter_edit_value = 0.0f;
    ui->align_active = false;
    ui->mirror_active = false;
    ui->align_hover_axis = -1;
    ui->align_hover_slot = -1;
    ui->mirror_hover_axis = -1;

    ui_file_browser_init(&ui->browser);
    ui->file_action = FILE_PROMPT_NONE;
    ui->project_path.clear();
    svg_import_options_init(&ui->svg_options);
    ui->svg_options_open = false;
    ui->svg_pending_path.clear();
    gear_params_init(&ui->gear_params);
    ui->gear_open = false;
    text_params_init(&ui->text_params);
    ui->text_open = false;
    polyhedron_params_init(&ui->poly_params);
    ui->poly_open = false;
    ui->bevel_node = OBC_NO_NODE;
    ui->bevel_edges.clear();
    ui->bevel_selected.clear();
    ui->bevel_hover = -1;
    ui->bevel_menu_open = false;
    ui->bevel_radius = 1.0f;
    ui->bevel_segments = 8;
    primitive_resolution_init(&ui->resolution);
    ui->resolution_menu_kind = -1;
    ui->resolution_menu_polarity = POLARITY_POSITIVE;
    ui->want_thumbnail = false;
}

float ui_left_width(const App *app) {
    return app->ui.show_tree ? app->ui.left_width : 0.0f;
}

float ui_right_width(const App *app) {
    return app->ui.show_shelf ? app->ui.right_width : 0.0f;
}

/* Snap grid */

#define SNAP_MIN_MM 0.001f
#define SNAP_MAX_MM 100.0f

void ui_set_snap_grid(App *app, float mm) {
    if (!(mm > 0.0f)) return; // also rejects NaN from the custom field
    if (mm < SNAP_MIN_MM) mm = SNAP_MIN_MM;
    if (mm > SNAP_MAX_MM) mm = SNAP_MAX_MM;
    app->ui.snap_grid_mm = mm;

    char msg[64];
    snprintf(msg, sizeof(msg), "Snap grid %g mm.", mm);
    ui_set_status(app, msg, false);
}

/* Multiplies rather than adds, so one keypress is a useful jump at every
 * magnitude from 0.01 mm to 100 mm. */
void ui_step_snap_grid(App *app, int direction) {
    float mm = app->ui.snap_grid_mm;
    ui_set_snap_grid(app, direction > 0 ? mm * 2.0f : mm * 0.5f);
}

/* Editing plane */

/*
 * The world and object planes are recomputed here every time rather than kept
 * in UiState: an object-relative plane has to follow its object, and a stored
 * copy would go stale the moment the object was rotated or deselected. Only
 * the face plane is stored, because the face that defined it is allowed to
 * move away afterwards.
 */
Workplane ui_active_workplane(const App *app) {
    const UiState &ui = app->ui;

    if (ui.workplane_mode == WORKPLANE_FACE) return ui.face_plane;

    if (ui.workplane_mode == WORKPLANE_OBJECT && !app->scene.selection.empty()) {
        Workplane w;
        /* The first selected node defines it, the same node align treats as
         * the reference, so "first selected" means one thing throughout. */
        w.basis = scene_world_rotation(&app->scene, app->scene.selection[0]);
        Bounds b = scene_selection_bounds(&app->scene);
        w.origin = b.valid ? bounds_center(b) : vec3(0.0f, 0.0f, 0.0f);
        return w;
    }

    return workplane_identity();
}

void ui_set_workplane_mode(App *app, int mode) {
    app->ui.workplane_mode = mode;
    app->ui.plane_pick_active = false;

    /* The reference describes an offset measured in the old plane, so it
     * cannot survive a change of frame. */
    app->ui.edit_ref.valid = false;
    app->ui.edit_ref.nodes.clear();

    char msg[96];
    snprintf(msg, sizeof(msg), "%s plane.", workplane_mode_name(mode));
    ui_set_status(app, msg, false);
}

/* P, per PLAN.md: toggles workplate-relative and object-relative. A face plane
 * is neither, so it drops back to the plate first. */
void ui_toggle_workplane(App *app) {
    if (app->ui.workplane_mode == WORKPLANE_FACE) {
        ui_set_workplane_mode(app, WORKPLANE_WORLD);
        return;
    }
    if (app->ui.workplane_mode == WORKPLANE_OBJECT) {
        ui_set_workplane_mode(app, WORKPLANE_WORLD);
        return;
    }
    if (app->scene.selection.empty()) {
        ui_set_status(app, "Select an object to use its own plane.", true);
        return;
    }
    ui_set_workplane_mode(app, WORKPLANE_OBJECT);
}

void ui_begin_plane_pick(App *app, bool center_on_face) {
    app->ui.plane_pick_active = true;
    app->ui.plane_pick_center = center_on_face;
    ui_set_status(app, center_on_face
        ? "Plane setting: click a face to centre the workplane on it. Esc to cancel."
        : "Plane setting: click a face to put the workplane at that point. Esc to cancel.",
        false);
}

/* Transform modes */

const char *ui_mode_name(TransformMode mode) {
    switch (mode) {
    case XFORM_SCALE:  return "Scale";
    case XFORM_ROTATE: return "Rotate";
    case XFORM_BEVEL:  return "Bevel";
    default:           return "Move";
    }
}

void ui_set_mode(App *app, TransformMode mode) {
    /* Pressing the active mode's key again returns to move, so S, D and B
     * toggle rather than trap the user in a mode. */
    if (app->ui.mode == mode) mode = XFORM_MOVE;
    app->ui.mode = mode;

    if (mode == XFORM_BEVEL) {
        /* Collected on entry, so the edges are up the moment the mode is. */
        app->ui.bevel_node = OBC_NO_NODE;
        ui_bevel_refresh(app);
        if (app->ui.bevel_node == OBC_NO_NODE) {
            ui_set_status(app, "Bevel needs one object selected.", true);
        }
        return;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "%s mode.", ui_mode_name(app->ui.mode));
    ui_set_status(app, msg, false);
}

void ui_draw(App *app) {
    ui_draw_menubar(app);
    ui_draw_toolbar(app);
    ui_draw_tree(app);
    ui_draw_bookshelf(app);
    ui_draw_viewport(app);
    ui_draw_file_prompt(app);
    ui_draw_svg_options(app);
    ui_draw_gear_options(app);
    ui_draw_text_options(app);
    ui_draw_polyhedron_options(app);
    ui_draw_bevel_options(app);
    /* After the panes, so it floats over them rather than under. */
    ui_draw_help(app);

    if (app->ui.show_about) {
        ImGui::OpenPopup("About OpenBoolCAD");
        app->ui.show_about = false;
    }
    if (ImGui::BeginPopupModal("About OpenBoolCAD", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("OpenBoolCAD");
        ImGui::Separator();
        ImGui::TextUnformatted("Boolean volume CAD. SDL2 + OpenGL + ImGui.");
        ImGui::Spacing();
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

/* Shared actions */

void ui_place_on_workplane(App *app, int id, Vec3 world_point) {
    SceneNode *n = scene_node(&app->scene, id);
    if (!n) return;

    /* position is in the parent's frame, so a world point has to come back
     * down the parent chain. */
    n->position = scene_point_from_world(&app->scene, n->parent, world_point);

    if (app->ui.workplane_mode == WORKPLANE_WORLD) return;

    /* Standing on the plane means the node's world rotation is the plane's,
     * and world = parent * local, so the parent's share is divided out. */
    Workplane plane = ui_active_workplane(app);
    Mat3 parent_rotation = scene_world_rotation(&app->scene, n->parent);
    n->rotation = mat3_to_euler_zyx(mat3_mul(mat3_transposed(parent_rotation), plane.basis));
}

void ui_action_add_primitive(App *app, PrimitiveKind kind, Polarity polarity) {
    undo_record(&app->undo, app->scene, "Add primitive");
    int parent = scene_insert_target(&app->scene);
    char name[64];
    snprintf(name, sizeof(name), "%s%s", polarity == POLARITY_NEGATIVE ? "Negative " : "",
             primitive_name(kind));

    /* Read before the add: an object plane is defined by the selection, and
     * the new object is about to become it. */
    Workplane plane = ui_active_workplane(app);
    bool on_plane = (app->ui.workplane_mode != WORKPLANE_WORLD);

    int id = scene_add_object(&app->scene, parent, name,
                              mesh_make_primitive_at(kind, app->ui.resolution), polarity);
    /* Centred on the plane rather than at the parent's origin. The workplate
     * keeps landing new objects where it always did. */
    if (on_plane) ui_place_on_workplane(app, id, plane.origin);

    scene_select_only(&app->scene, id);
}

/*
 * Gear generator. The mesh is built before anything is recorded or inserted,
 * so a set of parameters that cannot produce a solid leaves the scene and the
 * undo stack untouched and only reports why.
 */
void ui_action_add_gear(App *app) {
    /* Clamped here rather than while typing, so the fields keep whatever was
     * entered and the value actually built is the one left on screen. */
    gear_params_clamp(&app->ui.gear_params);
    const GearParams &g = app->ui.gear_params;

    Mesh mesh;
    std::string error;
    if (!gear_build(g, &mesh, &error)) {
        ui_set_status(app, error.c_str(), true);
        return;
    }

    undo_record(&app->undo, app->scene, "Add gear");
    int parent = scene_insert_target(&app->scene);

    char name[64];
    snprintf(name, sizeof(name), "%sGear %dT", g.negative ? "Negative " : "", g.teeth);
    Polarity polarity = g.negative ? POLARITY_NEGATIVE : POLARITY_POSITIVE;

    Workplane plane = ui_active_workplane(app);
    bool on_plane = (app->ui.workplane_mode != WORKPLANE_WORLD);

    int id = scene_add_object(&app->scene, parent, name, mesh, polarity);
    if (on_plane) ui_place_on_workplane(app, id, plane.origin);

    scene_select_only(&app->scene, id);

    char msg[160];
    snprintf(msg, sizeof(msg),
             "Gear: %d teeth, module %.2f, pitch %.2f mm, outside %.2f mm.",
             g.teeth, g.module_mm, gear_pitch_diameter(g), gear_outside_diameter(g));
    ui_set_status(app, msg, false);
}

void ui_draw_gear_options(App *app) {
    if (app->ui.gear_open) {
        ImGui::OpenPopup("Gear Generator");
        app->ui.gear_open = false;
    }
    if (!ImGui::BeginPopupModal("Gear Generator", NULL, ImGuiWindowFlags_AlwaysAutoResize)) return;

    GearParams *g = &app->ui.gear_params;

    ImGui::TextUnformatted("Involute spur gear.");
    ImGui::TextDisabled("Two gears mesh when their module and pressure angle match.");
    ImGui::Spacing();

    /* Clamped when a field is left rather than on every keystroke: clamping
     * live fights the typing, since a half entered "16" is momentarily 1. */
    const float field = 150.0f;
    ImGui::SetNextItemWidth(field);
    ImGui::InputInt("Teeth", &g->teeth);
    if (ImGui::IsItemDeactivatedAfterEdit()) gear_params_clamp(g);

    ImGui::SetNextItemWidth(field);
    ImGui::InputFloat("Module (mm)", &g->module_mm, 0.1f, 0.5f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) gear_params_clamp(g);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tooth size. Pitch diameter is module x teeth.");

    ImGui::SetNextItemWidth(field);
    ImGui::InputFloat("Pressure angle (deg)", &g->pressure_angle_deg, 0.5f, 5.0f, "%.1f");
    if (ImGui::IsItemDeactivatedAfterEdit()) gear_params_clamp(g);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("20 is the modern standard; 14.5 is the older one.");

    ImGui::SetNextItemWidth(field);
    ImGui::InputFloat("Thickness (mm)", &g->thickness_mm, 0.5f, 5.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) gear_params_clamp(g);

    ImGui::SetNextItemWidth(field);
    ImGui::InputFloat("Bore (mm)", &g->bore_mm, 0.5f, 2.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) gear_params_clamp(g);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 for no bore. Capped so a rim is left around it.");

    ImGui::Checkbox("Cut as a hole", &g->negative);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Adds the gear as a negative volume.");

    /* Shown from a clamped copy, so the readout is always the gear that Create
     * would actually build even while a field holds something out of range. */
    GearParams shown = *g;
    gear_params_clamp(&shown);

    ImGui::Separator();

    /* The very contours Create would extrude, so the picture cannot flatter
     * the part. Recomputed each frame; a gear profile is a few hundred points
     * and costs nothing next to drawing the dialog. */
    std::vector<std::vector<Vec2> > preview;
    std::string preview_error;
    if (gear_contours(shown, &preview, &preview_error)) {
        ui_draw_contour_preview(preview, 180.0f);
    } else {
        ImGui::TextDisabled("%s", preview_error.c_str());
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("Pitch    %.2f mm", gear_pitch_diameter(shown));
    ImGui::Text("Outside  %.2f mm", gear_outside_diameter(shown));
    ImGui::Text("Root     %.2f mm", gear_root_diameter(shown));
    ImGui::EndGroup();
    if (shown.teeth != g->teeth || shown.module_mm != g->module_mm ||
        shown.pressure_angle_deg != g->pressure_angle_deg ||
        shown.thickness_mm != g->thickness_mm || shown.bore_mm != g->bore_mm) {
        ImGui::TextDisabled("Out of range values will be clamped to these.");
    }

    ImGui::Separator();
    if (ImGui::Button("Create")) {
        ui_action_add_gear(app);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

/*
 * Text generator. Same contract as the gear: build first, so parameters that
 * cannot produce a solid leave the scene and the undo stack untouched.
 */
void ui_action_add_text(App *app) {
    text_params_clamp(&app->ui.text_params);
    const TextParams &t = app->ui.text_params;

    Mesh mesh;
    std::string error;
    if (!text_build(t, &mesh, &error)) {
        ui_set_status(app, error.c_str(), true);
        return;
    }

    undo_record(&app->undo, app->scene, "Add text");
    int parent = scene_insert_target(&app->scene);

    /* Sized for the longest text plus the prefix, so the node name is never
     * silently cut short. */
    char name[OBC_TEXT_MAX + 16];
    snprintf(name, sizeof(name), "%s%s", t.negative ? "Negative " : "", t.text);
    Polarity polarity = t.negative ? POLARITY_NEGATIVE : POLARITY_POSITIVE;

    Workplane plane = ui_active_workplane(app);
    bool on_plane = (app->ui.workplane_mode != WORKPLANE_WORLD);

    int id = scene_add_object(&app->scene, parent, name, mesh, polarity);
    if (on_plane) ui_place_on_workplane(app, id, plane.origin);
    scene_select_only(&app->scene, id);

    char msg[OBC_TEXT_MAX + 128];
    snprintf(msg, sizeof(msg), "Text \"%s\" in %s%s%s, %g mm tall.", t.text,
             text_font_name(t.family), t.bold ? " Bold" : "", t.italic ? " Italic" : "",
             t.height_mm);
    ui_set_status(app, msg, false);
}

void ui_draw_text_options(App *app) {
    if (app->ui.text_open) {
        ImGui::OpenPopup("Text Generator");
        app->ui.text_open = false;
    }
    if (!ImGui::BeginPopupModal("Text Generator", NULL, ImGuiWindowFlags_AlwaysAutoResize)) return;

    TextParams *t = &app->ui.text_params;

    ImGui::TextUnformatted("Extruded from the font's own outlines.");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(320.0f);
    ImGui::InputText("Text", t->text, sizeof(t->text));

    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::BeginCombo("Font", text_font_name(t->family))) {
        for (int i = 0; i < TEXT_FONT_COUNT; ++i) {
            if (ImGui::Selectable(text_font_name(i), t->family == i)) t->family = i;
        }
        ImGui::EndCombo();
    }

    /* Only the three text faces ship a bold; the display faces would have to
     * have one faked, and a fake bold looks like a mistake. */
    bool has_bold = text_font_has_bold(t->family);
    if (!has_bold) ImGui::BeginDisabled();
    ImGui::Checkbox("Bold", &t->bold);
    if (!has_bold) ImGui::EndDisabled();
    if (!has_bold && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("This font has no bold cut.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Italic", &t->italic);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Slanted, since none of these ship an italic cut.");
    ImGui::SameLine();
    ImGui::Checkbox("Cut as a hole", &t->negative);

    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputFloat("Height (mm)", &t->height_mm, 1.0f, 5.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) text_params_clamp(t);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Height of a capital, not of the em box.");

    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputFloat("Depth (mm)", &t->depth_mm, 0.5f, 2.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) text_params_clamp(t);

    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputFloat("Tracking", &t->spacing, 0.02f, 0.1f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) text_params_clamp(t);

    TextParams shown = *t;
    text_params_clamp(&shown);

    ImGui::Separator();
    std::vector<std::vector<Vec2> > preview;
    std::string preview_error;
    if (text_contours(shown, &preview, &preview_error)) {
        ui_draw_contour_preview(preview, 320.0f);
    } else {
        ImGui::TextDisabled("%s", preview_error.c_str());
    }

    ImGui::Separator();
    if (ImGui::Button("Create")) {
        ui_action_add_text(app);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

/*
 * N-sided solid generator. Same contract as the other two: the mesh is built
 * before anything is recorded, so parameters that cannot make a solid leave the
 * scene and the undo stack alone.
 */
void ui_action_add_polyhedron(App *app) {
    polyhedron_params_clamp(&app->ui.poly_params);
    const PolyhedronParams &p = app->ui.poly_params;

    Mesh mesh;
    std::string error;
    if (!polyhedron_build(p, &mesh, &error)) {
        ui_set_status(app, error.c_str(), true);
        return;
    }

    undo_record(&app->undo, app->scene, "Add solid");
    int parent = scene_insert_target(&app->scene);

    char name[96];
    if (polyhedron_kind_takes_sides(p.kind)) {
        snprintf(name, sizeof(name), "%s%d-sided %s", p.negative ? "Negative " : "",
                 p.sides, polyhedron_kind_name(p.kind));
    } else {
        snprintf(name, sizeof(name), "%s%s", p.negative ? "Negative " : "",
                 polyhedron_kind_name(p.kind));
    }
    Polarity polarity = p.negative ? POLARITY_NEGATIVE : POLARITY_POSITIVE;

    Workplane plane = ui_active_workplane(app);
    bool on_plane = (app->ui.workplane_mode != WORKPLANE_WORLD);

    int id = scene_add_object(&app->scene, parent, name, mesh, polarity);
    if (on_plane) ui_place_on_workplane(app, id, plane.origin);
    scene_select_only(&app->scene, id);

    char msg[160];
    snprintf(msg, sizeof(msg), "%s: %d faces, %.1f mm across.",
             polyhedron_kind_name(p.kind), polyhedron_face_count(p), p.size_mm);
    ui_set_status(app, msg, false);
}

void ui_draw_polyhedron_options(App *app) {
    if (app->ui.poly_open) {
        ImGui::OpenPopup("N-sided Solid");
        app->ui.poly_open = false;
    }
    if (!ImGui::BeginPopupModal("N-sided Solid", NULL, ImGuiWindowFlags_AlwaysAutoResize)) return;

    PolyhedronParams *p = &app->ui.poly_params;

    ImGui::TextUnformatted("Regular solids and the families built on an n-gon.");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::BeginCombo("Shape", polyhedron_kind_name(p->kind))) {
        /* The five regular solids first, then the parametric families: a
         * regular polyhedron only exists at 4, 6, 8, 12 and 20 faces, so those
         * are presets rather than a number to type. */
        for (int i = 0; i < POLY_COUNT; ++i) {
            if (i == POLY_PRISM) ImGui::Separator();
            if (ImGui::Selectable(polyhedron_kind_name(i), p->kind == i)) p->kind = i;
        }
        ImGui::EndCombo();
    }

    bool takes_sides = polyhedron_kind_takes_sides(p->kind);
    if (!takes_sides) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputInt("Sides", &p->sides);
    if (ImGui::IsItemDeactivatedAfterEdit()) polyhedron_params_clamp(p);
    if (!takes_sides) ImGui::EndDisabled();
    if (!takes_sides && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A regular solid's face count is fixed by its geometry.");
    }

    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputFloat("Size (mm)", &p->size_mm, 1.0f, 5.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) polyhedron_params_clamp(p);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Across the widest point.");

    bool takes_height = polyhedron_kind_takes_height(p->kind);
    if (!takes_height) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputFloat("Height (mm)", &p->height_mm, 1.0f, 5.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) polyhedron_params_clamp(p);
    if (!takes_height) ImGui::EndDisabled();

    ImGui::Checkbox("Cut as a hole", &p->negative);

    PolyhedronParams shown = *p;
    polyhedron_params_clamp(&shown);

    ImGui::Separator();
    Mesh preview;
    std::string preview_error;
    if (polyhedron_build(shown, &preview, &preview_error)) {
        ui_draw_mesh_preview(preview, 220.0f);
    } else {
        ImGui::TextDisabled("%s", preview_error.c_str());
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("%d faces", polyhedron_face_count(shown));
    ImGui::Text("%.1f mm across", shown.size_mm);
    if (polyhedron_kind_takes_height(shown.kind)) {
        ImGui::Text("%.1f mm tall", shown.height_mm);
    }
    ImGui::EndGroup();

    ImGui::Separator();
    if (ImGui::Button("Create")) {
        ui_action_add_polyhedron(app);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

/* Copy, paste and duplicate */

bool ui_can_paste(const App *app) {
    return !app->ui.clipboard.empty();
}

void ui_action_copy(App *app) {
    if (app->scene.selection.empty()) return;
    scene_copy_subtrees(&app->scene, app->scene.selection, &app->ui.clipboard);

    char msg[96];
    snprintf(msg, sizeof(msg), "Copied %d object%s.",
             (int)app->scene.selection.size(),
             app->scene.selection.size() == 1 ? "" : "s");
    ui_set_status(app, msg, false);
}

/*
 * Pasted where a new primitive would go, and nudged one grid step along the
 * plane so the copy is not hidden exactly inside the original.
 */
static void paste_fragment(App *app, const std::vector<SceneNode> &fragment,
                           const char *label) {
    if (fragment.empty()) return;

    undo_record(&app->undo, app->scene, label);
    int parent = scene_insert_target(&app->scene);

    std::vector<int> pasted;
    scene_paste_subtrees(&app->scene, parent, fragment, &pasted);
    if (pasted.empty()) return;

    Workplane plane = ui_active_workplane(app);
    Vec3 step = workplane_to_world(plane, vec3(app->ui.snap_grid_mm, app->ui.snap_grid_mm, 0.0f));
    for (size_t i = 0; i < pasted.size(); ++i) scene_translate_node(&app->scene, pasted[i], step);

    scene_select_set(&app->scene, pasted);

    char msg[96];
    snprintf(msg, sizeof(msg), "%s %d object%s.", label, (int)pasted.size(),
             pasted.size() == 1 ? "" : "s");
    ui_set_status(app, msg, false);
}

void ui_action_paste(App *app) {
    if (app->ui.clipboard.empty()) {
        ui_set_status(app, "Nothing to paste.", true);
        return;
    }
    paste_fragment(app, app->ui.clipboard, "Pasted");
}

/* Duplicate is a copy and a paste that does not disturb the clipboard, so it
 * can be used repeatedly without losing whatever was copied earlier. */
void ui_action_duplicate(App *app) {
    if (app->scene.selection.empty()) {
        ui_set_status(app, "Select something to duplicate.", true);
        return;
    }
    std::vector<SceneNode> fragment;
    scene_copy_subtrees(&app->scene, app->scene.selection, &fragment);
    paste_fragment(app, fragment, "Duplicated");
}

/*
 * Mesh repair, on demand.
 *
 * Every boolean already repairs its inputs, so this is for the case where the
 * user wants it *now*: an STL that imported with a warning, or a part that a
 * merge has just refused. Reporting what changed is the point - a silent fix is
 * how a subtly wrong model gets shipped - so the summary goes to the status
 * line whether anything needed doing or not.
 */
bool ui_can_repair(const App *app) {
    for (size_t i = 0; i < app->scene.selection.size(); ++i) {
        const SceneNode *n = scene_node(&app->scene, app->scene.selection[i]);
        if (n && n->kind == NODE_OBJECT && !n->mesh.vertices.empty()) return true;
    }
    return false;
}

void ui_action_repair_selection(App *app) {
    if (!ui_can_repair(app)) {
        ui_set_status(app, "Select an object to repair.", true);
        return;
    }

    /* Repaired into scratch first: a mesh that cannot be closed must not
     * replace the one already on screen. */
    std::vector<int> targets;
    std::vector<Mesh> repaired;
    std::vector<std::string> notes;
    int failed = 0;

    for (size_t i = 0; i < app->scene.selection.size(); ++i) {
        int id = app->scene.selection[i];
        const SceneNode *n = scene_node(&app->scene, id);
        if (!n || n->kind != NODE_OBJECT || n->mesh.vertices.empty()) continue;

        Mesh copy = n->mesh;
        std::string note;
        bool solid = csg_make_solid(&copy, &note);
        if (!solid) failed += 1;

        targets.push_back(id);
        repaired.push_back(copy);
        notes.push_back(note);
    }
    if (targets.empty()) return;

    undo_record(&app->undo, app->scene, "Repair");
    for (size_t i = 0; i < targets.size(); ++i) {
        SceneNode *n = scene_node(&app->scene, targets[i]);
        if (n) n->mesh = repaired[i];
    }

    /* One object reports what changed to it; several report the tally, since
     * a list of summaries would not fit the status line. */
    char msg[400];
    if (targets.size() == 1) {
        snprintf(msg, sizeof(msg), "Repaired %s%s.",
                 notes[0].empty() ? "(nothing to fix)" : notes[0].c_str(),
                 failed ? " - still not a closed solid" : "");
    } else {
        snprintf(msg, sizeof(msg), "Repaired %d objects%s.", (int)targets.size(),
                 failed ? (failed == (int)targets.size()
                           ? " - none came out closed"
                           : " - some are still not closed") : "");
    }
    ui_set_status(app, msg, failed > 0);
}

void ui_action_delete_selection(App *app) {
    if (app->scene.selection.empty()) return;
    undo_record(&app->undo, app->scene, "Delete");
    std::vector<int> sel = app->scene.selection;
    for (size_t i = 0; i < sel.size(); ++i) scene_delete_node(&app->scene, sel[i]);
    scene_select_clear(&app->scene);
}

void ui_action_group_selection(App *app) {
    if (app->scene.selection.size() < 2) return;
    undo_record(&app->undo, app->scene, "Group");

    /* The new group lands next to the first selected node. */
    const SceneNode *first = scene_node(&app->scene, app->scene.selection[0]);
    int parent = first ? first->parent : OBC_NO_NODE;
    int group = scene_add_group(&app->scene, parent, "Group");

    std::vector<int> sel = app->scene.selection;
    for (size_t i = 0; i < sel.size(); ++i) scene_reparent(&app->scene, sel[i], group);
    scene_select_only(&app->scene, group);
}

void ui_action_frame_selection(App *app) {
    Bounds b = app->scene.selection.empty()
        ? bounds_empty()
        : scene_selection_bounds(&app->scene);
    camera_frame_bounds(&app->camera, b);
}

void ui_set_status(App *app, const char *text, bool is_error) {
    app->ui.status = text ? text : "";
    app->ui.status_is_error = is_error;
}

/* Merge actions. The single selected node is what unmerge and remerge act on,
 * matching "when one is selected, Ctrl+Z / Ctrl+Y un- and remerge it". */
static int ui_single_selection(const App *app) {
    if (app->scene.selection.size() != 1) return OBC_NO_NODE;
    return app->scene.selection[0];
}

bool ui_can_merge(const App *app) {
    return app->scene.selection.size() >= 2;
}

bool ui_can_unmerge(const App *app) {
    int id = ui_single_selection(app);
    return id != OBC_NO_NODE && scene_node_is_merged(&app->scene, id);
}

bool ui_can_remerge(const App *app) {
    int id = ui_single_selection(app);
    if (id == OBC_NO_NODE) return false;
    const SceneNode *n = scene_node(&app->scene, id);
    return n && !n->merged && !n->children.empty();
}

void ui_action_merge_selection(App *app) {
    undo_record(&app->undo, app->scene, "Merge");
    std::string error, repair;
    size_t count = app->scene.selection.size();
    if (scene_merge_selection(&app->scene, &error, &repair)) {
        std::string msg = "Merged " + std::to_string((int)count) + " objects";
        /* Say so when the repair pass had to change the inputs: a silent fix is
         * how a subtly wrong model gets shipped. */
        msg += repair.empty() ? "." : (" (repaired: " + repair + ").");
        ui_set_status(app, msg.c_str(), false);
    } else {
        ui_set_status(app, error.c_str(), true);
    }
}

void ui_action_unmerge(App *app) {
    undo_record(&app->undo, app->scene, "Unmerge");
    std::string error;
    int id = ui_single_selection(app);
    if (scene_unmerge(&app->scene, id, &error)) ui_set_status(app, "Unmerged.", false);
    else ui_set_status(app, error.c_str(), true);
}

void ui_action_remerge(App *app) {
    undo_record(&app->undo, app->scene, "Remerge");
    std::string error, repair;
    int id = ui_single_selection(app);
    if (scene_remerge(&app->scene, id, &error, &repair)) {
        std::string msg = repair.empty() ? "Remerged." : ("Remerged (repaired: " + repair + ").");
        ui_set_status(app, msg.c_str(), false);
    } else {
        ui_set_status(app, error.c_str(), true);
    }
}

/* Align, mirror and undo */

bool ui_can_align(const App *app) {
    return app->scene.selection.size() >= 2;
}

void ui_action_align(App *app) {
    if (!ui_can_align(app)) {
        ui_set_status(app, "Select two or more objects to align.", true);
        return;
    }
    app->ui.align_active = !app->ui.align_active;
    app->ui.mirror_active = false;
    ui_set_status(app,
        app->ui.align_active
            ? "Align: click a line to align to the first selected object. Esc to finish."
            : "Align finished.",
        false);
}

void ui_action_mirror(App *app) {
    if (app->scene.selection.empty()) {
        ui_set_status(app, "Select something to mirror.", true);
        return;
    }
    app->ui.mirror_active = !app->ui.mirror_active;
    app->ui.align_active = false;
    ui_set_status(app,
        app->ui.mirror_active
            ? "Mirror: click a plane to flip the selection across it. Esc to finish."
            : "Mirror finished.",
        false);
}

/*
 * CTRL+Z is unmerge when a merged object is selected, as PLAN.md specifies, and
 * the global undo otherwise. The merge case is deliberately the more specific
 * one so the documented behaviour still wins where it applies.
 */
void ui_action_undo(App *app) {
    if (ui_can_unmerge(app)) {
        ui_action_unmerge(app);
        return;
    }

    std::string label;
    if (!undo_step_back(&app->undo, &app->scene, &label)) {
        ui_set_status(app, "Nothing to undo.", false);
        return;
    }
    app->ui.edit_ref.valid = false;
    app->ui.edit_ref.nodes.clear();

    std::string msg = "Undid " + label + ".";
    ui_set_status(app, msg.c_str(), false);
}

void ui_action_redo(App *app) {
    if (ui_can_remerge(app)) {
        ui_action_remerge(app);
        return;
    }

    std::string label;
    if (!undo_step_forward(&app->undo, &app->scene, &label)) {
        ui_set_status(app, "Nothing to redo.", false);
        return;
    }
    app->ui.edit_ref.valid = false;
    app->ui.edit_ref.nodes.clear();

    std::string msg = "Redid " + label + ".";
    ui_set_status(app, msg.c_str(), false);
}

/* Project files and export */

void ui_action_new(App *app) {
    scene_new_empty(&app->scene);
    camera_home(&app->camera);
    app->ui.project_path.clear();
    ui_set_status(app, "New project.", false);
}

void ui_action_open(App *app, const char *path) {
    std::string error;
    Scene loaded;
    Camera camera = app->camera;

    /* Loaded into scratch first: a failed load must not leave a half replaced
     * scene behind. */
    if (!project_load(path, &loaded, &camera, &error)) {
        ui_set_status(app, error.c_str(), true);
        return;
    }

    app->scene = loaded;
    app->camera = camera;
    app->ui.project_path = path;
    app->ui.edit_ref.valid = false;
    app->ui.edit_ref.nodes.clear();

    char msg[600];
    snprintf(msg, sizeof(msg), "Opened %s.", path);
    ui_set_status(app, msg, false);
}

void ui_action_save(App *app, const char *path) {
    std::string error;
    /* The thumbnail was captured at the end of the previous frame, so a save is
     * always one frame behind the view - close enough for a preview. */
    if (!project_save(path, app->scene, app->camera, app->thumbnail, &error)) {
        ui_set_status(app, error.c_str(), true);
        return;
    }
    app->ui.project_path = path;

    char msg[600];
    snprintf(msg, sizeof(msg), "Saved %s.", path);
    ui_set_status(app, msg, false);
}

/*
 * Save to the path already known, or ask for one. The browser has no path to
 * write back to - the host holds the file, not us - so a save there always goes
 * out through the host, which reuses the handle from the last save and writes
 * without asking again.
 */
void ui_action_save_current(App *app) {
    if (platform_native_file_dialogs() || app->ui.project_path.empty()) {
        ui_prompt_path(app, FILE_PROMPT_SAVE);
        return;
    }
    ui_action_save(app, app->ui.project_path.c_str());
}

bool ui_can_export(const App *app) {
    return !app->scene.selection.empty();
}

void ui_action_export_stl(App *app, const char *path) {
    std::string error, note;
    if (!export_stl(path, app->scene, app->scene.selection, &error, &note)) {
        ui_set_status(app, error.c_str(), true);
        return;
    }

    std::string msg = std::string("Exported ") + path;
    msg += note.empty() ? "." : (" (" + note + ").");
    ui_set_status(app, msg.c_str(), false);
}

/* Basename of a path, for suggesting a name in the save field. */
static std::string path_leaf(const std::string &path) {
    size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

/* One place where a chosen path turns into an action, shared by the in-app
 * file browser and the host dialogs the browser build uses. */
void ui_dispatch_path(App *app, int action, const char *path) {
    if (!path || !*path) return;

    if (action == FILE_PROMPT_OPEN) ui_action_open(app, path);
    else if (action == FILE_PROMPT_SAVE || action == FILE_PROMPT_SAVE_AS) ui_action_save(app, path);
    else if (action == FILE_PROMPT_EXPORT) ui_action_export_stl(app, path);
    else if (action == FILE_PROMPT_IMPORT_STL) ui_action_import_stl(app, path);
    else if (action == FILE_PROMPT_IMPORT_SVG) {
        /* The file is not read until the mode is chosen: outline and height map
         * produce completely different geometry. */
        app->ui.svg_pending_path = path;
        app->ui.svg_options_open = true;
    }
}

void ui_prompt_path(App *app, int prompt) {
    app->ui.file_action = prompt;

    /* The browser has no filesystem to walk, so the host's own dialogs stand in
     * for the in-app one. Inert on the desktop. */
    if (platform_native_file_dialogs()) {
        platform_prompt_path(app, prompt);
        return;
    }

    if (prompt == FILE_PROMPT_IMPORT_STL) {
        ui_file_browser_request(&app->ui.browser, "Import STL", ".stl", false, "");
    } else if (prompt == FILE_PROMPT_IMPORT_SVG) {
        ui_file_browser_request(&app->ui.browser, "Import SVG", ".svg", false, "");
    } else if (prompt == FILE_PROMPT_OPEN) {
        ui_file_browser_request(&app->ui.browser, "Open Project", ".obc", false, "");
    } else if (prompt == FILE_PROMPT_EXPORT) {
        ui_file_browser_request(&app->ui.browser, "Export Selection as STL", ".stl", true,
                                "export.stl");
    } else {
        std::string suggested = app->ui.project_path.empty()
            ? std::string("project.obc") : path_leaf(app->ui.project_path);
        ui_file_browser_request(&app->ui.browser, "Save Project As", ".obc", true,
                                suggested.c_str());
    }
}

void ui_draw_file_prompt(App *app) {
    /* Host dialogs answer whenever the user is done with them, so there is no
     * popup to draw - only a result to pick up. */
    if (platform_native_file_dialogs()) {
        platform_poll_files(app);
        return;
    }

    if (app->ui.file_action == FILE_PROMPT_NONE) return;

    std::string path;
    if (!ui_file_browser_draw(&app->ui.browser, &path)) {
        /* The popup closes itself on Cancel; drop the pending action with it. */
        if (!ImGui::IsPopupOpen(app->ui.browser.title.c_str())) {
            app->ui.file_action = FILE_PROMPT_NONE;
        }
        return;
    }

    int action = app->ui.file_action;
    app->ui.file_action = FILE_PROMPT_NONE;
    ui_dispatch_path(app, action, path.c_str());
}

/* Import */

void ui_action_import_stl(App *app, const char *path) {
    undo_record(&app->undo, app->scene, "Import STL");
    Mesh mesh;
    std::string error, note;
    if (!import_stl(path, &mesh, &error, &note)) {
        ui_set_status(app, error.c_str(), true);
        return;
    }

    int parent = scene_insert_target(&app->scene);
    std::string name = path;
    size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);

    int id = scene_add_object(&app->scene, parent, name.c_str(), mesh, POLARITY_POSITIVE);
    scene_select_only(&app->scene, id);

    std::string msg = "Imported " + name;
    msg += note.empty() ? "." : (" (" + note + ").");
    ui_set_status(app, msg.c_str(), note.find("not a closed solid") != std::string::npos);
}

void ui_action_import_svg(App *app, const char *path) {
    undo_record(&app->undo, app->scene, "Import SVG");
    SvgImportResult result;
    std::string error, note;
    if (!import_svg(path, app->ui.svg_options, &result, &error, &note)) {
        ui_set_status(app, error.c_str(), true);
        return;
    }

    std::string file = path;
    size_t slash = file.find_last_of('/');
    if (slash != std::string::npos) file = file.substr(slash + 1);

    int parent = scene_insert_target(&app->scene);
    /* More than one solid is grouped, so the import stays one thing in the
     * tree and can be merged in a single step. */
    if (result.meshes.size() > 1) {
        parent = scene_add_group(&app->scene, parent, file.c_str());
    }

    scene_select_clear(&app->scene);
    for (size_t i = 0; i < result.meshes.size(); ++i) {
        const char *name = (result.meshes.size() > 1) ? result.names[i].c_str() : file.c_str();
        int id = scene_add_object(&app->scene, parent, name, result.meshes[i], POLARITY_POSITIVE);
        scene_select_toggle(&app->scene, id);
    }

    std::string msg = "Imported " + file;
    msg += note.empty() ? "." : (" (" + note + ").");
    ui_set_status(app, msg.c_str(), false);
}

/* Asks how the artwork should become volume, before anything is read. */
void ui_draw_svg_options(App *app) {
    if (app->ui.svg_options_open) {
        ImGui::OpenPopup("SVG Import");
        app->ui.svg_options_open = false;
    }
    if (!ImGui::BeginPopupModal("SVG Import", NULL, ImGuiWindowFlags_AlwaysAutoResize)) return;

    SvgImportOptions *o = &app->ui.svg_options;

    ImGui::TextUnformatted("How should the artwork become a volume?");
    ImGui::Spacing();

    bool outline = (o->mode == SVG_IMPORT_OUTLINE);
    if (ImGui::RadioButton("Outline", outline)) o->mode = SVG_IMPORT_OUTLINE;
    ImGui::SameLine();
    ImGui::TextDisabled("every shape extruded to one height");

    bool heightmap = (o->mode == SVG_IMPORT_HEIGHTMAP);
    if (ImGui::RadioButton("Height map", heightmap)) o->mode = SVG_IMPORT_HEIGHTMAP;
    ImGui::SameLine();
    ImGui::TextDisabled("fill greyscale sets each shape's height");

    ImGui::Separator();
    ImGui::SetNextItemWidth(120.0f);
    if (o->mode == SVG_IMPORT_OUTLINE) {
        ImGui::InputFloat("Height (mm)", &o->height_mm, 0.5f, 5.0f, "%.2f");
    } else {
        ImGui::InputFloat("Tallest (mm)", &o->height_mm, 0.5f, 5.0f, "%.2f");
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputFloat("Flattest (mm)", &o->base_mm, 0.5f, 5.0f, "%.2f");
        ImGui::Checkbox("White is tallest", &o->invert);
        ImGui::TextDisabled("Shapes with no fill colour count as white.");
    }

    if (o->height_mm < 0.01f) o->height_mm = 0.01f;
    if (o->base_mm < 0.01f) o->base_mm = 0.01f;
    if (o->base_mm > o->height_mm) o->base_mm = o->height_mm;

    ImGui::Separator();
    if (ImGui::Button("Import")) {
        std::string path = app->ui.svg_pending_path;
        app->ui.svg_pending_path.clear();
        ImGui::CloseCurrentPopup();
        if (!path.empty()) ui_action_import_svg(app, path.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        app->ui.svg_pending_path.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* Menu bar */

void ui_draw_menubar(App *app) {
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "Ctrl+N")) ui_action_new(app);
        if (ImGui::MenuItem("Open...", "Ctrl+O")) ui_prompt_path(app, FILE_PROMPT_OPEN);
        if (ImGui::MenuItem("Save", "Ctrl+S")) ui_action_save_current(app);
        if (ImGui::MenuItem("Save As...")) ui_prompt_path(app, FILE_PROMPT_SAVE_AS);
        ImGui::Separator();
        if (ImGui::MenuItem("Import STL...")) ui_prompt_path(app, FILE_PROMPT_IMPORT_STL);
        if (ImGui::MenuItem("Import SVG...")) ui_prompt_path(app, FILE_PROMPT_IMPORT_SVG);
        if (ImGui::MenuItem("Export Selection as STL...", NULL, false, ui_can_export(app))) {
            ui_prompt_path(app, FILE_PROMPT_EXPORT);
        }
        /* A browser tab is closed by the browser, and quitting would only leave
         * a dead canvas behind, so the item is not offered there. */
        if (platform_can_quit()) {
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Alt+F4")) app->running = false;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        /* One entry: Ctrl+Z unmerges a selected merged object and otherwise
         * steps the global history. */
        std::string undo_label = "Undo";
        if (ui_can_unmerge(app)) undo_label = "Unmerge";
        else if (undo_can_step_back(&app->undo)) undo_label = std::string("Undo ") + undo_back_label(&app->undo);
        std::string redo_label = "Redo";
        if (ui_can_remerge(app)) redo_label = "Remerge";
        else if (undo_can_step_forward(&app->undo)) redo_label = std::string("Redo ") + undo_forward_label(&app->undo);

        bool can_undo = ui_can_unmerge(app) || undo_can_step_back(&app->undo);
        bool can_redo = ui_can_remerge(app) || undo_can_step_forward(&app->undo);
        if (ImGui::MenuItem(undo_label.c_str(), "Ctrl+Z", false, can_undo)) ui_action_undo(app);
        if (ImGui::MenuItem(redo_label.c_str(), "Ctrl+Y", false, can_redo)) ui_action_redo(app);
        ImGui::Separator();
        if (ImGui::MenuItem("Align...", "L", app->ui.align_active, ui_can_align(app))) {
            ui_action_align(app);
        }
        if (ImGui::MenuItem("Mirror...", "M", app->ui.mirror_active,
                            !app->scene.selection.empty())) {
            ui_action_mirror(app);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, !app->scene.selection.empty())) {
            ui_action_copy(app);
        }
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, ui_can_paste(app))) ui_action_paste(app);
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !app->scene.selection.empty())) {
            ui_action_duplicate(app);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Merge Selection", "Ctrl+G", false, ui_can_merge(app))) {
            ui_action_merge_selection(app);
        }
        if (ImGui::MenuItem("Group Selection", NULL, false, app->scene.selection.size() >= 2)) {
            ui_action_group_selection(app);
        }
        if (ImGui::MenuItem("Repair Meshes", NULL, false, ui_can_repair(app))) {
            ui_action_repair_selection(app);
        }
        if (ImGui::MenuItem("Invert Polarity", "I", false, !app->scene.selection.empty())) {
            scene_invert_selection_polarity(&app->scene);
        }
        if (ImGui::MenuItem("Delete", "Del", false, !app->scene.selection.empty())) {
            ui_action_delete_selection(app);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select All Visible", "A")) scene_select_all_visible(&app->scene);
        if (ImGui::MenuItem("Deselect All", "Esc")) scene_select_clear(&app->scene);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Project Tree", "T", app->ui.show_tree)) {
            app->ui.show_tree = !app->ui.show_tree;
        }
        if (ImGui::MenuItem("Bookshelf", "Y", app->ui.show_shelf)) {
            app->ui.show_shelf = !app->ui.show_shelf;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Home View")) camera_home(&app->camera);
        if (ImGui::MenuItem("Fit Selection")) ui_action_frame_selection(app);
        ImGui::Separator();
        bool ortho = app->camera.orthographic;
        if (ImGui::MenuItem("Orthographic", NULL, ortho)) app->camera.orthographic = !ortho;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("About")) {
        if (ImGui::MenuItem("Help", "F1")) app->ui.show_help = true;
        ImGui::Separator();
        if (ImGui::MenuItem("About OpenBoolCAD")) app->ui.show_about = true;
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

/* Toolbar */

static bool toolbar_button(const char *label, const char *tooltip, bool enabled) {
    if (!enabled) ImGui::BeginDisabled();
    bool pressed = ImGui::Button(label, ImVec2(0.0f, 26.0f));
    if (!enabled) ImGui::EndDisabled();
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tooltip);
    }
    ImGui::SameLine();
    return pressed;
}

static void toolbar_separator(void) {
    ImGui::SameLine();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = 26.0f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 2.0f), ImVec2(p.x, p.y + h - 2.0f),
                                        ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::Dummy(ImVec2(10.0f, h));
    ImGui::SameLine();
}

void ui_draw_toolbar(App *app) {
    float menu_h = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(0.0f, menu_h));
    ImGui::SetNextWindowSize(ImVec2((float)app->window_w, app->ui.toolbar_height));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    if (ImGui::Begin("##toolbar", NULL, flags)) {
        bool has_sel = !app->scene.selection.empty();

        /*
         * Pane folding, first in the row so it stays put whatever else is
         * enabled - on a small screen this is the control that gets the space
         * back.
         *
         * The "##tree" / "##shelf" suffixes are load bearing. ImGui derives a
         * widget's id from its label, and these two arrows swap glyphs with
         * their state: folding the tree turned both of them into "|>", which is
         * one id for two live widgets and an error. Everything after "##" is
         * id but not drawn, so the id stays put while the arrow turns round.
         */
        if (toolbar_button(app->ui.show_tree ? "<|##tree" : "|>##tree",
                           app->ui.show_tree ? "Hide the project tree (T)"
                                             : "Show the project tree (T)", true)) {
            app->ui.show_tree = !app->ui.show_tree;
        }
        if (toolbar_button(app->ui.show_shelf ? "|>##shelf" : "<|##shelf",
                           app->ui.show_shelf ? "Hide the bookshelf (Y)"
                                              : "Show the bookshelf (Y)", true)) {
            app->ui.show_shelf = !app->ui.show_shelf;
        }
        toolbar_separator();

        if (toolbar_button("Copy", "Copy selection (Ctrl+C)", has_sel)) ui_action_copy(app);
        if (toolbar_button("Paste", "Paste (Ctrl+V)", ui_can_paste(app))) ui_action_paste(app);
        if (toolbar_button("Dup", "Duplicate selection (Ctrl+D)", has_sel)) {
            ui_action_duplicate(app);
        }
        if (toolbar_button("Del", "Delete selection", has_sel)) ui_action_delete_selection(app);
        toolbar_separator();
        bool can_undo = ui_can_unmerge(app) || undo_can_step_back(&app->undo);
        bool can_redo = ui_can_remerge(app) || undo_can_step_forward(&app->undo);
        if (toolbar_button("Undo", "Undo, or unmerge a merged object (Ctrl+Z)", can_undo)) {
            ui_action_undo(app);
        }
        if (toolbar_button("Redo", "Redo, or remerge (Ctrl+Y)", can_redo)) ui_action_redo(app);

        /*
         * Right hand cluster, aligned to the panel edge like the mockup.
         *
         * Measured rather than a fixed width: the UI font is embedded and can
         * change, and a hardcoded figure tuned to one face silently runs the
         * last buttons off the end of the row when the next one is wider.
         */
        static const char *RIGHT_LABELS[] = {
            "Merge", "Group", "Ungroup", "Invert", "Align", "Mirror", "Measure", "Export"
        };
        const ImGuiStyle &style = ImGui::GetStyle();
        float right_cluster = 0.0f;
        for (int i = 0; i < (int)(sizeof(RIGHT_LABELS) / sizeof(RIGHT_LABELS[0])); ++i) {
            right_cluster += ImGui::CalcTextSize(RIGHT_LABELS[i]).x +
                             style.FramePadding.x * 2.0f + style.ItemSpacing.x;
        }
        right_cluster += 10.0f + style.ItemSpacing.x * 2.0f; // the one separator
        right_cluster += 16.0f;                              // window padding both sides

        float x = (float)app->window_w - right_cluster;
        if (x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(x);

        if (toolbar_button("Merge", "Merge selection (Ctrl+G)", ui_can_merge(app))) {
            ui_action_merge_selection(app);
        }
        if (toolbar_button("Group", "Group selection", app->scene.selection.size() >= 2)) {
            ui_action_group_selection(app);
        }
        toolbar_button("Ungroup", "Ungroup selection", false);
        if (toolbar_button("Invert", "Invert polarity (I)", has_sel)) {
            scene_invert_selection_polarity(&app->scene);
        }
        toolbar_separator();
        if (toolbar_button("Align", "Align selection (L)", ui_can_align(app))) ui_action_align(app);
        if (toolbar_button("Mirror", "Mirror selection (M)", !app->scene.selection.empty())) {
            ui_action_mirror(app);
        }
        toolbar_button("Measure", "Measure", false);
        if (toolbar_button("Export", "Export selection as STL", ui_can_export(app))) {
            ui_prompt_path(app, FILE_PROMPT_EXPORT);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}
