#ifndef OBC_PROJECT_H
#define OBC_PROJECT_H

#include <string>
#include <vector>

#include "camera.h"
#include "scene.h"

/*
 * Save format, per PLAN.md: a thumbnail followed by a GZIP compressed SQLite
 * database.
 *
 * Container, little endian, fixed size header:
 *
 *   0   8   magic "OBCAD\0\0\0"
 *   8   4   format version
 *   12  4   thumbnail width in pixels
 *   16  4   thumbnail height in pixels
 *   20  4   compressed thumbnail length
 *   24  8   compressed database length
 *   32  ... gzip stream: thumbnail, raw RGB rows top to bottom
 *   ...     gzip stream: the SQLite database file
 *
 * The thumbnail comes first and its length is in the header, so a file browser
 * can read the preview without touching the database.
 *
 * The database holds the whole node tree including the children of merged
 * nodes - which is where merge history lives (see scene.h) - plus each node's
 * current state mesh. That is what lets a session resume exactly where it
 * stopped and any earlier merge still be undone.
 */

#define OBC_PROJECT_VERSION 1

struct Thumbnail {
    int width;
    int height;
    std::vector<unsigned char> rgb; // 3 bytes per pixel, top row first
};

bool project_save(const char *path, const Scene &scene, const Camera &camera,
                  const Thumbnail &thumb, std::string *error);

bool project_load(const char *path, Scene *scene, Camera *camera, std::string *error);

/* Reads just the preview out of a project file, without the database. */
bool project_read_thumbnail(const char *path, Thumbnail *out, std::string *error);

#endif
