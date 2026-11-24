#ifndef CARDPAGE_H
#define CARDPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The microSD card's own page, under Settings > System: how full it is, what
// it is formatted as, whether it was taken out without being unmounted, a
// check of its filesystem, a short speed test, and formatting it.
//
// The check is the player's own and does one repair: it frees the space of
// clusters no file owns, on FAT32 and exFAT cards (system/device/fatcheck.h). The usual tools do more
// and are not here because they need more memory than the player has for a
// card of any usual size; for what the check finds and cannot repair the page
// points at a computer. Trimming is not offered either: the card driver takes
// no discard requests.
extern lv_obj_t *cardpage_screen;

void cardpage_init(gui_config_t *cfg);

#endif /* CARDPAGE_H */
