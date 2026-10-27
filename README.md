# Cultures – Die Saga: WebAssembly port for Firefox

This port runs the original game code in the browser. The game executable (`GameSaga.exe`,
or one of the other versions under [Game versions](#game-versions)) and the music
driver `gedx8musicdrv.dll` are **statically recompiled** from x86 machine code to C,
one C function per x86 function. They run on top of a hand-written runtime that
emulates the parts of Win32, DirectDraw 7, DirectSound 8, DirectInput and DirectMusic
the game uses. Everything is compiled to WebAssembly.

## Play it

Put this folder (`port/`) inside your Cultures installation, next to the game's executable
(`Game.exe`, or `GameSaga.exe` for Die Saga), build the game as described under [Build](#build),
then:

```sh
cd port
python3 serve.py            # serves the game folder one level up
# open http://127.0.0.1:8000/ in Firefox, click "Start game"
```

`serve.py --game /path/to/Cultures` points the server at another install. If you have built
more than one version, pick it with **Game version** on the start screen.

- Saves and settings are written to `port/userdata/<version>/`, one folder per game version; the
  original files are never modified.
- The game draws its own `.cur` cursors, which appear as CSS cursors over the canvas.
- "Log" shows the engine log, and `?log=2` gives verbose logging.

### Settings

**Settings** (in the header, and next to "Start game") holds what the original handled outside the
game:
- **Screen resolution:** 4:3 and 5:4 modes (800×600 to 1280×1024), 16:10 (1280×800 to 1920×1200),
  16:9 (1280×720 to 2560×1440) and 21:9 (2560×1080, 3440×1440). "Match this window" picks the mode
  closest to the browser window's shape. The choice is stored in the game's own
  `Saves/opt_glob.ini` and applies at the next start. The in-game Grafik tab lists the same modes
  and switches immediately. The game's interface keeps its pixel size, so it gets small at the
  highest resolutions; 3440×1440 runs at about 45 fps in Firefox.
- **Interface size:** Normal, Large (1.5×) or Huge (2×). "Match this window" then picks a lower
  resolution that the browser scales up, so the interface stays readable on large screens. With
  **Match at every start** this happens each time the game starts.
- **Display:** smooth or sharp-pixel scaling; **Skip intro video** goes straight to the menu (the
  menu's Introsequenz still plays it); **Edge scrolling** keeps the map scrolling when the cursor leaves
  the game past an edge.
- **Saves:** an autosave interval for every game (the game's own autosave, Options → Speichern,
  is off each time it starts); export this version's saves and settings as a zip, or import such a
  zip (before starting the game).
- **Sound:** master volume, and an option to mute while the tab is in the background.
- **Multiplayer server.**

Everything else, such as scrolling, music and detail level, is in the game's own options window
(the scroll icon in the sidebar).

### While playing

- **Game speed:** the selector in the header sets ½× to 10×, and the **Pause** key pauses and
  resumes. It uses the game's own speed command (the one behind its `p` and `l` keys), so it also
  follows those keys, and in multiplayer every player's game changes with it (there, up to 3×). For
  an unknown game executable it scales the game's clock instead, in single player only.
- **Quickload:** **Shift F9** loads the quicksave that **F9** (or Ctrl S) saves.
- **Keys:** the header button lists the game's keyboard shortcuts.
- **Screenshot:** the header button or **Print Screen** saves a PNG at the game's resolution.

### Multiplayer

Choose **Mehrspieler** in the main menu. One player creates a game (Spiel anlegen) and the others
join (Spiel beitreten). Leave the address field empty: every game hosted on the same server is
listed.

- **Same computer:** use two browser tabs or windows on one `serve.py`; its built-in server at
  `/net` is the default. The tabs share `port/userdata`, so they also share the stored player name.
- **Several computers:** every player runs their own `serve.py` with their own copy of the game. One
  machine that everyone can reach runs the relay:

  ```sh
  python3 relay.py              # listens on ws://0.0.0.0:8001/
  ```

  Each player then enters `ws://<that machine>:8001/` under Settings → Multiplayer server. The relay
  only forwards messages and never touches files.

## Game versions

Each game executable gets its own build. Versions are recognised by the **content** of the
executable (its CRC-32), not by its file name: the original releases all call it `Game.exe`, and
renamed copies work too. The build records the checksum and only runs with that exact file;
it finds it in the game folder whatever it is called.

| Version | Build name | Status |
|---|---|---|
| Cultures – Die Saga (2005) | `gamesaga` | Fully supported: single player, multiplayer, videos |
| Cultures – 8th Wonder of the World (Nov 2003) | `game8thwonder` | Works with its own data (menu tested) and with the Saga data (campaign and multiplayer tested) |
| Cultures – Northland (Sep 2003) | `gamenorthland` | Needs its own data: menu and first mission tested |

Any other executable of this engine builds as `<file name>-<crc>` (for example `game-8e30ed31`).
`python3 recomp/variant.py name /path/to/Game.exe` shows what a file is recognised as.

### Playing 8th Wonder of the World and Northland

These are separate games with their own data, and are not part of Die Saga. You need your own
copy of each, for example the GOG releases. The steps assume the browser version of Die Saga
is already built (see [Build](#build)).

1. **Copy each game's install folder into the Saga folder**, next to `port/`. The folder names
   don't matter. The whole install is needed, not just `Game.exe`: each game has its own maps,
   graphics and texts in `DataX/`.

   ```
   Cultures Saga/
     GameSaga.exe, DataX/, ...
     8th Wonder of the World/   Game.exe, DataX/, gedx8musicdrv.dll, ...
     Northland/                 Game.exe, DataX/, gedx8musicdrv.dll, ...
     port/
   ```

   An existing Windows or Wine install can be copied as it is. Without Windows, GOG's offline
   installers can also be unpacked with `innoextract` (not tested here).

2. **Check that the executables are recognised:**

   ```sh
   cd port
   python3 recomp/variant.py scan ..
   ```

   Both `Game.exe` files should be listed. `python3 recomp/variant.py name "../Northland/Game.exe"`
   should print `gamenorthland`, and the 8th Wonder one `game8thwonder`. A name like
   `game-1a2b3c4d` means a different release or patch level than the one tested here; it can
   still be built, but may need fixes (see the end of this section).

3. **Translate and compile each game,** choosing it with `VERSION=`. Each takes about as long
   as the Saga build:

   ```sh
   make gen VERSION=8thwonder PYTHON=.venv/bin/python && make web VERSION=8thwonder -j$(nproc)
   make gen VERSION=northland PYTHON=.venv/bin/python && make web VERSION=northland -j$(nproc)
   ```

   Or build everything found at once with `make all-versions PYTHON=.venv/bin/python -j$(nproc)`.
   This also builds any other game executables in the folders (such as the `GameMp.exe` files
   GOG ships), which haven't been tested.

4. **Start the server** with `python3 serve.py`, as for Die Saga. It prints every executable
   it found. Open http://127.0.0.1:8000/, pick the game under **Game version** and click
   **Start game**. Each game runs from its own folder and keeps its saves and settings in
   `port/userdata/<version>/`.

5. **Native build (optional):** `make native VERSION=northland -j$(nproc)`, then
   `./build/native/gamenorthland/cultures ../Northland`.

### Several installations

`serve.py` and `make all-versions` look in the game folder and one level below it (the layout
above). They find each game's executable whatever it is called, and run each version from its
own folder. If a version is in several folders (say `GameNorthland.exe` copied next to
`GameSaga.exe`, and `Northland/Game.exe`), the folder with the fewest game executables wins,
which is that game's own install.

Installs elsewhere can be given with `--game` (repeatable):

```sh
python3 serve.py --game /games/Cultures8thWonder --game /games/CulturesNorthland
```

To build them, point `GAME_DIR` at each folder, or build everything at once:

```sh
make gen GAME_DIR=/games/Cultures8thWonder PYTHON=.venv/bin/python && make web GAME_DIR=/games/Cultures8thWonder -j$(nproc)
make all-versions GAME_DIRS="/games/Cultures8thWonder /games/CulturesNorthland" PYTHON=.venv/bin/python -j$(nproc)
```

`all-versions` builds each version once, from the executables in `GAME_DIRS` (default: the folder
above `port/`) and their subfolders. Subfolders may have spaces in their names; folders listed
in `GAME_DIRS` may not (build those one at a time with `GAME_DIR=` or `EXE=`). The music driver
`gedx8musicdrv.dll` next to each executable is used for its build. `serve.py --version-dir
gamenorthland=/games/CulturesNorthland` (the build name, not the short `VERSION` name) forces a
version's folder if automatic matching isn't wanted. The native build takes the game folder as its argument:
`./build/native/gamenorthland/cultures ../Northland`.

**Each executable reads the data formats of its own release.** Northland is older than the Saga
data: between the two, the game's object type numbers moved by one. Run on Saga data, Northland
looks up the wrong record for every object and stops with a division by zero while loading the
menu's demo map. The original Windows game behaves the same, so give Northland its own
installation.

If a version stops with "dispatch: no function at 0x…", the recompiler missed a function: add
that address to `recomp/extra/<exe file name in lower case>.txt` (one hex address per line) and
run `make gen` and `make web` again. The multiplayer hooks find the game's two network threads
by their code, so they work in every version that shares this engine's code for them; the
patterns are in `runtime/dplay.c`.

## Build

This repository contains no game code or data: `web/cultures-<version>.wasm` and `build/gen/` are
generated from your own copy of the game. Build them once with the steps below, and again after changing
anything in `recomp/` or `runtime/`.

### Building the browser version

Tested on Linux with Python 3.14, zig 0.16 and binaryen (wasm-opt) 133. Development builds of zig
0.18 no longer bundle the WASI C library (`'string.h' file not found`); use a zig 0.16 release, for
example with `make web WASM_CC="/path/to/zig cc -target wasm32-wasi"`.

1. **Install the tools.**
   - `python3` and `make`.
   - [zig](https://ziglang.org/download/): its `zig cc` is the C compiler for WebAssembly and
     brings the WASI C library.
   - `wasm-opt` from [binaryen](https://github.com/WebAssembly/binaryen).

   On Arch: `pacman -S python make zig binaryen`. On Debian/Ubuntu: `apt install python3-venv make
   binaryen`, plus zig from ziglang.org.

2. **Create a Python environment for the recompiler**, which needs `capstone` and `pefile`:

   ```sh
   cd port
   python3 -m venv .venv
   .venv/bin/pip install capstone pefile
   ```

3. **Translate the game to C.** This finds the game's executable in the folder above `port/` (or
   its subfolders), whatever it is called, and writes about 1 million lines of C to
   `build/gen/<version>/`, for example `build/gen/gamesaga/`. It takes about 30 seconds:

   ```sh
   make gen PYTHON=.venv/bin/python
   ```

   If the folder holds more than one game version, `make gen` lists them and stops. Pick one
   with `VERSION=`, and use the same `VERSION=` for the next step:

   ```
   $ make gen
   Several game versions found; choose one with VERSION=...:
     VERSION=saga        Cultures – Die Saga (2005)                 ../GameSaga.exe
     VERSION=8thwonder   Cultures – 8th Wonder of the World (2003)  ../8th Wonder of the World/Game.exe
     VERSION=northland   Cultures – Northland (2003)                ../Northland/Game.exe
   or build them all with: make all-versions
   $ make gen VERSION=saga PYTHON=.venv/bin/python
   ```

   `EXE=/path/to/Game.exe` names an executable directly, and `GAME_DIR=/path/to/game` searches
   another folder.

4. **Compile to WebAssembly.** This writes `web/cultures-<version>.wasm`. The first build takes a
   few minutes; later builds only recompile what changed:

   ```sh
   make web -j$(nproc)              # add the same VERSION=... as for make gen, if you used one
   ```

   To build every version found at once: `make all-versions PYTHON=.venv/bin/python -j$(nproc)`.

5. **Run it:**

   ```sh
   python3 serve.py
   ```

   Open http://127.0.0.1:8000/ in Firefox and click **Start game**. If an older version still
   shows, reload with Ctrl+Shift+R.

Use the `http://127.0.0.1` or `http://localhost` address. The game needs SharedArrayBuffer, which
browsers only allow on a page served with `serve.py`'s isolation headers, and only from localhost or
HTTPS. Opening `index.html` as a file, or through another computer's IP address, will not work. To
play with others, each player runs their own `serve.py` (see [Multiplayer](#multiplayer)).

### Native debug build (optional)

Needs `clang` and SDL2 as well:

```sh
make native -j$(nproc)                     # -> build/native/<version>/cultures (VERSION=... as for make gen)
./build/native/gamesaga/cultures ..        # run it with the game folder as argument
./build/native/gamenorthland/cultures ../Northland
```

The native build never writes to the game directory: saves and settings go to
`port/userdata-native/<version>/` (override with `USERDATA=dir`).

## Layout

| Path | What |
|---|---|
| `recomp/analyze.py` | Function and CFG recovery: recursive descent seeded from entry, exports, relocations and data pointers; switch tables; gap filling |
| `recomp/regsum.py` | Interprocedural register summaries, so calls spill and reload only what is needed |
| `recomp/translate.py` | x86 → C emitter: lazy flags, cmp/jcc folding, x87 and MMX |
| `runtime/` | CPU helpers, guest memory and heap, PE loader, kernel32/user32/gdi/winmm/…, DirectDraw, DirectSound mixer, DirectInput, DirectMusic and the DLS synth (`dmsynth.c`) |
| `runtime/platform_web.c`, `web/` | Browser layer (see below) |
| `runtime/video.c` | The DirectShow multimedia-stream objects the game plays its MPEG-1 videos with, decoded by pl_mpeg |
| `runtime/dplay.c` | DirectPlay 8 (peer sessions) over the relay, plus the game's two network threads run as cooperative tasks |
| `relay.py` | WebSocket relay that multiplayer uses (`serve.py` serves it at `/net`) |
| `runtime/qol.c` | Autosave default, game speed and quickload through the game's own routines, found by their code in each version |
| `runtime/platform_sdl.c` | Native layer, with debug hooks `HEADLESS`, `CTL_FILE` (also `qol <slot> <value>`), `AUDIO_DUMP`, `TRACE_CALLS`, `DUMP_FRAMES`; `NET_URL` sets the relay (default `ws://127.0.0.1:8000/net`); `SPEED` (clock percent), `SKIP_INTRO`, `AUTOSAVE` (seconds) |

The browser layer works like this:

- **Worker:** the game runs in a Web Worker.
- **Files:** game files are read with synchronous ranged XHR.
- **Display and input:** frames and input travel through a SharedArrayBuffer.
- **Audio:** an AudioWorklet plays the mixed output.
- **Network:** the page owns the multiplayer WebSocket, because the worker never returns to its
  event loop. Incoming messages reach the game through a ring buffer in the same SharedArrayBuffer.

Guest memory is identity-mapped: guest address == wasm address, with the runtime's
own data above 0x38000000.

## Not supported

- **CD audio music:** the "CD Audio" music option needs the game CD; use DirectMusic.
- **Music:** it plays through an approximation of DirectMusic. It has reverb, but no vibrato, filters
  or chord mapping, so it won't sound identical to the original.

## Third-party code

`runtime/third_party/pl_mpeg.h` (MPEG-1 video and MP2 audio decoder for the FMV sequences) is by
Dominic Szablewski, MIT licence.
