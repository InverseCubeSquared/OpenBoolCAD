#include "project.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

/* Container primitives */

#define OBC_MAGIC "OBCAD\0\0\0"
#define OBC_MAGIC_LEN 8
#define OBC_HEADER_LEN 32

static void put_u32(std::vector<unsigned char> *out, uint32_t v) {
    out->push_back((unsigned char)(v & 0xFF));
    out->push_back((unsigned char)((v >> 8) & 0xFF));
    out->push_back((unsigned char)((v >> 16) & 0xFF));
    out->push_back((unsigned char)((v >> 24) & 0xFF));
}

static void put_u64(std::vector<unsigned char> *out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out->push_back((unsigned char)((v >> (i * 8)) & 0xFF));
}

static uint32_t get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

/* GZIP, i.e. deflate with the gzip wrapper: windowBits 15 + 16. */
static bool gzip_deflate(const unsigned char *data, size_t len,
                         std::vector<unsigned char> *out, std::string *error) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        if (error) *error = "Could not start compression.";
        return false;
    }

    zs.next_in = (Bytef *)data;
    zs.avail_in = (uInt)len;

    unsigned char buffer[65536];
    int status;
    do {
        zs.next_out = buffer;
        zs.avail_out = sizeof(buffer);
        status = deflate(&zs, Z_FINISH);
        if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) {
            deflateEnd(&zs);
            if (error) *error = "Compression failed.";
            return false;
        }
        out->insert(out->end(), buffer, buffer + (sizeof(buffer) - zs.avail_out));
    } while (status != Z_STREAM_END);

    deflateEnd(&zs);
    return true;
}

static bool gzip_inflate(const unsigned char *data, size_t len,
                         std::vector<unsigned char> *out, std::string *error) {
    if (len == 0) return true;

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, 15 + 16) != Z_OK) {
        if (error) *error = "Could not start decompression.";
        return false;
    }

    zs.next_in = (Bytef *)data;
    zs.avail_in = (uInt)len;

    unsigned char buffer[65536];
    int status;
    do {
        zs.next_out = buffer;
        zs.avail_out = sizeof(buffer);
        status = inflate(&zs, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            inflateEnd(&zs);
            if (error) *error = "File is corrupt or not an OpenBoolCAD project.";
            return false;
        }
        out->insert(out->end(), buffer, buffer + (sizeof(buffer) - zs.avail_out));
    } while (status != Z_STREAM_END);

    inflateEnd(&zs);
    return true;
}

/* Schema */

static const char *SCHEMA_SQL =
    "PRAGMA journal_mode=OFF;"
    "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);"
    /* One row per live node, ids preserved: they are indices the merge history
     * refers to, so they must survive a round trip unchanged. */
    "CREATE TABLE node ("
    "  id INTEGER PRIMARY KEY, parent INTEGER, kind INTEGER, name TEXT,"
    "  visible INTEGER, expanded INTEGER, merged INTEGER, polarity INTEGER,"
    "  px REAL, py REAL, pz REAL, rx REAL, ry REAL, rz REAL,"
    "  sx REAL, sy REAL, sz REAL, ordinal INTEGER);"
    /* Normals are not stored: they are recomputed from the triangles on load,
     * which halves the mesh payload and keeps the two in agreement. */
    "CREATE TABLE mesh (node_id INTEGER PRIMARY KEY, vertices BLOB, edges BLOB);"
    "CREATE TABLE selection (node_id INTEGER);"
    "CREATE TABLE camera (yaw REAL, pitch REAL, distance REAL,"
    "  tx REAL, ty REAL, tz REAL, ortho INTEGER);";

static bool db_exec(sqlite3 *db, const char *sql, std::string *error) {
    char *msg = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &msg) == SQLITE_OK) return true;
    if (error) *error = std::string("Database error: ") + (msg ? msg : "unknown");
    sqlite3_free(msg);
    return false;
}

static void blob_from_vec3(const std::vector<Vec3> &v, std::vector<float> *out) {
    out->clear();
    out->reserve(v.size() * 3);
    for (size_t i = 0; i < v.size(); ++i) {
        out->push_back(v[i].x);
        out->push_back(v[i].y);
        out->push_back(v[i].z);
    }
}

