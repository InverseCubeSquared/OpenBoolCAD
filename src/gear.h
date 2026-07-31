#ifndef OBC_GEAR_H
#define OBC_GEAR_H

#include <string>
#include <vector>

#include "mesh.h"

/*
 * Involute spur gear generator.
 *
 * Sized the metric way, by module: pitch diameter is module x teeth, so two
 * gears mesh when they share a module and a pressure angle - which is why the
 * dialog leads with those two rather than with a diameter. Tooth proportions
 * are the ISO standard ones, addendum 1 module and dedendum 1.25.
 *
 * The profile is a closed 2D contour handed to csg_extrude, so the result is a
 * closed solid or a reported error, and a bore is just a second contour that
 * nests inside the first.
 */
struct GearParams {
    int teeth;
    float module_mm;
    float pressure_angle_deg;
    float thickness_mm;
    float bore_mm;   // 0 for none
    bool negative;   // cut a gear shaped pocket instead of adding a solid
};

void gear_params_init(GearParams *p);

/* Forces every field into a range that can produce a real gear, including a
 * bore that still leaves a rim around it. */
void gear_params_clamp(GearParams *p);

/* Derived sizes, so the dialog can show what the numbers add up to. */
float gear_pitch_diameter(const GearParams &p);
float gear_outside_diameter(const GearParams &p);
float gear_root_diameter(const GearParams &p);

/*
 * Builds the solid, centred on the origin and standing on the workplane like
 * every other primitive. Clamps its own copy of the parameters, so a caller
 * cannot get an invalid gear by skipping gear_params_clamp.
 */
bool gear_build(const GearParams &params, Mesh *out, std::string *error);

/*
 * The 2D contours the solid is extruded from: the tooth profile first, then the
 * bore if there is one. Exposed so the dialog can draw exactly the shape that
 * Create would build, rather than an artist's impression of a gear.
 */
bool gear_contours(const GearParams &params, std::vector<std::vector<Vec2> > *out,
                   std::string *error);

#endif
