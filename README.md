# Cultures – Die Saga: WebAssembly port for Firefox

This port runs the original game code in the browser. `GameSaga.exe` and the music
driver `gedx8musicdrv.dll` are **statically recompiled** from x86 machine code to C,
one C function per x86 function. They run on top of a hand-written runtime that
emulates the parts of Win32, DirectDraw 7, DirectSound 8, DirectInput and DirectMusic
the game uses. Everything is compiled to WebAssembly.

## Play it

Put this folder (`port/`) inside your Cultures – Die Saga installation, next to `GameSaga.exe`,
build `web/cultures.wasm` as described under [Build](#build), then:

```sh
cd port
python3 serve.py            # serves the game folder one level up
# open http://127.0.0.1:8000/ in Firefox, click "Start game"
```

`serve.py --game /path/to/Cultures` points the server at another install.

- Saves and settings are written to `port/userdata/`; the original files are never modified.
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

## Build

This repository contains no game code or data: `web/cultures.wasm` and `build/gen/` are generated
from your own copy of `GameSaga.exe`. Build them once with the steps below, and again after changing
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
   about 1 million lines of C to `build/gen/`. It takes about 30 seconds:

   ```sh
   make gen PYTHON=.venv/bin/python
   ```

4. **Compile to WebAssembly.** This writes `web/cultures.wasm`. The first build takes a few minutes;
   later builds only recompile what changed:

   ```sh
   make web -j$(nproc)
   ```

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
make native -j$(nproc)                     # -> build/native/cultures
./build/native/cultures ..                 # run from the game directory
```

The native build never writes to the game directory: saves and settings go to
`port/userdata-native/` (override with `USERDATA=dir`).

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