static void vec3_from_blob(const void *data, int bytes, std::vector<Vec3> *out) {
    out->clear();
    if (!data || bytes < (int)(3 * sizeof(float))) return;

    size_t count = (size_t)bytes / (3 * sizeof(float));
    const float *f = (const float *)data;
    out->reserve(count);
    for (size_t i = 0; i < count; ++i) {
        out->push_back(vec3(f[i * 3 + 0], f[i * 3 + 1], f[i * 3 + 2]));
    }
}

/* Save */

static bool write_scene(sqlite3 *db, const Scene &scene, const Camera &camera, std::string *error) {
    if (!db_exec(db, SCHEMA_SQL, error)) return false;
    if (!db_exec(db, "BEGIN", error)) return false;

    char version[32];
    snprintf(version, sizeof(version), "%d", OBC_PROJECT_VERSION);
    sqlite3_stmt *st = NULL;

    if (sqlite3_prepare_v2(db, "INSERT INTO meta VALUES (?, ?)", -1, &st, NULL) != SQLITE_OK) {
        if (error) *error = "Could not write metadata.";
        return false;
    }
    const char *keys[2] = { "version", "units" };
    const char *values[2] = { version, "mm" };
    for (int i = 0; i < 2; ++i) {
        sqlite3_reset(st);
        sqlite3_bind_text(st, 1, keys[i], -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, values[i], -1, SQLITE_STATIC);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);

    /* Nodes. The ordinal keeps sibling order stable, which the tree shows. */
    if (sqlite3_prepare_v2(db,
            "INSERT INTO node VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &st, NULL)
        != SQLITE_OK) {
        if (error) *error = "Could not write nodes.";
        return false;
    }
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        const SceneNode &n = scene.nodes[i];
        if (n.kind == NODE_DELETED) continue; // tombstones need no row

        int ordinal = 0;
        const std::vector<int> *siblings = &scene.roots;
        if (n.parent != OBC_NO_NODE) {
            const SceneNode *p = scene_node(&scene, n.parent);
            if (p) siblings = &p->children;
        }
        for (size_t k = 0; k < siblings->size(); ++k) {
            if ((*siblings)[k] == n.id) {
                ordinal = (int)k;
                break;
            }
        }

        sqlite3_reset(st);
        sqlite3_bind_int(st, 1, n.id);
        sqlite3_bind_int(st, 2, n.parent);
        sqlite3_bind_int(st, 3, (int)n.kind);
        sqlite3_bind_text(st, 4, n.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 5, n.visible ? 1 : 0);
        sqlite3_bind_int(st, 6, n.expanded ? 1 : 0);
        sqlite3_bind_int(st, 7, n.merged ? 1 : 0);
        sqlite3_bind_int(st, 8, (int)n.polarity);
        sqlite3_bind_double(st, 9, n.position.x);
        sqlite3_bind_double(st, 10, n.position.y);
        sqlite3_bind_double(st, 11, n.position.z);
        sqlite3_bind_double(st, 12, n.rotation.x);
        sqlite3_bind_double(st, 13, n.rotation.y);
        sqlite3_bind_double(st, 14, n.rotation.z);
        sqlite3_bind_double(st, 15, n.scale.x);
        sqlite3_bind_double(st, 16, n.scale.y);
        sqlite3_bind_double(st, 17, n.scale.z);
        sqlite3_bind_int(st, 18, ordinal);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            if (error) *error = "Could not write a node.";
            return false;
        }
    }
    sqlite3_finalize(st);

    /* Meshes */
    if (sqlite3_prepare_v2(db, "INSERT INTO mesh VALUES (?,?,?)", -1, &st, NULL) != SQLITE_OK) {
        if (error) *error = "Could not write meshes.";
        return false;
    }
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        const SceneNode &n = scene.nodes[i];
        if (n.kind == NODE_DELETED || n.mesh.vertices.empty()) continue;

        std::vector<float> verts, edges;
        blob_from_vec3(n.mesh.vertices, &verts);
        blob_from_vec3(n.mesh.edges, &edges);

        sqlite3_reset(st);
        sqlite3_bind_int(st, 1, n.id);
        sqlite3_bind_blob(st, 2, verts.empty() ? NULL : &verts[0],
                          (int)(verts.size() * sizeof(float)), SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 3, edges.empty() ? NULL : &edges[0],
                          (int)(edges.size() * sizeof(float)), SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            if (error) *error = "Could not write a mesh.";
            return false;
        }
    }
    sqlite3_finalize(st);

    /* Selection and camera, so a session resumes as it was left. */
    if (sqlite3_prepare_v2(db, "INSERT INTO selection VALUES (?)", -1, &st, NULL) == SQLITE_OK) {
        for (size_t i = 0; i < scene.selection.size(); ++i) {
            sqlite3_reset(st);
            sqlite3_bind_int(st, 1, scene.selection[i]);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }

    if (sqlite3_prepare_v2(db, "INSERT INTO camera VALUES (?,?,?,?,?,?,?)", -1, &st, NULL)
        == SQLITE_OK) {
        sqlite3_bind_double(st, 1, camera.yaw);
        sqlite3_bind_double(st, 2, camera.pitch);
        sqlite3_bind_double(st, 3, camera.distance);
        sqlite3_bind_double(st, 4, camera.target.x);
        sqlite3_bind_double(st, 5, camera.target.y);
        sqlite3_bind_double(st, 6, camera.target.z);
        sqlite3_bind_int(st, 7, camera.orthographic ? 1 : 0);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    return db_exec(db, "COMMIT", error);
}

bool project_save(const char *path, const Scene &scene, const Camera &camera,
                  const Thumbnail &thumb, std::string *error) {
    /* Built in memory and serialized out, so no temporary file is involved. */
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        if (error) *error = "Could not create the project database.";
        if (db) sqlite3_close(db);
        return false;
    }

    if (!write_scene(db, scene, camera, error)) {
        sqlite3_close(db);
        return false;
    }

    sqlite3_int64 db_size = 0;
    unsigned char *db_bytes = sqlite3_serialize(db, "main", &db_size, 0);
    if (!db_bytes || db_size <= 0) {
        sqlite3_close(db);
        if (error) *error = "Could not serialize the project database.";
        return false;
    }

    std::vector<unsigned char> db_gz, thumb_gz;
    bool ok = gzip_deflate(db_bytes, (size_t)db_size, &db_gz, error);
    sqlite3_free(db_bytes);
    sqlite3_close(db);
    if (!ok) return false;

    if (!thumb.rgb.empty() &&
        !gzip_deflate(&thumb.rgb[0], thumb.rgb.size(), &thumb_gz, error)) {
        return false;
    }

    std::vector<unsigned char> header;
    /* Indexed rather than OBC_MAGIC + OBC_MAGIC_LEN: the magic has embedded
     * NULs, and pointer arithmetic on a string literal is what clang warns
     * about because it so often means a concatenation was intended. */
    header.insert(header.end(), &OBC_MAGIC[0], &OBC_MAGIC[OBC_MAGIC_LEN]);
    put_u32(&header, OBC_PROJECT_VERSION);
    put_u32(&header, (uint32_t)(thumb.rgb.empty() ? 0 : thumb.width));
    put_u32(&header, (uint32_t)(thumb.rgb.empty() ? 0 : thumb.height));
    put_u32(&header, (uint32_t)thumb_gz.size());
    put_u64(&header, (uint64_t)db_gz.size());

    FILE *f = fopen(path, "wb");
    if (!f) {
        if (error) *error = std::string("Could not open ") + path + " for writing.";
        return false;
    }
    bool written = fwrite(&header[0], 1, header.size(), f) == header.size();
    if (written && !thumb_gz.empty()) {
        written = fwrite(&thumb_gz[0], 1, thumb_gz.size(), f) == thumb_gz.size();
    }
    if (written && !db_gz.empty()) {
        written = fwrite(&db_gz[0], 1, db_gz.size(), f) == db_gz.size();
    }
    fclose(f);

    if (!written) {
        if (error) *error = "Writing the project file failed.";
        return false;
    }
    return true;
}

