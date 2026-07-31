#include "svg.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 2D affine transform, row major:  x' = a*x + c*y + e,  y' = b*x + d*y + f.
 * Same element order as the SVG matrix() function, so parsing is a direct copy.
 */
struct Xform {
    float a, b, c, d, e, f;
};

static Xform xform_identity(void) {
    Xform t;
    t.a = 1.0f; t.b = 0.0f; t.c = 0.0f;
    t.d = 1.0f; t.e = 0.0f; t.f = 0.0f;
    return t;
}

/* Applies "second" after "first", i.e. the SVG nesting order. */
static Xform xform_mul(Xform first, Xform second) {
    Xform r;
    r.a = first.a * second.a + first.b * second.c;
    r.b = first.a * second.b + first.b * second.d;
    r.c = first.c * second.a + first.d * second.c;
    r.d = first.c * second.b + first.d * second.d;
    r.e = first.e * second.a + first.f * second.c + second.e;
    r.f = first.e * second.b + first.f * second.d + second.f;
    return r;
}

static Vec2 xform_apply(Xform t, Vec2 p) {
    return vec2(t.a * p.x + t.c * p.y + t.e, t.b * p.x + t.d * p.y + t.f);
}

/* Number and attribute scanning */

static void skip_separators(const char **p) {
    while (**p && (isspace((unsigned char)**p) || **p == ',')) (*p)++;
}

static bool read_number(const char **p, float *out) {
    skip_separators(p);
    char *end = NULL;
    float v = strtof(*p, &end);
    if (end == *p) return false;
    *p = end;
    *out = v;
    return true;
}

/* Value of attr="..." inside one element's text. */
static bool find_attribute(const std::string &element, const char *name, std::string *out) {
    std::string needle = std::string(name);
    size_t pos = 0;
    while (true) {
        pos = element.find(needle, pos);
        if (pos == std::string::npos) return false;

        /* Must be preceded by whitespace and followed by = so that "x" does not
         * match inside "rx". */
        bool left_ok = (pos > 0) && (isspace((unsigned char)element[pos - 1]) != 0);
        size_t after = pos + needle.size();
        while (after < element.size() && isspace((unsigned char)element[after])) after++;
        bool right_ok = (after < element.size()) && element[after] == '=';
        if (!left_ok || !right_ok) {
            pos += needle.size();
            continue;
        }

        size_t quote = element.find_first_of("\"'", after);
        if (quote == std::string::npos) return false;
        char q = element[quote];
        size_t end = element.find(q, quote + 1);
        if (end == std::string::npos) return false;

        *out = element.substr(quote + 1, end - quote - 1);
        return true;
    }
}

static float attribute_number(const std::string &element, const char *name, float fallback) {
    std::string value;
    if (!find_attribute(element, name, &value)) return fallback;
    const char *p = value.c_str();
    float v;
    return read_number(&p, &v) ? v : fallback;
}

/* Lengths and colours */

/* SVG user units are CSS pixels: 96 per inch. */
#define MM_PER_PX (25.4f / 96.0f)

