// The C facade over Gearboy. See gbcore.h.
//
// The only C++ file written here: everything else under src/gb/core is upstream
// Gearboy verbatim, and stays that way so updating it is a copy and not a
// merge.

#include "gbcore.h"

#include <new>
#include <sstream>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unistd.h>

#include "core/Cartridge.h"
#include "core/GearboyCore.h"
#include "core/Memory.h"
#include "core/MemoryRule.h"
#include "core/definitions.h"

// Gearboy expects the host program to define this: its Log_func() checks it to
// decide whether to stay quiet. True here because the player has no console to
// print to, and a printf per core event on the emulation thread would cost more
// than the emulation.
bool g_mcp_stdio_mode = true;

struct gb_core {
	GearboyCore *core;
	char name[24];
	int sample_rate;
	// Key state from the previous frame: the core wants press/release edges
	// while callers pass a full state mask, so the difference is taken here.
	uint16_t keys;
	// A hash of the battery RAM as it was last loaded or written, so a flush
	// asked for while nothing has changed costs no write to the card.
	uint32_t ram_hash;
	bool ram_hash_valid;
	// The core always needs somewhere to put audio, even when the caller does
	// not want it: passing NULL would still be written through.
	s16 scratch[GB_AUDIO_MAX_SAMPLES];
};

// The order does not affect correctness, only readability: the same eight
// Gameboy_Keys values, listed the way a thumb reaches them.
static const struct {
	uint16_t bit;
	Gameboy_Keys key;
} KEY_MAP[] = {
	{GB_KEY_UP, Up_Key},		 {GB_KEY_DOWN, Down_Key},	  {GB_KEY_LEFT, Left_Key},
	{GB_KEY_RIGHT, Right_Key},   {GB_KEY_A, A_Key},			  {GB_KEY_B, B_Key},
	{GB_KEY_SELECT, Select_Key}, {GB_KEY_START, Start_Key},
};