/* Load */

static bool read_file(const char *path, std::vector<unsigned char> *out, std::string *error) {
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
        if (error) *error = "Project file is empty.";
        return false;
    }
    out->resize((size_t)size);
    bool ok = fread(&(*out)[0], 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok && error) *error = "Could not read the project file.";
    return ok;
}

struct ProjectParts {
    uint32_t version;
    int thumb_w, thumb_h;
    const unsigned char *thumb;
    size_t thumb_len;
    const unsigned char *db;
    size_t db_len;
};

static bool split_container(const std::vector<unsigned char> &file, ProjectParts *parts,
                           std::string *error) {
    if (file.size() < OBC_HEADER_LEN || memcmp(&file[0], OBC_MAGIC, OBC_MAGIC_LEN) != 0) {
        if (error) *error = "Not an OpenBoolCAD project file.";
        return false;
    }

    parts->version = get_u32(&file[8]);
    if (parts->version > OBC_PROJECT_VERSION) {
        if (error) *error = "Project was written by a newer version.";
        return false;
    }

    parts->thumb_w = (int)get_u32(&file[12]);
    parts->thumb_h = (int)get_u32(&file[16]);
    parts->thumb_len = (size_t)get_u32(&file[20]);
    parts->db_len = (size_t)get_u64(&file[24]);

    if (OBC_HEADER_LEN + parts->thumb_len + parts->db_len > file.size()) {
        if (error) *error = "Project file is truncated.";
        return false;
    }
    parts->thumb = &file[OBC_HEADER_LEN];
    parts->db = parts->thumb + parts->thumb_len;
    return true;
}