static float length_to_mm(const std::string &text, float fallback) {
    const char *p = text.c_str();
    float v;
    if (!read_number(&p, &v)) return fallback;

    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "mm", 2) == 0) return v;
    if (strncmp(p, "cm", 2) == 0) return v * 10.0f;
    if (strncmp(p, "in", 2) == 0) return v * 25.4f;
    if (strncmp(p, "pt", 2) == 0) return v * 25.4f / 72.0f;
    if (strncmp(p, "pc", 2) == 0) return v * 25.4f / 6.0f;
    if (strncmp(p, "%", 1) == 0) return fallback;
    return v * MM_PER_PX; // px or unitless
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Luminance of the fill, which the height map mode maps to a height. */
static bool parse_fill_grey(const std::string &element, float *grey) {
    std::string fill;
    if (!find_attribute(element, "fill", &fill)) {
        /* style="fill:#abc" is just as common as a fill attribute. */
        std::string style;
        if (!find_attribute(element, "style", &style)) return false;
        size_t at = style.find("fill:");
        if (at == std::string::npos) return false;
        fill = style.substr(at + 5);
        size_t end = fill.find(';');
        if (end != std::string::npos) fill = fill.substr(0, end);
    }

    size_t start = fill.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return false;
    fill = fill.substr(start);

    if (fill.compare(0, 4, "none") == 0) return false;

    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (fill[0] == '#') {
        if (fill.size() >= 7) {
            r = (float)(hex_value(fill[1]) * 16 + hex_value(fill[2])) / 255.0f;
            g = (float)(hex_value(fill[3]) * 16 + hex_value(fill[4])) / 255.0f;
            b = (float)(hex_value(fill[5]) * 16 + hex_value(fill[6])) / 255.0f;
        } else if (fill.size() >= 4) {
            r = (float)(hex_value(fill[1]) * 17) / 255.0f;
            g = (float)(hex_value(fill[2]) * 17) / 255.0f;
            b = (float)(hex_value(fill[3]) * 17) / 255.0f;
        } else {
            return false;
        }
    } else if (fill.compare(0, 4, "rgb(") == 0) {
        const char *p = fill.c_str() + 4;
        float v[3] = { 0.0f, 0.0f, 0.0f };
        for (int i = 0; i < 3; ++i) {
            if (!read_number(&p, &v[i])) return false;
            skip_separators(&p);
        }
        r = v[0] / 255.0f;
        g = v[1] / 255.0f;
        b = v[2] / 255.0f;
    } else if (fill.compare(0, 5, "black") == 0) {
        r = g = b = 0.0f;
    } else if (fill.compare(0, 5, "white") == 0) {
        r = g = b = 1.0f;
    } else {
        return false;
    }

    /* Rec. 601 luma: matches how a greyscale conversion is normally seen. */
    *grey = 0.299f * r + 0.587f * g + 0.114f * b;
    return true;
}

static Xform parse_transform(const std::string &element) {
    Xform result = xform_identity();

    std::string text;
    if (!find_attribute(element, "transform", &text)) return result;

    const char *p = text.c_str();
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        const char *name_start = p;
        while (*p && (isalpha((unsigned char)*p))) p++;
        std::string name(name_start, (size_t)(p - name_start));
        if (name.empty()) break;

        while (*p && *p != '(') p++;
        if (*p != '(') break;
        p++;

        float v[6];
        int count = 0;
        while (count < 6 && read_number(&p, &v[count])) count++;
        while (*p && *p != ')') p++;
        if (*p == ')') p++;

        Xform t = xform_identity();
        if (name == "translate" && count >= 1) {
            t.e = v[0];
            t.f = (count >= 2) ? v[1] : 0.0f;
        } else if (name == "scale" && count >= 1) {
            t.a = v[0];
            t.d = (count >= 2) ? v[1] : v[0];
        } else if (name == "rotate" && count >= 1) {
            float rad = deg_to_rad(v[0]);
            Xform r = xform_identity();
            r.a = cosf(rad); r.b = sinf(rad);
            r.c = -sinf(rad); r.d = cosf(rad);
            if (count >= 3) {
                /* rotate(a cx cy) is translate(c) rotate(a) translate(-c). */
                Xform to = xform_identity();
                to.e = -v[1]; to.f = -v[2];
                Xform back = xform_identity();
                back.e = v[1]; back.f = v[2];
                t = xform_mul(to, xform_mul(r, back));
            } else {
                t = r;
            }
        } else if (name == "matrix" && count >= 6) {
            t.a = v[0]; t.b = v[1]; t.c = v[2];
            t.d = v[3]; t.e = v[4]; t.f = v[5];
        } else if (name == "skewX" && count >= 1) {
            t.c = tanf(deg_to_rad(v[0]));
        } else if (name == "skewY" && count >= 1) {
            t.b = tanf(deg_to_rad(v[0]));
        }

        result = xform_mul(t, result);
    }
    return result;
}

/* Curve flattening */

#define CURVE_STEPS 24

static void add_point(std::vector<Vec2> *contour, Vec2 p) {
    /* Repeated points make degenerate edges the triangulator has to discard. */
    if (!contour->empty()) {
        Vec2 last = contour->back();
        float dx = last.x - p.x;
        float dy = last.y - p.y;
        if (dx * dx + dy * dy < 1e-12f) return;
    }
    contour->push_back(p);
}

