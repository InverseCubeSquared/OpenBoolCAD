#include "text3d.h"

#include <math.h>
#include <string.h>

#include "csg.h"
#include "fonts.h"

/*
 * stb_truetype comes in with ImGui, but imgui_draw.cpp compiles it STBTT_STATIC
 * so none of it is linkable from here. Building our own static copy is the way
 * that leaves the vendored files untouched, and the duplicate is only ever
 * reached through this file.
 *
 * The vendored header is not ours to fix, so its warnings are silenced rather
 * than the project's warning set being loosened for everything in src/.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#pragma GCC diagnostic pop

/* Curves are flattened to this many segments. Enough that a 20 mm letter reads
 * as smooth without burying the triangulator in points. */
#define CURVE_STEPS 8

/* Slant used for a synthetic italic, as a fraction of height. Real oblique
 * faces sit near 12 degrees; this is tanf of about that. */
#define ITALIC_SHEAR 0.21f

void text_params_init(TextParams *p) {
    memset(p, 0, sizeof(*p));
    snprintf(p->text, sizeof(p->text), "%s", "Text");
    p->family = TEXT_FONT_SANS;
    p->bold = false;
    p->italic = false;
    p->height_mm = 10.0f;
    p->depth_mm = 4.0f;
    p->spacing = 0.0f;
    p->negative = false;
}

void text_params_clamp(TextParams *p) {
    if (p->family < 0 || p->family >= TEXT_FONT_COUNT) p->family = TEXT_FONT_SANS;
    if (p->height_mm < 0.5f) p->height_mm = 0.5f;
    if (p->height_mm > 500.0f) p->height_mm = 500.0f;
    if (p->depth_mm < 0.1f) p->depth_mm = 0.1f;
    if (p->depth_mm > 500.0f) p->depth_mm = 500.0f;
    if (p->spacing < -0.3f) p->spacing = -0.3f;
    if (p->spacing > 2.0f) p->spacing = 2.0f;
    if (!text_font_has_bold(p->family)) p->bold = false;
    p->text[OBC_TEXT_MAX - 1] = 0;
}

const char *text_font_name(int family) {
    switch (family) {
    case TEXT_FONT_SERIF:      return "Serif";
    case TEXT_FONT_MONO:       return "Monospace";
    case TEXT_FONT_JUPITEROID: return "Jupiteroid";
    case TEXT_FONT_UNITBLOCK:  return "Unitblock";
    case TEXT_FONT_ZEROVE:     return "Zerove";
    case TEXT_FONT_TYPELIGHT:  return "TypeLight";
    case TEXT_FONT_PIXEL:      return "Pixelated";
    default:                   return "Sans";
    }
}

bool text_font_has_bold(int family) {
    return family == TEXT_FONT_SANS || family == TEXT_FONT_SERIF || family == TEXT_FONT_MONO;
}

/* Which embedded file backs a family and weight. */
static const char *font_file(int family, bool bold) {
    switch (family) {
    case TEXT_FONT_SERIF:      return bold ? "LiberationSerif-Bold" : "LiberationSerif-Regular";
    case TEXT_FONT_MONO:       return bold ? "LiberationMono-Bold" : "LiberationMono-Regular";
    case TEXT_FONT_JUPITEROID: return "Jupiteroid";
    case TEXT_FONT_UNITBLOCK:  return "Unitblock";
    case TEXT_FONT_ZEROVE:     return "Zerove";
    case TEXT_FONT_TYPELIGHT:  return "TypeLight";
    case TEXT_FONT_PIXEL:      return "PixelatedElegance";
    default:                   return bold ? "LiberationSans-Bold" : "LiberationSans-Regular";
    }
}

/* Contour building */

static void push_point(std::vector<Vec2> *contour, float x, float y) {
    if (!contour->empty()) {
        Vec2 last = contour->back();
        float dx = x - last.x;
        float dy = y - last.y;
        /* Font units are integers, so coincident points are common where a
         * curve meets a line; a degenerate edge upsets the triangulator. */
        if (dx * dx + dy * dy < 1e-12f) return;
    }
    contour->push_back(vec2(x, y));
}

static void flatten_quadratic(std::vector<Vec2> *contour, float x0, float y0,
                              float cx, float cy, float x1, float y1) {
    for (int i = 1; i <= CURVE_STEPS; ++i) {
        float t = (float)i / (float)CURVE_STEPS;
        float u = 1.0f - t;
        push_point(contour, u * u * x0 + 2.0f * u * t * cx + t * t * x1,
                            u * u * y0 + 2.0f * u * t * cy + t * t * y1);
    }
}

static void flatten_cubic(std::vector<Vec2> *contour, float x0, float y0,
                          float c0x, float c0y, float c1x, float c1y,
                          float x1, float y1) {
    for (int i = 1; i <= CURVE_STEPS; ++i) {
        float t = (float)i / (float)CURVE_STEPS;
        float u = 1.0f - t;
        float a = u * u * u;
        float b = 3.0f * u * u * t;
        float c = 3.0f * u * t * t;
        float d = t * t * t;
        push_point(contour, a * x0 + b * c0x + c * c1x + d * x1,
                            a * y0 + b * c0y + c * c1y + d * y1);
    }
}

