#include "ui.h"

#include "app.h"
#include "imgui.h"

/*
 * Bookshelf of primitives. Thumbnails are drawn with ImDrawList instead of
 * textures: no asset pipeline is needed, and the same routine can render the
 * drag preview.
 */

#define SHELF_COLUMNS 3
#define SHELF_CELL 72.0f

static const BookshelfItem SHELF_ITEMS[] = {
    { PRIM_CUBE,     POLARITY_POSITIVE },
    { PRIM_CYLINDER, POLARITY_POSITIVE },
    { PRIM_SPHERE,   POLARITY_POSITIVE },
    { PRIM_CONE,     POLARITY_POSITIVE },
    { PRIM_PYRAMID,  POLARITY_POSITIVE },
    { PRIM_WEDGE,    POLARITY_POSITIVE },
    { PRIM_CUBE,     POLARITY_NEGATIVE },
    { PRIM_CYLINDER, POLARITY_NEGATIVE },
    { PRIM_SPHERE,   POLARITY_NEGATIVE },
    { PRIM_CONE,     POLARITY_NEGATIVE },
    { PRIM_PYRAMID,  POLARITY_NEGATIVE },
    { PRIM_WEDGE,    POLARITY_NEGATIVE }
};
#define SHELF_ITEM_COUNT ((int)(sizeof(SHELF_ITEMS) / sizeof(SHELF_ITEMS[0])))

static ImU32 shade(ImU32 base, float f, float alpha) {
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(base);
    c.x *= f; c.y *= f; c.z *= f; c.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

static ImU32 primitive_color(PrimitiveKind kind) {
    switch (kind) {
    case PRIM_CUBE:     return IM_COL32(198, 56, 56, 255);
    case PRIM_CYLINDER: return IM_COL32(226, 128, 38, 255);
    case PRIM_SPHERE:   return IM_COL32(38, 150, 210, 255);
    case PRIM_CONE:     return IM_COL32(140, 74, 178, 255);
    case PRIM_PYRAMID:  return IM_COL32(224, 198, 40, 255);
    case PRIM_WEDGE:    return IM_COL32(64, 168, 112, 255);
    default:            return IM_COL32(140, 140, 140, 255);
    }
}

/* Diagonal hatch, the visual shorthand for a negative volume. */
static void hatch_region(ImDrawList *dl, ImVec2 c, float r, ImU32 col) {
    dl->PushClipRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), true);
    for (float o = -2.0f * r; o < 2.0f * r; o += 5.0f) {
        dl->AddLine(ImVec2(c.x - r + o, c.y + r), ImVec2(c.x + r + o, c.y - r), col, 1.0f);
    }
    dl->PopClipRect();
}