static void flatten_cubic(std::vector<Vec2> *c, Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3) {
    for (int i = 1; i <= CURVE_STEPS; ++i) {
        float t = (float)i / (float)CURVE_STEPS;
        float u = 1.0f - t;
        float x = u * u * u * p0.x + 3.0f * u * u * t * p1.x + 3.0f * u * t * t * p2.x + t * t * t * p3.x;
        float y = u * u * u * p0.y + 3.0f * u * u * t * p1.y + 3.0f * u * t * t * p2.y + t * t * t * p3.y;
        add_point(c, vec2(x, y));
    }
}

static void flatten_quadratic(std::vector<Vec2> *c, Vec2 p0, Vec2 p1, Vec2 p2) {
    for (int i = 1; i <= CURVE_STEPS; ++i) {
        float t = (float)i / (float)CURVE_STEPS;
        float u = 1.0f - t;
        float x = u * u * p0.x + 2.0f * u * t * p1.x + t * t * p2.x;
        float y = u * u * p0.y + 2.0f * u * t * p1.y + t * t * p2.y;
        add_point(c, vec2(x, y));
    }
}

/* Endpoint parameterisation from the SVG spec, appendix F.6. */
static void flatten_arc(std::vector<Vec2> *c, Vec2 from, float rx, float ry,
                        float rotation_deg, bool large_arc, bool sweep, Vec2 to) {
    if (rx == 0.0f || ry == 0.0f) {
        add_point(c, to);
        return;
    }
    rx = fabsf(rx);
    ry = fabsf(ry);

    float phi = deg_to_rad(rotation_deg);
    float cos_phi = cosf(phi);
    float sin_phi = sinf(phi);

    float dx2 = (from.x - to.x) * 0.5f;
    float dy2 = (from.y - to.y) * 0.5f;
    float x1 = cos_phi * dx2 + sin_phi * dy2;
    float y1 = -sin_phi * dx2 + cos_phi * dy2;

    /* Grow the radii if they cannot span the chord. */
    float lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1.0f) {
        float s = sqrtf(lambda);
        rx *= s;
        ry *= s;
    }

    float sign = (large_arc != sweep) ? 1.0f : -1.0f;
    float num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    float den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    float factor = (den <= 0.0f || num <= 0.0f) ? 0.0f : sign * sqrtf(num / den);

    float cx1 = factor * rx * y1 / ry;
    float cy1 = -factor * ry * x1 / rx;
    float cx = cos_phi * cx1 - sin_phi * cy1 + (from.x + to.x) * 0.5f;
    float cy = sin_phi * cx1 + cos_phi * cy1 + (from.y + to.y) * 0.5f;

    float start = atan2f((y1 - cy1) / ry, (x1 - cx1) / rx);
    float end = atan2f((-y1 - cy1) / ry, (-x1 - cx1) / rx);
    float sweep_angle = end - start;
    if (!sweep && sweep_angle > 0.0f) sweep_angle -= 2.0f * 3.14159265358979f;
    if (sweep && sweep_angle < 0.0f) sweep_angle += 2.0f * 3.14159265358979f;

    for (int i = 1; i <= CURVE_STEPS; ++i) {
        float t = start + sweep_angle * (float)i / (float)CURVE_STEPS;
        float px = cos_phi * rx * cosf(t) - sin_phi * ry * sinf(t) + cx;
        float py = sin_phi * rx * cosf(t) + cos_phi * ry * sinf(t) + cy;
        add_point(c, vec2(px, py));
    }
}

/* Path data */

