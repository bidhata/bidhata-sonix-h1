#ifndef GUI_VIDEOPLAY_H
#define GUI_VIDEOPLAY_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The screen a video plays on: the picture over the whole panel, and a handful
// of controls that come up over it at a touch.
//
// A picture wider than tall is shown turned a quarter turn (see video.h), and
// the controls turn with it, so that held sideways everything reads the right
// way up.

extern lv_obj_t *videoplay_screen;

void videoplay_init(gui_config_t *cfg);

// Starts a file and opens the screen. A film left part way through carries on
// from a few seconds before where it was left (src/system/video/videoresume).
void videoplay_open(const char *path);

// A hardware or remote key while this screen is up: play/pause is play/pause,
// next and previous skip ten seconds. False when the screen is not up, and the
// key is the music player's as usual.
bool videoplay_key(gui_key_t key);

#endif /* GUI_VIDEOPLAY_H */
