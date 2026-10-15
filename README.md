# Cultures – Die Saga: WebAssembly port for Firefox

This port runs the original game code in the browser. The game executable (`GameSaga.exe`,
or one of the other versions under [Game versions](#game-versions)) and the music
driver `gedx8musicdrv.dll` are **statically recompiled** from x86 machine code to C,
one C function per x86 function. They run on top of a hand-written runtime that
emulates the parts of Win32, DirectDraw 7, DirectSound 8, DirectInput and DirectMusic
the game uses. Everything is compiled to WebAssembly.

## Play it

Put this folder (`port/`) inside your Cultures – Die Saga installation, next to `GameSaga.exe`,
build the game as described under [Build](#build), then:

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
- **Display:** smooth or sharp-pixel scaling.
- **Sound:** master volume, and an option to mute while the tab is in the background.
- **Multiplayer server.**

Everything else, such as scrolling, music and detail level, is in the game's own options window
(the scroll icon in the sidebar).

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

Each game executable gets its own build. The runtime is the same for all of them; the build
records which file it was made from (name and CRC-32) and refuses to run with any other file.

| Version | Executable | Status |
|---|---|---|
| Cultures – Die Saga (2005) | `GameSaga.exe` | Fully supported: single player, multiplayer, videos |
| Cultures – 8th Wonder of the World (Nov 2003) | `Game8thwonder.exe` | Works with the Saga data: its campaign and multiplayer were tested |
| Cultures – Northland (Sep 2003) | `GameNorthland.exe` | Needs Northland's own game data (see below) |

**Each executable reads the data formats of its own release.** Northland is older than the Saga
data in this folder: between the two, the game's object type numbers moved by one. Run on Saga
data, Northland looks up the wrong record for every object and stops with a division by zero
while loading the menu's demo map. The original Windows game behaves the same, since the
data, not the port, is the problem. Point Northland at its own installation instead:

```sh
python3 serve.py --version-dir gamenorthland=/path/to/Northland
```

`--version-dir VERSION=DIR` can be given once per version. Versions without one use `--game`.
The native build takes the game folder as its argument:
`./build/native/gamenorthland/cultures /path/to/Northland`.

Adding another version: put its `.exe` in the game folder, then run `make gen EXE=../Other.exe` and
`make web EXE=../Other.exe`. If its own `gedx8musicdrv.dll` sits next to it, that one is used.
If a version stops with "dispatch: no function at 0x…", the recompiler missed a function: add
that address to `recomp/extra/<exe name in lower case>.txt` (one hex address per line) and run
`make gen` and `make web` again. The multiplayer hooks find the game's two network threads by their code, so they work in every
version that shares this engine's code for them; the patterns are in `runtime/dplay.c`.

## Build

This repository contains no game code or data: `web/cultures-<version>.wasm` and `build/gen/` are
generated from your own copy of the game. Build them once with the steps below, and again after changing
anything in `recomp/` or `runtime/`.

### Building the browser version

Tested on Linux with Python 3.14, zig 0.16 and binaryen (wasm-opt) 133.

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

3. **Translate the game to C.** This reads `../GameSaga.exe` and `../gedx8musicdrv.dll` and writes
   about 1 million lines of C to `build/gen/gamesaga/`. It takes about 30 seconds:

   ```sh
   make gen PYTHON=.venv/bin/python
   ```

4. **Compile to WebAssembly.** This writes `web/cultures-gamesaga.wasm`. The first build takes a
   few minutes; later builds only recompile what changed:

   ```sh
   make web -j$(nproc)
   ```

   For another version, add `EXE=`, for example `make gen EXE=../Game8thwonder.exe PYTHON=.venv/bin/python`
   followed by `make web EXE=../Game8thwonder.exe -j$(nproc)`. To build every version found in
   the game folder at once: `make all-versions PYTHON=.venv/bin/python -j$(nproc)`.

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
make native -j$(nproc)                     # -> build/native/gamesaga/cultures (EXE=... for others)
./build/native/gamesaga/cultures ..        # run from the game directory
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
| `runtime/platform_sdl.c` | Native layer, with debug hooks `HEADLESS`, `CTL_FILE`, `AUDIO_DUMP`, `TRACE_CALLS`, `DUMP_FRAMES`; `NET_URL` sets the relay (default `ws://127.0.0.1:8000/net`) |

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
