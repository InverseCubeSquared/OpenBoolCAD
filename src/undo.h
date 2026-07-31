#ifndef OBC_UNDO_H
#define OBC_UNDO_H

#include <string>
#include <vector>

#include "scene.h"

/*
 * Global undo, separate from merge history.
 *
 * These are two different ideas and the editor needs both. Merge history is
 * part of the object model: it is persisted, and it is what CTRL+Z acts on when
 * a merged object is selected (see PLAN.md). This stack is editor level and not
 * persisted: it exists so an align, a mirror, a delete or a drag can be taken
 * back.
 *
 * Whole-scene snapshots rather than per-action deltas. A delta system needs an
 * inverse for every operation and gets a class of bugs where the inverse is
 * subtly wrong; a snapshot cannot be wrong, at the price of memory. The stack is
 * bounded by both entry count and total bytes so an imported STL cannot let it
 * grow without limit.
 */

#define UNDO_MAX_ENTRIES 64
#define UNDO_MAX_BYTES (512u * 1024u * 1024u)

struct UndoEntry {
    Scene scene;
    std::string label;
};

struct UndoStack {
    std::vector<UndoEntry> past;
    std::vector<UndoEntry> future;
};

void undo_init(UndoStack *u);

/* Snapshots the scene as it is now. Call before mutating, naming the action the
 * way it should read in the Edit menu ("Align", "Delete", "Move"). */
void undo_record(UndoStack *u, const Scene &scene, const char *label);

/* Both return false when there is nothing to step to. The current scene is
 * handed in so it can be pushed onto the opposite stack. */
bool undo_step_back(UndoStack *u, Scene *scene, std::string *label);
bool undo_step_forward(UndoStack *u, Scene *scene, std::string *label);

bool undo_can_step_back(const UndoStack *u);
bool undo_can_step_forward(const UndoStack *u);
const char *undo_back_label(const UndoStack *u);
const char *undo_forward_label(const UndoStack *u);

#endif
