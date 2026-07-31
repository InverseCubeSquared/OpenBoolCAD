#include "scene.h"

#include <algorithm>

#include "csg.h"

/* Node access */

SceneNode *scene_node(Scene *s, int id) {
    if (id < 0 || id >= (int)s->nodes.size()) return NULL;
    if (s->nodes[id].kind == NODE_DELETED) return NULL;
    return &s->nodes[id];
}

const SceneNode *scene_node(const Scene *s, int id) {
    if (id < 0 || id >= (int)s->nodes.size()) return NULL;
    if (s->nodes[id].kind == NODE_DELETED) return NULL;
    return &s->nodes[id];
}

static int scene_new_node(Scene *s, int parent, NodeKind kind, const char *name) {
    SceneNode n;
    n.id = (int)s->nodes.size();
    n.parent = parent;
    n.kind = kind;
    n.name = name ? name : "Object";
    n.visible = true;
    n.expanded = true;
    n.merged = false;
    n.polarity = POLARITY_POSITIVE;
    n.position = vec3(0.0f, 0.0f, 0.0f);
    n.rotation = vec3(0.0f, 0.0f, 0.0f);
    n.scale = vec3(1.0f, 1.0f, 1.0f);
    s->nodes.push_back(n);

    if (parent == OBC_NO_NODE) {
        s->roots.push_back(n.id);
    } else {
        SceneNode *p = scene_node(s, parent);
        if (p) p->children.push_back(n.id);
        else s->roots.push_back(n.id);
    }
    return n.id;
}

void scene_init(Scene *s) {
    s->nodes.clear();
    s->roots.clear();
    s->selection.clear();
}

void scene_new_empty(Scene *s) {
    scene_init(s);
    scene_add_group(s, OBC_NO_NODE, "Main");
}

void scene_seed_demo(Scene *s) {
    scene_init(s);
    int main_group = scene_add_group(s, OBC_NO_NODE, "Main");

    int cube = scene_add_object(s, main_group, "Object 1", mesh_make_primitive(PRIM_CUBE), POLARITY_POSITIVE);
    SceneNode *n = scene_node(s, cube);
    if (n) n->position = vec3(-15.0f, -10.0f, 0.0f);

    int cyl = scene_add_object(s, main_group, "Object 2", mesh_make_primitive(PRIM_CYLINDER), POLARITY_POSITIVE);
    n = scene_node(s, cyl);
    if (n) n->position = vec3(18.0f, 12.0f, 0.0f);

    int group1 = scene_add_group(s, main_group, "Group 1");

    int sub = scene_add_object(s, group1, "Subobject1", mesh_make_primitive(PRIM_SPHERE), POLARITY_POSITIVE);
    n = scene_node(s, sub);
    if (n) n->position = vec3(-20.0f, 25.0f, 0.0f);

    int subgroup1 = scene_add_group(s, group1, "Subgroup1");
    int subsub = scene_add_object(s, subgroup1, "SubSubObject1", mesh_make_primitive(PRIM_CYLINDER), POLARITY_NEGATIVE);
    n = scene_node(s, subsub);
    if (n) {
        n->position = vec3(-15.0f, -10.0f, 0.0f);
        n->scale = vec3(0.5f, 0.5f, 1.4f);
    }

    int subgroup2 = scene_add_group(s, group1, "Subgroup2");
    n = scene_node(s, subgroup2);
    if (n) n->expanded = false;
    int cone = scene_add_object(s, subgroup2, "Cone 1", mesh_make_primitive(PRIM_CONE), POLARITY_POSITIVE);
    n = scene_node(s, cone);
    if (n) n->position = vec3(25.0f, -20.0f, 0.0f);
}

int scene_add_object(Scene *s, int parent, const char *name, const Mesh &mesh, Polarity polarity) {
    int id = scene_new_node(s, parent, NODE_OBJECT, name);
    s->nodes[id].mesh = mesh;
    s->nodes[id].polarity = polarity;
    return id;
}

int scene_add_group(Scene *s, int parent, const char *name) {
    return scene_new_node(s, parent, NODE_GROUP, name);
}

static void scene_detach(Scene *s, int id) {
    const SceneNode *n = scene_node(s, id);
    if (!n) return;
    std::vector<int> *list = &s->roots;
    if (n->parent != OBC_NO_NODE) {
        SceneNode *p = scene_node(s, n->parent);
        if (p) list = &p->children;
    }
    list->erase(std::remove(list->begin(), list->end(), id), list->end());
}

void scene_delete_node(Scene *s, int id) {
    SceneNode *n = scene_node(s, id);
    if (!n) return;

    std::vector<int> kids = n->children;
    for (size_t i = 0; i < kids.size(); ++i) scene_delete_node(s, kids[i]);

    scene_detach(s, id);
    s->nodes[id].kind = NODE_DELETED;
    s->nodes[id].children.clear();
    mesh_clear(&s->nodes[id].mesh);
    s->selection.erase(std::remove(s->selection.begin(), s->selection.end(), id), s->selection.end());
}

