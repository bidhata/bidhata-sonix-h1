#ifndef ACCENTPAGE_H
#define ACCENTPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The picker behind Appearance's seventh swatch: a preview of the chosen
// colour and sliders for hue, saturation and brightness. The interface follows
// the finger on release; OK commits the colour, and leaving any other way puts
// the persisted accent back.

extern lv_obj_t *accentpage_screen;

void accentpage_init(gui_config_t *cfg);

#endif /* ACCENTPAGE_H */
