#include "export_stl.h"

#include <stdio.h>
#include <string.h>

#include "csg.h"
#include "mesh_repair.h"

/* Binary STL writing */

static void put_le32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void put_float(unsigned char *p, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    put_le32(p, bits);
}

/*
 * 80 byte comment header, a triangle count, then 50 bytes per triangle:
 * normal, three vertices, and a 16 bit attribute word that stays zero.
 */
static bool write_binary_stl(const char *path, const Mesh &mesh, std::string *error) {
    size_t tri_count = mesh.vertices.size() / 3;
    if (tri_count == 0) {
        if (error) *error = "Nothing to export.";
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        if (error) *error = std::string("Could not open ") + path + " for writing.";
        return false;
    }

    unsigned char header[84];
    memset(header, 0, sizeof(header));
    snprintf((char *)header, 80, "OpenBoolCAD binary STL, millimeters");
    put_le32(header + 80, (uint32_t)tri_count);
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        if (error) *error = "Writing the STL header failed.";
        return false;
    }

    bool ok = true;
    for (size_t t = 0; t < tri_count && ok; ++t) {
        Vec3 a = mesh.vertices[t * 3 + 0];
        Vec3 b = mesh.vertices[t * 3 + 1];
        Vec3 c = mesh.vertices[t * 3 + 2];
        Vec3 n = (t * 3 < mesh.normals.size())
            ? mesh.normals[t * 3]
            : vec3_normalized(vec3_cross(vec3_sub(b, a), vec3_sub(c, a)));

        unsigned char rec[50];
        memset(rec, 0, sizeof(rec));
        put_float(rec + 0, n.x);  put_float(rec + 4, n.y);  put_float(rec + 8, n.z);
        put_float(rec + 12, a.x); put_float(rec + 16, a.y); put_float(rec + 20, a.z);
        put_float(rec + 24, b.x); put_float(rec + 28, b.y); put_float(rec + 32, b.z);
        put_float(rec + 36, c.x); put_float(rec + 40, c.y); put_float(rec + 44, c.z);
        ok = fwrite(rec, 1, sizeof(rec), f) == sizeof(rec);
    }

    fclose(f);
    if (!ok && error) *error = "Writing STL triangles failed.";
    return ok;
}

/* Export */

bool export_stl(const char *path, const Scene &scene, const std::vector<int> &nodes,
                std::string *error, std::string *note) {
    if (nodes.empty()) {
        if (error) *error = "Select something to export.";
        return false;
    }

    /* Flattened into world space, exactly as the merge does, so an exported
     * group matches what the view shows. */
    std::vector<Mesh> positives, negatives;
    for (size_t i = 0; i < nodes.size(); ++i) {
        scene_collect_meshes(&scene, nodes[i], OBC_NO_NODE, false, &positives, &negatives);
    }

    if (positives.empty()) {
        if (error) *error = "Nothing solid to export: the selection has no positive volume.";
        return false;
    }

    Mesh out;
    if (positives.size() == 1 && negatives.empty()) {
        /* A single solid needs no boolean, only the repair pass so the file is
         * watertight for a slicer. */
        out = positives[0];
        std::string repair;
        if (!csg_make_solid(&out, &repair) && note) {
            *note = "exported mesh is not closed (" + repair + ")";
        } else if (note && !repair.empty()) {
            *note = "repaired: " + repair;
        }
    } else {
        std::string repair;
        if (!csg_merge(positives, negatives, &out, error, &repair)) return false;
        if (note && !repair.empty()) *note = "repaired: " + repair;
    }

    return write_binary_stl(path, out, error);
}
