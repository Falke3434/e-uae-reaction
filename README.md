# E-UAE ReAction

Amiga emulator for **AmigaOS 4.1 (PPC)** with a native **ReAction GUI**.
E-UAE 2.0.0 / ReAction GUI 1.0 (internal build number: v17, see [CHANGELOG.md](CHANGELOG.md)).

This is a fork of [khval/e-uae-1-0-0](https://github.com/khval/e-uae-1-0-0)
(branch `uae-1-1-2`, E-UAE PowerPC JIT project v1.1.2, based on E-UAE 0.8.29-WIP).
The first commit of this repository is the unmodified upstream snapshot, so
`git diff <first-commit> HEAD` shows exactly what was changed.

## What is different from upstream

Details per build are in [CHANGELOG.md](CHANGELOG.md). In short:

- ReAction configuration GUI (`src/gui-reaction/`) with locale catalogs and tooltips
- CD32 emulation: Akiko chip ported from WinUAE (CD-ROM controller, C2P, NVRAM), CD32 ROMs, CD32 pad
- CD-ROM images (ISO, CUE/BIN), IDE controller emulation (Gayle / A4000 IDE)
- Rewritten `bsdsocket.library` emulation, LHA/LZH and LZX archive support
- Fixes and improvements in the CPU/FPU core (items marked **[core]** in the changelog)
- Hand-written, minimal `configure` that runs in the AmigaOS shell

## Building

### On AmigaOS 4.1 (SDK, gcc)

```
sh configure LIBS=-lamiga
make
```

`configure` is a small hand-written Bourne shell script (no autotools needed).
The original Autoconf output is kept as `configure.autoconf-2.71`.
After changing headers run `make clean` (dependency tracking is off).
Options are listed at the top of `configure`.

### Cross-compile on Linux (Docker)

[`tools/e-uae-reaction_os4_build.sh`](tools/e-uae-reaction_os4_build.sh) builds with the
`walkero/amigagccondocker:os4-gcc11` image. It takes a source ZIP of this repository
(for example GitHub's "Download ZIP"):

```
./tools/e-uae-reaction_os4_build.sh 1     # install Docker / pull the image
EUAE_ZIP=/path/to/e-uae-reaction-main.zip ./tools/e-uae-reaction_os4_build.sh 2   # build
./tools/e-uae-reaction_os4_build.sh 3     # create the E-UAE/ distribution folder
```

Python 3 is needed on the build machine: the script runs `tools/mkcatalog.py`.

### Locale catalogs

`Catalogs/E-UAE.cd` (English, message IDs) and `Catalogs/<language>.ct` are the single
source of all GUI texts. `python3 tools/mkcatalog.py` generates
`src/gui-reaction/locale_ids.h`, `src/gui-reaction/locale_defs.c` and
`Catalogs/<language>/E-UAE.catalog`; the generated files are committed.

## Documentation

- `E-UAE.readme` – Aminet-style readme
- `LIESMICH.txt` – German notes
- `docs/` – upstream E-UAE documentation
- `README_original`, `ChangeLog_original`, `CHANGES_original` – upstream files

## License and credits

GNU General Public License, see [COPYING](COPYING). Third-party code and its origin:
[THIRD_PARTY.md](THIRD_PARTY.md).

E-UAE: Richard Drummond and the UAE/WinUAE authors; PowerPC JIT: Almos Rajnai;
upstream maintenance: Kjetil Hvalstrand (khval); ReAction GUI and AmigaOS 4.1 work: Falke_34.
