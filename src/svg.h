#ifndef OBC_SVG_H
#define OBC_SVG_H

#include <string>
#include <vector>

#include "math3d.h"

/*
 * SVG reader, deliberately a subset: enough of the format to get artwork into
 * the editor as closed outlines, not a renderer.
 *
 * Handled: <path> (M L H V C S Q T A Z, absolute and relative), <rect> with
 * corner radii, <circle>, <ellipse>, <polygon>, <polyline>, <line>, nested <g>,
 * and transform lists of translate / scale / rotate / matrix / skewX / skewY.
 * Curves are flattened to line segments; strokes are ignored, since only filled
 * area can become a volume.
 *
 * Not handled: text, gradients, clipping, patterns, use/defs references, CSS
 * stylesheets. Those need a full SVG engine and none of them change the outline
 * of a shape that is already a path.
 *
 * Output is in millimeters on the workplane, Y flipped (SVG grows downward) and
 * centred on the origin, so an import lands where the user is looking.
 */

struct SvgShape {
    /* Subpaths of one element. Nesting decides which are holes, which is
     * resolved during extrusion rather than here. */
    std::vector<std::vector<Vec2> > contours;
    float grey; // fill luminance, 0 black to 1 white; 1 when no fill is given
};

struct SvgDocument {
    std::vector<SvgShape> shapes;
    float width_mm;
    float height_mm;
};

bool svg_parse_file(const char *path, SvgDocument *out, std::string *error);

#endif