bool text_contours(const TextParams &params, std::vector<std::vector<Vec2> > *out,
                   std::string *error) {
    TextParams p = params;
    text_params_clamp(&p);
    out->clear();

    if (p.text[0] == 0) {
        if (error) *error = "Nothing to build: type some text.";
        return false;
    }

    const EmbeddedFont *font = font_find(font_file(p.family, p.bold));
    if (!font) {
        if (error) *error = "That font is not built into this copy.";
        return false;
    }

    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, font->data, stbtt_GetFontOffsetForIndex(font->data, 0))) {
        if (error) *error = "The font could not be read.";
        return false;
    }

    /*
     * Sized on the ascent rather than the em box, so "height" means the height
     * of a capital and two families at the same setting look the same size.
     */
    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
    if (ascent <= 0) {
        if (error) *error = "The font has no usable metrics.";
        return false;
    }
    float scale = p.height_mm / (float)ascent;

    int em = 0, dummy = 0;
    stbtt_GetCodepointHMetrics(&info, 'M', &em, &dummy);
    float tracking = p.spacing * (float)em * scale;

    float pen = 0.0f;
    for (const char *c = p.text; *c; ++c) {
        int codepoint = (unsigned char)*c;

        int advance = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(&info, codepoint, &advance, &bearing);

        stbtt_vertex *verts = NULL;
        int count = stbtt_GetCodepointShape(&info, codepoint, &verts);

        /* Built in glyph space first, where the control points live, then moved
         * as a block. A space has no outline at all and only advances the pen. */
        std::vector<std::vector<Vec2> > glyph;
        std::vector<Vec2> contour;
        float x = 0.0f, y = 0.0f;

        for (int i = 0; i < count; ++i) {
            float vx = (float)verts[i].x * scale;
            float vy = (float)verts[i].y * scale;

            if (verts[i].type == STBTT_vmove) {
                if (contour.size() >= 3) glyph.push_back(contour);
                contour.clear();
                push_point(&contour, vx, vy);
            } else if (verts[i].type == STBTT_vline) {
                push_point(&contour, vx, vy);
            } else if (verts[i].type == STBTT_vcurve) {
                flatten_quadratic(&contour, x, y,
                                  (float)verts[i].cx * scale, (float)verts[i].cy * scale,
                                  vx, vy);
            } else if (verts[i].type == STBTT_vcubic) {
                flatten_cubic(&contour, x, y,
                              (float)verts[i].cx * scale, (float)verts[i].cy * scale,
                              (float)verts[i].cx1 * scale, (float)verts[i].cy1 * scale,
                              vx, vy);
            }
            x = vx;
            y = vy;
        }
        if (contour.size() >= 3) glyph.push_back(contour);
        if (verts) stbtt_FreeShape(&info, verts);

        for (size_t g = 0; g < glyph.size(); ++g) {
            for (size_t v = 0; v < glyph[g].size(); ++v) {
                Vec2 point = glyph[g][v];
                /* Synthetic oblique: lean the glyph by shearing x with height.
                 * None of these families ship an italic face, and a shear is
                 * what an oblique is. */
                if (p.italic) point.x += point.y * ITALIC_SHEAR;
                point.x += pen;
                glyph[g][v] = point;
            }
            out->push_back(glyph[g]);
        }

        pen += (float)advance * scale + tracking;
        (void)bearing;
    }

    if (out->empty()) {
        if (error) *error = "None of those characters have outlines in this font.";
        return false;
    }

    /* Centred on the origin, so text lands where the user is looking rather
     * than running off to one side of it. */
    float min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f;
    bool any = false;
    for (size_t g = 0; g < out->size(); ++g) {
        for (size_t v = 0; v < (*out)[g].size(); ++v) {
            Vec2 point = (*out)[g][v];
            if (!any) {
                min_x = max_x = point.x;
                min_y = max_y = point.y;
                any = true;
                continue;
            }
            if (point.x < min_x) min_x = point.x;
            if (point.x > max_x) max_x = point.x;
            if (point.y < min_y) min_y = point.y;
            if (point.y > max_y) max_y = point.y;
        }
    }
    float shift_x = -0.5f * (min_x + max_x);
    float shift_y = -0.5f * (min_y + max_y);
    for (size_t g = 0; g < out->size(); ++g) {
        for (size_t v = 0; v < (*out)[g].size(); ++v) {
            (*out)[g][v].x += shift_x;
            (*out)[g][v].y += shift_y;
        }
    }
    return true;
}

bool text_build(const TextParams &params, Mesh *out, std::string *error) {
    TextParams p = params;
    text_params_clamp(&p);

    std::vector<std::vector<Vec2> > contours;
    if (!text_contours(p, &contours, error)) return false;

    return csg_extrude(contours, p.depth_mm, out, error);
}
