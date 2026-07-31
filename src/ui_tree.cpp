#include "ui.h"

#include "app.h"
#include "imgui.h"

/*
 * Project tree. Rows are drawn by hand rather than with TreeNodeEx so the
 * +/- toggle, the label and the two right hand state columns can sit exactly
 * where the mockup puts them.
 */

#define ROW_INDENT 22.0f
#define TOGGLE_W 18.0f
#define STATE_COL_W 26.0f
/* Breathing room between the two state buttons. Without it they sit a button
 * width apart, which at this font is close enough to touching that the wrong
 * one gets clicked. */
#define STATE_COL_GAP 12.0f

/*
 * Where the two state columns start. Shared by the rows and by the header, so
 * the labels cannot drift away from the buttons they name.
 */
static void tree_state_columns_x(float *visibility_x, float *polarity_x) {
    float right = ImGui::GetWindowWidth() - ImGui::GetStyle().ScrollbarSize - 8.0f;
    *polarity_x = right - STATE_COL_W;
    *visibility_x = *polarity_x - STATE_COL_W - STATE_COL_GAP;
}

static void tree_begin_rename(UiState *ui, const SceneNode *n) {
    ui->rename_node = n->id;
    ui->rename_focus = true;
    snprintf(ui->rename_buf, sizeof(ui->rename_buf), "%s", n->name.c_str());
}

static void tree_draw_state_columns(App *app, SceneNode *n, float row_top) {
    float visibility_x, polarity_x;
    tree_state_columns_x(&visibility_x, &polarity_x);

    ImGui::PushID(n->id);
    ImGui::SetCursorPos(ImVec2(visibility_x, row_top));

    bool visible = n->visible;
    bool dimmed = !scene_is_effectively_visible(&app->scene, n->id);
    if (dimmed && visible) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (ImGui::SmallButton(visible ? "[V]" : "[ ]")) {
        scene_set_visible(&app->scene, n->id, !visible);
    }
    if (dimmed && visible) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Visibility. Hidden objects cannot be selected.");

    ImGui::SetCursorPos(ImVec2(polarity_x, row_top));
    if (n->kind == NODE_OBJECT) {
        bool negative = (n->polarity == POLARITY_NEGATIVE);
        if (ImGui::SmallButton(negative ? "[-]" : "[+]")) {
            n->polarity = negative ? POLARITY_POSITIVE : POLARITY_NEGATIVE;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Volume polarity: positive or negative.");
    } else {
        ImGui::TextDisabled("[ ]");
    }
    ImGui::PopID();
}

static void tree_draw_node(App *app, int id, int depth) {
    SceneNode *n = scene_node(&app->scene, id);
    if (!n) return;

    UiState *ui = &app->ui;
    ImGui::PushID(id);

    float row_top = ImGui::GetCursorPosY();
    float row_height = ImGui::GetFrameHeight();
    float indent = 8.0f + (float)depth * ROW_INDENT;
    /* A merged node's children are history, so it reads as a leaf and shows a
     * marker instead of an expand toggle. */
    bool has_children = !n->children.empty() && !n->merged;

    /* Expand toggle */
    ImGui::SetCursorPos(ImVec2(indent, row_top));
    if (n->merged) {
        ImGui::TextUnformatted("*");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Merged object. Ctrl+Z unmerges it.");
    } else if (has_children) {
        if (ImGui::SmallButton(n->expanded ? "-" : "+")) n->expanded = !n->expanded;
    } else {
        ImGui::Dummy(ImVec2(TOGGLE_W, ImGui::GetTextLineHeight()));
    }
    ImGui::SameLine(0.0f, 6.0f);

    /* Label, selectable and renamable */
    if (ui->rename_node == id) {
        ImGui::SetNextItemWidth(180.0f);
        if (ui->rename_focus) {
            ImGui::SetKeyboardFocusHere();
            ui->rename_focus = false;
        }
        if (ImGui::InputText("##rename", ui->rename_buf, sizeof(ui->rename_buf),
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            if (ui->rename_buf[0] != 0) n->name = ui->rename_buf;
            ui->rename_node = OBC_NO_NODE;
        }
        if (ImGui::IsItemDeactivated()) ui->rename_node = OBC_NO_NODE;
    } else {
        bool selected = scene_is_selected(&app->scene, id);
        bool dimmed = !scene_is_effectively_visible(&app->scene, id);
        if (dimmed) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

        char label[96];
        snprintf(label, sizeof(label), "%s", n->name.c_str());
        if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(200.0f, 0.0f))) {
            ImGuiIO &io = ImGui::GetIO();
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                tree_begin_rename(ui, n);
            } else if (io.KeyShift || io.KeyCtrl) {
                scene_select_toggle(&app->scene, id);
            } else {
                scene_select_only(&app->scene, id);
            }
        }
        if (dimmed) ImGui::PopStyleColor();

        /* Dragging one row onto another groups them. */
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload(OBC_DND_TREE_NODE, &id, sizeof(int));
            ImGui::Text("Move %s", n->name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(OBC_DND_TREE_NODE);
            if (payload && payload->DataSize == (int)sizeof(int)) {
                int moved = *(const int *)payload->Data;
                SceneNode *target = scene_node(&app->scene, id);
                if (target && scene_can_reparent(&app->scene, moved, id)) {
                    if (target->kind == NODE_GROUP) {
                        scene_reparent(&app->scene, moved, id);
                    } else {
                        /* Dropping onto an object turns the pair into a group
                         * in place, which is what the plan asks for. */
                        int parent = target->parent;
                        int group = scene_add_group(&app->scene, parent, "Group");
                        scene_reparent(&app->scene, id, group);
                        scene_reparent(&app->scene, moved, group);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    tree_draw_state_columns(app, n, row_top);

    /* Rows advance by a fixed height; every widget above placed itself
     * explicitly, so the flow cursor has to be reset here. */
    ImGui::SetCursorPos(ImVec2(0.0f, row_top + row_height));

    if (has_children && n->expanded) {
        for (size_t i = 0; i < n->children.size(); ++i) {
            tree_draw_node(app, n->children[i], depth + 1);
        }
    }

    ImGui::PopID();
}

void ui_draw_tree(App *app) {
    if (!app->ui.show_tree) return;

    float menu_h = ImGui::GetFrameHeight();
    float top = menu_h + app->ui.toolbar_height;

    ImGui::SetNextWindowPos(ImVec2(0.0f, top));
    ImGui::SetNextWindowSize(ImVec2(app->ui.left_width, (float)app->window_h - top));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (ImGui::Begin("##tree", NULL, flags)) {
        /* Column headers, mirroring the two state icons in the mockup. Placed
         * from the same helper the buttons use rather than with SameLine, so
         * each label stays over its own column. */
        float visibility_x, polarity_x;
        tree_state_columns_x(&visibility_x, &polarity_x);
        ImGui::SetCursorPosX(visibility_x);
        ImGui::TextDisabled("Vis");
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetCursorPosX(polarity_x);
        ImGui::TextDisabled("+/-");
        ImGui::Separator();

        for (size_t i = 0; i < app->scene.roots.size(); ++i) {
            tree_draw_node(app, app->scene.roots[i], 0);
        }

        /* Empty space below the tree drops nodes back to the root level. */
        ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, 40.0f));
        if (ImGui::BeginDragDropTarget()) {
            const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(OBC_DND_TREE_NODE);
            if (payload && payload->DataSize == (int)sizeof(int)) {
                scene_reparent(&app->scene, *(const int *)payload->Data, OBC_NO_NODE);
            }
            ImGui::EndDragDropTarget();
        }
    }
    ImGui::End();
}
