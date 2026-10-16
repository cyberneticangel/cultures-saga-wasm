# Cultures: static recompilation build.  One build per game executable ("variant").
#
#   make gen    [VERSION=...]   regenerate C from the game executable (needs python + capstone + pefile)
#   make web    [VERSION=...]   WebAssembly build      -> web/cultures-<variant>.wasm (+ .json)
#   make native [VERSION=...]   SDL2 build for debugging -> build/native/<variant>/cultures
#   make all-versions           gen + web for every game executable found
#
# The executable is found in GAME_DIR and its subfolders, whatever it is called (Game.exe,
# GameSaga.exe, ...): versions are recognised by content (CRC-32, see recomp/variant.py).
# With one version there, it is used; with several, pick one with VERSION=saga / 8thwonder /
# northland (or a variant name or CRC); `make gen` lists them.  EXE=/path/to/Game.exe overrides.
# Known executables build as gamesaga / game8thwonder / gamenorthland, others as <name>-<crc>.
GAME_DIR  ?= ..
GAME_DIRS ?= $(GAME_DIR)
VERSION   ?=
PYTHON    ?= python3
OPT       ?= -O2

ifeq ($(origin EXE),undefined)
EXE := $(shell python3 recomp/variant.py find "$(VERSION)" "$(GAME_DIR)" 2>/dev/null)
endif
ifeq ($(EXE),)
ifneq ($(filter all gen web native,$(or $(MAKECMDGOALS),all)),)
# show variant.py's explanation (the versions found, or what is missing), then stop
$(shell python3 recomp/variant.py find "$(VERSION)" "$(GAME_DIR)" >/dev/null)
$(error no game executable chosen)
endif
endif

VARIANT  := $(shell python3 recomp/variant.py name "$(EXE)" 2>/dev/null || basename "$(EXE)" .exe | tr 'A-Z' 'a-z')
GEN      := build/gen/$(VARIANT)
GEN_SRC  := $(wildcard $(GEN)/code_*.c) $(GEN)/table.c
RT_SRC   := cpu.c mem.c loader.c vfs.c kernel32.c user32.c misc.c ddraw.c dsound.c dinput.c main.c dmusic.c dmsynth.c cursor.c dplay.c video.c
RT_HDR   := runtime/recomp.h runtime/runtime.h runtime/dmusic_int.h $(GEN)/funcs.h
CFLAGS_COMMON := -std=gnu11 -Iruntime -I$(GEN) -fno-strict-aliasing -fwrapv \
                 -Wno-parentheses-equality -Wno-unused-variable -Wno-unused-label -Wno-unused-but-set-variable

.PHONY: all gen native web all-versions clean
all: native

# the music driver next to the executable (each game version ships its own), else GAME_DIR's
MUSIC_DLL := $(shell d=$$(dirname "$(EXE)"); [ -f "$$d/gedx8musicdrv.dll" ] && echo "$$d/gedx8musicdrv.dll" || echo "$(GAME_DIR)/gedx8musicdrv.dll")

gen:
	$(PYTHON) recomp/translate.py $(GEN) "$(EXE)" "$(MUSIC_DLL)"

# every Cultures executable in GAME_DIRS and their subfolders (space-separated; recognised by
# content), each version once
all-versions:
	@python3 recomp/variant.py scan-unique $(GAME_DIRS) | while IFS= read -r exe; do \
	  echo "== $$exe -> $$(python3 recomp/variant.py name "$$exe")"; \
	  $(MAKE) --no-print-directory gen EXE="$$exe" && \
	  $(MAKE) --no-print-directory web EXE="$$exe" || exit 1; \
	done

# ---------------------------------------------------------------- native
NATIVE   := build/native/$(VARIANT)
NATIVE_OBJ := $(patsubst $(GEN)/%.c,$(NATIVE)/gen/%.o,$(GEN_SRC)) \
              $(patsubst %.c,$(NATIVE)/rt/%.o,$(RT_SRC) platform_sdl.c)
native: $(NATIVE)/cultures

$(NATIVE)/gen/%.o: $(GEN)/%.c runtime/recomp.h $(GEN)/funcs.h
	@mkdir -p $(dir $@)
	clang $(CFLAGS_COMMON) $(OPT) -g -c $< -o $@

$(NATIVE)/rt/%.o: runtime/%.c $(RT_HDR)
	@mkdir -p $(dir $@)
	clang $(CFLAGS_COMMON) -O1 -g -Wall -Wno-unused-function -c $< -o $@

$(NATIVE)/cultures: $(NATIVE_OBJ)
	clang -o $@ $^ -lSDL2 -lm

# ---------------------------------------------------------------- web
# Guest memory occupies [0, 0x38000000); the runtime's own data is placed above it.
WEBB       := build/web/$(VARIANT)
WASM_CC    := zig cc -target wasm32-wasi
WASM_FLAGS := $(CFLAGS_COMMON) -mbulk-memory -mnontrapping-fptoint
WEB_OBJ := $(patsubst $(GEN)/%.c,$(WEBB)/gen/%.o,$(GEN_SRC)) \
           $(patsubst %.c,$(WEBB)/rt/%.o,$(RT_SRC) platform_web.c)
web: web/cultures-$(VARIANT).wasm web/cultures-$(VARIANT).json

$(WEBB)/gen/%.o: $(GEN)/%.c runtime/recomp.h $(GEN)/funcs.h
	@mkdir -p $(dir $@)
	$(WASM_CC) $(WASM_FLAGS) $(OPT) -c $< -o $@

$(WEBB)/rt/%.o: runtime/%.c $(RT_HDR)
	@mkdir -p $(dir $@)
	$(WASM_CC) $(WASM_FLAGS) -O2 -c $< -o $@

web/cultures-$(VARIANT).wasm: $(WEBB)/cultures.raw.wasm
	wasm-opt -O2 --strip-debug --enable-bulk-memory --enable-nontrapping-float-to-int --enable-sign-ext --enable-mutable-globals $< -o $@

# tells the page which executable this build belongs to
web/cultures-$(VARIANT).json: $(GEN)/table.c
	python3 recomp/variant.py json "$(EXE)" > $@

$(WEBB)/cultures.raw.wasm: $(WEB_OBJ)
	$(WASM_CC) -s -o $@ $^ -Wl,--global-base=939524096 -Wl,--initial-memory=1006632960 \
	  -Wl,--max-memory=2147483648 -Wl,--export=game_main -Wl,-z,stack-size=1048576 \
	  -mexec-model=reactor

clean:
	rm -rf build/native build/web
