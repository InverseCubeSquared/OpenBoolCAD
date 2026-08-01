#include "ui.h"

#include "app.h"
#include "imgui.h"
#include "undo.h"

/*
 * Bevel tool.
 *
 * Edges are collected in the object's own space and kept there, so moving or
 * turning the part does not invalidate the set - only editing its geometry
 * does, which is what bevel_node guards against.
 *
 * Picking is in screen space, like every other gizmo here, because the edges
 * are drawn at a fixed pixel width and a fixed pixel radius is what matches
 * what the user sees.
 */

#define BEVEL_PICK_RADIUS 10.0f

/* Which single object the tool works on: the bevel applies to one mesh, so a
 * group or a multi-selection has nothing to act on. */
static int bevel_target(const App *app) {
    if (app->scene.selection.size() != 1) return OBC_NO_NODE;
    int id = app->scene.selection[0];
    const SceneNode *n = scene_node(&app->scene, id);
    if (!n || n->kind != NODE_OBJECT || n->mesh.vertices.empty()) return OBC_NO_NODE;
    return id;
}

void ui_bevel_refresh(App *app) {
    UiState *ui = &app->ui;
    int id = bevel_target(app);

    if (id == OBC_NO_NODE) {
        ui->bevel_node = OBC_NO_NODE;
        ui->bevel_edges.clear();
        ui->bevel_selected.clear();
        ui->bevel_hover = -1;
        return;
    }
    if (id == ui->bevel_node) return; // already collected for this object

    const SceneNode *n = scene_node(&app->scene, id);
    ui->bevel_node = id;
    ui->bevel_selected.clear();
    ui->bevel_hover = -1;
    bevel_collect_edges(n->mesh, &ui->bevel_edges);

    char msg[160];
    snprintf(msg, sizeof(msg),
             "Bevel: %d edges. Left click to pick, right click to drop, Shift+B to set.",
             (int)ui->bevel_edges.size());
    ui_set_status(app, msg, false);
}