bool project_read_thumbnail(const char *path, Thumbnail *out, std::string *error) {
    std::vector<unsigned char> file;
    if (!read_file(path, &file, error)) return false;

    ProjectParts parts;
    if (!split_container(file, &parts, error)) return false;

    out->width = parts.thumb_w;
    out->height = parts.thumb_h;
    out->rgb.clear();
    if (parts.thumb_len == 0) return true;
    return gzip_inflate(parts.thumb, parts.thumb_len, &out->rgb, error);
}

static bool read_scene(sqlite3 *db, Scene *scene, Camera *camera, std::string *error) {
    scene_init(scene);

    /* Ids are indices, so the vector is grown to the highest id and the gaps
     * stay tombstoned. That keeps history references valid across a reload. */
    sqlite3_stmt *st = NULL;
    int max_id = -1;
    if (sqlite3_prepare_v2(db, "SELECT MAX(id) FROM node", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) max_id = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    if (max_id < 0) return true; // empty project

    scene->nodes.resize((size_t)max_id + 1);
    for (size_t i = 0; i < scene->nodes.size(); ++i) {
        SceneNode &n = scene->nodes[i];
        n.id = (int)i;
        n.parent = OBC_NO_NODE;
        n.kind = NODE_DELETED;
        n.visible = true;
        n.expanded = true;
        n.merged = false;
        n.polarity = POLARITY_POSITIVE;
        n.position = vec3(0.0f, 0.0f, 0.0f);
        n.rotation = vec3(0.0f, 0.0f, 0.0f);
        n.scale = vec3(1.0f, 1.0f, 1.0f);
    }

    /* Ordered by ordinal so sibling order is rebuilt as it was saved. */
    const char *node_sql =
        "SELECT id,parent,kind,name,visible,expanded,merged,polarity,"
        "px,py,pz,rx,ry,rz,sx,sy,sz FROM node ORDER BY parent, ordinal";
    if (sqlite3_prepare_v2(db, node_sql, -1, &st, NULL) != SQLITE_OK) {
        if (error) *error = "Project database is missing its node table.";
        return false;
    }
    std::vector<int> order;
    while (sqlite3_step(st) == SQLITE_ROW) {
        int id = sqlite3_column_int(st, 0);
        if (id < 0 || id >= (int)scene->nodes.size()) continue;

        SceneNode &n = scene->nodes[id];
        n.id = id;
        n.parent = sqlite3_column_int(st, 1);
        n.kind = (NodeKind)sqlite3_column_int(st, 2);
        const unsigned char *name = sqlite3_column_text(st, 3);
        n.name = name ? (const char *)name : "Object";
        n.visible = sqlite3_column_int(st, 4) != 0;
        n.expanded = sqlite3_column_int(st, 5) != 0;
        n.merged = sqlite3_column_int(st, 6) != 0;
        n.polarity = (Polarity)sqlite3_column_int(st, 7);
        n.position = vec3((float)sqlite3_column_double(st, 8),
                          (float)sqlite3_column_double(st, 9),
                          (float)sqlite3_column_double(st, 10));
        n.rotation = vec3((float)sqlite3_column_double(st, 11),
                          (float)sqlite3_column_double(st, 12),
                          (float)sqlite3_column_double(st, 13));
        n.scale = vec3((float)sqlite3_column_double(st, 14),
                       (float)sqlite3_column_double(st, 15),
                       (float)sqlite3_column_double(st, 16));
        if (n.kind == NODE_DELETED) n.kind = NODE_OBJECT; // never stored, but be safe
        order.push_back(id);
    }
    sqlite3_finalize(st);

    for (size_t i = 0; i < order.size(); ++i) {
        int id = order[i];
        const SceneNode &n = scene->nodes[id];
        if (n.parent == OBC_NO_NODE) {
            scene->roots.push_back(id);
        } else if (n.parent >= 0 && n.parent < (int)scene->nodes.size()) {
            scene->nodes[n.parent].children.push_back(id);
        } else {
            scene->nodes[id].parent = OBC_NO_NODE;
            scene->roots.push_back(id);
        }
    }

    /* Meshes. Normals are rebuilt from the triangles. */
    if (sqlite3_prepare_v2(db, "SELECT node_id,vertices,edges FROM mesh", -1, &st, NULL)
        == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            int id = sqlite3_column_int(st, 0);
            if (id < 0 || id >= (int)scene->nodes.size()) continue;

            std::vector<Vec3> verts, edges;
            vec3_from_blob(sqlite3_column_blob(st, 1), sqlite3_column_bytes(st, 1), &verts);
            vec3_from_blob(sqlite3_column_blob(st, 2), sqlite3_column_bytes(st, 2), &edges);

            Mesh &m = scene->nodes[id].mesh;
            mesh_clear(&m);
            for (size_t v = 0; v + 2 < verts.size(); v += 3) {
                mesh_add_triangle(&m, verts[v], verts[v + 1], verts[v + 2]);
            }
            for (size_t e = 0; e + 1 < edges.size(); e += 2) {
                mesh_add_edge(&m, edges[e], edges[e + 1]);
            }
        }
        sqlite3_finalize(st);
    }

    if (sqlite3_prepare_v2(db, "SELECT node_id FROM selection", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            int id = sqlite3_column_int(st, 0);
            if (scene_node(scene, id) && scene_is_effectively_visible(scene, id)) {
                scene->selection.push_back(id);
            }
        }
        sqlite3_finalize(st);
    }

    if (camera && sqlite3_prepare_v2(db,
            "SELECT yaw,pitch,distance,tx,ty,tz,ortho FROM camera", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            camera->yaw = (float)sqlite3_column_double(st, 0);
            camera->pitch = (float)sqlite3_column_double(st, 1);
            camera->distance = (float)sqlite3_column_double(st, 2);
            camera->target = vec3((float)sqlite3_column_double(st, 3),
                                  (float)sqlite3_column_double(st, 4),
                                  (float)sqlite3_column_double(st, 5));
            camera->orthographic = sqlite3_column_int(st, 6) != 0;
        }
        sqlite3_finalize(st);
    }

    return true;
}

