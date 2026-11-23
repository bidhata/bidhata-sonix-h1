#ifndef RADIO_VOLUME_H
#define RADIO_VOLUME_H

#include "src/system/audio/swvolume.h"

// Radio always opens the output in stereo, and it runs the equaliser before
// this call. The volume belongs to the player here: the stream is decoded on
// the device, so the level is the one the listener set for the USB-C output.
static inline void radio_volume_apply(void *samples, int frames, int bits)
{
	if (!swvolume_active())
		return;
	if (bits == 16)
		swvolume_apply_s16((short *)samples, frames * 2);
	else if (bits == 32)
		swvolume_apply_s32((int32_t *)samples, frames * 2);
}

#endif /* RADIO_VOLUME_H */