static bool scene_is_ancestor(const Scene *s, int ancestor, int id) {
    int walk = id;
    while (walk != OBC_NO_NODE) {
        if (walk == ancestor) return true;
        const SceneNode *n = scene_node(s, walk);
        if (!n) break;
        walk = n->parent;
    }
    return false;
}

/* Copy and paste */

/* Appends node "id" and everything under it, remapping ids to fragment
 * indices. Returns the fragment index of the node itself. */
static int copy_subtree(const Scene *s, int id, int fragment_parent,
                        std::vector<SceneNode> *fragment) {
    const SceneNode *n = scene_node(s, id);
    if (!n) return OBC_NO_NODE;

    int index = (int)fragment->size();
    fragment->push_back(*n);          // mesh and all
    (*fragment)[index].id = index;
    (*fragment)[index].parent = fragment_parent;
    (*fragment)[index].children.clear();

    for (size_t i = 0; i < n->children.size(); ++i) {
        int child = copy_subtree(s, n->children[i], index, fragment);
        if (child != OBC_NO_NODE) (*fragment)[index].children.push_back(child);
    }
    return index;
}

/* True when "id" sits anywhere under "ancestor". */
static bool is_descendant(const Scene *s, int id, int ancestor) {
    int walk = id;
    while (walk != OBC_NO_NODE) {
        const SceneNode *n = scene_node(s, walk);
        if (!n) return false;
        walk = n->parent;
        if (walk == ancestor) return true;
    }
    return false;
}

void scene_copy_subtrees(const Scene *s, const std::vector<int> &ids,
                         std::vector<SceneNode> *fragment) {
    fragment->clear();
    for (size_t i = 0; i < ids.size(); ++i) {
        /* A node already inside another selected node would otherwise be
         * copied twice: once on its own and once with its ancestor. */
        bool nested = false;
        for (size_t k = 0; k < ids.size() && !nested; ++k) {
            if (k != i && is_descendant(s, ids[i], ids[k])) nested = true;
        }
        if (!nested) copy_subtree(s, ids[i], OBC_NO_NODE, fragment);
    }
}

/*
 * Drops an id from the root list.
 *
 * scene_new_node registers every node it makes as a root, since that is where a
 * parentless node belongs. A paste then re-parents most of them, and the stale
 * root entry has to go with it: a node left in both places is walked twice by
 * the renderer, once under its parent and once at the root without the parent's
 * transform, which is a second copy sitting wherever the parent used to be.
 */
static void detach_from_roots(Scene *s, int id) {
    for (size_t i = 0; i < s->roots.size(); ++i) {
        if (s->roots[i] != id) continue;
        s->roots.erase(s->roots.begin() + (long)i);
        return;
    }
}

void scene_paste_subtrees(Scene *s, int parent, const std::vector<SceneNode> &fragment,
                          std::vector<int> *new_roots) {
    if (new_roots) new_roots->clear();
    if (fragment.empty()) return;

    /* Nodes are created first so every fragment index has a live id, then the
     * links are written: a child can appear before its parent in the list. */
    std::vector<int> mapped(fragment.size(), OBC_NO_NODE);
    for (size_t i = 0; i < fragment.size(); ++i) {
        mapped[i] = scene_new_node(s, OBC_NO_NODE, fragment[i].kind, fragment[i].name.c_str());
    }

    for (size_t i = 0; i < fragment.size(); ++i) {
        SceneNode *n = scene_node(s, mapped[i]);
        if (!n) continue;

        int id = n->id;                 // minted above, not the fragment's
        *n = fragment[i];
        n->id = id;
        n->children.clear();
        for (size_t c = 0; c < fragment[i].children.size(); ++c) {
            n->children.push_back(mapped[fragment[i].children[c]]);
        }
        n->parent = (fragment[i].parent == OBC_NO_NODE)
                  ? OBC_NO_NODE : mapped[fragment[i].parent];
        /* Everything inside the fragment now hangs off its own parent, so it
         * must not also stand at the root. */
        if (n->parent != OBC_NO_NODE) detach_from_roots(s, mapped[i]);
    }

    /* The fragment's own roots are attached where the paste asked for. */
    for (size_t i = 0; i < fragment.size(); ++i) {
        if (fragment[i].parent != OBC_NO_NODE) continue;
        SceneNode *n = scene_node(s, mapped[i]);
        if (!n) continue;

        n->parent = parent;
        if (parent != OBC_NO_NODE) {
            SceneNode *p = scene_node(s, parent);
            if (p) {
                p->children.push_back(mapped[i]);
                detach_from_roots(s, mapped[i]); // scene_new_node put it there
            }
        }
        /* A fragment root pasted at the top level keeps the root entry
         * scene_new_node already gave it. */
        if (new_roots) new_roots->push_back(mapped[i]);
    }
}