void ui_draw_primitive_glyph(PrimitiveKind kind, Polarity polarity, float size,
                             float screen_x, float screen_y) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 c = ImVec2(screen_x + size * 0.5f, screen_y + size * 0.5f);
    float r = size * 0.34f;

    bool negative = (polarity == POLARITY_NEGATIVE);
    ImU32 base = negative ? IM_COL32(150, 150, 150, 255) : primitive_color(kind);
    float alpha = negative ? 0.55f : 1.0f;

    ImU32 top  = shade(base, 1.00f, alpha);
    ImU32 left = shade(base, 0.72f, alpha);
    ImU32 rght = shade(base, 0.55f, alpha);
    ImU32 line = IM_COL32(40, 40, 40, negative ? 150 : 210);

    switch (kind) {
    case PRIM_CUBE: {
        /* Isometric box: top rhombus plus two side faces. */
        ImVec2 t = ImVec2(c.x, c.y - r);
        ImVec2 l = ImVec2(c.x - r, c.y - r * 0.5f);
        ImVec2 rr = ImVec2(c.x + r, c.y - r * 0.5f);
        ImVec2 m = ImVec2(c.x, c.y);
        ImVec2 bl = ImVec2(c.x - r, c.y + r * 0.5f);
        ImVec2 br = ImVec2(c.x + r, c.y + r * 0.5f);
        ImVec2 bm = ImVec2(c.x, c.y + r);
        dl->AddQuadFilled(t, rr, m, l, top);
        dl->AddQuadFilled(l, m, bm, bl, left);
        dl->AddQuadFilled(m, rr, br, bm, rght);
        dl->AddLine(t, l, line); dl->AddLine(t, rr, line);
        dl->AddLine(l, m, line); dl->AddLine(rr, m, line);
        dl->AddLine(l, bl, line); dl->AddLine(rr, br, line);
        dl->AddLine(m, bm, line);
        dl->AddLine(bl, bm, line); dl->AddLine(bm, br, line);
        break;
    }
    case PRIM_CYLINDER: {
        float rx = r * 0.85f;
        float ry = r * 0.34f;
        float top_y = c.y - r * 0.65f;
        float bot_y = c.y + r * 0.65f;
        dl->AddRectFilled(ImVec2(c.x - rx, top_y), ImVec2(c.x + rx, bot_y), left);
        dl->AddEllipseFilled(ImVec2(c.x, bot_y), ImVec2(rx, ry), rght);
        dl->AddEllipseFilled(ImVec2(c.x, top_y), ImVec2(rx, ry), top);
        dl->AddEllipse(ImVec2(c.x, top_y), ImVec2(rx, ry), line);
        dl->AddLine(ImVec2(c.x - rx, top_y), ImVec2(c.x - rx, bot_y), line);
        dl->AddLine(ImVec2(c.x + rx, top_y), ImVec2(c.x + rx, bot_y), line);
        /* Elliptical, not circular: PathArcTo would bulge down by rx instead of
         * following the base ellipse's ry. */
        dl->PathEllipticalArcTo(ImVec2(c.x, bot_y), ImVec2(rx, ry), 0.0f, 0.0f, 3.14159265f, 20);
        dl->PathStroke(line, 0, 1.0f);
        break;
    }
    case PRIM_SPHERE: {
        dl->AddCircleFilled(c, r * 0.9f, left, 32);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.2f, c.y - r * 0.22f), r * 0.62f, top, 32);
        dl->AddCircle(c, r * 0.9f, line, 32);
        break;
    }
    case PRIM_CONE: {
        ImVec2 apex = ImVec2(c.x, c.y - r);
        float rx = r * 0.8f;
        float ry = r * 0.3f;
        float base_y = c.y + r * 0.6f;
        dl->AddTriangleFilled(apex, ImVec2(c.x - rx, base_y), ImVec2(c.x + rx, base_y), left);
        dl->AddEllipseFilled(ImVec2(c.x, base_y), ImVec2(rx, ry), rght);
        dl->AddTriangleFilled(apex, ImVec2(c.x - rx, base_y), ImVec2(c.x, base_y), top);
        dl->AddLine(apex, ImVec2(c.x - rx, base_y), line);
        dl->AddLine(apex, ImVec2(c.x + rx, base_y), line);
        dl->AddEllipse(ImVec2(c.x, base_y), ImVec2(rx, ry), line);
        break;
    }
    case PRIM_PYRAMID: {
        ImVec2 apex = ImVec2(c.x, c.y - r);
        ImVec2 fl = ImVec2(c.x - r * 0.85f, c.y + r * 0.45f);
        ImVec2 fr = ImVec2(c.x + r * 0.85f, c.y + r * 0.45f);
        ImVec2 fm = ImVec2(c.x, c.y + r * 0.75f);
        dl->AddTriangleFilled(apex, fl, fm, top);
        dl->AddTriangleFilled(apex, fm, fr, left);
        dl->AddLine(apex, fl, line); dl->AddLine(apex, fr, line);
        dl->AddLine(apex, fm, line);
        dl->AddLine(fl, fm, line); dl->AddLine(fm, fr, line);
        break;
    }
    case PRIM_WEDGE: {
        /* Ramp seen from the same corner as the cube: the tall wall on the
         * left, the sloping face running down to the right. */
        float d = r * 0.42f;
        ImVec2 fa = ImVec2(c.x - r * 0.75f, c.y + r * 0.55f);  // bottom, tall end
        ImVec2 fb = ImVec2(c.x + r * 0.95f, c.y + r * 0.55f);  // bottom, thin end
        ImVec2 ft = ImVec2(c.x - r * 0.75f, c.y - r * 0.55f);  // top of the wall
        ImVec2 ba = ImVec2(fa.x + d, fa.y - d);
        ImVec2 bb = ImVec2(fb.x + d, fb.y - d);
        ImVec2 bt = ImVec2(ft.x + d, ft.y - d);

        dl->AddTriangleFilled(ba, bb, bt, rght);          // far end
        dl->AddQuadFilled(ft, fb, bb, bt, top);           // the slope
        dl->AddTriangleFilled(fa, fb, ft, left);          // near end
        dl->AddLine(fa, fb, line); dl->AddLine(fb, ft, line); dl->AddLine(ft, fa, line);
        dl->AddLine(ft, bt, line); dl->AddLine(fb, bb, line);
        dl->AddLine(bt, bb, line); dl->AddLine(ba, bb, line); dl->AddLine(ba, bt, line);
        break;
    }
    default:
        break;
    }

    if (negative) hatch_region(dl, c, r * 1.05f, IM_COL32(90, 90, 90, 120));
}

