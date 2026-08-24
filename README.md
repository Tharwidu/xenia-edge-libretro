<p align="center">
    <img height="200px" src="https://raw.githubusercontent.com/xenia-canary/xenia/master/assets/icon/256.png" />
</p>

<h1 align="center">Xenia Edge — libretro core</h1>

Xbox 360 emulation as a libretro core. This packages
[has207/xenia-edge](https://github.com/has207/xenia-edge) — an experimental fork
of [Xenia Canary](https://github.com/xenia-canary/xenia-canary) — as a single
`xenia_edge_libretro` library that RetroArch loads like any other core.

It works across RetroArch versions rather than against one: every frontend
capability it uses is probed for and has a fallback, so an older build that
lacks a callback gets the fallback instead of a broken core.

[![CI](https://github.com/Tharwidu/xenia-edge-libretro/actions/workflows/CI.yml/badge.svg?branch=ra175-compat)](https://github.com/Tharwidu/xenia-edge-libretro/actions/workflows/CI.yml)

**[Download the latest release](https://github.com/Tharwidu/xenia-edge-libretro/releases/latest)** — Windows x64 and Linux x64.

## What this is, and is not

- **Not a new emulator.** Emulation behaviour, compatibility and performance are
  upstream xenia-edge's. Bugs in a game are almost always upstream bugs.
- **Headless by design.** There is no xenia window and no ImGui dialog stack, so
  the core answers the 360's own system dialogs itself — message boxes, the
  storage-device picker, sign-in, and the virtual keyboard. Those answers are
  steerable; see *Configuring*.
- **A personal build.** It is not on the libretro buildbot and is not distributed
  through RetroArch's core downloader. Releases here are the channel.

## Install

Download the zip for your platform and copy the pieces where RetroArch expects
them:

```
xenia_edge_libretro.dll  ->  RetroArch\cores\
xenia_edge_libretro.info ->  RetroArch\info\
config\ (folder)         ->  merge into RetroArch\config\
D3D12\ (folder)          ->  next to retroarch.exe
```

The `config` folder holds a per-core override that stops the left analog stick
doubling as the d-pad — otherwise the stick scrolls menus while you walk. A core
cannot set that itself; there is no libretro call for it. Merge the folder in
rather than replacing your own.

The `D3D12` folder is the DirectX Agility runtime. With it, the Direct3D 12
backend is available and preferred on Windows; without it the core falls back to
its self-contained Vulkan backend on its own.

On Linux, keep `libSDL3.so.0` beside the `.so` — the core resolves it through an
`$ORIGIN` rpath.

## Content formats

| Format | How to launch it |
|---|---|
| ISO (XGD2/XGD3) | the `.iso` directly |
| GOD / SVOD | the extension-less package header file, with its `.data` folder beside it |
| XBLA / STFS | the package file directly |
| ZAR | the `.zar` directly |

Because GOD and XBLA packages have no file extension, frontends that filter by
extension cannot see them. Drop a **`.x360` pointer file** next to your library
instead: a one-line text file containing the absolute path to the package
header. The core follows it.

## Configuring

Three separate mechanisms change how a game runs, and two of them involve files
called "config":

| | What it reaches | Where |
|---|---|---|
| Core options | the ~38 exposed settings | RetroArch's own config |
| xenia config | all 251 xenia cvars | `system/xenia/` |
| Game patches | guest memory, not settings | `<save dir>/patches/` |

Which one wins, how per-title overrides work, and the traps that waste the most
time are documented in **[docs/libretro-configuration.md](docs/libretro-configuration.md)**.
Two worth knowing before you start:

- A per-game `.opt` file **replaces** your global core options rather than
  merging with them, so anything it omits falls back to the *core's* default.
- An option absent from `retroarch-core-options.cfg` likewise takes the core
  default, not whatever you set globally.

## Status

Verified on Windows: Zuma, Viva Piñata, Sonic Unleashed, Halo: Reach, Fable II
(including DLC) and Skate 2 — covering ISO, GOD/SVOD and XBLA/STFS containers,
boot to gameplay.

The same set runs on Linux **except Fable II**, which never presents a frame
there. That failure reproduces on standalone xenia-edge and xenia-canary with
this core removed entirely, so it is an upstream Linux issue rather than a
packaging one.

Save states are not supported — xenia has no save-state implementation to expose.

## Building

Linux:

```sh
python3 xenia-build.py slang
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DXENIA_BUILD_LIBRETRO=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
python3 -c "import importlib.util as u; s=u.spec_from_file_location('xb','xenia-build.py'); \
m=u.module_from_spec(s); s.loader.exec_module(m); m.generate_version_h('build')"
ninja -C build xenia-libretro
```

The result is `build/bin/Linux/xenia_edge_libretro.so`. Windows builds go
through `.github/workflows/libretro-release.yml` (clang-cl, static CRT), which
also produces the redistributable zips.

## Credits

Xenia is the work of Ben Vanik and the Xenia contributors; xenia-canary and
[has207/xenia-edge](https://github.com/has207/xenia-edge) build on it, and this
core is that fork with a libretro front end attached. This repository is a fork
of [danprice142/xenia-edge-libretro](https://github.com/danprice142/xenia-edge-libretro),
which wrote that front end.

Upstream's own FAQ about the desktop emulator — game compatibility, per-title
config advice, platform differences — is
[here](https://github.com/has207/xenia-edge#faq) and applies to the emulation
underneath this core.

Released under the BSD license; see [LICENSE](LICENSE). Not affiliated with
Microsoft. Xbox 360 and Xbox LIVE are trademarks of Microsoft Corporation.