static void parse_path_data(const std::string &data, std::vector<std::vector<Vec2> > *out) {
    const char *p = data.c_str();
    std::vector<Vec2> current;
    Vec2 point = vec2(0.0f, 0.0f);
    Vec2 start = vec2(0.0f, 0.0f);
    Vec2 last_cubic = point;
    Vec2 last_quad = point;
    char command = 0;
    char previous = 0;

    while (*p) {
        skip_separators(&p);
        if (!*p) break;

        if (isalpha((unsigned char)*p)) {
            command = *p;
            p++;
        } else if (command == 'M') {
            command = 'L'; // repeated moveto coordinates are implicit linetos
        } else if (command == 'm') {
            command = 'l';
        }

        bool relative = (command >= 'a' && command <= 'z');
        char op = (char)toupper((unsigned char)command);

        if (op == 'Z') {
            if (current.size() >= 3) out->push_back(current);
            current.clear();
            point = start;
            previous = op;
            continue;
        }

        float v[7];
        int need = 0;
        switch (op) {
        case 'M': case 'L': case 'T': need = 2; break;
        case 'H': case 'V': need = 1; break;
        case 'C': need = 6; break;
        case 'S': case 'Q': need = 4; break;
        case 'A': need = 7; break;
        default: return; // unknown command, give up on this path
        }
        for (int i = 0; i < need; ++i) {
            if (!read_number(&p, &v[i])) return;
        }

        Vec2 target = point;
        switch (op) {
        case 'M':
            if (current.size() >= 3) out->push_back(current);
            current.clear();
            target = relative ? vec2(point.x + v[0], point.y + v[1]) : vec2(v[0], v[1]);
            point = target;
            start = target;
            add_point(&current, point);
            break;
        case 'L':
            target = relative ? vec2(point.x + v[0], point.y + v[1]) : vec2(v[0], v[1]);
            point = target;
            add_point(&current, point);
            break;
        case 'H':
            point = vec2(relative ? point.x + v[0] : v[0], point.y);
            add_point(&current, point);
            break;
        case 'V':
            point = vec2(point.x, relative ? point.y + v[0] : v[0]);
            add_point(&current, point);
            break;
        case 'C': {
            Vec2 c1 = relative ? vec2(point.x + v[0], point.y + v[1]) : vec2(v[0], v[1]);
            Vec2 c2 = relative ? vec2(point.x + v[2], point.y + v[3]) : vec2(v[2], v[3]);
            target = relative ? vec2(point.x + v[4], point.y + v[5]) : vec2(v[4], v[5]);
            flatten_cubic(&current, point, c1, c2, target);
            last_cubic = c2;
            point = target;
            break;
        }
        case 'S': {
            /* The first control point mirrors the previous curve's second. */
            Vec2 c1 = (previous == 'C' || previous == 'S')
                ? vec2(2.0f * point.x - last_cubic.x, 2.0f * point.y - last_cubic.y)
                : point;
            Vec2 c2 = relative ? vec2(point.x + v[0], point.y + v[1]) : vec2(v[0], v[1]);
            target = relative ? vec2(point.x + v[2], point.y + v[3]) : vec2(v[2], v[3]);
            flatten_cubic(&current, point, c1, c2, target);
            last_cubic = c2;
            point = target;
            break;
        }
        case 'Q': {
            Vec2 c1 = relative ? vec2(point.x + v[0], point.y + v[1]) : vec2(v[0], v[1]);
            target = relative ? vec2(point.x + v[2], point.y + v[3]) : vec2(v[2], v[3]);
            flatten_quadratic(&current, point, c1, target);
            last_quad = c1;
            point = target;
            break;
        }
        case 'T': {
            Vec2 c1 = (previous == 'Q' || previous == 'T')
                ? vec2(2.0f * point.x - last_quad.x, 2.0f * point.y - last_quad.y)
                : point;
            target = relative ? vec2(point.x + v[0], point.y + v[1]) : vec2(v[0], v[1]);
            flatten_quadratic(&current, point, c1, target);
            last_quad = c1;
            point = target;
            break;
        }
        case 'A': {
            target = relative ? vec2(point.x + v[5], point.y + v[6]) : vec2(v[5], v[6]);
            flatten_arc(&current, point, v[0], v[1], v[2], v[3] != 0.0f, v[4] != 0.0f, target);
            point = target;
            break;
        }
        default:
            break;
        }
        previous = op;
    }

    /* An unclosed subpath still bounds an area once its ends are joined, which
     * is the only reading that can become a volume. */
    if (current.size() >= 3) out->push_back(current);
}

/* Element shapes */