/*
 * Gear glyph. Not a PrimitiveKind, so it does not go through the switch above:
 * a generator produces geometry from parameters rather than from a fixed
 * builder, and the shelf shows it in its own section.
 */
void ui_draw_gear_glyph(float size, float screen_x, float screen_y, bool negative) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 c = ImVec2(screen_x + size * 0.5f, screen_y + size * 0.5f);
    float r = size * 0.34f;

    ImU32 base = negative ? IM_COL32(150, 150, 150, 255) : IM_COL32(120, 132, 148, 255);
    float alpha = negative ? 0.55f : 1.0f;
    ImU32 body = shade(base, 1.00f, alpha);
    ImU32 tooth = shade(base, 0.82f, alpha);
    ImU32 hub = shade(base, 0.55f, alpha);
    ImU32 line = IM_COL32(40, 40, 40, negative ? 150 : 210);

    const int teeth = 10;
    float r_body = r * 0.74f;
    float r_tip = r * 1.02f;
    float half = 3.14159265f / (float)teeth * 0.42f;

    for (int i = 0; i < teeth; ++i) {
        float a = 2.0f * 3.14159265f * (float)i / (float)teeth;
        ImVec2 p0 = ImVec2(c.x + cosf(a - half) * r_body * 0.98f,
                           c.y + sinf(a - half) * r_body * 0.98f);
        ImVec2 p1 = ImVec2(c.x + cosf(a - half * 0.72f) * r_tip,
                           c.y + sinf(a - half * 0.72f) * r_tip);
        ImVec2 p2 = ImVec2(c.x + cosf(a + half * 0.72f) * r_tip,
                           c.y + sinf(a + half * 0.72f) * r_tip);
        ImVec2 p3 = ImVec2(c.x + cosf(a + half) * r_body * 0.98f,
                           c.y + sinf(a + half) * r_body * 0.98f);
        dl->AddQuadFilled(p0, p1, p2, p3, tooth);
        dl->AddLine(p0, p1, line); dl->AddLine(p1, p2, line); dl->AddLine(p2, p3, line);
    }

    dl->AddCircleFilled(c, r_body, body, 32);
    dl->AddCircle(c, r_body, line, 32);
    dl->AddCircleFilled(c, r * 0.26f, hub, 24);
    dl->AddCircle(c, r * 0.26f, line, 24);

    if (negative) hatch_region(dl, c, r * 1.05f, IM_COL32(90, 90, 90, 120));
}

/*
 * Resolution menu, on a right click over a round primitive.
 *
 * Segment count is not a cosmetic choice: it is what a boolean has to chew on
 * later, so a draft part and a finished one want different numbers. Picking it
 * before placing beats placing and regretting, since nothing re-tessellates an
 * object after the fact.
 */
static void resolution_menu(App *app, PrimitiveKind kind) {
    PrimitiveResolution *r = &app->ui.resolution;

    ImGui::TextDisabled("%s resolution", primitive_name(kind));
    ImGui::Separator();

    struct Preset { const char *name; int segments; int rings; };
    static const Preset presets[] = {
        { "Draft",  12, 6 },
        { "Normal", 32, 16 },
        { "Smooth", 64, 32 },
        { "Fine",  128, 64 }
    };

    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::SmallButton(presets[i].name)) {
            if (kind == PRIM_SPHERE) {
                r->sphere_segments = presets[i].segments;
                r->sphere_rings = presets[i].rings;
            } else if (kind == PRIM_CYLINDER) {
                r->cylinder_segments = presets[i].segments;
            } else {
                r->cone_segments = presets[i].segments;
            }
        }
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(150.0f);
    if (kind == PRIM_SPHERE) {
        ImGui::InputInt("Segments", &r->sphere_segments);
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputInt("Rings", &r->sphere_rings);
    } else if (kind == PRIM_CYLINDER) {
        ImGui::InputInt("Segments", &r->cylinder_segments);
    } else {
        ImGui::InputInt("Segments", &r->cone_segments);
    }
    primitive_resolution_clamp(r);

    ImGui::Separator();
    ImGui::Text("%d triangles", primitive_triangle_count(kind, *r));

    ImGui::Spacing();
    if (ImGui::Button("Place")) {
        ui_action_add_primitive(app, kind, app->ui.resolution_menu_polarity);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
}

