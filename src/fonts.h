#ifndef OBC_FONTS_H
#define OBC_FONTS_H

/*
 * Fonts baked into the binary from third_party/fonts, by tools/embed_fonts.py.
 *
 * They have to be in the binary rather than beside it: the browser build has no
 * filesystem to read them from, and a desktop build that looked for them
 * relative to the working directory would break the moment it was launched from
 * anywhere else. One arrangement that behaves the same on both platforms beats
 * two that do not.
 *
 * Two things use them - the UI font, and the text generator, which reads glyph
 * outlines out of the same bytes with stb_truetype.
 */

struct EmbeddedFont {
    const char *name;          // file stem, e.g. "LiberationSans-Regular"
    const unsigned char *data;
    unsigned int size;
};

extern const EmbeddedFont OBC_FONTS[];
extern const int OBC_FONT_COUNT;

/* Returns NULL when nothing matches, so a caller can fall back rather than
 * trusting a name that came from a saved file or a stale preset. */
const EmbeddedFont *font_find(const char *name);

#endif
