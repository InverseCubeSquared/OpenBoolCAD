#ifndef OBC_IMPORT_STL_H
#define OBC_IMPORT_STL_H

#include <string>

#include "mesh.h"

/*
 * STL import, binary or ASCII (detected from the file, not the extension).
 *
 * STL has no topology: every triangle carries its own three vertices, so a
 * loaded file is a soup of unshared corners. It is put through mesh_repair(),
 * which welds it into a closed solid where it can and reports what it could
 * not, and feature edges are derived by dihedral angle so imported meshes get
 * the same thin black outline as the primitives.
 *
 * "note" receives the repair summary, empty when nothing needed fixing.
 */
bool import_stl(const char *path, Mesh *out, std::string *error, std::string *note);

#endif
