#!/usr/bin/env bash
# Standalone emcc build for the wasm platform target, against the
# already-configured libmgba.a in ../../../build-wasm (see spikes 1-6 in
# harmony's plans/gba-wasm-core.md for how that tree was configured:
# LIBMGBA_ONLY, M_CORE_GBA/GB, USE_PTHREADS=OFF, ENABLE_DEBUGGERS=ON).
# Not wired into mgba's own CMake platform machinery -- this is simpler and
# more robust for a one-off export layer than adding a new CMake platform.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MGBA_ROOT="$(cd "$HERE/../../.." && pwd)"
BUILD_DIR="$MGBA_ROOT/build-wasm"
OUT_DIR="${1:-/tmp/mgba-spike/dist}"

mkdir -p "$OUT_DIR"

source "$HOME/dev/taybart/emsdk/emsdk_env.sh" >/dev/null 2>&1

emcc -O3 -std=c11 -D_GNU_SOURCE -DENABLE_VFS -DENABLE_DIRECTORIES -DENABLE_DEBUGGERS -DBUILD_STATIC \
  -I "$MGBA_ROOT/include" -I "$BUILD_DIR/include" -I "$MGBA_ROOT/src" \
  "$HERE/mgba-wasm.c" "$BUILD_DIR/libmgba.a" \
  -sMODULARIZE=1 \
  -sEXPORT_NAME=MGBA \
  -sEXPORT_ES6=0 \
  -sEXPORTED_FUNCTIONS=_mgba_create,_mgba_set_video_buffer,_mgba_load_rom,_mgba_destroy,_mgba_frequency,_mgba_frame_cycles,_mgba_run_frame,_mgba_audio_sample_rate,_mgba_audio_read,_mgba_set_keys,_mgba_read8,_mgba_read16,_mgba_read32,_mgba_write8,_mgba_write16,_mgba_write32,_mgba_set_breakpoint_callback,_mgba_set_breakpoint,_mgba_clear_breakpoint,_mgba_get_register,_mgba_set_register,_mgba_state_size,_mgba_save_state,_mgba_load_state,_mgba_savedata_size,_mgba_savedata_read,_mgba_savedata_write,_mgba_savedata_dirty,_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,addFunction,removeFunction,HEAPU8,HEAP16,HEAPU32 \
  -sALLOW_MEMORY_GROWTH=1 \
  -sALLOW_TABLE_GROWTH=1 \
  -sENVIRONMENT=web,worker \
  -o "$OUT_DIR/mgba.js"

ls -la "$OUT_DIR"/mgba.js "$OUT_DIR"/mgba.wasm