bool project_load(const char *path, Scene *scene, Camera *camera, std::string *error) {
    std::vector<unsigned char> file;
    if (!read_file(path, &file, error)) return false;

    ProjectParts parts;
    if (!split_container(file, &parts, error)) return false;

    std::vector<unsigned char> db_bytes;
    if (!gzip_inflate(parts.db, parts.db_len, &db_bytes, error)) return false;
    if (db_bytes.empty()) {
        if (error) *error = "Project contains no database.";
        return false;
    }

    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        if (error) *error = "Could not open the project database.";
        if (db) sqlite3_close(db);
        return false;
    }

    /* sqlite3_deserialize takes ownership of an sqlite3_malloc'd buffer. */
    unsigned char *owned = (unsigned char *)sqlite3_malloc64(db_bytes.size());
    if (!owned) {
        sqlite3_close(db);
        if (error) *error = "Out of memory reading the project.";
        return false;
    }
    memcpy(owned, &db_bytes[0], db_bytes.size());

    if (sqlite3_deserialize(db, "main", owned, (sqlite3_int64)db_bytes.size(),
                            (sqlite3_int64)db_bytes.size(),
                            SQLITE_DESERIALIZE_FREEONCLOSE) != SQLITE_OK) {
        sqlite3_close(db);
        if (error) *error = "Project database could not be read.";
        return false;
    }

    bool ok = read_scene(db, scene, camera, error);
    sqlite3_close(db);
    return ok;
}
