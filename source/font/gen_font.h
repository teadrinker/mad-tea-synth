/* Auto-generated font data — produced by font_editor export */
/* Font: Assembly Line */
#ifndef GEN_FONT_H
#define GEN_FONT_H

#include "glyph_render.h"

void get_font_assembly_line(Tsys *sys, FONT_SETTINGS *dst);
void get_font_idealist_hacker_mono(Tsys *sys, FONT_SETTINGS *dst);

// The built-in fonts by index, 0 = Assembly Line. Names are short, for pickers.
#define GEN_FONT_COUNT 2
extern const char *const gen_font_names[GEN_FONT_COUNT];
void get_font_by_index(Tsys *sys, int index, FONT_SETTINGS *dst);

#endif /* GEN_FONT_H */
