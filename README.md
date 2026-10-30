<h1 align="center">PlayShell</h1>

<p align="center"><b>Emulators inside the PS5 home screen.</b></p>

<p align="center">
  <img src="https://img.shields.io/badge/version-0.1-white?style=flat-square" alt="version">
  <img src="https://img.shields.io/badge/platform-PS5-white?style=flat-square" alt="platform">
  <img src="https://img.shields.io/badge/consoles-12-white?style=flat-square" alt="consoles">
  <img src="https://img.shields.io/badge/license-GPL--3.0-white?style=flat-square" alt="license">
</p>

---

## Features

- **Play retro games right on the home screen**, in a pop-up over your tiles. No app to open, no "closing" screen
- **12 consoles** in one payload: PlayStation, Nintendo 64, Nintendo DS, GBA, SNES, NES, Game Boy / Color, Game Gear, PC Engine, WonderSwan, DOS
- **JIT** for PlayStation, N64, DS and DOS: full speed on the PS5's CPU
- **PlayStation at 2x resolution**, memory cards per game (shared by its discs)
- **Fullscreen** with the touchpad, or the pop-up with a frame around the game
- **Sound** played through the home screen
- **Hold R3** anywhere to open the menu: the game pauses, R3 again goes back to it
- **Zip and 7z** games for the cartridge consoles
- **Loading spinner** with the game's name while big games load

## Install

Needs a jailbroken PS5 with kstuff and a payload loader.

1. Put your games in `/data/PlayShell/roms/<console>` (the folders are made on the first start).
2. Load `playshell.elf`. A notification says **PlayShell is active**.
3. **Hold R3** on the home screen, pick a console, pick a game.

## Consoles

| Console | Folder | Files | BIOS (`/data/PlayShell/bios`) |
|---|---|---|---|
| PlayStation | `ps1` | `.cue` `.chd` `.pbp` `.iso` `.m3u` | `SCPH1001.BIN` recommended |
| Nintendo 64 | `n64` | `.z64` `.n64` `.v64` | - |
| Nintendo DS | `nds` | `.nds` | optional (`bios7.bin`, `bios9.bin`, `firmware.bin`) |
| Game Boy Advance | `gba` | `.gba` | `gba_bios.bin` |
| SNES | `snes` | `.sfc` `.smc` | - |
| NES | `nes` | `.nes` | - |
| Game Boy / Color | `gb` `gbc` | `.gb` `.gbc` | - |
| Game Gear | `gg` | `.gg` | - |
| PC Engine | `pce` | `.pce` | - |
| WonderSwan / Color | `ws` | `.ws` `.wsc` | - |
| DOS | `dos` | `.zip` `.exe` `.com` `.bat` `.iso` | - |

## Controls

| Button | Action |
|---|---|
| Hold R3 | Open / close the PlayShell menu (the game pauses) |
| ✕ | Open a console / play a game |
| ○ | Back / close |
| L1 / R1 | Jump to the previous / next letter in the game list |
| Touchpad | Fullscreen on / off |
| R2 + right stick | Touch screen (Nintendo DS) |

## Folders on the PS5

| Path | What |
|---|---|
| `/data/PlayShell/roms/` | Your games, one folder per console |
| `/data/PlayShell/bios/` | BIOS files |
| `/data/PlayShell/saves/` | Memory cards and saves |
| `/data/PlayShell/crop/<console>.txt` | Optional edge crop per console (`left right top bottom`) |
| `/data/PlayShell/playshell.log` | Log |

## Credits

PlayShell is built on these emulators (all open source):

- **[Mesen 2](https://github.com/SourMesen/Mesen2)**: NES, SNES, GBA, Game Gear, PC Engine, WonderSwan (GPL-3.0)
- **[PCSX-ReARMed](https://github.com/libretro/pcsx_rearmed)**: PlayStation (GPL-2.0)
- **[melonDS](https://github.com/libretro/melonDS)**: Nintendo DS (GPL-3.0)
- **[Mupen64Plus-Next](https://github.com/libretro/mupen64plus-libretro-nx)**: Nintendo 64 (GPL-2.0)
- **[DOSBox Pure](https://github.com/schellingb/dosbox-pure)**: DOS (GPL-2.0)
- **Walnut-CGB** ([Mr-PauI](https://github.com/Mr-PauI)), based on **[Peanut-GB](https://github.com/deltabeard/Peanut-GB)**, with **minigb_apu** (from [MiniGBS](https://github.com/baines/MiniGBS)): Game Boy / Color (MIT)
- **[OnionHEN](https://github.com/aydencharles/onionHEN/)**: payload injector, detour library and ShellUI groundwork
- **[PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk)**: toolchain and PS5 libraries

## License

GPL-3.0. See [LICENSE](LICENSE).
