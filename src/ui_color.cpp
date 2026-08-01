#include "ui.h"

#include "app.h"
#include "imgui.h"
#include "undo.h"

/*
 * Object colour.
 *
 * "C" opens the picker on whatever the selection is wearing now. Editing is
 * live - the scene takes each change as it is made, so the 3D view behind the
 * dialog shows the actual colour rather than a swatch that has to be trusted.
 *
 * That means Cancel has real work to do, which is what color_before is for: the
 * colour of every node as it stood when the dialog opened, put back on the way
 * out. One undo step is recorded when the dialog opens, so a whole session of
 * dragging around the wheel costs one CTRL+Z rather than dozens.
 */

/* Enough to tell the common cases apart at a glance, and all of them read
 * against the white workplate. The last is the default, so "back to normal" is
 * a click rather than something to hunt for in the wheel. */
static const struct { const char *name; float r, g, b; } SWATCHES[] = {
    { "Red",    0.85f, 0.30f, 0.28f },
    { "Orange", 0.93f, 0.58f, 0.24f },
    { "Yellow", 0.93f, 0.83f, 0.30f },
    { "Green",  0.42f, 0.72f, 0.40f },
    { "Teal",   0.30f, 0.70f, 0.68f },
    { "Blue",   0.35f, 0.55f, 0.85f },
    { "Purple", 0.60f, 0.45f, 0.80f },
    { "Pink",   0.90f, 0.60f, 0.72f },
    { "Brown",  0.60f, 0.45f, 0.32f },
    { "Black",  0.22f, 0.22f, 0.24f },
    { "White",  0.93f, 0.93f, 0.93f },
    { "Grey",   0.68f, 0.74f, 0.80f }
};
#define SWATCH_COUNT ((int)(sizeof(SWATCHES) / sizeof(SWATCHES[0])))

/* Remembers what every node was wearing, indexed by node id. Whole scene rather
 * than just the selection: a group applies down its subtree, so the set of
 * nodes a cancel has to restore is not the selection list. */
static void snapshot_colors(App *app) {
    const Scene &s = app->scene;
    app->ui.color_before.resize(s.nodes.size());
    for (size_t i = 0; i < s.nodes.size(); ++i) {
        app->ui.color_before[i] = s.nodes[i].color;
    }
}

static void restore_colors(App *app) {
    Scene *s = &app->scene;
    size_t count = app->ui.color_before.size();
    if (count > s->nodes.size()) count = s->nodes.size();
    for (size_t i = 0; i < count; ++i) {
        s->nodes[i].color = app->ui.color_before[i];
    }
}

static void apply_live(App *app) {
    scene_set_selection_color(&app->scene, app->ui.color_value);
}

void ui_action_choose_color(App *app) {
    if (app->scene.selection.empty()) {
        ui_set_status(app, "Select something to colour.", true);
        return;
    }

    Vec3 current;
    if (!scene_selection_color(&app->scene, &current)) {
        /* Groups with nothing in them: there is no object to paint, and saying
         * so beats opening a picker that cannot change anything. */
        ui_set_status(app, "Nothing in the selection has a colour.", true);
        return;
    }

    /* Recorded here rather than on OK, so the undo step describes the state
     * before any of the live edits landed. */
    undo_record(&app->undo, app->scene, "Colour");
    snapshot_colors(app);

    app->ui.color_value = current;
    app->ui.color_open = true;
    app->ui.color_active = true;
}

void ui_draw_color_options(App *app) {
    UiState *ui = &app->ui;
    if (ui->color_open) {
        ImGui::OpenPopup("Colour");
        ui->color_open = false;
    }
    if (!ImGui::BeginPopupModal("Colour", NULL, ImGuiWindowFlags_AlwaysAutoResize)) return;

    float rgb[3] = { ui->color_value.x, ui->color_value.y, ui->color_value.z };

    /* No alpha: transparency is what marks a negative volume, so letting it be
     * set by hand would make a solid that lies about which it is. */
    if (ImGui::ColorPicker3("##wheel", rgb,
                            ImGuiColorEditFlags_NoSidePreview |
                            ImGuiColorEditFlags_NoSmallPreview)) {
        ui->color_value = vec3(rgb[0], rgb[1], rgb[2]);
        apply_live(app);
    }

    ImGui::Spacing();
    for (int i = 0; i < SWATCH_COUNT; ++i) {
        if (i % 6 != 0) ImGui::SameLine();
        ImVec4 col = ImVec4(SWATCHES[i].r, SWATCHES[i].g, SWATCHES[i].b, 1.0f);
        /* The name is the id as well as the tooltip, so two swatches can never
         * collide the way two same-labelled buttons would. */
        if (ImGui::ColorButton(SWATCHES[i].name, col, 0, ImVec2(28.0f, 22.0f))) {
            ui->color_value = vec3(SWATCHES[i].r, SWATCHES[i].g, SWATCHES[i].b);
            apply_live(app);
        }
    }

    ImGui::Separator();
    if (ImGui::Button("OK")) {
        int touched = scene_set_selection_color(&app->scene, ui->color_value);
        char msg[96];
        snprintf(msg, sizeof(msg), "Coloured %d object%s.", touched, touched == 1 ? "" : "s");
        ui_set_status(app, msg, false);
        ui->color_before.clear();
        ui->color_active = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        restore_colors(app);
        /* The undo step recorded on open now describes a change that never
         * happened, so it goes back rather than sitting in the history. */
        undo_drop_last(&app->undo);
        ui->color_before.clear();
        ui->color_active = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