extern "C" {

gb_core_t *gb_core_create(void) {
	gb_core_t *wrap = new (std::nothrow) gb_core_t;
	if (!wrap) {
		return NULL;
	}
	memset(wrap, 0, sizeof(*wrap));
	wrap->sample_rate = 44100;

	wrap->core = new (std::nothrow) GearboyCore();
	if (!wrap->core) {
		delete wrap;
		return NULL;
	}

	// RGB565 straight from the core: it is the framebuffer format, so no
	// per-pixel conversion is left between the emulator and the panel.
	wrap->core->Init(GB_PIXEL_RGB565);

	// No Super Game Boy. Two calls rather than one, deliberately:
	//
	//   SetSGBEnabled(false)  turns the mode off entirely. This is the one that
	//       matters: without it an SGB-enhanced cartridge (ROM byte 0x146 equal
	//       to 3) enables it by itself, and RenderSGBFrame() starts writing
	//       256x224 into a 160x144 buffer -- 68 KB past the end, every frame.
	//   SetSGBBorder(false)   drops the border. If SGB is ever re-enabled, with
	//       the border off the core draws into its own buffer and copies out
	//       160x144: same path, without the overrun.
	//
	// Both must be set before loading the ROM, where m_bSGB is computed.
	//
	// Nothing is lost as the page stands: the SGB border is 256x224 while the
	// game view is sized for 160x144 at 3x. An SGB game still runs, in plain
	// Game Boy mode, which is how it ran on a Game Boy.
	wrap->core->SetSGBEnabled(false);
	wrap->core->SetSGBBorder(false);

	return wrap;
}

void gb_core_destroy(gb_core_t *wrap) {
	if (!wrap) {
		return;
	}
	delete wrap->core;
	delete wrap;
}

bool gb_core_load_file(gb_core_t *wrap, const char *path, bool force_dmg) {
	if (!wrap || !path || !path[0]) {
		return false;
	}
	if (!wrap->core->LoadROM(path, force_dmg)) {
		return false;
	}

	// After the load, not before: loading a ROM rebuilds the audio unit, so a
	// sample rate set earlier would be lost.
	wrap->core->SetSoundSampleRate(wrap->sample_rate);

	const char *name = wrap->core->GetCartridge() ? wrap->core->GetCartridge()->GetName() : "";
	snprintf(wrap->name, sizeof(wrap->name), "%s", name ? name : "");
	wrap->keys = 0;
	return true;
}

const char *gb_core_rom_name(gb_core_t *wrap) { return wrap ? wrap->name : ""; }

bool gb_core_is_color(gb_core_t *wrap) { return wrap ? wrap->core->IsCGB() : false; }

// Where a file of this cartridge goes in `dir`: the ROM's whole file name, its
// own extension included, with `suffix` after it ("Zelda.gbc.sav"). The
// extension stays because Games/GB/Zelda.gb and Games/GBC/Zelda.gbc are two
// cartridges sharing one saves folder.
//
// With `legacy` it is the name without the ROM's extension ("Zelda.sav"), the
// one Gearboy itself gives a save: read when the other does not exist, never
// written.
static std::string save_path(GearboyCore *core, const char *dir, const char *suffix, bool legacy) {
	std::string path = dir;
	path += "/";
	path += core->GetCartridge()->GetFileName();
	if (legacy) {
		std::string::size_type dot = path.rfind('.');
		if (dot != std::string::npos) {
			path.erase(dot);
		}
	}
	path += suffix;
	return path;
}

static bool file_exists(const std::string &path) { return access(path.c_str(), F_OK) == 0; }

// The whole file or nothing: written beside its place, flushed to the card and
// renamed over the old one, so a full card or a power cut leaves the file that
// was there.
static bool write_whole_file(const std::string &path, const void *data, size_t size) {
	std::string tmp = path + ".new";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f) {
		return false;
	}
	bool ok = fwrite(data, 1, size, f) == size;
	ok = fflush(f) == 0 && ok;
	ok = fsync(fileno(f)) == 0 && ok;
	// The close matters as much as the write: it is the last place a failed
	// write is reported.
	if (fclose(f) != 0) {
		ok = false;
	}
	if (ok && rename(tmp.c_str(), path.c_str()) != 0) {
		ok = false;
	}
	if (!ok) {
		remove(tmp.c_str());
	}
	return ok;
}

// The battery RAM as the core writes it to a .sav, in memory. False for a
// cartridge that keeps nothing.
static bool ram_image(GearboyCore *core, std::string &out, uint32_t *hash) {
	Cartridge *cart = core->GetCartridge();
	Memory *mem = core->GetMemory();
	MemoryRule *rule = mem ? mem->GetCurrentRule() : NULL;
	if (!cart || !rule || !cart->IsLoadedROM()) {
		return false;
	}
	if (!cart->HasBattery() && rule->GetMapperType() != Cartridge::CartridgeMBC6) {
		return false;
	}

	std::ostringstream stream(std::ios::out | std::ios::binary);
	rule->SaveRam(stream);
	out = stream.str();
	if (out.empty()) {
		return false;
	}

	// Over the RAM alone: a clock cartridge appends its clock, which differs
	// at every call and is not a reason to write.
	size_t n = rule->GetRamSize();
	if (n == 0 || n > out.size()) {
		n = out.size();
	}
	uint32_t h = 2166136261u; // FNV-1a
	for (size_t i = 0; i < n; i++) {
		h = (h ^ (uint8_t)out[i]) * 16777619u;
	}
	*hash = h;
	return true;
}

// Gearboy tells a directory from a full path with a boolean: with fullPath
// false it appends the cartridge's file name to whatever it is given and swaps
// the extension for "sav": the legacy name, read when there is no file under
// the full one. NULL falls back to its default, the file next to the ROM.
void gb_core_load_ram(gb_core_t *wrap, const char *dir) {
	if (!wrap) {
		return;
	}
	if (dir && dir[0] && wrap->core->GetCartridge()) {
		std::string path = save_path(wrap->core, dir, ".sav", false);
		if (file_exists(path)) {
			wrap->core->LoadRam(path.c_str(), true);
		} else {
			wrap->core->LoadRam(dir, false);
		}
	} else {
		wrap->core->LoadRam(NULL, false);
	}

	std::string image;
	wrap->ram_hash_valid = ram_image(wrap->core, image, &wrap->ram_hash);
}

