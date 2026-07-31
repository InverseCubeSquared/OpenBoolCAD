#ifndef OBC_IMPORT_SVG_H
#define OBC_IMPORT_SVG_H

#include <string>
#include <vector>

#include "mesh.h"

/*
 * SVG import, in the two modes the import dialog offers.
 *
 * OUTLINE extrudes every shape to the same height, which is what you want for
 * a logo, a stencil or a flat part.
 *
 * HEIGHTMAP reads each shape's fill greyscale and maps it to a height between
 * "base" and "height": black is tallest and white is flattest, matching how a
 * relief or lithophane is normally authored. It is per shape rather than per
 * pixel because an SVG is vector artwork - there are no pixels to sample until
 * something rasterises it, and rasterising would throw away the clean outlines
 * that make the result printable.
 */
enum SvgImportMode {
    SVG_IMPORT_OUTLINE = 0,
    SVG_IMPORT_HEIGHTMAP
};

struct SvgImportOptions {
    SvgImportMode mode;
    float height_mm;  // outline: the extrusion; heightmap: the tallest shape
    float base_mm;    // heightmap only: height of a white shape
    bool invert;      // heightmap only: make white tallest instead of black
};

void svg_import_options_init(SvgImportOptions *o);

/*
 * One mesh per resulting solid, with a name for each. Outline mode returns a
 * single mesh; height map mode returns one per distinct height, so each band
 * stays separately editable.
 */
struct SvgImportResult {
    std::vector<Mesh> meshes;
    std::vector<std::string> names;
    float width_mm;
    float height_mm;
};

bool import_svg(const char *path, const SvgImportOptions &options,
                SvgImportResult *out, std::string *error, std::string *note);

#endif