bool scene_can_reparent(const Scene *s, int id, int new_parent) {
    if (id == new_parent) return false;
    if (!scene_node(s, id)) return false;
    if (new_parent != OBC_NO_NODE && !scene_node(s, new_parent)) return false;
    if (new_parent != OBC_NO_NODE && scene_is_ancestor(s, id, new_parent)) return false;
    return true;
}

void scene_reparent(Scene *s, int id, int new_parent) {
    if (!scene_can_reparent(s, id, new_parent)) return;
    scene_detach(s, id);
    s->nodes[id].parent = new_parent;
    if (new_parent == OBC_NO_NODE) {
        s->roots.push_back(id);
    } else {
        s->nodes[new_parent].children.push_back(id);
        s->nodes[new_parent].expanded = true;
    }
}

/* Selection */

bool scene_is_selected(const Scene *s, int id) {
    return std::find(s->selection.begin(), s->selection.end(), id) != s->selection.end();
}

bool scene_is_effectively_visible(const Scene *s, int id) {
    const SceneNode *self = scene_node(s, id);
    if (!self || !self->visible) return false;

    int walk = self->parent;
    while (walk != OBC_NO_NODE) {
        const SceneNode *n = scene_node(s, walk);
        if (!n) return false;
        if (!n->visible) return false;
        /* Inside a merged object this node is history, not geometry: it must
         * not render, pick, or be selectable. */
        if (n->merged) return false;
        walk = n->parent;
    }
    return true;
}

void scene_select_only(Scene *s, int id) {
    s->selection.clear();
    if (scene_is_effectively_visible(s, id)) s->selection.push_back(id);
}

void scene_select_toggle(Scene *s, int id) {
    if (scene_is_selected(s, id)) {
        s->selection.erase(std::remove(s->selection.begin(), s->selection.end(), id), s->selection.end());
        return;
    }
    if (scene_is_effectively_visible(s, id)) s->selection.push_back(id);
}

void scene_select_clear(Scene *s) {
    s->selection.clear();
}

void scene_select_set(Scene *s, const std::vector<int> &ids) {
    s->selection.clear();
    for (size_t i = 0; i < ids.size(); ++i) {
        int id = ids[i];
        if (!scene_is_effectively_visible(s, id)) continue;
        if (scene_is_selected(s, id)) continue; // the caller may repeat an id
        s->selection.push_back(id);
    }
}

void scene_select_all_visible(Scene *s) {
    s->selection.clear();
    for (size_t i = 0; i < s->nodes.size(); ++i) {
        if (s->nodes[i].kind != NODE_OBJECT) continue;
        if (!scene_is_effectively_visible(s, (int)i)) continue;
        s->selection.push_back((int)i);
    }
}

void scene_set_visible(Scene *s, int id, bool visible) {
    SceneNode *n = scene_node(s, id);
    if (!n) return;
    n->visible = visible;
    if (visible) return;

    /* Hidden nodes cannot stay selected. */
    std::vector<int> kept;
    for (size_t i = 0; i < s->selection.size(); ++i) {
        if (scene_is_effectively_visible(s, s->selection[i])) kept.push_back(s->selection[i]);
    }
    s->selection = kept;
}

void scene_invert_selection_polarity(Scene *s) {
    for (size_t i = 0; i < s->selection.size(); ++i) {
        SceneNode *n = scene_node(s, s->selection[i]);
        if (!n) continue;
        n->polarity = (n->polarity == POLARITY_POSITIVE) ? POLARITY_NEGATIVE : POLARITY_POSITIVE;
    }
}

/* Transforms */

static Vec3 apply_local(const SceneNode *n, Vec3 p) {
    Vec3 v = vec3_scaled(p, n->scale);

    float rx = deg_to_rad(n->rotation.x);
    float ry = deg_to_rad(n->rotation.y);
    float rz = deg_to_rad(n->rotation.z);

    float y1 = v.y * cosf(rx) - v.z * sinf(rx);
    float z1 = v.y * sinf(rx) + v.z * cosf(rx);
    v.y = y1; v.z = z1;

    float x2 = v.x * cosf(ry) + v.z * sinf(ry);
    float z2 = -v.x * sinf(ry) + v.z * cosf(ry);
    v.x = x2; v.z = z2;

    float x3 = v.x * cosf(rz) - v.y * sinf(rz);
    float y3 = v.x * sinf(rz) + v.y * cosf(rz);
    v.x = x3; v.y = y3;

    return vec3_add(v, n->position);
}