static bool save_ram(gb_core_t *wrap, const char *dir, bool only_if_changed) {
	if (!wrap) {
		return false;
	}
	std::string image;
	uint32_t hash = 0;
	if (!ram_image(wrap->core, image, &hash)) {
		return true; // nothing to keep
	}
	if (only_if_changed && wrap->ram_hash_valid && hash == wrap->ram_hash) {
		return true;
	}
	if (!dir || !dir[0]) {
		// No saves directory: the core's own file, next to the ROM.
		wrap->core->SaveRam(NULL, false);
	} else if (!write_whole_file(save_path(wrap->core, dir, ".sav", false), image.data(), image.size())) {
		return false;
	}
	wrap->ram_hash = hash;
	wrap->ram_hash_valid = true;
	return true;
}

bool gb_core_save_ram(gb_core_t *wrap, const char *dir) { return save_ram(wrap, dir, false); }

bool gb_core_save_ram_if_changed(gb_core_t *wrap, const char *dir) { return save_ram(wrap, dir, true); }

// The save-state path: the ROM's file name with ".state<index>" after it, or,
// with `legacy`, in place of its extension (see save_path). Without a saves
// directory it is the file next to the ROM, extension replaced.
static std::string state_path(GearboyCore *core, const char *dir, int index, bool legacy) {
	char suffix[24];
	snprintf(suffix, sizeof(suffix), ".state%d", index);

	if (dir && dir[0]) {
		return save_path(core, dir, suffix, legacy);
	}

	std::string path = core->GetCartridge()->GetFilePath();
	std::string::size_type dot = path.rfind('.');
	if (dot != std::string::npos) {
		path.erase(dot);
	}
	path += suffix;
	return path;
}

// Nothing the core writes comes near this; a file larger is not a state, and
// its size is not a number to allocate by.
#define STATE_MAX_BYTES (8 * 1024 * 1024)

// The state is serialised through a memory buffer instead of the std::ofstream
// Gearboy would use on its own.
//
// The reason is the card. Serialisation is hundreds of small stream.write()
// calls -- a register, a byte, a block -- and on an ofstream opened on the
// microSD each one eventually becomes a real write. Serialising into RAM and
// handing it over with a single fwrite turns hundreds of trips to the card into
// one.
//
// Gearboy already supports this: SaveState(buffer, size, screenshot) with a
// NULL buffer computes the size, and with a real buffer fills it. That is two
// serialisations instead of one, but both in memory, which on this device is
// orders of magnitude faster than the card.
//
// The last argument is the embedded thumbnail: false, since it is another 92 KB
// per state and this menu does not show it.
bool gb_core_save_state(gb_core_t *wrap, const char *dir, int index) {
	if (!wrap || !wrap->core->GetCartridge()) {
		return false;
	}

	size_t size = 0;
	if (!wrap->core->SaveState(NULL, size, false) || size == 0) {
		return false;
	}

	u8 *buffer = new (std::nothrow) u8[size];
	if (!buffer) {
		return false;
	}

	bool ok = wrap->core->SaveState(buffer, size, false);
	if (ok) {
		ok = write_whole_file(state_path(wrap->core, dir, index, false), buffer, size);
	}

	delete[] buffer;
	return ok;
}

