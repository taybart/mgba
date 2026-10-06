/* Minimal single-threaded WASM platform target for mGBA, built against
 * mCore directly (no SDL/Qt/libretro, no pthreads). See
 * harmony/plans/gba-wasm-core.md for the project this serves.
 *
 * Built standalone with emcc against the mgba-emu core static library
 * (see build.sh in this directory), not wired into mgba's own CMake
 * platform machinery.
 */

#include <mgba/gba/core.h>
#include <mgba/core/core.h>
#include <mgba/internal/gba/gba.h> /* GBA_ARM7TDMI_FREQUENCY, struct GBA, memory.savedata.dirty */
#include <mgba/internal/defines.h> /* mSAVEDATA_DIRT_NEW */
#include <mgba/debugger/debugger.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/arm/debugger/debugger.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/vfs.h>

#include <emscripten.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct MgbaInstance {
	struct mCore* core;
	mColor* videoBuffer;

	struct mDebugger debugger;
	struct mDebuggerModule debuggerModule;
	void (*breakpointCallback)(struct MgbaInstance*, uint32_t pc);
} MgbaInstance;

static void _mgbaModuleEntered(struct mDebuggerModule* module, enum mDebuggerEntryReason reason, struct mDebuggerEntryInfo* info) {
	if (reason != DEBUGGER_ENTER_BREAKPOINT) {
		return;
	}
	MgbaInstance* instance = (MgbaInstance*) ((char*) module - offsetof(MgbaInstance, debuggerModule));
	if (instance->breakpointCallback) {
		instance->breakpointCallback(instance, info->address);
	}
	// We drive execution via core->runFrame() ourselves, not mDebugger's own
	// step loop, so nothing is "resuming" us -- just clear the flag
	// mDebuggerEnter() set so debugger->state bookkeeping stays sane.
	module->isPaused = false;
}

// --- Lifecycle ---

EMSCRIPTEN_KEEPALIVE
MgbaInstance* mgba_create(void) {
	MgbaInstance* instance = calloc(1, sizeof(MgbaInstance));
	if (!instance) {
		return NULL;
	}

	instance->core = GBACoreCreate();
	if (!instance->core) {
		free(instance);
		return NULL;
	}
	if (!instance->core->init(instance->core)) {
		instance->core->deinit(instance->core);
		free(instance);
		return NULL;
	}
	// Required before core->reset(): _GBACoreReset calls
	// mCoreConfigGetBoolValue internally, which segfaults on an
	// uninitialized config table otherwise (spike 2 finding).
	mCoreInitConfig(instance->core, "wasm");
	instance->core->opts.skipBios = true;

	mDebuggerInit(&instance->debugger);
	mDebuggerAttach(&instance->debugger, instance->core);
	instance->debugger.state = DEBUGGER_RUNNING;
	instance->debuggerModule.entered = _mgbaModuleEntered;
	mDebuggerAttachModule(&instance->debugger, &instance->debuggerModule);

	return instance;
}

// GBA's screen is a fixed 240x160; `buffer` must be a caller-owned block of
// at least 240*160*4 bytes (RGBA8, native-endian -- see core format notes in
// the project report). Call once, any time before the first mgba_run_frame().
EMSCRIPTEN_KEEPALIVE
void mgba_set_video_buffer(MgbaInstance* instance, mColor* buffer) {
	instance->videoBuffer = buffer;
	instance->core->setVideoBuffer(instance->core, buffer, 240);
}

// Copies `size` bytes from `data` (caller frees `data` after this returns),
// loads it as a ROM, and resets the core. Returns 0 on success.
EMSCRIPTEN_KEEPALIVE
int mgba_load_rom(MgbaInstance* instance, const uint8_t* data, int size) {
	void* romCopy = malloc(size);
	if (!romCopy) {
		return -1;
	}
	memcpy(romCopy, data, size);
	struct VFile* vf = VFileFromMemory(romCopy, size);
	if (!vf) {
		free(romCopy);
		return -2;
	}
	if (!instance->core->loadROM(instance->core, vf)) {
		// loadROM failing doesn't guarantee vf was consumed; VFileFromMemory
		// close() frees romCopy either way.
		vf->close(vf);
		return -3;
	}

	instance->core->reset(instance->core);
	instance->core->setKeys(instance->core, 0);
	return 0;
}

