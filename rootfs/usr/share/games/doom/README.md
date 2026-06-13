# DOOM WAD files

This directory must contain the DOOM WAD data files to play DOOM on Tiramisù.
**WAD files are not distributed with this project** — you must supply your own
legally obtained copies.

## Required files

| File       | Game           | Source                                   |
|------------|----------------|------------------------------------------|
| `Doom1.WAD`| DOOM (1993)    | Buy on Steam / GOG, or use the shareware |
| `Doom2.wad`| DOOM II (1994) | Buy on Steam / GOG                       |
| `Doom3.WAD`| Ultimate DOOM  | Buy on Steam / GOG                       |

## Where to get them legally

- **Steam**: [DOOM + DOOM II bundle](https://store.steampowered.com/bundle/27490)
- **GOG**: [DOOM](https://www.gog.com/game/the_ultimate_doom)
- **Shareware**: The original shareware `doom1.wad` (Episode 1 only) is freely
  available from id Software's historical releases.
- **FreeDOOM**: A free, open-source WAD replacement —
  [freedoom.github.io](https://freedoom.github.io/)

## How to use

After placing the WAD files here, rebuild the OS with `make all`.
The WADs are packed into the TFS boot image and available at
`/usr/share/games/doom/` at runtime.

Launch DOOM from the Tiramisù shell:

```sh
doom --version=1   # Doom1.WAD
doom --version=2   # Doom2.wad
doom --version=3   # Doom3.WAD
doom               # auto-detect first available WAD
```
