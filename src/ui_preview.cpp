#include "ui.h"

#include <algorithm>

#include "imgui.h"

/*
 * Shape preview for the generator dialogs.
 *
 * It draws the same contours the generator hands to csg_extrude, so the picture
 * and the part cannot disagree - which is the whole point of previewing rather
 * than illustrating.
 *
 * The fill is a scanline with the even-odd rule, not one filled path per
 * contour. Filling each contour separately and painting the holes back in the
 * background colour only works when a hole happens to be drawn after the shape
 * it sits in, and when no two contours at the same level overlap. Neither holds
 * across fonts: a pixel face is built from many small blocks and some faces
 * emit their inner contours first, both of which came out as garbage. Even-odd
 * over all contours at once has no order to get wrong, and it is the same rule
 * csg_extrude resolves nesting with, so the preview agrees with the solid.
 */

static void fill_scanline(ImDrawList *dl, const std::vector<std::vector<ImVec2> > &polys,
                          ImVec2 lo, ImVec2 hi, ImU32 col) {
    std::vector<float> crossings;

    for (float y = lo.y + 0.5f; y < hi.y; y += 1.0f) {
        crossings.clear();

        for (size_t p = 0; p < polys.size(); ++p) {
            const std::vector<ImVec2> &poly = polys[p];
            size_t n = poly.size();
            if (n < 3) continue;

            for (size_t i = 0, j = n - 1; i < n; j = i++) {
                ImVec2 a = poly[i];
                ImVec2 b = poly[j];
                /* Half open in y, so a vertex exactly on the scanline is
                 * counted once rather than zero or twice. */
                if ((a.y > y) == (b.y > y)) continue;
                float t = (y - a.y) / (b.y - a.y);
                crossings.push_back(a.x + t * (b.x - a.x));
            }
        }

        if (crossings.size() < 2) continue;
        std::sort(crossings.begin(), crossings.end());

        for (size_t i = 0; i + 1 < crossings.size(); i += 2) {
            float x0 = crossings[i] < lo.x ? lo.x : crossings[i];
            float x1 = crossings[i + 1] > hi.x ? hi.x : crossings[i + 1];
            if (x1 <= x0) continue;
            dl->AddRectFilled(ImVec2(x0, y - 0.5f), ImVec2(x1, y + 0.5f), col);
        }
    }
}

void ui_draw_contour_preview(const std::vector<std::vector<Vec2> > &contours, float size) {
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 lo = origin;
    ImVec2 hi = ImVec2(origin.x + size, origin.y + size);
    dl->AddRectFilled(lo, hi, IM_COL32(250, 250, 252, 255));
    dl->AddRect(lo, hi, IM_COL32(170, 175, 185, 255));

    /* Fit the artwork, whatever scale its parameters happen to be in. */
    float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
    bool any = false;
    for (size_t c = 0; c < contours.size(); ++c) {
        for (size_t i = 0; i < contours[c].size(); ++i) {
            Vec2 p = contours[c][i];
            if (!any) {
                min_x = max_x = p.x;
                min_y = max_y = p.y;
                any = true;
                continue;
            }
            if (p.x < min_x) min_x = p.x;
            if (p.x > max_x) max_x = p.x;
            if (p.y < min_y) min_y = p.y;
            if (p.y > max_y) max_y = p.y;
        }
    }
    if (!any) return;

    float span = (max_x - min_x) > (max_y - min_y) ? (max_x - min_x) : (max_y - min_y);
    if (span < 1e-6f) return;

    float scale = (size - 16.0f) / span;
    float cx = 0.5f * (min_x + max_x);
    float cy = 0.5f * (min_y + max_y);
    ImVec2 middle = ImVec2(origin.x + size * 0.5f, origin.y + size * 0.5f);

    /* Projected once, then used for both the fill and the outline, so the two
     * cannot drift apart by a rounding difference. */
    std::vector<std::vector<ImVec2> > polys;
    polys.reserve(contours.size());
    for (size_t c = 0; c < contours.size(); ++c) {
        if (contours[c].size() < 3) continue;
        std::vector<ImVec2> poly;
        poly.reserve(contours[c].size());
        for (size_t i = 0; i < contours[c].size(); ++i) {
            /* Y up in millimetres, y down on screen. */
            poly.push_back(ImVec2(middle.x + (contours[c][i].x - cx) * scale,
                                  middle.y - (contours[c][i].y - cy) * scale));
        }
        polys.push_back(poly);
    }
    if (polys.empty()) return;

    dl->PushClipRect(lo, hi, true);
    fill_scanline(dl, polys, lo, hi, IM_COL32(126, 160, 196, 255));

    for (size_t p = 0; p < polys.size(); ++p) {
        for (size_t i = 0; i < polys[p].size(); ++i) dl->PathLineTo(polys[p][i]);
        dl->PathStroke(IM_COL32(40, 45, 55, 255), ImDrawFlags_Closed, 1.0f);
    }
    dl->PopClipRect();
}