EMSCRIPTEN_KEEPALIVE
void mgba_destroy(MgbaInstance* instance) {
	if (!instance) {
		return;
	}
	mDebuggerDetachModule(&instance->debugger, &instance->debuggerModule);
	instance->core->detachDebugger(instance->core);
	mDebuggerDeinit(&instance->debugger);
	instance->core->deinit(instance->core);
	// instance->videoBuffer is caller-owned (JS malloc'd it); not freed here.
	free(instance);
}

// --- Frame pacing ---
// GBA's native refresh rate is NOT 60Hz. Actual rate = mgba_frequency() /
// mgba_frame_cycles() = 16777216 / 280896 = ~59.7275Hz (period ~16.743ms).
// Pace the JS rAF loop off this, not a hardcoded 60.

EMSCRIPTEN_KEEPALIVE
uint32_t mgba_frequency(void) {
	return GBA_ARM7TDMI_FREQUENCY;
}

EMSCRIPTEN_KEEPALIVE
uint32_t mgba_frame_cycles(MgbaInstance* instance) {
	return instance->core->frameCycles(instance->core);
}

EMSCRIPTEN_KEEPALIVE
void mgba_run_frame(MgbaInstance* instance) {
	instance->core->runFrame(instance->core);
}

// --- Audio ---
// Pulls up to maxFrames interleaved stereo int16 frames (L,R,L,R,...) into
// `out` (caller-owned, at least maxFrames*2*sizeof(int16_t) bytes). Returns
// the number of frames actually written (may be less than maxFrames, or 0).
// Native rate is mgba_audio_sample_rate() (fixed 32768Hz baseline; can
// change at runtime if a game writes SOUNDBIAS's resolution bits -- not
// handled here, same as the rest of this layer: poll the rate each pull if
// that matters to the caller).

EMSCRIPTEN_KEEPALIVE
uint32_t mgba_audio_sample_rate(MgbaInstance* instance) {
	return instance->core->audioSampleRate(instance->core);
}

EMSCRIPTEN_KEEPALIVE
size_t mgba_audio_read(MgbaInstance* instance, int16_t* out, size_t maxFrames) {
	struct mAudioBuffer* buf = instance->core->getAudioBuffer(instance->core);
	return mAudioBufferRead(buf, out, maxFrames);
}

// --- Input ---
// bitmask bit order: A=0 B=1 Select=2 Start=3 Right=4 Left=5 Up=6 Down=7 R=8 L=9

EMSCRIPTEN_KEEPALIVE
void mgba_set_keys(MgbaInstance* instance, uint32_t bitmask) {
	instance->core->setKeys(instance->core, bitmask);
}

// --- Memory access (cheats) ---

EMSCRIPTEN_KEEPALIVE
uint8_t mgba_read8(MgbaInstance* instance, uint32_t address) {
	return (uint8_t) instance->core->busRead8(instance->core, address);
}
EMSCRIPTEN_KEEPALIVE
uint16_t mgba_read16(MgbaInstance* instance, uint32_t address) {
	return (uint16_t) instance->core->busRead16(instance->core, address);
}
EMSCRIPTEN_KEEPALIVE
uint32_t mgba_read32(MgbaInstance* instance, uint32_t address) {
	return instance->core->busRead32(instance->core, address);
}
EMSCRIPTEN_KEEPALIVE
void mgba_write8(MgbaInstance* instance, uint32_t address, uint8_t value) {
	instance->core->busWrite8(instance->core, address, value);
}
EMSCRIPTEN_KEEPALIVE
void mgba_write16(MgbaInstance* instance, uint32_t address, uint16_t value) {
	instance->core->busWrite16(instance->core, address, value);
}
EMSCRIPTEN_KEEPALIVE
void mgba_write32(MgbaInstance* instance, uint32_t address, uint32_t value) {
	instance->core->busWrite32(instance->core, address, value);
}

