#ifndef OBC_TEXT3D_H
#define OBC_TEXT3D_H

#include <string>
#include <vector>

#include "mesh.h"

/*
 * Extruded text.
 *
 * Glyph outlines come from the embedded fonts through stb_truetype, which ImGui
 * already vendors - so no new dependency, and the same bytes serve the UI font.
 * Quadratic and cubic segments are flattened to line segments and handed to
 * csg_extrude as contours, which means the result is a closed solid or a
 * reported error, and counters (the hole in an "o") fall out of the nesting
 * rule csg_extrude already applies.
 */

#define OBC_TEXT_MAX 128

enum TextFontFamily {
    TEXT_FONT_SANS = 0,
    TEXT_FONT_SERIF,
    TEXT_FONT_MONO,
    TEXT_FONT_JUPITEROID,
    TEXT_FONT_UNITBLOCK,
    TEXT_FONT_ZEROVE,
    TEXT_FONT_TYPELIGHT,
    TEXT_FONT_PIXEL,
    TEXT_FONT_COUNT
};

struct TextParams {
    char text[OBC_TEXT_MAX];
    int family;         // TextFontFamily
    bool bold;
    bool italic;
    float height_mm;    // cap height of the line, not the em box
    float depth_mm;
    float spacing;      // extra tracking, in fractions of the em
    bool negative;
};

void text_params_init(TextParams *p);
void text_params_clamp(TextParams *p);

const char *text_font_name(int family);
/* Whether the family ships a real bold face. The rest synthesise nothing, so
 * the dialog can grey the box out rather than lie about it. */
bool text_font_has_bold(int family);

/*
 * Outlines for the current text, in millimetres, centred on the origin. Used
 * both for the preview and as the input to the extrusion, so the dialog cannot
 * show something other than what Create builds.
 */
bool text_contours(const TextParams &params, std::vector<std::vector<Vec2> > *out,
                   std::string *error);

bool text_build(const TextParams &params, Mesh *out, std::string *error);

#endif
