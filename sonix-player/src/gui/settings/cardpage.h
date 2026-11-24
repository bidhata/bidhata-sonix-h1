#ifndef CARDPAGE_H
#define CARDPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The microSD card's own page, under Settings > System: how full it is, what
// it is formatted as, whether it was taken out without being unmounted, a
// short speed test, and formatting it.
//
// What the page does not offer, and why: checking and repairing a filesystem
// (the device has no tool for it, and one needs more memory than the player
// has for any card of a usual size), and trimming (the card driver takes no
// discard requests). For both the page points at a computer.
extern lv_obj_t *cardpage_screen;

void cardpage_init(gui_config_t *cfg);

#endif /* CARDPAGE_H */