// --- Breakpoints (cheats: "hook exact") ---
// Proven in spike 4: fires synchronously, BEFORE the trapped instruction's
// side effects take place, through the normal fast core->runFrame() path --
// no slow single-step driving loop needed. mgba_get_register/set_register
// let a cheat patch CPU state from inside the callback and have it take
// effect on the instruction about to execute.

EMSCRIPTEN_KEEPALIVE
void mgba_set_breakpoint_callback(MgbaInstance* instance, void (*callback)(MgbaInstance*, uint32_t pc)) {
	instance->breakpointCallback = callback;
}

// Returns a breakpoint id (>=0) or -1 on failure. Persistent/re-arming: the
// same id keeps firing every time execution reaches `address` until cleared.
EMSCRIPTEN_KEEPALIVE
int32_t mgba_set_breakpoint(MgbaInstance* instance, uint32_t address) {
	return (int32_t) ARMDebuggerSetSoftwareBreakpoint(instance->debugger.platform, &instance->debuggerModule, address, MODE_ARM);
}

// Cheap, live -- no core recreation needed (spike 4).
EMSCRIPTEN_KEEPALIVE
bool mgba_clear_breakpoint(MgbaInstance* instance, int32_t id) {
	return instance->debugger.platform->clearBreakpoint(instance->debugger.platform, id);
}

EMSCRIPTEN_KEEPALIVE
uint32_t mgba_get_register(MgbaInstance* instance, int reg) {
	struct ARMCore* cpu = (struct ARMCore*) instance->core->cpu;
	return cpu->gprs[reg];
}

EMSCRIPTEN_KEEPALIVE
void mgba_set_register(MgbaInstance* instance, int reg, uint32_t value) {
	struct ARMCore* cpu = (struct ARMCore*) instance->core->cpu;
	cpu->gprs[reg] = value;
}

// --- Save state (snapshots) ---
// Caller-owned buffer throughout, same as the video buffer: query the size
// once, malloc it in JS, pass the pointer straight through both ways.

EMSCRIPTEN_KEEPALIVE
uint32_t mgba_state_size(MgbaInstance* instance) {
	return (uint32_t) instance->core->stateSize(instance->core);
}

EMSCRIPTEN_KEEPALIVE
bool mgba_save_state(MgbaInstance* instance, void* buffer) {
	return instance->core->saveState(instance->core, buffer);
}

EMSCRIPTEN_KEEPALIVE
bool mgba_load_state(MgbaInstance* instance, const void* buffer) {
	return instance->core->loadState(instance->core, buffer);
}

// --- Save data (cart battery SRAM/flash/eeprom) ---
// Lazily auto-detected the same way gba.js does it: size reads 0 until the
// game actually touches its save hardware's address range (spike 6). Unlike
// save state, mCore's underlying savedataClone() always allocates its own
// buffer (there's no size-only query), so this wraps it into the same
// caller-buffer-after-size-query shape as the rest of this API: call
// mgba_savedata_size(), malloc that many bytes, call mgba_savedata_read()
// to fill it.

EMSCRIPTEN_KEEPALIVE
uint32_t mgba_savedata_size(MgbaInstance* instance) {
	void* tmp = NULL;
	size_t size = instance->core->savedataClone(instance->core, &tmp);
	free(tmp);
	return (uint32_t) size;
}

// Fills `buffer` (caller-owned, exactly mgba_savedata_size() bytes) with the
// current save data. Returns false if there's no save data yet.
EMSCRIPTEN_KEEPALIVE
bool mgba_savedata_read(MgbaInstance* instance, void* buffer) {
	void* tmp = NULL;
	size_t size = instance->core->savedataClone(instance->core, &tmp);
	if (!tmp || !size) {
		return false;
	}
	memcpy(buffer, tmp, size);
	free(tmp);
	return true;
}

