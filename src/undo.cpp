#include "undo.h"

/* Rough footprint of a snapshot, dominated by mesh data. */
static size_t scene_bytes(const Scene &s) {
    size_t total = s.nodes.size() * sizeof(SceneNode);
    for (size_t i = 0; i < s.nodes.size(); ++i) {
        const Mesh &m = s.nodes[i].mesh;
        total += m.vertices.size() * sizeof(Vec3);
        total += m.normals.size() * sizeof(Vec3);
        total += m.edges.size() * sizeof(Vec3);
        total += s.nodes[i].children.size() * sizeof(int);
        total += s.nodes[i].name.capacity();
    }
    return total;
}

static size_t stack_bytes(const std::vector<UndoEntry> &entries) {
    size_t total = 0;
    for (size_t i = 0; i < entries.size(); ++i) total += scene_bytes(entries[i].scene);
    return total;
}

/* Oldest entries go first: the recent ones are the ones anybody reaches for. */
static void trim(std::vector<UndoEntry> *entries) {
    while (entries->size() > UNDO_MAX_ENTRIES) entries->erase(entries->begin());
    while (entries->size() > 1 && stack_bytes(*entries) > UNDO_MAX_BYTES) {
        entries->erase(entries->begin());
    }
}

void undo_init(UndoStack *u) {
    u->past.clear();
    u->future.clear();
}

void undo_record(UndoStack *u, const Scene &scene, const char *label) {
    UndoEntry entry;
    entry.scene = scene;
    entry.label = label ? label : "Edit";
    u->past.push_back(entry);
    trim(&u->past);

    /* A fresh action abandons the redo branch, as everywhere else. */
    u->future.clear();
}

bool undo_step_back(UndoStack *u, Scene *scene, std::string *label) {
    if (u->past.empty()) return false;

    UndoEntry entry = u->past.back();
    u->past.pop_back();

    UndoEntry current;
    current.scene = *scene;
    current.label = entry.label;
    u->future.push_back(current);
    trim(&u->future);

    *scene = entry.scene;
    if (label) *label = entry.label;
    return true;
}

bool undo_step_forward(UndoStack *u, Scene *scene, std::string *label) {
    if (u->future.empty()) return false;

    UndoEntry entry = u->future.back();
    u->future.pop_back();

    UndoEntry current;
    current.scene = *scene;
    current.label = entry.label;
    u->past.push_back(current);
    trim(&u->past);

    *scene = entry.scene;
    if (label) *label = entry.label;
    return true;
}

bool undo_can_step_back(const UndoStack *u) { return !u->past.empty(); }
bool undo_can_step_forward(const UndoStack *u) { return !u->future.empty(); }

const char *undo_back_label(const UndoStack *u) {
    return u->past.empty() ? "" : u->past.back().label.c_str();
}

const char *undo_forward_label(const UndoStack *u) {
    return u->future.empty() ? "" : u->future.back().label.c_str();
}
