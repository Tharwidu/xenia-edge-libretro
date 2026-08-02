# Configuring the Xenia Edge libretro core

Three separate mechanisms can change how a game runs, and they are easy to
confuse because two of them involve files called "config". This is what each one
reaches, where it lives, and which wins.

All three are plain text and can be written by hand. None of them requires the
RetroArch menu — which matters, because in some frontends (EmuVR, for one) the
menu is not reachable while a game is running.

| | What it reaches | Scope | Where |
|---|---|---|---|
| **Core options** | the ~38 exposed settings | all games, or one | RetroArch's own config |
| **xenia config** | all 251 xenia cvars | all games, or one | `system/xenia/` |
| **Game patches** | guest memory, not settings | one game | `<save dir>/patches/` |

Paths below are written with forward slashes; on Windows they are backslashes.

---

## 1. Core options — the ~38 exposed settings

The normal libretro surface. Set them in the frontend's core options menu where
you have one, or edit the file directly:

```
retroarch-core-options.cfg      # RetroArch root
```

```
xenia_gpu_backend = "vulkan"
xenia_apply_patches = "whole_file"
```

**Many require a restart.** 23 of the options say "Requires restart" in their
description, and they mean it — the graphics backend, render-target setup, VFS
mounts, memory and JIT init, audio init, and everything the guest reads once at
boot (locale, console type, profile, patches, title updates). The core does
re-read options while running, but xenia's subsystems only sample most cvars at
init, so changing one mid-game will appear to do nothing. That is not a bug, and
it matches standalone xenia, which requires editing `config.toml` and
restarting.

### Per-game core options

RetroArch reads a per-game options file if one exists:

```
config/Xenia Edge/<game name>.opt
```

`Xenia Edge` is the core name and `<game name>` matches the content file. When
it is picked up the log says `Per-Game Options: game-specific core options found
at ...`.

> **Trap: a `.opt` file REPLACES the global core options, it does not merge
> with them.** Anything the `.opt` does not mention falls back to the *core's
> built-in default* — not to whatever you set globally. libretro documents these
> as "full configurations loaded instead of the base core option settings".
>
> So a per-game `.opt` must list **every** option you want in force, not just
> the ones that differ. A file containing only `xenia_gpu_backend` will silently
> reset your patch settings, your render target path, and everything else.

---

## 2. xenia config — all 251 cvars

Core options expose about 38 settings. Xenia itself has 251. The rest — the
targeted EDRAM and accuracy knobs, the per-title workarounds that community
configs are built around — are reachable through xenia's own config file:

```
system/xenia/xenia-edge.config.toml
```

The core creates this on first run and writes it back fully commented, the same
as standalone xenia, so it documents itself. Edit it and restart.

### Per-title cvars

```
system/xenia/config/<TITLEID>.config.toml
```

`<TITLEID>` is the eight-hex-digit title ID, uppercase — e.g.
`4D5307F2.config.toml`. The core clears the previous title's overrides before
applying these, so nothing leaks between games. The log confirms it:
`Applied per-title config for 4D5307F2: ...`.

### Which wins

Loading order is: **xenia config → per-title config → core options**. Core
options are applied last and overwrite, so for the ~38 settings they expose,
**core options win.**

That would make the config file useless for every setting a core option covers —
so an option that can defer offers **`auto`**, meaning "leave whatever the config
set". **To control an exposed setting from a config file, set the corresponding
core option to `auto`.** It is the default for every option that has it, so this
works out of the box.

**Every option that maps to a xenia cvar offers `auto`, and defaults to it.** So
the config file is authoritative out of the box, and a core option only takes
over once you set it to something specific.

Two options offer `auto` but do **not** default to it, because the core
deliberately overrides xenia's own default and silently changing that would
alter behaviour for existing setups:

| Option | Core default | xenia default |
|---|---|---|
| XMA Decoder | `old` | `new` |
| License Mask | `1` (Full) | `0` (None — no DLC) |

Set either to `auto` if you want the config to decide.

The handful of options with no `auto` — VSync, Audio Enabled, Mute, Boot Splash,
Auto Profile — are core-level behaviour rather than xenia cvars, so there is
nothing in a config file for them to defer to.

---

## 3. Game patches — guest memory, not settings

Distinct from everything above. A patch is not a setting: it is a raw guest
memory write. Each entry is an address plus bytes, and the core translates the
address, unprotects the heap, writes, and restores protection while the game
runs. Nothing about it touches cvars.

The upstream library at
[`xenia-canary/game-patches`](https://github.com/xenia-canary/game-patches)
carries roughly 480 of these — 60 fps unlocks, resolution changes, effect
toggles.

**Patches go in the save directory:**

```
<save dir>/patches/<title>.patch.toml
```

> **Trap: this is the SAVE directory, not `system/xenia`.** Putting patch files
> in the system directory does nothing at all, and says nothing in the log. If
> patches appear to be ignored, check this first.

A patch file looks like this:

```toml
title_id = "4D5307F2"
hash = "B138AE95A6337A52"    # gates on default.xex, so it is version-specific
[[patch]]
  name = "60 FPS"
  is_enabled = false          # upstream ships every patch disabled
  [[patch.be8]]
    address = 0x8246ab54
    value = 0x01
```

The `hash` must match the game's executable — the log prints
`Module Hash: ...` so you can check. A patch file for a different release of the
same title will be skipped.

### Turning patches on

The **Apply Game Patches** option (`xenia_apply_patches`) has three states:

- **`enabled`** (default) — honour each patch's own `is_enabled` flag, exactly
  as standalone xenia does. Since upstream ships everything as
  `is_enabled = false`, this means downloading a patch file does nothing until
  you open it and edit the flag.
- **`whole_file`** — apply every patch in the file. Dropping the file in enables
  it; deleting the file disables it. No editing.
- **`disabled`** — ignore all patch files without deleting anything.

`whole_file` is the convenient one, but it is opt-in for a reason: **a patch
file is not necessarily one patch.** Viva Piñata and Skate 3 ship one each, but
Fable II ships three and Sonic Unleashed ships seven in a single file — among
them Disable Shadow Maps, Disable Depth of Field and Aspect Ratio. Those are
individual taste choices, and switching all seven on because a file exists would
surprise anyone. For the common single-patch case the two modes are identical.

Requires a restart either way.

---

## Worked example

Viva Piñata at 60 fps, with no file editing:

1. Download `4D5307F2 - Viva Pinata.patch.toml` from the upstream repo.
2. Drop it in `<save dir>/patches/`.
3. Set `xenia_apply_patches = "whole_file"`.
4. Restart the core.

The log should show the module hash matching the patch's `hash` line. Measured
result on this core: **guest frame rate 30.0 → 60.0 fps.**

---

## Finding your directories

The save and system directories are the frontend's, not the core's, and the core
logs both on startup:

```
Config folder: <system dir>/xenia
```

In RetroArch they default to `saves/` and `system/` under the RetroArch root,
and both are configurable in Settings → Directory.