static void emit_rect(const std::string &el, std::vector<std::vector<Vec2> > *out) {
    float x = attribute_number(el, "x", 0.0f);
    float y = attribute_number(el, "y", 0.0f);
    float w = attribute_number(el, "width", 0.0f);
    float h = attribute_number(el, "height", 0.0f);
    if (w <= 0.0f || h <= 0.0f) return;

    float rx = attribute_number(el, "rx", 0.0f);
    float ry = attribute_number(el, "ry", rx);
    if (rx <= 0.0f) rx = ry;
    if (ry <= 0.0f) ry = rx;
    if (rx > w * 0.5f) rx = w * 0.5f;
    if (ry > h * 0.5f) ry = h * 0.5f;

    std::vector<Vec2> c;
    if (rx <= 0.0f || ry <= 0.0f) {
        add_point(&c, vec2(x, y));
        add_point(&c, vec2(x + w, y));
        add_point(&c, vec2(x + w, y + h));
        add_point(&c, vec2(x, y + h));
    } else {
        add_point(&c, vec2(x + rx, y));
        add_point(&c, vec2(x + w - rx, y));
        flatten_arc(&c, vec2(x + w - rx, y), rx, ry, 0.0f, false, true, vec2(x + w, y + ry));
        add_point(&c, vec2(x + w, y + h - ry));
        flatten_arc(&c, vec2(x + w, y + h - ry), rx, ry, 0.0f, false, true, vec2(x + w - rx, y + h));
        add_point(&c, vec2(x + rx, y + h));
        flatten_arc(&c, vec2(x + rx, y + h), rx, ry, 0.0f, false, true, vec2(x, y + h - ry));
        add_point(&c, vec2(x, y + ry));
        flatten_arc(&c, vec2(x, y + ry), rx, ry, 0.0f, false, true, vec2(x + rx, y));
    }
    out->push_back(c);
}

static void emit_ellipse(float cx, float cy, float rx, float ry,
                         std::vector<std::vector<Vec2> > *out) {
    if (rx <= 0.0f || ry <= 0.0f) return;

    std::vector<Vec2> c;
    const int steps = 64;
    for (int i = 0; i < steps; ++i) {
        float a = 2.0f * 3.14159265358979f * (float)i / (float)steps;
        add_point(&c, vec2(cx + cosf(a) * rx, cy + sinf(a) * ry));
    }
    out->push_back(c);
}

static void emit_points(const std::string &el, bool closed,
                        std::vector<std::vector<Vec2> > *out) {
    std::string data;
    if (!find_attribute(el, "points", &data)) return;

    std::vector<Vec2> c;
    const char *p = data.c_str();
    float x, y;
    while (read_number(&p, &x) && read_number(&p, &y)) add_point(&c, vec2(x, y));

    (void)closed; // an open polyline still has to be closed to bound an area
    if (c.size() >= 3) out->push_back(c);
}

/* Document scan */

static std::string element_name(const std::string &element) {
    size_t i = 0;
    while (i < element.size() && (isspace((unsigned char)element[i]) || element[i] == '<')) i++;
    size_t start = i;
    while (i < element.size() && (isalnum((unsigned char)element[i]) || element[i] == ':')) i++;
    return element.substr(start, i - start);
}