// Maps a byte count to mGBA's save-type enum. Used only as a fallback when
// the save type hasn't been established yet (see mgba_savedata_write below);
// when it HAS been established (by an mGBA built-in override, as happens
// automatically for known commercial ROMs like Pokemon games via
// GBAOverrideApplyDefaults in core->reset(), or by the game's own access
// pattern), that real type always wins -- this is only a fallback.
static enum GBASavedataType _sizeToSavedataType(uint32_t size) {
	switch (size) {
		case 0x00000200: return GBA_SAVEDATA_EEPROM512;
		case 0x00002000: return GBA_SAVEDATA_EEPROM;
		case 0x00008000: return GBA_SAVEDATA_SRAM;
		case 0x00010000: return GBA_SAVEDATA_FLASH512; // ambiguous with SRAM512 (same size); flash is far more common
		case 0x00020000: return GBA_SAVEDATA_FLASH1M;
		default: return GBA_SAVEDATA_AUTODETECT;
	}
}

// Writes `size` bytes of save data from `data` into the cart's save
// hardware. For a ROM mGBA already has an override for (applied
// automatically on core->reset() -- confirmed for Pokemon games, which
// force FLASH1M before this is ever called), the buffer is already
// correctly sized and this is a normal write.
//
// BUT: for a ROM with no override entry, relying purely on lazy auto-detect
// (which only fires when the GAME ITSELF touches its save address range),
// calling this before that has happened is a SILENT NO-OP -- mGBA's
// GBASavedataLoad() has nowhere to write to yet (no buffer, no backing
// VFile) and just returns true having done nothing. Confirmed empirically:
// on a ROM with no override (Doom, game code ADME), writing before any
// frame has run reports success but mgba_savedata_size() reads 0 both
// before AND after, and stays 0 even after 300 frames of real execution --
// the data is silently dropped, not buffered for later.
//
// Fix: if the save type hasn't been established yet, force it from the
// buffer size being handed in (the caller -- main.js -- knows the size of
// the real save file it's restoring) before attempting the write, so the
// data has somewhere real to land instead of vanishing.
EMSCRIPTEN_KEEPALIVE
bool mgba_savedata_write(MgbaInstance* instance, const void* data, uint32_t size) {
	struct GBA* gba = (struct GBA*) instance->core->board;
	if (gba->memory.savedata.type == GBA_SAVEDATA_AUTODETECT) {
		enum GBASavedataType inferred = _sizeToSavedataType(size);
		if (inferred != GBA_SAVEDATA_AUTODETECT) {
			GBASavedataForceType(&gba->memory.savedata, inferred);
		}
	}
	return instance->core->savedataRestore(instance->core, data, size, true);
}

// Read-and-clear dirty bit for the cart's save hardware specifically (SRAM
// store, flash program/erase, EEPROM write) -- NOT a generic "any bus write
// happened" flag, so a cheat poking work RAM via mgba_write8/16/32 never
// trips this. Backed by mGBA's own internal GBASavedata.dirty tracking
// (src/gba/memory.c's SRAM store path and src/gba/savedata.c's flash/EEPROM
// write paths both set mSAVEDATA_DIRT_NEW already; this just exposes it).
// Returns true if the save data changed since the last call to this
// function, then clears the flag.
EMSCRIPTEN_KEEPALIVE
bool mgba_savedata_dirty(MgbaInstance* instance) {
	struct GBA* gba = (struct GBA*) instance->core->board;
	bool wasDirty = (gba->memory.savedata.dirty & mSAVEDATA_DIRT_NEW) != 0;
	gba->memory.savedata.dirty &= ~mSAVEDATA_DIRT_NEW;
	return wasDirty;
}

int main(void) {
	return 0;
}
