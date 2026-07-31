#include "gear.h"

#include <math.h>

#include "csg.h"

#define GEAR_PI 3.14159265358979f

/* Points along one involute flank, across a tooth tip and along a root gap.
 * Enough that the flank reads as a curve at print scale without turning the
 * contour into thousands of points. */
#define FLANK_STEPS 10
#define TIP_STEPS 4
#define ROOT_STEPS 4
#define BORE_SEGMENTS 64

void gear_params_init(GearParams *p) {
    p->teeth = 16;
    p->module_mm = 1.5f;
    p->pressure_angle_deg = 20.0f;
    p->thickness_mm = 6.0f;
    p->bore_mm = 5.0f;
    p->negative = false;
}

float gear_pitch_diameter(const GearParams &p) {
    return p.module_mm * (float)p.teeth;
}

float gear_outside_diameter(const GearParams &p) {
    return gear_pitch_diameter(p) + 2.0f * p.module_mm;
}

float gear_root_diameter(const GearParams &p) {
    return gear_pitch_diameter(p) - 2.5f * p.module_mm;
}

void gear_params_clamp(GearParams *p) {
    /* Below four teeth the root circle reaches the centre and there is no gear
     * left to speak of. */
    if (p->teeth < 4) p->teeth = 4;
    if (p->teeth > 400) p->teeth = 400;
    if (p->module_mm < 0.05f) p->module_mm = 0.05f;
    if (p->module_mm > 50.0f) p->module_mm = 50.0f;
    if (p->pressure_angle_deg < 10.0f) p->pressure_angle_deg = 10.0f;
    if (p->pressure_angle_deg > 35.0f) p->pressure_angle_deg = 35.0f;
    if (p->thickness_mm < 0.1f) p->thickness_mm = 0.1f;
    if (p->thickness_mm > 1000.0f) p->thickness_mm = 1000.0f;

    if (!(p->bore_mm > 0.0f)) p->bore_mm = 0.0f; // also catches NaN from the field
    /* A bore at or past the root circle would cut the teeth off their own
     * body, so it has to leave a rim. */
    float max_bore = gear_root_diameter(*p) * 0.9f;
    if (p->bore_mm > max_bore) p->bore_mm = max_bore;
}

/* Profile */

static float involute(float a) {
    return tanf(a) - a;
}

/*
 * Half the angular width of a tooth at radius r.
 *
 * At the pitch circle this is pi/(2*teeth), which is the standard tooth
 * thickness of half the circular pitch. Away from it the involute's own polar
 * angle takes over, which is what makes the flank a proper involute rather
 * than a radial wedge.
 */
static float tooth_half_angle(float r, float rb, float base_inv, int teeth) {
    float half = GEAR_PI / (2.0f * (float)teeth) + base_inv;
    if (r <= rb) return half; // the involute starts at the base circle
    return half - involute(acosf(rb / r));
}

/*
 * Radius at which the two flanks of a tooth meet.
 *
 * Past it they would cross and the contour would self intersect, which happens
 * on few teeth at a high pressure angle. Capping the tip there gives a pointed
 * tooth, which is what a real cutter would leave.
 */
static float pointed_radius(float rb, float base_inv, int teeth, float ra, float r_start) {
    if (tooth_half_angle(ra, rb, base_inv, teeth) > 0.0f) return ra;

    float lo = r_start;
    float hi = ra;
    for (int i = 0; i < 40; ++i) {
        float mid = 0.5f * (lo + hi);
        if (tooth_half_angle(mid, rb, base_inv, teeth) > 0.0f) lo = mid;
        else hi = mid;
    }
    return lo;
}

/*
 * Appends a polar point, dropping it when it lands on the one before.
 *
 * The profile is built from pieces that deliberately share their endpoints -
 * root to flank, flank to tip arc - so filtering here once is simpler than
 * making every piece agree about which end it owns, and it keeps degenerate
 * triangles out of the triangulator.
 */
static void push_polar(std::vector<Vec2> *out, float r, float a) {
    Vec2 p = vec2(r * cosf(a), r * sinf(a));
    if (!out->empty()) {
        Vec2 last = out->back();
        float dx = p.x - last.x;
        float dy = p.y - last.y;
        if (dx * dx + dy * dy < 1e-10f) return;
    }
    out->push_back(p);
}