/* Text generator glyph: a serif "A", which reads as "type" at 72 pixels far
 * better than a word would. */
void ui_draw_text_glyph(float size, float screen_x, float screen_y, bool negative) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 c = ImVec2(screen_x + size * 0.5f, screen_y + size * 0.5f);
    float r = size * 0.34f;

    ImU32 base = negative ? IM_COL32(150, 150, 150, 255) : IM_COL32(196, 128, 74, 255);
    float alpha = negative ? 0.55f : 1.0f;
    ImU32 face = shade(base, 1.00f, alpha);
    ImU32 side = shade(base, 0.62f, alpha);
    ImU32 line = IM_COL32(40, 40, 40, negative ? 150 : 210);

    ImVec2 apex = ImVec2(c.x, c.y - r);
    ImVec2 left = ImVec2(c.x - r * 0.78f, c.y + r * 0.8f);
    ImVec2 right = ImVec2(c.x + r * 0.78f, c.y + r * 0.8f);
    float d = r * 0.2f;

    /* An extruded slab behind the letter, so it reads as a solid not a label. */
    dl->AddTriangleFilled(ImVec2(apex.x + d, apex.y - d), ImVec2(left.x + d, left.y - d),
                          ImVec2(right.x + d, right.y - d), side);
    dl->AddTriangleFilled(apex, left, right, face);
    dl->AddLine(apex, left, line, 1.5f);
    dl->AddLine(apex, right, line, 1.5f);
    dl->AddLine(left, right, line, 1.5f);

    /* The counter, punched with the cell colour behind it. */
    ImVec2 bar_l = ImVec2(c.x - r * 0.34f, c.y + r * 0.16f);
    ImVec2 bar_r = ImVec2(c.x + r * 0.34f, c.y + r * 0.16f);
    dl->AddTriangleFilled(ImVec2(c.x, c.y - r * 0.42f), bar_l, bar_r,
                          IM_COL32(236, 236, 238, negative ? 140 : 255));

    if (negative) hatch_region(dl, c, r * 1.05f, IM_COL32(90, 90, 90, 120));
}

/* An icosahedron-ish glyph: a faceted ball, which is what "n-sided solid"
 * looks like at 72 pixels. */
void ui_draw_polyhedron_glyph(float size, float screen_x, float screen_y, bool negative) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 c = ImVec2(screen_x + size * 0.5f, screen_y + size * 0.5f);
    float r = size * 0.33f;

    ImU32 base = negative ? IM_COL32(150, 150, 150, 255) : IM_COL32(92, 158, 132, 255);
    float alpha = negative ? 0.55f : 1.0f;
    ImU32 line = IM_COL32(40, 40, 40, negative ? 150 : 210);

    /* A hexagonal silhouette split into three rhombi reads as a faceted solid
     * from a three quarter view, the same way the cube glyph does. */
    ImVec2 hex[6];
    for (int i = 0; i < 6; ++i) {
        float a = 3.14159265f / 6.0f + 3.14159265f / 3.0f * (float)i;
        hex[i] = ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r);
    }
    ImVec2 mid = c;
    ImVec2 upper = ImVec2(c.x, c.y - r * 0.42f);

    dl->AddQuadFilled(hex[0], hex[1], upper, hex[5], shade(base, 1.00f, alpha));
    dl->AddQuadFilled(hex[1], hex[2], hex[3], upper, shade(base, 0.74f, alpha));
    dl->AddQuadFilled(upper, hex[3], hex[4], hex[5], shade(base, 0.56f, alpha));
    (void)mid;

    for (int i = 0; i < 6; ++i) dl->AddLine(hex[i], hex[(i + 1) % 6], line, 1.0f);
    dl->AddLine(hex[1], upper, line, 1.0f);
    dl->AddLine(hex[3], upper, line, 1.0f);
    dl->AddLine(hex[5], upper, line, 1.0f);

    if (negative) hatch_region(dl, c, r * 1.05f, IM_COL32(90, 90, 90, 120));
}