/* Distance from a point to a projected segment, in pixels. */
static float segment_distance(const App *app, float vw, float vh, Vec3 a, Vec3 b,
                              float sx, float sy) {
    float ax, ay, bx, by;
    if (!camera_project(app->camera, (int)vw, (int)vh, a, &ax, &ay)) return 1e9f;
    if (!camera_project(app->camera, (int)vw, (int)vh, b, &bx, &by)) return 1e9f;

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

static int bevel_nearest(const App *app, float vw, float vh, float sx, float sy) {
    const UiState &ui = app->ui;
    if (ui.bevel_node == OBC_NO_NODE) return -1;

    int best = -1;
    float best_dist = BEVEL_PICK_RADIUS;
    for (size_t i = 0; i < ui.bevel_edges.size(); ++i) {
        /* An edge is a whole run - a cylinder rim is one of them - so the whole
         * polyline is tested and one click takes the lot. */
        const BevelEdge &edge = ui.bevel_edges[i];
        size_t count = edge.points.size();
        size_t steps = edge.closed ? count : (count > 0 ? count - 1 : 0);
        for (size_t s = 0; s < steps; ++s) {
            Vec3 a = scene_world_point(&app->scene, ui.bevel_node, edge.points[s]);
            Vec3 b = scene_world_point(&app->scene, ui.bevel_node,
                                       edge.points[(s + 1) % count]);
            float d = segment_distance(app, vw, vh, a, b, sx, sy);
            if (d <= best_dist) {
                best_dist = d;
                best = (int)i;
            }
        }
    }
    return best;
}

void ui_bevel_update_hover(App *app, float region_w, float region_h, float sx, float sy) {
    app->ui.bevel_hover = bevel_nearest(app, region_w, region_h, sx, sy);
}

void ui_bevel_pick(App *app, float region_w, float region_h, float sx, float sy, bool add) {
    int hit = bevel_nearest(app, region_w, region_h, sx, sy);
    if (hit < 0) return;

    std::vector<int> &chosen = app->ui.bevel_selected;
    for (size_t i = 0; i < chosen.size(); ++i) {
        if (chosen[i] != hit) continue;
        if (!add) chosen.erase(chosen.begin() + (long)i);
        return; // already in the set: left click leaves it, right click drops it
    }
    if (add) chosen.push_back(hit);
}

/* Applying */

void ui_action_apply_bevel(App *app) {
    UiState *ui = &app->ui;
    if (ui->bevel_node == OBC_NO_NODE) {
        ui_set_status(app, "Select one object to bevel.", true);
        return;
    }
    const SceneNode *n = scene_node(&app->scene, ui->bevel_node);
    if (!n) return;

    /*
     * Built before anything is recorded, so a bevel that cannot produce a solid
     * leaves the scene and the undo stack untouched - the same contract the
     * generators keep.
     */
    Mesh result;
    std::string error;
    if (!bevel_apply(n->mesh, ui->bevel_edges, ui->bevel_selected,
                     ui->bevel_radius, ui->bevel_segments, &result, &error)) {
        ui_set_status(app, error.c_str(), true);
        return;
    }

    undo_record(&app->undo, app->scene, "Bevel");
    SceneNode *target = scene_node(&app->scene, ui->bevel_node);
    if (!target) return;
    target->mesh = result;

    int count = (int)ui->bevel_selected.size();

    /* The geometry changed, so the edge list it was collected from is stale.
     * Clearing bevel_node makes the next refresh rebuild it. */
    ui->bevel_node = OBC_NO_NODE;
    ui->bevel_selected.clear();
    ui->bevel_hover = -1;
    ui_bevel_refresh(app);

    char msg[160];
    snprintf(msg, sizeof(msg), "Bevelled %d edge%s at %.2f mm, %d segment%s.",
             count, count == 1 ? "" : "s", ui->bevel_radius,
             ui->bevel_segments, ui->bevel_segments == 1 ? "" : "s");
    ui_set_status(app, msg, false);
}

void ui_draw_bevel_options(App *app) {
    UiState *ui = &app->ui;
    if (ui->bevel_menu_open) {
        ImGui::OpenPopup("Bevel");
        ui->bevel_menu_open = false;
    }
    if (!ImGui::BeginPopupModal("Bevel", NULL, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("%d edge%s selected.", (int)ui->bevel_selected.size(),
                ui->bevel_selected.size() == 1 ? "" : "s");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputFloat("Amount (mm)", &ui->bevel_radius, 0.1f, 1.0f, "%.2f");
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputInt("Segments", &ui->bevel_segments);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("1 is a chamfer. More segments round it off.");
    }

    if (ui->bevel_segments < 1) ui->bevel_segments = 1;
    if (ui->bevel_segments > 64) ui->bevel_segments = 64;

    /* Clamped against the part rather than left to fail in the boolean: a
     * radius past the faces the edge sits between has no answer. */
    const SceneNode *n = scene_node(&app->scene, ui->bevel_node);
    float limit = 100.0f;
    if (n) limit = bevel_max_radius(n->mesh, ui->bevel_edges, ui->bevel_selected);
    if (ui->bevel_radius < 0.01f) ui->bevel_radius = 0.01f;
    if (ui->bevel_radius > limit) ui->bevel_radius = limit;

    ImGui::TextDisabled("Up to %.2f mm on these edges.", limit);
    ImGui::TextDisabled("%s", ui->bevel_segments == 1 ? "Chamfer." : "Rounded.");

    ImGui::Separator();
    bool can_apply = !ui->bevel_selected.empty();
    if (!can_apply) ImGui::BeginDisabled();
    if (ImGui::Button("Apply")) {
        ui_action_apply_bevel(app);
        ImGui::CloseCurrentPopup();
    }
    if (!can_apply) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
