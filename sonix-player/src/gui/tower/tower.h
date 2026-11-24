#ifndef GUI_TOWER_H
#define GUI_TOWER_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Tower, from the More page: a block crosses the screen above a tower and a tap
// drops it. What lands on the tower stays, what hangs over the edge is cut off
// and falls, so the blocks get narrower until one misses altogether.
//
// It is drawn with plain LVGL rectangles and labels: there is no artwork to
// load and nothing to free. It makes no sound and leaves the music alone. The
// best score is kept in the config, [tower] best.

extern lv_obj_t *tower_screen;

void tower_init(gui_config_t *cfg);

#endif /* GUI_TOWER_H */
