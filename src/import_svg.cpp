#include "import_svg.h"

#include <stdio.h>

#include "csg.h"
#include "svg.h"

void svg_import_options_init(SvgImportOptions *o) {
    o->mode = SVG_IMPORT_OUTLINE;
    o->height_mm = 10.0f; // the plan's default for an extruded outline
    o->base_mm = 1.0f;
    o->invert = false;
}

/* Shapes at the same height are extruded together, so a height map comes out as
 * one solid per band rather than one per path. */
struct HeightBand {
    float height;
    std::vector<std::vector<Vec2> > contours;
};

#define HEIGHT_EPSILON 0.01f

static void add_to_band(std::vector<HeightBand> *bands, float height,
                        const std::vector<std::vector<Vec2> > &contours) {
    for (size_t i = 0; i < bands->size(); ++i) {
        if (fabsf((*bands)[i].height - height) < HEIGHT_EPSILON) {
            for (size_t k = 0; k < contours.size(); ++k) {
                (*bands)[i].contours.push_back(contours[k]);
            }
            return;
        }
    }

    HeightBand band;
    band.height = height;
    band.contours = contours;
    bands->push_back(band);
}

bool import_svg(const char *path, const SvgImportOptions &options,
                SvgImportResult *out, std::string *error, std::string *note) {
    SvgDocument doc;
    if (!svg_parse_file(path, &doc, error)) return false;

    out->meshes.clear();
    out->names.clear();
    out->width_mm = doc.width_mm;
    out->height_mm = doc.height_mm;

    int skipped = 0;

    if (options.mode == SVG_IMPORT_OUTLINE) {
        /* Every contour in the file forms one solid, so overlapping shapes and
         * their holes resolve against each other exactly once. */
        std::vector<std::vector<Vec2> > all;
        for (size_t s = 0; s < doc.shapes.size(); ++s) {
            for (size_t i = 0; i < doc.shapes[s].contours.size(); ++i) {
                all.push_back(doc.shapes[s].contours[i]);
            }
        }

        Mesh mesh;
        if (!csg_extrude(all, options.height_mm, &mesh, error)) return false;
        out->meshes.push_back(mesh);
        out->names.push_back("SVG Outline");
    } else {
        float span = options.height_mm - options.base_mm;
        std::vector<HeightBand> bands;

        for (size_t s = 0; s < doc.shapes.size(); ++s) {
            const SvgShape &shape = doc.shapes[s];
            /* Black tallest by default: darker ink reads as more material. */
            float t = options.invert ? shape.grey : (1.0f - shape.grey);
            float height = options.base_mm + span * t;
            if (height < HEIGHT_EPSILON) height = HEIGHT_EPSILON;
            add_to_band(&bands, height, shape.contours);
        }

        for (size_t i = 0; i < bands.size(); ++i) {
            Mesh mesh;
            std::string band_error;
            if (!csg_extrude(bands[i].contours, bands[i].height, &mesh, &band_error)) {
                /* One bad band should not lose the rest of the artwork. */
                skipped += 1;
                continue;
            }
            char name[64];
            snprintf(name, sizeof(name), "SVG %.2f mm", bands[i].height);
            out->meshes.push_back(mesh);
            out->names.push_back(name);
        }

        if (out->meshes.empty()) {
            if (error) *error = "No SVG shape could be extruded.";
            return false;
        }
    }

    if (note) {
        char text[160];
        if (skipped > 0) {
            snprintf(text, sizeof(text), "%d solids, %d shapes skipped, %.1f x %.1f mm",
                     (int)out->meshes.size(), skipped, out->width_mm, out->height_mm);
        } else {
            snprintf(text, sizeof(text), "%d solids, %.1f x %.1f mm",
                     (int)out->meshes.size(), out->width_mm, out->height_mm);
        }
        *note = text;
    }
    return true;
}