static Vec3 apply_local_inverse(const SceneNode *n, Vec3 p) {
    Vec3 v = vec3_sub(p, n->position);

    float rx = deg_to_rad(n->rotation.x);
    float ry = deg_to_rad(n->rotation.y);
    float rz = deg_to_rad(n->rotation.z);

    float x3 = v.x * cosf(-rz) - v.y * sinf(-rz);
    float y3 = v.x * sinf(-rz) + v.y * cosf(-rz);
    v.x = x3; v.y = y3;

    float x2 = v.x * cosf(-ry) + v.z * sinf(-ry);
    float z2 = -v.x * sinf(-ry) + v.z * cosf(-ry);
    v.x = x2; v.z = z2;

    float y1 = v.y * cosf(-rx) - v.z * sinf(-rx);
    float z1 = v.y * sinf(-rx) + v.z * cosf(-rx);
    v.y = y1; v.z = z1;

    /* A zero scale axis is not invertible; leave the component alone rather
     * than producing infinities. */
    if (fabsf(n->scale.x) > 1e-6f) v.x /= n->scale.x;
    if (fabsf(n->scale.y) > 1e-6f) v.y /= n->scale.y;
    if (fabsf(n->scale.z) > 1e-6f) v.z /= n->scale.z;
    return v;
}

/* Walks up from id, stopping when it reaches "boundary" (exclusive).
 * boundary == OBC_NO_NODE gives the full world transform. */
static Vec3 point_up_to(const Scene *s, int id, Vec3 local, int boundary) {
    Vec3 p = local;
    int walk = id;
    while (walk != boundary && walk != OBC_NO_NODE) {
        const SceneNode *n = scene_node(s, walk);
        if (!n) break;
        p = apply_local(n, p);
        walk = n->parent;
    }
    return p;
}

Vec3 scene_world_point(const Scene *s, int id, Vec3 local) {
    return point_up_to(s, id, local, OBC_NO_NODE);
}

Vec3 scene_point_from_world(const Scene *s, int boundary, Vec3 world) {
    if (boundary == OBC_NO_NODE) return world;

    /* Collect the chain root-ward, then undo it from the root down. */
    std::vector<int> chain;
    int walk = boundary;
    while (walk != OBC_NO_NODE) {
        const SceneNode *n = scene_node(s, walk);
        if (!n) break;
        chain.push_back(walk);
        walk = n->parent;
    }

    Vec3 p = world;
    for (size_t i = chain.size(); i-- > 0; ) {
        const SceneNode *n = scene_node(s, chain[i]);
        if (n) p = apply_local_inverse(n, p);
    }
    return p;
}

Bounds scene_node_bounds(const Scene *s, int id) {
    Bounds b = bounds_empty();
    const SceneNode *n = scene_node(s, id);
    if (!n) return b;

    for (size_t i = 0; i < n->mesh.vertices.size(); ++i) {
        bounds_add_point(&b, scene_world_point(s, id, n->mesh.vertices[i]));
    }
    if (n->merged) return b; // children are history, not geometry

    for (size_t i = 0; i < n->children.size(); ++i) {
        Bounds cb = scene_node_bounds(s, n->children[i]);
        bounds_merge(&b, cb);
    }
    return b;
}

Bounds scene_selection_bounds(const Scene *s) {
    Bounds b = bounds_empty();
    for (size_t i = 0; i < s->selection.size(); ++i) {
        Bounds nb = scene_node_bounds(s, s->selection[i]);
        bounds_merge(&b, nb);
    }
    return b;
}

Mat3 scene_world_rotation(const Scene *s, int id) {
    Mat3 r = mat3_identity();
    int walk = id;
    while (walk != OBC_NO_NODE) {
        const SceneNode *n = scene_node(s, walk);
        if (!n) break;
        /* Rotations compose parent-first, the same order apply_local walks. */
        r = mat3_mul(mat3_from_euler_zyx(n->rotation), r);
        walk = n->parent;
    }
    return r;
}

/*
 * Bounds in a rotated frame.
 *
 * Every vertex is transformed rather than the eight corners of the world box:
 * rotating a world-aligned box and re-fitting it would only grow it, and the
 * whole point of an object-relative plane is a box that hugs a rotated part.
 */
Bounds scene_node_bounds_in(const Scene *s, int id, Mat3 world_to_plane) {
    Bounds b = bounds_empty();
    const SceneNode *n = scene_node(s, id);
    if (!n) return b;

    for (size_t i = 0; i < n->mesh.vertices.size(); ++i) {
        Vec3 world = scene_world_point(s, id, n->mesh.vertices[i]);
        bounds_add_point(&b, mat3_apply(world_to_plane, world));
    }
    if (n->merged) return b; // children are history, not geometry

    for (size_t i = 0; i < n->children.size(); ++i) {
        bounds_merge(&b, scene_node_bounds_in(s, n->children[i], world_to_plane));
    }
    return b;
}

Bounds scene_selection_bounds_in(const Scene *s, Mat3 world_to_plane) {
    Bounds b = bounds_empty();
    for (size_t i = 0; i < s->selection.size(); ++i) {
        bounds_merge(&b, scene_node_bounds_in(s, s->selection[i], world_to_plane));
    }
    return b;
}

/* Picking */

