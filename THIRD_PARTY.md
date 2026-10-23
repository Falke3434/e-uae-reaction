# Third-party code

The whole tree is distributed under the GPL (see `COPYING`) as E-UAE. The parts below
come from other projects; the file headers are authoritative for authorship and terms.
Please check them yourself before redistributing outside this repository.

| Files | Origin | Note |
|---|---|---|
| `src/akiko.c` | WinUAE `akiko.cpp` and `flashrom.cpp` (Toni Wilen), C2P algorithm by Mequa | CD32 Akiko chip, ported in v17 |
| `src/lha_*.c`, `src/include/lha*.h` | LHa for UNIX (Y. Tagawa, M. Oki, N. Watazaki, T. Okamoto and others) | as used by WinUAE; global names prefixed `lha_x_` |
| `src/archive_lzx.c` | `unlzx.c` 1.0 (Oliver Gantert, Erik Meusel, Stefano Coletta), via WinUAE (Toni Wilen) | decrunch code; extraction glue is new |
| `src/archive_lha.c` | glue for the LHa decoder | new |
| `src/dms/` | xDMS 1.3 (Andre Rodrigues de la Rocha), public domain per its header | unchanged from upstream |
| `src/` (rest, `docs/`) | UAE / E-UAE (Bernd Schmidt, Richard Drummond, Almos Rajnai, Kjetil Hvalstrand and others) | upstream, see `README_original`, `ChangeLog_original` |
