#ifndef OBC_SCENE_H
#define OBC_SCENE_H

#include <string>
#include <vector>
#include "mesh.h"

enum NodeKind {
    NODE_OBJECT = 0,
    NODE_GROUP,
    NODE_DELETED
};

enum Polarity {
    POLARITY_POSITIVE = 0,
    POLARITY_NEGATIVE
};

/*
 * Nodes live in one flat vector and are addressed by id, where id is the index
 * into that vector. Deleting tombstones the slot (NODE_DELETED) so ids stay
 * stable for the history records that will reference them.
 */
#define OBC_NO_NODE (-1)

/*
 * A merged object is a NODE_OBJECT that still owns children. Its own mesh is
 * the boolean result; the children are the history it was built from, kept in
 * full so the merge can be undone at any point in any later session. While
 * "merged" is set, the children do not render, pick, or show in the tree -
 * everything downstream keys off scene_node_is_merged().
 */
/*
 * Display colours. A node's default depends on its polarity, so placing a
 * primitive looks exactly as it did before anyone thought about colour: a solid
 * is the pale blue-grey, a hole the neutral grey it draws transparent in.
 * Selection still overrides both, since that feedback has to win.
 */
#define OBC_COLOR_SOLID vec3(0.68f, 0.74f, 0.80f)
#define OBC_COLOR_HOLE  vec3(0.55f, 0.55f, 0.55f)

struct SceneNode {
    int id;
    int parent;
    NodeKind kind;
    std::string name;
    bool visible;
    bool expanded;
    bool merged;
    Polarity polarity;
    Vec3 color; // rgb, 0..1

    Vec3 position;
    Vec3 rotation; // degrees, applied Z then Y then X
    Vec3 scale;

    Mesh mesh; // objects only; groups aggregate their children
    std::vector<int> children;
};

struct Scene {
    std::vector<SceneNode> nodes;
    std::vector<int> roots;
    std::vector<int> selection;
};

void scene_init(Scene *s);

/* An empty project: one "Main" group and nothing in it. This is what the editor
 * starts with and what File > New returns to. */
void scene_new_empty(Scene *s);

/* Sample content, used by the tests rather than at startup. */
void scene_seed_demo(Scene *s);

SceneNode *scene_node(Scene *s, int id);
const SceneNode *scene_node(const Scene *s, int id);

int scene_add_object(Scene *s, int parent, const char *name, const Mesh &mesh, Polarity polarity);
int scene_add_group(Scene *s, int parent, const char *name);
void scene_delete_node(Scene *s, int id);

/*
 * Copy and paste of whole subtrees.
 *
 * A fragment is a standalone node list whose parent and children fields index
 * *within the fragment*, so it survives the scene changing underneath it - ids
 * in the live scene are vector indices and a paste has to mint new ones anyway.
 */
void scene_copy_subtrees(const Scene *s, const std::vector<int> &ids,
                         std::vector<SceneNode> *fragment);
void scene_paste_subtrees(Scene *s, int parent, const std::vector<SceneNode> &fragment,
                          std::vector<int> *new_roots);

/* Reparenting. Rejects moves that would put a node inside its own subtree. */
bool scene_can_reparent(const Scene *s, int id, int new_parent);
void scene_reparent(Scene *s, int id, int new_parent);

/* Selection. Hidden nodes can never enter the selection. */
bool scene_is_selected(const Scene *s, int id);
void scene_select_only(Scene *s, int id);
void scene_select_toggle(Scene *s, int id);
void scene_select_clear(Scene *s);
void scene_select_all_visible(Scene *s);
/* Replaces the selection wholesale, dropping hidden nodes and duplicates. Lets
 * a caller that recomputes its whole result each frame - the rubber band - hand
 * the list over without tracking what changed. */
void scene_select_set(Scene *s, const std::vector<int> &ids);
bool scene_is_effectively_visible(const Scene *s, int id);

void scene_set_visible(Scene *s, int id, bool visible);
void scene_invert_selection_polarity(Scene *s);

/*
 * Colour of the selection.
 *
 * Applied down each selected subtree, because a group has no mesh of its own -
 * colouring one has to mean colouring what is in it, or the action would do
 * nothing on exactly the node the tree makes easiest to select. Returns how
 * many objects it reached, so the caller can report an edit that hit nothing.
 */
int scene_set_selection_color(Scene *s, Vec3 color);

/* The colour to show for the current selection: the shared one where every
 * object in it agrees, and the first object's otherwise, so the picker opens on
 * something recognisable rather than on a default. */
bool scene_selection_color(const Scene *s, Vec3 *out);

/* World transform helpers. Nested transforms compose through the parents. */
Vec3 scene_world_point(const Scene *s, int id, Vec3 local);
Bounds scene_node_bounds(const Scene *s, int id);
Bounds scene_selection_bounds(const Scene *s);

/*
 * The node's orientation in world space, parents included. Scale is left out:
 * a non-uniform scale on an ancestor would shear the axes, and the editing
 * plane is defined as a rotation.
 */
Mat3 scene_world_rotation(const Scene *s, int id);

