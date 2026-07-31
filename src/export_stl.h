#ifndef OBC_EXPORT_STL_H
#define OBC_EXPORT_STL_H

#include <string>
#include <vector>

#include "scene.h"

/*
 * Binary STL export of the given nodes.
 *
 * A group is exported in merged state: the volumes under it are flattened and
 * put through the boolean merge in the exporter's own scratch space, so the
 * scene keeps whatever merge state it had. Negative volumes are subtracted
 * rather than written out, since an STL is one solid surface and a hole is not
 * a separate object in it.
 */
bool export_stl(const char *path, const Scene &scene, const std::vector<int> &nodes,
                std::string *error, std::string *note);

#endif