// The same in reverse: the file is read in one go and handed to the core,
// rather than letting it read a field at a time off the card.
bool gb_core_load_state(gb_core_t *wrap, const char *dir, int index) {
	if (!wrap || !wrap->core->GetCartridge()) {
		return false;
	}

	std::string path = state_path(wrap->core, dir, index, false);
	if (!file_exists(path)) {
		path = state_path(wrap->core, dir, index, true);
	}
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) {
		return false;
	}

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || size > STATE_MAX_BYTES) {
		fclose(f);
		return false;
	}

	u8 *buffer = new (std::nothrow) u8[(size_t)size];
	if (!buffer) {
		fclose(f);
		return false;
	}

	bool ok = fread(buffer, 1, (size_t)size, f) == (size_t)size;
	fclose(f);

	if (ok) {
		ok = wrap->core->LoadState(buffer, (size_t)size);
	}

	delete[] buffer;
	return ok;
}

void gb_core_set_keys(gb_core_t *wrap, uint16_t mask) {
	if (!wrap) {
		return;
	}

	uint16_t changed = (uint16_t)(mask ^ wrap->keys);
	if (!changed) {
		return;
	}

	for (size_t i = 0; i < sizeof(KEY_MAP) / sizeof(KEY_MAP[0]); i++) {
		if (!(changed & KEY_MAP[i].bit)) {
			continue;
		}
		if (mask & KEY_MAP[i].bit) {
			wrap->core->KeyPressed(KEY_MAP[i].key);
		} else {
			wrap->core->KeyReleased(KEY_MAP[i].key);
		}
	}

	wrap->keys = mask;
}

void gb_core_set_sample_rate(gb_core_t *wrap, int rate) {
	if (wrap) {
		wrap->sample_rate = rate;
		wrap->core->SetSoundSampleRate(rate);
	}
}

void gb_core_run_frame(gb_core_t *wrap, uint16_t *pixels, int16_t *audio, int *audio_samples) {
	if (!wrap) {
		return;
	}

	int count = 0;
	s16 *sink = audio ? reinterpret_cast<s16 *>(audio) : wrap->scratch;
	wrap->core->RunToVBlank(reinterpret_cast<u16 *>(pixels), sink, &count, false, NULL, pixels != NULL);

	if (audio_samples) {
		// How many the core produced, not how many were copied out: the APU
		// runs inside the CPU loop regardless, and this count is what paces
		// time (738 per channel at 59.7 frames per second).
		*audio_samples = count;
	}
}

void gb_core_set_dmg_palette(gb_core_t *wrap, const uint8_t colors[4][3]) {
	if (!wrap || !colors) {
		return;
	}
	GB_Color c[4];
	for (int i = 0; i < 4; i++) {
		c[i].red = colors[i][0];
		c[i].green = colors[i][1];
		c[i].blue = colors[i][2];
	}
	wrap->core->SetDMGPalette(c[0], c[1], c[2], c[3]);
}

void gb_core_set_color_correction(gb_core_t *wrap, bool enabled) {
	if (wrap) {
		wrap->core->EnableColorCorrection(enabled);
	}
}

bool gb_core_set_bootroms(gb_core_t *wrap, const char *dmg_path, const char *cgb_path) {
	if (!wrap || !dmg_path || !cgb_path) {
		return false;
	}
	// The files are checked here because Memory::LoadBootroom only logs when a
	// file is missing, and enabling a boot ROM that never loaded gives a white
	// screen when the game starts.
	FILE *a = fopen(dmg_path, "rb");
	FILE *b = fopen(cgb_path, "rb");
	bool ok = a != NULL && b != NULL;
	if (a) {
		fclose(a);
	}
	if (b) {
		fclose(b);
	}
	if (!ok) {
		return false;
	}
	Memory *mem = wrap->core->GetMemory();
	if (!mem) {
		return false;
	}
	mem->LoadBootromDMG(dmg_path);
	mem->LoadBootromGBC(cgb_path);
	mem->EnableBootromDMG(true);
	mem->EnableBootromGBC(true);
	return true;
}

const char *gb_core_build(void) {
#ifdef PERFORMANCE
	return "Gearboy PERFORMANCE (75 machine cycles between updates)";
#else
	return "Gearboy accurate (one machine cycle at a time)";
#endif
}

} // extern "C"
