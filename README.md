# Pastime

Pastime is a fork of [RetroArch](https://github.com/libretro/RetroArch) that delivers a [MinUI](https://github.com/shauninman/MinUI)-style experience — no config required, folder-based content, minimalist launcher UX — on modern Android handhelds.

It is built for devices like the Retroid Pocket series, Ayn Odin, Anbernic Win/Android handhelds, and similar hardware that MinUI itself does not target. Pastime piggybacks on RetroArch's mature core ecosystem and platform support, but replaces the front-end UX with something closer in spirit to a Game Boy menu than to XMB.

## Status

Early. See [`PLAN.md`](PLAN.md) for the roadmap and current milestone.

## How it works

The filesystem is the configuration. Pastime looks for a `Pastime/` directory at storage root:

```
Pastime/
   Roms/
      Super Nintendo (snes9x)/
         Chrono Trigger.smc
      Game Boy Advance (mgba)/
         Metroid Fusion.gba
   Bios/
   Saves/
   States/
```

System folders inside `Roms/` are named freely, with a `(<core_ident>)` suffix that names the short libretro core identifier — `snes9x`, `mgba`, `genesis_plus_gx`, etc. Folders without the suffix, or whose core isn't on the libretro buildbot, are simply hidden. Cores download automatically the first time you launch a game that needs them.

Internally:

- The Pastime UI is implemented as a new RetroArch **menu driver** (`menu/drivers/pastime.c`), alongside XMB / Ozone / RGUI.
- Storage layout, folder parsing, and core auto-download live in a self-contained `pastime/` module.
- Edits to upstream RetroArch files are kept minimal, marked with `/* PASTIME: ... */`, and enumerated in `PLAN.md`. The goal is a fork that can rebase cleanly on upstream master indefinitely.

## Building

Same as upstream RetroArch — `./configure && make` for desktop, per-platform `Makefile.*` for everything else. See `CONTRIBUTING.md` and the [RetroArch documentation](https://docs.libretro.com/) for platform-specific build instructions.

## Relationship to upstream

Pastime tracks `libretro/RetroArch` master and rebases periodically. Bug fixes and improvements to RetroArch internals that aren't Pastime-specific should be sent upstream rather than carried as fork patches.

The upstream RetroArch project, libretro API, and all included cores are the work of the libretro team and contributors. See `AUTHORS.h` and the upstream repository for credits.

## License

Same as RetroArch — see `COPYING`.