/*
 * Bounds measured in a rotated frame rather than along the world axes, for the
 * workplane-relative selection box. "world_to_plane" is the inverse of the
 * plane basis; the result is in plane coordinates and maps back with the
 * forward basis.
 */
Bounds scene_node_bounds_in(const Scene *s, int id, Mat3 world_to_plane);
Bounds scene_selection_bounds_in(const Scene *s, Mat3 world_to_plane);

/* The tree level a new primitive is inserted at: the selected group, the
 * selected object's parent, or the first root group. */
int scene_insert_target(const Scene *s);

/* Raycast against visible objects. Returns the hit node id, or OBC_NO_NODE.
 * Hidden objects are skipped, so they can never be picked. */
int scene_pick(const Scene *s, Vec3 origin, Vec3 dir, float *out_distance);

/* Same raycast, also reporting the world normal of the triangle that was hit.
 * That is what SHIFT+P needs to lay the workplane onto a face. */
int scene_pick_surface(const Scene *s, Vec3 origin, Vec3 dir,
                       float *out_distance, Vec3 *out_normal);

/*
 * Centre of the flat face a hit belongs to: the area weighted centroid of every
 * triangle of that node lying in the same plane.
 *
 * Coplanarity alone, without walking adjacency - two separate faces of one node
 * that share a plane *and* a normal would have to be parallel and facing the
 * same way, which a closed solid does not do at the same offset.
 */
Vec3 scene_face_center(const Scene *s, int id, Vec3 world_point, Vec3 world_normal);

/* Boolean merge. See the SceneNode comment for how history is stored. */
bool scene_node_is_merged(const Scene *s, int id);

/* Merges the selection into one node and selects it. The selection needs two
 * or more independent nodes and at least one positive volume. */
bool scene_merge_selection(Scene *s, std::string *error, std::string *repair_note);

/* Ctrl+Z and Ctrl+Y on a selected merged node. Unmerge exposes the history
 * again; remerge recomputes the result from it. */
bool scene_unmerge(Scene *s, int id, std::string *error);
bool scene_remerge(Scene *s, int id, std::string *error, std::string *repair_note);

/*
 * Flattened copy of every visible volume under a node, split by polarity.
 * Transforms are baked up to "boundary" (an ancestor id, or OBC_NO_NODE for
 * world space). Merged nodes contribute their result mesh and their history is
 * not descended into. Used by the merge and, later, by the STL exporter.
 */
void scene_collect_meshes(const Scene *s, int id, int boundary, bool inherited_negative,
                          std::vector<Mesh> *positives, std::vector<Mesh> *negatives);

/* Local space of "boundary" -> world, and back. The inverse is what lets a
 * merge result computed in world space be stored on a node that sits under a
 * transformed parent. */
Vec3 scene_point_from_world(const Scene *s, int boundary, Vec3 world);

/*
 * Editing handles on the selection box: 8 corners then 6 face centers.
 *
 * Both the renderer and the picker read them from here, so the markers drawn
 * and the grips that respond can never drift apart. "anchor" is the point that
 * stays put while this handle is dragged, and "mask" is 1 on each axis the
 * handle scales.
 */
#define OBC_HANDLE_COUNT 14

struct TransformHandle {
    Vec3 pos;
    Vec3 anchor;
    Vec3 mask;
};

TransformHandle scene_handle(const Bounds &b, int index);

/*
 * Selection edits. All three take world space arguments and write back through
 * each node's parent chain, so a node under a transformed group still follows
 * the pointer.
 */
void scene_translate_selection(Scene *s, Vec3 world_delta);
void scene_translate_node(Scene *s, int id, Vec3 world_delta);
void scene_rotate_selection(Scene *s, Vec3 world_center, Vec3 world_axis, float degrees);

/*
 * Scale about an anchor, with the factors given in the active plane's axes.
 *
 * A node whose own axes line up with the plane takes them componentwise, which
 * is exactly what object-relative mode arranges and what the workplate gives an
 * unrotated node. Any other orientation cannot be expressed by a per-axis scale
 * at all, so each node axis follows whichever plane axis it lines up with most.
 * That is the same approximation the world-axis version always made, only now
 * measured against the plane rather than the world - which means the case that
 * used to be wrong, scaling a rotated object, is now the case that is exact.
 */
void scene_scale_selection(Scene *s, Vec3 world_anchor, Vec3 factor, Mat3 plane_basis);

/*
 * Align and mirror.
 *
 * Alignment moves every selected node except the reference so that the chosen
 * feature of its bounding box - low edge, centre or high edge - matches the
 * same feature of the reference. Only the one axis moves.
 *
 * Mirroring reflects the selection across a plane perpendicular to an axis,
 * which negates that axis of each node's scale. A mirrored solid has inverted
 * winding; nothing downstream cares, because shading is two sided and the
 * repair pass flips inside-out shells before any boolean.
 */
#define ALIGN_SLOT_MIN 0
#define ALIGN_SLOT_CENTER 1
#define ALIGN_SLOT_MAX 2

float scene_align_target(const Bounds &reference, int axis, int slot);
void scene_align_selection(Scene *s, int reference_id, int axis, int slot);
void scene_mirror_selection(Scene *s, int axis, float plane_coord);

#endif