static bool gear_profile(const GearParams &p, std::vector<Vec2> *out, std::string *error) {
    int z = p.teeth;
    float m = p.module_mm;
    float alpha = p.pressure_angle_deg * GEAR_PI / 180.0f;

    float rp = 0.5f * m * (float)z;
    float rb = rp * cosf(alpha);
    float ra = rp + m;
    float rf = rp - 1.25f * m;
    if (rf < 0.01f) rf = 0.01f;

    float base_inv = involute(alpha);
    float r_start = (rf > rb) ? rf : rb;
    float base_half = tooth_half_angle(r_start, rb, base_inv, z);
    float pitch_step = 2.0f * GEAR_PI / (float)z;

    /* Teeth wider at the root than the gap between them would overlap their
     * neighbours. The clamped parameter ranges do not reach it, so this is a
     * guard rather than a case to handle. */
    if (base_half >= 0.5f * pitch_step) {
        if (error) *error = "Those proportions make the teeth overlap at the root.";
        return false;
    }

    float ra_eff = pointed_radius(rb, base_inv, z, ra, r_start);
    float tip_half = tooth_half_angle(ra_eff, rb, base_inv, z);
    if (tip_half < 0.0f) tip_half = 0.0f;

    for (int i = 0; i < z; ++i) {
        float c = pitch_step * (float)i;

        /* Below the base circle there is no involute. A straight radial drop
         * to the root stands in for the trochoidal fillet a cut gear would
         * have: slightly thinner at the root than the real thing, which is the
         * safe direction for a printed part. Skipped by push_polar when the
         * root is already at or above the base circle. */
        push_polar(out, rf, c - base_half);

        for (int k = 0; k <= FLANK_STEPS; ++k) {
            float r = r_start + (ra_eff - r_start) * (float)k / (float)FLANK_STEPS;
            push_polar(out, r, c - tooth_half_angle(r, rb, base_inv, z));
        }
        for (int k = 1; k < TIP_STEPS; ++k) {
            float t = (float)k / (float)TIP_STEPS;
            push_polar(out, ra_eff, c - tip_half + 2.0f * tip_half * t);
        }
        for (int k = FLANK_STEPS; k >= 0; --k) {
            float r = r_start + (ra_eff - r_start) * (float)k / (float)FLANK_STEPS;
            push_polar(out, r, c + tooth_half_angle(r, rb, base_inv, z));
        }
        push_polar(out, rf, c + base_half);

        /* Root gap across to the next tooth; both ends are already in. */
        float a0 = c + base_half;
        float a1 = c + pitch_step - base_half;
        for (int k = 1; k < ROOT_STEPS; ++k) {
            float t = (float)k / (float)ROOT_STEPS;
            push_polar(out, rf, a0 + (a1 - a0) * t);
        }
    }

    /* The contour closes implicitly, so a last point sitting on the first one
     * would be a duplicate that push_polar cannot see coming. */
    if (out->size() > 1) {
        Vec2 first = out->front();
        Vec2 last = out->back();
        float dx = last.x - first.x;
        float dy = last.y - first.y;
        if (dx * dx + dy * dy < 1e-10f) out->pop_back();
    }

    if (out->size() < 3) {
        if (error) *error = "Gear profile came out empty.";
        return false;
    }
    return true;
}

/* Build */

bool gear_contours(const GearParams &params, std::vector<std::vector<Vec2> > *out,
                   std::string *error) {
    GearParams p = params;
    gear_params_clamp(&p);

    out->clear();
    out->push_back(std::vector<Vec2>());
    if (!gear_profile(p, &out->back(), error)) {
        out->clear();
        return false;
    }

    if (p.bore_mm > 0.01f) {
        std::vector<Vec2> bore;
        float r = 0.5f * p.bore_mm;
        bore.reserve(BORE_SEGMENTS);
        for (int i = 0; i < BORE_SEGMENTS; ++i) {
            float a = 2.0f * GEAR_PI * (float)i / (float)BORE_SEGMENTS;
            bore.push_back(vec2(r * cosf(a), r * sinf(a)));
        }
        /* csg_extrude decides island versus hole by nesting, so the bore does
         * not have to be wound any particular way here. */
        out->push_back(bore);
    }
    return true;
}

bool gear_build(const GearParams &params, Mesh *out, std::string *error) {
    GearParams p = params;
    gear_params_clamp(&p);

    std::vector<std::vector<Vec2> > contours;
    if (!gear_contours(p, &contours, error)) return false;

    return csg_extrude(contours, p.thickness_mm, out, error);
}