static bool ray_triangle(Vec3 origin, Vec3 dir, Vec3 a, Vec3 b, Vec3 c, float *out_t) {
    Vec3 e1 = vec3_sub(b, a);
    Vec3 e2 = vec3_sub(c, a);
    Vec3 p = vec3_cross(dir, e2);
    float det = vec3_dot(e1, p);
    if (fabsf(det) < 1e-9f) return false; // ray parallel to the triangle

    float inv = 1.0f / det;
    Vec3 t_vec = vec3_sub(origin, a);
    float u = vec3_dot(t_vec, p) * inv;
    if (u < 0.0f || u > 1.0f) return false;

    Vec3 q = vec3_cross(t_vec, e1);
    float v = vec3_dot(dir, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;

    float t = vec3_dot(e2, q) * inv;
    if (t <= 0.0f) return false;
    *out_t = t;
    return true;
}

int scene_pick_surface(const Scene *s, Vec3 origin, Vec3 dir,
                       float *out_distance, Vec3 *out_normal) {
    int best = OBC_NO_NODE;
    float best_t = 0.0f;
    Vec3 best_normal = vec3(0.0f, 0.0f, 1.0f);

    for (size_t i = 0; i < s->nodes.size(); ++i) {
        const SceneNode *n = &s->nodes[i];
        if (n->kind != NODE_OBJECT) continue;
        if (!scene_is_effectively_visible(s, (int)i)) continue;

        for (size_t v = 0; v + 2 < n->mesh.vertices.size(); v += 3) {
            Vec3 a = scene_world_point(s, (int)i, n->mesh.vertices[v]);
            Vec3 b = scene_world_point(s, (int)i, n->mesh.vertices[v + 1]);
            Vec3 c = scene_world_point(s, (int)i, n->mesh.vertices[v + 2]);
            float t;
            if (!ray_triangle(origin, dir, a, b, c, &t)) continue;
            if (best == OBC_NO_NODE || t < best_t) {
                best = (int)i;
                best_t = t;
                /* Taken from the transformed corners, not the stored normal:
                 * a mirrored or non-uniformly scaled node would not agree. */
                best_normal = vec3_normalized(vec3_cross(vec3_sub(b, a), vec3_sub(c, a)));
            }
        }
    }

    if (out_distance) *out_distance = best_t;
    if (out_normal) {
        /* Face the viewer, so a plane laid on a back face is not upside down. */
        if (vec3_dot(best_normal, dir) > 0.0f) best_normal = vec3_mul(best_normal, -1.0f);
        *out_normal = best_normal;
    }
    return best;
}

int scene_pick(const Scene *s, Vec3 origin, Vec3 dir, float *out_distance) {
    return scene_pick_surface(s, origin, dir, out_distance, NULL);
}

Vec3 scene_face_center(const Scene *s, int id, Vec3 world_point, Vec3 world_normal) {
    const SceneNode *n = scene_node(s, id);
    if (!n) return world_point;

    Vec3 normal = vec3_normalized(world_normal);
    float plane_offset = vec3_dot(normal, world_point);

    Vec3 sum = vec3(0.0f, 0.0f, 0.0f);
    float total_area = 0.0f;

    for (size_t v = 0; v + 2 < n->mesh.vertices.size(); v += 3) {
        Vec3 a = scene_world_point(s, id, n->mesh.vertices[v]);
        Vec3 b = scene_world_point(s, id, n->mesh.vertices[v + 1]);
        Vec3 c = scene_world_point(s, id, n->mesh.vertices[v + 2]);

        Vec3 cross = vec3_cross(vec3_sub(b, a), vec3_sub(c, a));
        float area = 0.5f * vec3_length(cross);
        if (area < 1e-9f) continue;

        Vec3 tri_normal = vec3_normalized(cross);
        if (vec3_dot(tri_normal, normal) < 0.999f) continue; // a different facing

        Vec3 centroid = vec3_mul(vec3_add(vec3_add(a, b), c), 1.0f / 3.0f);
        if (fabsf(vec3_dot(normal, centroid) - plane_offset) > 1e-3f) continue; // parallel, elsewhere

        sum = vec3_add(sum, vec3_mul(centroid, area));
        total_area += area;
    }

    if (total_area < 1e-9f) return world_point;
    return vec3_mul(sum, 1.0f / total_area);
}

/* Editing handles and selection transforms */

TransformHandle scene_handle(const Bounds &b, int index) {
    TransformHandle h;
    h.pos = vec3(0.0f, 0.0f, 0.0f);
    h.anchor = vec3(0.0f, 0.0f, 0.0f);
    h.mask = vec3(0.0f, 0.0f, 0.0f);
    if (!b.valid || index < 0 || index >= OBC_HANDLE_COUNT) return h;

    Vec3 c = bounds_center(b);

    if (index < 8) {
        /* Corners scale on all three axes, anchored at the opposite corner. */
        float xs[2] = { b.min.x, b.max.x };
        float ys[2] = { b.min.y, b.max.y };
        float zs[2] = { b.min.z, b.max.z };
        int ix = index & 1;
        int iy = (index >> 1) & 1;
        int iz = (index >> 2) & 1;
        h.pos = vec3(xs[ix], ys[iy], zs[iz]);
        h.anchor = vec3(xs[1 - ix], ys[1 - iy], zs[1 - iz]);
        h.mask = vec3(1.0f, 1.0f, 1.0f);
        return h;
    }

    /* Face centers scale on one axis, anchored at the opposite face. */
    int face = index - 8;
    switch (face) {
    case 0: h.pos = vec3(b.min.x, c.y, c.z); h.anchor = vec3(b.max.x, c.y, c.z); h.mask = vec3(1, 0, 0); break;
    case 1: h.pos = vec3(b.max.x, c.y, c.z); h.anchor = vec3(b.min.x, c.y, c.z); h.mask = vec3(1, 0, 0); break;
    case 2: h.pos = vec3(c.x, b.min.y, c.z); h.anchor = vec3(c.x, b.max.y, c.z); h.mask = vec3(0, 1, 0); break;
    case 3: h.pos = vec3(c.x, b.max.y, c.z); h.anchor = vec3(c.x, b.min.y, c.z); h.mask = vec3(0, 1, 0); break;
    case 4: h.pos = vec3(c.x, c.y, b.min.z); h.anchor = vec3(c.x, c.y, b.max.z); h.mask = vec3(0, 0, 1); break;
    default: h.pos = vec3(c.x, c.y, b.max.z); h.anchor = vec3(c.x, c.y, b.min.z); h.mask = vec3(0, 0, 1); break;
    }
    return h;
}

/* Converts a world space delta into the space a node's position lives in. Two
 * points are transformed rather than the vector itself, so parent rotation and
 * scale are accounted for. */
static Vec3 world_delta_to_parent(const Scene *s, int parent, Vec3 reference, Vec3 world_delta) {
    Vec3 a = scene_point_from_world(s, parent, reference);
    Vec3 b = scene_point_from_world(s, parent, vec3_add(reference, world_delta));
    return vec3_sub(b, a);
}

void scene_translate_node(Scene *s, int id, Vec3 world_delta) {
    SceneNode *n = scene_node(s, id);
    if (!n) return;

    Vec3 world_origin = scene_world_point(s, id, vec3(0.0f, 0.0f, 0.0f));
    Vec3 local = world_delta_to_parent(s, n->parent, world_origin, world_delta);
    n->position = vec3_add(n->position, local);
}

void scene_translate_selection(Scene *s, Vec3 world_delta) {
    for (size_t i = 0; i < s->selection.size(); ++i) {
        scene_translate_node(s, s->selection[i], world_delta);
    }
}

void scene_rotate_selection(Scene *s, Vec3 world_center, Vec3 world_axis, float degrees) {
    Mat3 extra = mat3_axis_angle(world_axis, degrees);

    for (size_t i = 0; i < s->selection.size(); ++i) {
        int id = s->selection[i];
        SceneNode *n = scene_node(s, id);
        if (!n) continue;

        /* Orientation: pre-multiply, then read ZYX angles back out. */
        Mat3 composed = mat3_mul(extra, mat3_from_euler_zyx(n->rotation));
        n->rotation = mat3_to_euler_zyx(composed);

        /* Position: orbit the node origin about the pivot. */
        Vec3 world_origin = scene_world_point(s, id, vec3(0.0f, 0.0f, 0.0f));
        Vec3 rotated = vec3_add(world_center, mat3_apply(extra, vec3_sub(world_origin, world_center)));
        Vec3 local = world_delta_to_parent(s, n->parent, world_origin, vec3_sub(rotated, world_origin));
        n->position = vec3_add(n->position, local);
    }
}

/*
 * Re-expresses plane-axis factors in a node's own axes.
 *
 * Each node axis takes the factor of whichever plane axis it lines up with
 * most. When the two frames agree - object-relative mode, or an unrotated node
 * on the workplate - that is the identity and the scale is exact.
 */
static Vec3 factor_in_node_axes(const Scene *s, int id, Mat3 plane_basis, Vec3 factor) {
    Mat3 node_rot = scene_world_rotation(s, id);
    const float f[3] = { factor.x, factor.y, factor.z };
    float out[3] = { 1.0f, 1.0f, 1.0f };

    for (int a = 0; a < 3; ++a) {
        Vec3 axis = mat3_column(node_rot, a);
        int best = 0;
        float best_dot = -1.0f;
        for (int p = 0; p < 3; ++p) {
            float d = fabsf(vec3_dot(axis, mat3_column(plane_basis, p)));
            if (d > best_dot) {
                best_dot = d;
                best = p;
            }
        }
        out[a] = f[best];
    }
    return vec3(out[0], out[1], out[2]);
}

void scene_scale_selection(Scene *s, Vec3 world_anchor, Vec3 factor, Mat3 plane_basis) {
    Mat3 to_plane = mat3_transposed(plane_basis);

    for (size_t i = 0; i < s->selection.size(); ++i) {
        int id = s->selection[i];
        SceneNode *n = scene_node(s, id);
        if (!n) continue;

        n->scale = vec3_scaled(n->scale, factor_in_node_axes(s, id, plane_basis, factor));

        /* The node origin moves away from the anchor in plane coordinates, so
         * a row of parts grows along the plane rather than along world X/Y. */
        Vec3 world_origin = scene_world_point(s, id, vec3(0.0f, 0.0f, 0.0f));
        Vec3 rel = mat3_apply(to_plane, vec3_sub(world_origin, world_anchor));
        Vec3 moved = vec3_add(world_anchor, mat3_apply(plane_basis, vec3_scaled(rel, factor)));

        Vec3 local = world_delta_to_parent(s, n->parent, world_origin, vec3_sub(moved, world_origin));
        n->position = vec3_add(n->position, local);
    }
}

/* Align and mirror */

static float bounds_component(const Bounds &b, int axis, int slot) {
    Vec3 v;
    if (slot == ALIGN_SLOT_MIN) v = b.min;
    else if (slot == ALIGN_SLOT_MAX) v = b.max;
    else v = bounds_center(b);
    return (axis == 0) ? v.x : (axis == 1) ? v.y : v.z;
}

float scene_align_target(const Bounds &reference, int axis, int slot) {
    return bounds_component(reference, axis, slot);
}

void scene_align_selection(Scene *s, int reference_id, int axis, int slot) {
    const SceneNode *ref = scene_node(s, reference_id);
    if (!ref) return;

    Bounds ref_bounds = scene_node_bounds(s, reference_id);
    if (!ref_bounds.valid) return;
    float target = bounds_component(ref_bounds, axis, slot);

    for (size_t i = 0; i < s->selection.size(); ++i) {
        int id = s->selection[i];
        if (id == reference_id) continue; // the reference is what everything moves to

        Bounds b = scene_node_bounds(s, id);
        if (!b.valid) continue;

        float current = bounds_component(b, axis, slot);
        float shift = target - current;
        if (fabsf(shift) < 1e-6f) continue;

        Vec3 delta = vec3(axis == 0 ? shift : 0.0f,
                          axis == 1 ? shift : 0.0f,
                          axis == 2 ? shift : 0.0f);
        scene_translate_node(s, id, delta);
    }
}

void scene_mirror_selection(Scene *s, int axis, float plane_coord) {
    for (size_t i = 0; i < s->selection.size(); ++i) {
        int id = s->selection[i];
        SceneNode *n = scene_node(s, id);
        if (!n) continue;

        /* Flip the axis of the node's own scale, then reflect its origin about
         * the plane so the geometry lands on the far side. */
        if (axis == 0) n->scale.x = -n->scale.x;
        else if (axis == 1) n->scale.y = -n->scale.y;
        else n->scale.z = -n->scale.z;

        Vec3 world_origin = scene_world_point(s, id, vec3(0.0f, 0.0f, 0.0f));
        float coord = (axis == 0) ? world_origin.x : (axis == 1) ? world_origin.y : world_origin.z;
        float shift = 2.0f * (plane_coord - coord);

        Vec3 delta = vec3(axis == 0 ? shift : 0.0f,
                          axis == 1 ? shift : 0.0f,
                          axis == 2 ? shift : 0.0f);
        scene_translate_node(s, id, delta);
    }
}

/* Boolean merge */

bool scene_node_is_merged(const Scene *s, int id) {
    const SceneNode *n = scene_node(s, id);
    return n && n->merged;
}

/* Rebuilds a mesh with every vertex moved from node-local into "boundary"
 * space. Triangles go back through mesh_add_triangle so normals are recomputed
 * rather than transformed, which keeps them correct under non-uniform scale. */
static Mesh bake_mesh(const Scene *s, int id, int boundary, const Mesh &src) {
    Mesh out;
    for (size_t i = 0; i + 2 < src.vertices.size(); i += 3) {
        mesh_add_triangle(&out,
                          point_up_to(s, id, src.vertices[i + 0], boundary),
                          point_up_to(s, id, src.vertices[i + 1], boundary),
                          point_up_to(s, id, src.vertices[i + 2], boundary));
    }
    for (size_t i = 0; i + 1 < src.edges.size(); i += 2) {
        mesh_add_edge(&out,
                      point_up_to(s, id, src.edges[i + 0], boundary),
                      point_up_to(s, id, src.edges[i + 1], boundary));
    }
    return out;
}

static Mesh unbake_mesh(const Scene *s, int boundary, const Mesh &src) {
    Mesh out;
    for (size_t i = 0; i + 2 < src.vertices.size(); i += 3) {
        mesh_add_triangle(&out,
                          scene_point_from_world(s, boundary, src.vertices[i + 0]),
                          scene_point_from_world(s, boundary, src.vertices[i + 1]),
                          scene_point_from_world(s, boundary, src.vertices[i + 2]));
    }
    for (size_t i = 0; i + 1 < src.edges.size(); i += 2) {
        mesh_add_edge(&out,
                      scene_point_from_world(s, boundary, src.edges[i + 0]),
                      scene_point_from_world(s, boundary, src.edges[i + 1]));
    }
    return out;
}

void scene_collect_meshes(const Scene *s, int id, int boundary, bool inherited_negative,
                          std::vector<Mesh> *positives, std::vector<Mesh> *negatives) {
    const SceneNode *n = scene_node(s, id);
    if (!n || !n->visible) return;

    /* Polarity nests: a negative group flips everything inside it. */
    bool negative = inherited_negative != (n->polarity == POLARITY_NEGATIVE);

    if (!n->mesh.vertices.empty()) {
        Mesh baked = bake_mesh(s, id, boundary, n->mesh);
        if (negative) negatives->push_back(baked);
        else positives->push_back(baked);
    }

    if (n->merged) return; // history does not contribute geometry

    for (size_t i = 0; i < n->children.size(); ++i) {
        scene_collect_meshes(s, n->children[i], boundary, negative, positives, negatives);
    }
}

/* Drops any selected node that already has a selected ancestor, so a group and
 * its own child are never merged against each other. */
static std::vector<int> independent_selection(const Scene *s) {
    std::vector<int> out;
    for (size_t i = 0; i < s->selection.size(); ++i) {
        int id = s->selection[i];
        const SceneNode *n = scene_node(s, id);
        if (!n) continue;

        bool covered = false;
        for (size_t k = 0; k < s->selection.size() && !covered; ++k) {
            if (s->selection[k] == id) continue;
            if (scene_is_ancestor(s, s->selection[k], n->parent)) covered = true;
        }
        if (!covered) out.push_back(id);
    }
    return out;
}

static bool merge_into(Scene *s, int target, const std::vector<int> &sources,
                       std::string *error, std::string *repair_note) {
    std::vector<Mesh> positives, negatives;
    for (size_t i = 0; i < sources.size(); ++i) {
        scene_collect_meshes(s, sources[i], OBC_NO_NODE, false, &positives, &negatives);
    }

    Mesh result;
    if (!csg_merge(positives, negatives, &result, error, repair_note)) return false;

    /* The result is in world space; the node may hang under a transformed
     * parent, so pull it back into that parent's frame. */
    const SceneNode *t = scene_node(s, target);
    int boundary = t ? t->parent : OBC_NO_NODE;
    s->nodes[target].mesh = unbake_mesh(s, boundary, result);
    return true;
}

bool scene_merge_selection(Scene *s, std::string *error, std::string *repair_note) {
    std::vector<int> sources = independent_selection(s);
    if (sources.size() < 2) {
        if (error) *error = "Select two or more objects to merge.";
        return false;
    }

    const SceneNode *first = scene_node(s, sources[0]);
    int parent = first ? first->parent : OBC_NO_NODE;

    int merge_id = scene_add_object(s, parent, "Merge", Mesh(), POLARITY_POSITIVE);
    if (!merge_into(s, merge_id, sources, error, repair_note)) {
        scene_delete_node(s, merge_id);
        return false;
    }

    for (size_t i = 0; i < sources.size(); ++i) scene_reparent(s, sources[i], merge_id);
    s->nodes[merge_id].merged = true;
    s->nodes[merge_id].expanded = false;

    scene_select_only(s, merge_id);
    return true;
}

bool scene_unmerge(Scene *s, int id, std::string *error) {
    SceneNode *n = scene_node(s, id);
    if (!n || !n->merged) {
        if (error) *error = "Select a merged object to unmerge.";
        return false;
    }

    n->merged = false;
    n->kind = NODE_GROUP;
    n->expanded = true;
    mesh_clear(&n->mesh);
    return true;
}

bool scene_remerge(Scene *s, int id, std::string *error, std::string *repair_note) {
    SceneNode *n = scene_node(s, id);
    if (!n || n->merged || n->children.empty()) {
        if (error) *error = "Select an unmerged object to remerge.";
        return false;
    }

    /* Children are collected in world space and pulled back into this node's
     * own frame, since the node keeps whatever transform it has. */
    std::vector<Mesh> positives, negatives;
    for (size_t i = 0; i < n->children.size(); ++i) {
        scene_collect_meshes(s, n->children[i], OBC_NO_NODE, false, &positives, &negatives);
    }

    Mesh result;
    if (!csg_merge(positives, negatives, &result, error, repair_note)) return false;

    s->nodes[id].mesh = unbake_mesh(s, id, result);
    s->nodes[id].merged = true;
    s->nodes[id].kind = NODE_OBJECT;
    s->nodes[id].expanded = false;
    return true;
}

int scene_insert_target(const Scene *s) {
    if (!s->selection.empty()) {
        int id = s->selection.back();
        const SceneNode *n = scene_node(s, id);
        if (n) return (n->kind == NODE_GROUP) ? id : n->parent;
    }
    for (size_t i = 0; i < s->roots.size(); ++i) {
        const SceneNode *n = scene_node(s, s->roots[i]);
        if (n && n->kind == NODE_GROUP) return s->roots[i];
    }
    return OBC_NO_NODE;
}
