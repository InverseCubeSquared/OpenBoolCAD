#include "import_stl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "mesh_repair.h"

/* File reading */

static bool read_whole_file(const char *path, std::vector<unsigned char> *out, std::string *error) {
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
        if (error) *error = "STL file is empty.";
        return false;
    }
    out->resize((size_t)size);
    bool ok = fread(&(*out)[0], 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok && error) *error = "Could not read the STL file.";
    return ok;
}

static float read_le_float(const unsigned char *p) {
    uint32_t bits = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

/*
 * Binary or ASCII is decided by size, not by the leading "solid" keyword: plenty
 * of binary files start with it inside their 80 byte comment header. A binary
 * file is exactly 84 + 50 * triangles bytes long, which is unambiguous.
 */
static bool looks_binary(const std::vector<unsigned char> &data) {
    if (data.size() < 84) return false;
    uint32_t tris = (uint32_t)data[80] | ((uint32_t)data[81] << 8) |
                    ((uint32_t)data[82] << 16) | ((uint32_t)data[83] << 24);
    return (size_t)84 + (size_t)tris * 50 == data.size();
}

static bool parse_binary(const std::vector<unsigned char> &data, Mesh *out, std::string *error) {
    uint32_t tris = (uint32_t)data[80] | ((uint32_t)data[81] << 8) |
                    ((uint32_t)data[82] << 16) | ((uint32_t)data[83] << 24);

    for (uint32_t t = 0; t < tris; ++t) {
        const unsigned char *rec = &data[84 + (size_t)t * 50];
        /* The stored normal is ignored: it is frequently wrong or zero, and
         * mesh_add_triangle derives it from the winding anyway. */
        Vec3 a = vec3(read_le_float(rec + 12), read_le_float(rec + 16), read_le_float(rec + 20));
        Vec3 b = vec3(read_le_float(rec + 24), read_le_float(rec + 28), read_le_float(rec + 32));
        Vec3 c = vec3(read_le_float(rec + 36), read_le_float(rec + 40), read_le_float(rec + 44));
        mesh_add_triangle(out, a, b, c);
    }

    if (out->vertices.empty()) {
        if (error) *error = "Binary STL contains no triangles.";
        return false;
    }
    return true;
}

static bool parse_ascii(const std::vector<unsigned char> &data, Mesh *out, std::string *error) {
    std::string text((const char *)&data[0], data.size());

    Vec3 corner[3];
    int have = 0;
    size_t pos = 0;

    while (true) {
        size_t at = text.find("vertex", pos);
        if (at == std::string::npos) break;

        const char *cursor = text.c_str() + at + 6;
        char *end = NULL;
        float v[3];
        bool ok = true;
        for (int i = 0; i < 3; ++i) {
            v[i] = strtof(cursor, &end);
            if (end == cursor) {
                ok = false;
                break;
            }
            cursor = end;
        }
        pos = (size_t)(cursor - text.c_str());
        if (!ok) continue;

        corner[have++] = vec3(v[0], v[1], v[2]);
        if (have == 3) {
            mesh_add_triangle(out, corner[0], corner[1], corner[2]);
            have = 0;
        }
    }

    if (out->vertices.empty()) {
        if (error) *error = "ASCII STL contains no triangles.";
        return false;
    }
    return true;
}

bool import_stl(const char *path, Mesh *out, std::string *error, std::string *note) {
    std::vector<unsigned char> data;
    if (!read_whole_file(path, &data, error)) return false;

    mesh_clear(out);
    bool parsed = looks_binary(data) ? parse_binary(data, out, error)
                                     : parse_ascii(data, out, error);
    if (!parsed) return false;

    /* Weld the soup into a solid and rebuild the outline edges. An import that
     * cannot be closed is still returned, with the reason in "note", since a
     * broken mesh is usually more useful on screen than no mesh at all. */
    IndexedMesh indexed;
    MeshRepairReport report;
    bool solid = mesh_repair(*out, &indexed, &report);
    *out = mesh_from_indexed(indexed);

    if (note) {
        std::string summary = mesh_repair_summary(report);
        if (!solid) {
            *note = "not a closed solid" + (summary.empty() ? "" : (": " + summary));
        } else {
            *note = summary;
        }
    }
    return true;
}