void ui_draw_bookshelf(App *app) {
    if (!app->ui.show_shelf) return;

    float menu_h = ImGui::GetFrameHeight();
    float top = menu_h + app->ui.toolbar_height;
    float w = app->ui.right_width;

    ImGui::SetNextWindowPos(ImVec2((float)app->window_w - w, top));
    ImGui::SetNextWindowSize(ImVec2(w, (float)app->window_h - top));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (ImGui::Begin("##bookshelf", NULL, flags)) {
        int col = 0;
        Polarity section = POLARITY_POSITIVE;
        ImGui::TextDisabled("Solids");
        ImGui::Separator();

        for (int i = 0; i < SHELF_ITEM_COUNT; ++i) {
            BookshelfItem item = SHELF_ITEMS[i];

            /* Holes start their own row so a shelf row never mixes the two. */
            if (item.polarity != section) {
                section = item.polarity;
                if (col != 0) {
                    ImGui::NewLine();
                    col = 0;
                }
                ImGui::Spacing();
                ImGui::TextDisabled("Holes");
                ImGui::Separator();
            }

            ImGui::PushID(i);
            ImVec2 cell = ImGui::GetCursorScreenPos();
            if (ImGui::Button("##cell", ImVec2(SHELF_CELL, SHELF_CELL))) {
                ui_action_add_primitive(app, item.kind, item.polarity);
            }
            /* Opened before the tooltip so a right click does not have to
             * fight it for the same frame. */
            if (primitive_has_resolution(item.kind) &&
                ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                app->ui.resolution_menu_kind = (int)item.kind;
                app->ui.resolution_menu_polarity = item.polarity;
                ImGui::OpenPopup("##resolution");
            }
            if (ImGui::BeginPopup("##resolution")) {
                resolution_menu(app, item.kind);
                ImGui::EndPopup();
            }

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s%s\nClick to place, or drag into the view.%s",
                                  item.polarity == POLARITY_NEGATIVE ? "Negative " : "",
                                  primitive_name(item.kind),
                                  primitive_has_resolution(item.kind)
                                      ? "\nRight click to choose the resolution." : "");
            }
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ImGui::SetDragDropPayload(OBC_DND_PRIMITIVE, &item, sizeof(BookshelfItem));
                ImVec2 preview = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(48.0f, 48.0f));
                ui_draw_primitive_glyph(item.kind, item.polarity, 48.0f, preview.x, preview.y);
                ImGui::EndDragDropSource();
            }

            /* Painted after the button so the glyph sits on top of its face. */
            ui_draw_primitive_glyph(item.kind, item.polarity, SHELF_CELL, cell.x, cell.y);
            ImGui::PopID();

            if (++col < SHELF_COLUMNS && i + 1 < SHELF_ITEM_COUNT) {
                ImGui::SameLine();
            } else {
                col = 0;
            }
        }

        /*
         * Generators build geometry from parameters rather than from a fixed
         * builder, so there is nothing to drag before the numbers exist: the
         * cell opens its dialog and the object appears on Create.
         */
        if (col != 0) ImGui::NewLine();
        ImGui::Spacing();
        ImGui::TextDisabled("Generators");
        ImGui::Separator();

        ImVec2 cell = ImGui::GetCursorScreenPos();
        if (ImGui::Button("##gear", ImVec2(SHELF_CELL, SHELF_CELL))) {
            app->ui.gear_open = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Gear\nClick to set the parameters.");
        }
        ui_draw_gear_glyph(SHELF_CELL, cell.x, cell.y, app->ui.gear_params.negative);

        ImGui::SameLine();
        ImVec2 text_cell = ImGui::GetCursorScreenPos();
        if (ImGui::Button("##text", ImVec2(SHELF_CELL, SHELF_CELL))) {
            app->ui.text_open = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Text\nClick to type a word and pick a font.");
        }
        ui_draw_text_glyph(SHELF_CELL, text_cell.x, text_cell.y, app->ui.text_params.negative);

        ImGui::SameLine();
        ImVec2 poly_cell = ImGui::GetCursorScreenPos();
        if (ImGui::Button("##poly", ImVec2(SHELF_CELL, SHELF_CELL))) {
            app->ui.poly_open = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("N-sided solid\nPlatonic solids, prisms and pyramids.");
        }
        ui_draw_polyhedron_glyph(SHELF_CELL, poly_cell.x, poly_cell.y,
                                 app->ui.poly_params.negative);
    }
    ImGui::End();
}