bool svg_parse_file(const char *path, SvgDocument *out, std::string *error) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (error) *error = std::string("Could not open ") + path + ".";
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        if (error) *error = "SVG file is empty.";
        return false;
    }
    std::string text;
    text.resize((size_t)size);
    bool read_ok = fread(&text[0], 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!read_ok) {
        if (error) *error = "Could not read the SVG file.";
        return false;
    }

    out->shapes.clear();
    out->width_mm = 0.0f;
    out->height_mm = 0.0f;

    /* Group transforms nest, so the stack holds the accumulated transform. */
    std::vector<Xform> stack;
    stack.push_back(xform_identity());

    float view_w = 0.0f, view_h = 0.0f;
    float view_x = 0.0f, view_y = 0.0f;
    float doc_w_mm = 0.0f, doc_h_mm = 0.0f;
    bool have_viewbox = false;

    size_t pos = 0;
    while (true) {
        size_t open = text.find('<', pos);
        if (open == std::string::npos) break;

        /* Comments, CDATA and processing instructions carry no geometry. */
        if (text.compare(open, 4, "<!--") == 0) {
            size_t close = text.find("-->", open);
            pos = (close == std::string::npos) ? text.size() : close + 3;
            continue;
        }

        size_t close = text.find('>', open);
        if (close == std::string::npos) break;

        std::string element = text.substr(open, close - open + 1);
        pos = close + 1;

        if (element.size() > 1 && element[1] == '/') {
            /* Closing tag: only groups pushed a transform. */
            std::string name = element_name(element.substr(2));
            if (name == "g" || name == "svg") {
                if (stack.size() > 1) stack.pop_back();
            }
            continue;
        }
        if (element.size() > 1 && (element[1] == '?' || element[1] == '!')) continue;

        std::string name = element_name(element);
        bool self_closing = element.size() >= 2 && element[element.size() - 2] == '/';

        if (name == "svg") {
            std::string w, h, vb;
            if (find_attribute(element, "width", &w)) doc_w_mm = length_to_mm(w, 0.0f);
            if (find_attribute(element, "height", &h)) doc_h_mm = length_to_mm(h, 0.0f);
            if (find_attribute(element, "viewBox", &vb)) {
                const char *p = vb.c_str();
                float v[4];
                int count = 0;
                while (count < 4 && read_number(&p, &v[count])) count++;
                if (count == 4) {
                    view_x = v[0]; view_y = v[1];
                    view_w = v[2]; view_h = v[3];
                    have_viewbox = true;
                }
            }
            if (!self_closing) stack.push_back(stack.back());
            continue;
        }

        if (name == "g") {
            Xform t = xform_mul(parse_transform(element), stack.back());
            if (self_closing) continue; // nothing can be inside it
            stack.push_back(t);
            continue;
        }

        std::vector<std::vector<Vec2> > contours;
        if (name == "path") {
            std::string d;
            if (find_attribute(element, "d", &d)) parse_path_data(d, &contours);
        } else if (name == "rect") {
            emit_rect(element, &contours);
        } else if (name == "circle") {
            float r = attribute_number(element, "r", 0.0f);
            emit_ellipse(attribute_number(element, "cx", 0.0f),
                         attribute_number(element, "cy", 0.0f), r, r, &contours);
        } else if (name == "ellipse") {
            emit_ellipse(attribute_number(element, "cx", 0.0f),
                         attribute_number(element, "cy", 0.0f),
                         attribute_number(element, "rx", 0.0f),
                         attribute_number(element, "ry", 0.0f), &contours);
        } else if (name == "polygon") {
            emit_points(element, true, &contours);
        } else if (name == "polyline") {
            emit_points(element, false, &contours);
        } else {
            continue; // text, defs, styles and the rest carry no outline
        }

        if (contours.empty()) continue;

        Xform t = xform_mul(parse_transform(element), stack.back());
        SvgShape shape;
        shape.grey = 1.0f;
        parse_fill_grey(element, &shape.grey);

        for (size_t i = 0; i < contours.size(); ++i) {
            std::vector<Vec2> c;
            c.reserve(contours[i].size());
            for (size_t k = 0; k < contours[i].size(); ++k) {
                c.push_back(xform_apply(t, contours[i][k]));
            }
            shape.contours.push_back(c);
        }
        out->shapes.push_back(shape);
    }

    if (out->shapes.empty()) {
        if (error) *error = "No filled outlines found in the SVG.";
        return false;
    }

    /*
     * User units become millimeters. With both a viewBox and a physical size the
     * ratio between them is the scale; otherwise user units are CSS pixels.
     */
    float scale_x = MM_PER_PX;
    float scale_y = MM_PER_PX;
    if (have_viewbox && view_w > 0.0f && view_h > 0.0f && doc_w_mm > 0.0f && doc_h_mm > 0.0f) {
        scale_x = doc_w_mm / view_w;
        scale_y = doc_h_mm / view_h;
    }

    /* Y is flipped because SVG grows downward while the workplane grows away
     * from the viewer, then the artwork is centred on the origin. */
    Bounds b = bounds_empty();
    for (size_t s = 0; s < out->shapes.size(); ++s) {
        for (size_t i = 0; i < out->shapes[s].contours.size(); ++i) {
            std::vector<Vec2> &c = out->shapes[s].contours[i];
            for (size_t k = 0; k < c.size(); ++k) {
                c[k] = vec2((c[k].x - view_x) * scale_x, -(c[k].y - view_y) * scale_y);
                bounds_add_point(&b, vec3(c[k].x, c[k].y, 0.0f));
            }
        }
    }

    if (b.valid) {
        Vec3 centre = bounds_center(b);
        for (size_t s = 0; s < out->shapes.size(); ++s) {
            for (size_t i = 0; i < out->shapes[s].contours.size(); ++i) {
                std::vector<Vec2> &c = out->shapes[s].contours[i];
                for (size_t k = 0; k < c.size(); ++k) {
                    c[k] = vec2(c[k].x - centre.x, c[k].y - centre.y);
                }
            }
        }
        Vec3 size = bounds_size(b);
        out->width_mm = size.x;
        out->height_mm = size.y;
    }
    return true;
}
