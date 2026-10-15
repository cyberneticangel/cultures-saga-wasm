# Cultures - Die Saga: static recompilation build.
#
#   make gen      regenerate C from the game executable (needs python + capstone + pefile)
#   make native   SDL2 build for debugging     -> build/native/cultures
#   make web      WebAssembly build            -> web/cultures.wasm
#
GAME_DIR ?= ..
EXE      ?= $(GAME_DIR)/GameSaga.exe
PYTHON   ?= python3
OPT      ?= -O2

GEN_SRC  := $(wildcard build/gen/code_*.c) build/gen/table.c
RT_SRC   := cpu.c mem.c loader.c vfs.c kernel32.c user32.c misc.c ddraw.c dsound.c dinput.c main.c dmusic.c dmsynth.c cursor.c dplay.c video.c
CFLAGS_COMMON := -std=gnu11 -Iruntime -Ibuild/gen -fno-strict-aliasing -fwrapv \
                 -Wno-parentheses-equality -Wno-unused-variable -Wno-unused-label -Wno-unused-but-set-variable

.PHONY: all gen native web clean
all: native

gen:
	$(PYTHON) recomp/translate.py build/gen $(EXE) $(GAME_DIR)/gedx8musicdrv.dll

# ---------------------------------------------------------------- native
NATIVE_OBJ := $(patsubst build/gen/%.c,build/native/gen/%.o,$(GEN_SRC)) \
              $(patsubst %.c,build/native/rt/%.o,$(RT_SRC) platform_sdl.c)
native: build/native/cultures

build/native/gen/%.o: build/gen/%.c runtime/recomp.h build/gen/funcs.h
	@mkdir -p $(dir $@)
	clang $(CFLAGS_COMMON) $(OPT) -g -c $< -o $@

build/native/rt/%.o: runtime/%.c runtime/recomp.h runtime/runtime.h runtime/dmusic_int.h build/gen/funcs.h
	@mkdir -p $(dir $@)
	clang $(CFLAGS_COMMON) -O1 -g -Wall -Wno-unused-function -c $< -o $@

build/native/cultures: $(NATIVE_OBJ)
	clang -o $@ $^ -lSDL2 -lm

# ---------------------------------------------------------------- web
# Guest memory occupies [0, 0x38000000); the runtime's own data is placed above it.
WASM_CC    := zig cc -target wasm32-wasi
WASM_FLAGS := $(CFLAGS_COMMON) -mbulk-memory -mnontrapping-fptoint
WEB_OBJ := $(patsubst build/gen/%.c,build/web/gen/%.o,$(GEN_SRC)) \
           $(patsubst %.c,build/web/rt/%.o,$(RT_SRC) platform_web.c)
web: web/cultures.wasm

build/web/gen/%.o: build/gen/%.c runtime/recomp.h build/gen/funcs.h
	@mkdir -p $(dir $@)
	$(WASM_CC) $(WASM_FLAGS) $(OPT) -c $< -o $@

build/web/rt/%.o: runtime/%.c runtime/recomp.h runtime/runtime.h build/gen/funcs.h
	@mkdir -p $(dir $@)
	$(WASM_CC) $(WASM_FLAGS) -O2 -c $< -o $@

web/cultures.wasm: build/web/cultures.raw.wasm
	wasm-opt -O2 --strip-debug --enable-bulk-memory --enable-nontrapping-float-to-int --enable-sign-ext --enable-mutable-globals $< -o $@

build/web/cultures.raw.wasm: $(WEB_OBJ)
	$(WASM_CC) -s -o $@ $^ -Wl,--global-base=939524096 -Wl,--initial-memory=1006632960 \
	  -Wl,--max-memory=2147483648 -Wl,--export=game_main -Wl,-z,stack-size=1048576 \
	  -mexec-model=reactor

clean:
	rm -rf build/native build/web
