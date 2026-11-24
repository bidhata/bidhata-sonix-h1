#ifndef FWUPDATE_H
#define FWUPDATE_H

#include <stdbool.h>

// The "Update firmware" card: from the internet (ota.h) or from the card
// (firmware.h), over a dimmed page.
//
//   choice      Via internet / From SD / Cancel
//   internet    checking -> the newest release, its notes and an Update
//               button, or why there is none -> the download with a progress
//               bar -> recovery, exactly as from the card
//   from SD     the file on the card, or a notice that there is none
//
// The screen stays on while the card is open.

void fwupdate_show(void);

// Whether there is charge enough to start an update: at least 30 %, or on the
// charger. Asked when the card opens and again before each step that commits
// to the update, since the card can stay open for as long as it likes. True
// when the level cannot be read.
bool fwupdate_battery_ok(void);

#endif /* FWUPDATE_H */