/*
 * Solid preview.
 *
 * A fixed three-quarter view, orthographic, with the triangles sorted back to
 * front and shaded on the CPU - the same two-sided shading the 3D view uses, so
 * a preview and the placed object read the same way. ImGui has no depth buffer,
 * which is why the sort is doing the work a renderer would normally do.
 */
void ui_draw_mesh_preview(const Mesh &mesh, float size) {
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 lo = origin;
    ImVec2 hi = ImVec2(origin.x + size, origin.y + size);
    dl->AddRectFilled(lo, hi, IM_COL32(250, 250, 252, 255));
    dl->AddRect(lo, hi, IM_COL32(170, 175, 185, 255));
    if (mesh.vertices.size() < 3) return;

    /* Matches the camera's home angles closely enough that the preview and the
     * first look at the object in the view agree. */
    Vec3 forward = vec3_normalized(vec3(0.52f, 0.74f, -0.42f));
    Vec3 right = vec3_normalized(vec3_cross(forward, vec3(0.0f, 0.0f, 1.0f)));
    Vec3 up = vec3_cross(right, forward);
    Vec3 light = vec3_normalized(vec3(-0.35f, -0.45f, 0.82f));

    Bounds b = mesh_bounds(mesh);
    Vec3 centre = bounds_center(b);
    Vec3 span = bounds_size(b);
    float widest = span.x > span.y ? span.x : span.y;
    if (span.z > widest) widest = span.z;
    if (widest < 1e-6f) return;

    float scale = (size - 20.0f) / widest;
    ImVec2 middle = ImVec2(origin.x + size * 0.5f, origin.y + size * 0.5f);

    struct Face { float depth; ImVec2 p[3]; float shade; };
    std::vector<Face> faces;
    faces.reserve(mesh.vertices.size() / 3);

    for (size_t t = 0; t + 2 < mesh.vertices.size(); t += 3) {
        Face f;
        Vec3 sum = vec3(0.0f, 0.0f, 0.0f);
        for (int i = 0; i < 3; ++i) {
            Vec3 v = vec3_sub(mesh.vertices[t + i], centre);
            sum = vec3_add(sum, v);
            f.p[i] = ImVec2(middle.x + vec3_dot(v, right) * scale,
                            middle.y - vec3_dot(v, up) * scale);
        }
        f.depth = vec3_dot(vec3_mul(sum, 1.0f / 3.0f), forward);

        Vec3 n = (t < mesh.normals.size()) ? mesh.normals[t]
               : vec3_normalized(vec3_cross(vec3_sub(mesh.vertices[t + 1], mesh.vertices[t]),
                                            vec3_sub(mesh.vertices[t + 2], mesh.vertices[t])));
        f.shade = 0.42f + 0.58f * fabsf(vec3_dot(n, light));
        faces.push_back(f);
    }

    /* Back to front: the far triangles are painted over by the near ones. */
    std::sort(faces.begin(), faces.end(),
              [](const Face &a, const Face &c) { return a.depth < c.depth; });

    dl->PushClipRect(lo, hi, true);
    for (size_t i = 0; i < faces.size(); ++i) {
        float s = faces[i].shade;
        ImU32 col = IM_COL32((int)(126.0f * s), (int)(160.0f * s), (int)(196.0f * s), 255);
        dl->AddTriangleFilled(faces[i].p[0], faces[i].p[1], faces[i].p[2], col);
    }

    /* Feature edges on top, which is what makes the facets readable. */
    for (size_t i = 0; i + 1 < mesh.edges.size(); i += 2) {
        Vec3 a = vec3_sub(mesh.edges[i], centre);
        Vec3 c = vec3_sub(mesh.edges[i + 1], centre);
        dl->AddLine(ImVec2(middle.x + vec3_dot(a, right) * scale,
                           middle.y - vec3_dot(a, up) * scale),
                    ImVec2(middle.x + vec3_dot(c, right) * scale,
                           middle.y - vec3_dot(c, up) * scale),
                    IM_COL32(40, 45, 55, 160), 1.0f);
    }
    dl->PopClipRect();
}
