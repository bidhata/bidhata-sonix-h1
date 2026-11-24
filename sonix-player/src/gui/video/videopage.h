#ifndef GUI_VIDEOPAGE_H
#define GUI_VIDEOPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The list of videos: what is in the Video folder of the card.

extern lv_obj_t *videopage_screen;

void videopage_init(gui_config_t *cfg);

#endif /* GUI_VIDEOPAGE_H */
