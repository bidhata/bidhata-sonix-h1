#ifndef GUI_HOPPER_H
#define GUI_HOPPER_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Hopper, from the More page: a round creature bounces by itself from ledge to
// ledge, up and up; holding the left or the right half of the screen steers
// it, and going off one side brings it back on the other. A fall past the
// bottom of the screen ends the game.
//
// The picture is made by the game itself -- the creature, the ledges, clouds
// low down and stars high up over a sky that darkens with the height -- so
// there is no artwork to load. It makes no sound and leaves the music alone.
// The best score is kept in the config, [hopper] best.

extern lv_obj_t *hopper_screen;

void hopper_init(gui_config_t *cfg);

#endif /* GUI_HOPPER_H */
