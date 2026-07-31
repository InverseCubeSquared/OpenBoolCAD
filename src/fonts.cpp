#include "fonts.h"

#include <string.h>

const EmbeddedFont *font_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < OBC_FONT_COUNT; ++i) {
        if (strcmp(OBC_FONTS[i].name, name) == 0) return &OBC_FONTS[i];
    }
    return NULL;
}
