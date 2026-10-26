#!/bin/bash
# =============================================================================
# e-uae-reaction_os4_build_v11.sh – E-UAE + ReAction-GUI für AmigaOS 4.1 PPC
#                                  (Cross-Compile auf Ubuntu per Docker)
#
# Quelle:       e-uae-reaction-v10.zip (E-UAE 0.8.29-WIP + ReAction-GUI, mit dem
#               handgeschriebenen configure für AmigaOS 4.1) neben dem Skript
# Docker-Image: walkero/amigagccondocker:os4-gcc11   (wie descent3_150_os4_build)
#
# Vorgehen (Cross-Compile statt "sh configure" auf dem Amiga):
#   1. configure mit --host=ppc-amigaos, CC=ppc-amigaos-gcc, CC_FOR_BUILD=gcc.
#      Das Skript führt keine Testprogramme aus (nur Compile-/Link-Tests),
#      funktioniert also auch beim Cross-Build. Wegen CC_FOR_BUILD
#      konfiguriert es src/tools mit dem NATIVEN gcc.
#   2. Die Generatoren (build68k, gencpu, gencomp, genblitter, genlinetoscr)
#      laufen auf dem Host und erzeugen cpudefs.c, cpuemu_*.c, compstbl.c,
#      blit*.c, linetoscr.c. Das passiert seriell VOR dem parallelen make.
#      genlinetoscr bekommt -b, weil configure für das Ziel Big-Endian erkennt.
#   3. make baut src/uae (+ readdisk, make_hdf) für PPC.
#
# Aufruf:  ./e-uae-reaction_os4_build_v11.sh [1|2|3]
#   1 = Docker installieren / Image holen
#   2 = E-UAE bauen
#   3 = Distributions-Ordner (E-UAE/) erstellen
#
# Umgebungsvariablen:
#   EUAE_ZIP=pfad.zip        anderes Quellarchiv
#   EUAE_CONFIG_OPTS="..."   zusätzliche configure-Optionen, z. B.
#                            "--disable-jit --disable-cycle-exact-cpu"
#   EUAE_LIBS="-lamiga"      Wert für LIBS= (Standard -lamiga)
#   EUAE_CFLAGS="..."        Wert für CFLAGS= (Standard: -g -O2 -fcommon -fgnu89-inline)
#   JOBS=N                   parallele Jobs (Standard: Anzahl CPU-Kerne)
#   OS4_PATCH_ONLY=1         Option 2 stoppt vor Docker (nur entpacken)
# =============================================================================

set -e
set -o pipefail

GREEN='\033[0;32m'; CYAN='\033[0;36m'; YELLOW='\033[1;33m'; RED='\033[0;31m'
BOLD='\033[1m'; NC='\033[0m'
info() { echo -e "${CYAN}>>> $*${NC}"; }
ok()   { echo -e "${GREEN}>>> $*${NC}"; }
warn() { echo -e "${YELLOW}>>> WARNUNG: $*${NC}"; }
die()  { echo -e "${RED}>>> FEHLER: $*${NC}"; exit 1; }

SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
if [ -n "$SUDO_USER" ]; then REALHOME="/home/$SUDO_USER"; else REALHOME="$HOME"; fi
WORKDIR="${WORKDIR:-$REALHOME/code}"
DOCKER_IMAGE="walkero/amigagccondocker:os4-gcc11"

EUAE_ZIP="${EUAE_ZIP:-}"
if [ -z "$EUAE_ZIP" ]; then
    # erstes passende Archiv neben dem Skript (e-uae-reaction*.zip)
    EUAE_ZIP="$(ls -t "$SCRIPTDIR"/*e-uae-reaction*.zip 2>/dev/null | head -n1 || true)"
fi
PROJ_DIR_NAME="e-uae-reaction"
EUAE_LIBS="${EUAE_LIBS:--lamiga}"
EUAE_CONFIG_OPTS="${EUAE_CONFIG_OPTS:-}"
# Der Code ist von ~2010 (gnu89): gcc >= 10 braucht -fcommon (sonst "multiple
# definition" beim Linken) und -fgnu89-inline (sonst fehlen die INLINE-
# Funktionen in dms/u_heavy.c: "undefined reference to decode_c/decode_p").
EUAE_CFLAGS="${EUAE_CFLAGS:--g -O2 -fcommon -fgnu89-inline}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

CHOICE="${1:-}"
if [ -z "$CHOICE" ]; then
    echo ""
    echo -e "${BOLD}╔══════════════════════════════════════════════════╗${NC}"
    echo -e "${BOLD}║   E-UAE + ReAction-GUI für AmigaOS 4.1  (v11)    ║${NC}"
    echo -e "${BOLD}╠══════════════════════════════════════════════════╣${NC}"
    echo -e "${BOLD}║                                                  ║${NC}"
    echo -e "${BOLD}║  1)${NC} Docker installieren                         ${BOLD}║${NC}"
    echo -e "${BOLD}║  2)${NC} E-UAE bauen                                 ${BOLD}║${NC}"
    echo -e "${BOLD}║  3)${NC} Distributions-Ordner erstellen              ${BOLD}║${NC}"
    echo -e "${BOLD}║  0)${NC} Beenden                                     ${BOLD}║${NC}"
    echo -e "${BOLD}║                                                  ║${NC}"
    echo -e "${BOLD}╚══════════════════════════════════════════════════╝${NC}"
    echo ""
    read -rp "Auswahl [0/1/2/3]: " CHOICE
fi

case "$CHOICE" in
    1) DO_DOCKER=1 ;;
    2) DO_BUILD=1  ;;
    3) DO_DIST=1   ;;
    0) echo "Abgebrochen."; exit 0 ;;
    *) die "Ungültige Auswahl!" ;;
esac

# ─── SCHRITT 1: Docker installieren ──────────────────────────────────────────
if [ -n "$DO_DOCKER" ]; then
    info "━━━ Docker installieren ━━━"
    if command -v docker &>/dev/null; then
        ok "Docker bereits installiert: $(docker --version)"
    else
        sudo apt-get update -q
        sudo apt-get install -y -q ca-certificates curl gnupg lsb-release
        sudo install -m 0755 -d /etc/apt/keyrings
        curl -fsSL https://download.docker.com/linux/ubuntu/gpg | \
            sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg
        sudo chmod a+r /etc/apt/keyrings/docker.gpg
        echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" | \
            sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
        sudo apt-get update -q
        sudo apt-get install -y -q docker-ce docker-ce-cli containerd.io docker-buildx-plugin
        sudo usermod -aG docker "$USER"
        sudo systemctl enable docker && sudo systemctl start docker
        ok "Docker installiert."
        warn "Bitte neu einloggen, dann Option 2 starten."
    fi
    docker pull "$DOCKER_IMAGE"
    ok "Docker-Image bereit."
fi

# ─── SCHRITT 2: Bauen ────────────────────────────────────────────────────────
if [ -n "$DO_BUILD" ]; then
    info "━━━ Prüfe Voraussetzungen ━━━"
    command -v unzip &>/dev/null || die "unzip fehlt: sudo apt install unzip"
    if [ -z "$OS4_PATCH_ONLY" ]; then
        command -v docker &>/dev/null || die "Docker fehlt – Option 1 ausführen."
    fi
    [ -n "$EUAE_ZIP" ] && [ -f "$EUAE_ZIP" ] || \
        die "Quellarchiv fehlt: e-uae-reaction*.zip neben das Skript legen (oder EUAE_ZIP=... setzen)."

    mkdir -p "$WORKDIR"
    PROJDIR="$WORKDIR/$PROJ_DIR_NAME"
    # Dateien aus früheren Container-Läufen können root gehören
    rm -rf "$PROJDIR" 2>/dev/null || sudo rm -rf "$PROJDIR"

    info "Entpacke $EUAE_ZIP..."
    TMPUNZ="$WORKDIR/euae-unzip.tmp"
    rm -rf "$TMPUNZ" 2>/dev/null || sudo rm -rf "$TMPUNZ"
    mkdir -p "$TMPUNZ"
    unzip -q -o "$EUAE_ZIP" -d "$TMPUNZ" || die "Entpacken fehlgeschlagen."
    TOP="$(find "$TMPUNZ" -maxdepth 3 -type f -name configure.in -not -path '*/src/*' | head -n1)"
    [ -n "$TOP" ] || die "ZIP enthält kein configure.in (kein E-UAE-Quellbaum?)"
    TOPDIR="$(dirname "$TOP")"
    [ -f "$TOPDIR/configure" ]            || die "configure fehlt im Archiv."
    [ -d "$TOPDIR/src/gui-reaction" ]     || die "src/gui-reaction fehlt im Archiv (ReAction-GUI)."
    mv "$TOPDIR" "$PROJDIR"
    rm -rf "$TMPUNZ"
    # Zeilenenden: falls das Archiv unter Windows entstand
    for f in configure src/tools/configure; do
        [ -f "$PROJDIR/$f" ] && sed -i 's/\r$//' "$PROJDIR/$f"
    done
    ok "Quelle entpackt: $PROJDIR"

    # --- Keine Patches mehr noetig ----------------------------------------
    # Alle Fixes und Erweiterungen (v1-v10: Chooser, Farben/WORDS_BIGENDIAN,
    # Beenden, CPU-Last, Locale, Tooltips, Shortcuts-Seite, ...) stecken schon im
    # Quellarchiv e-uae-reaction-v10.zip.
    command -v python3 &>/dev/null || die "python3 fehlt (Katalog-Erzeugung): sudo apt install python3"
    ( cd "$PROJDIR" && python3 tools/mkcatalog.py ) || die "mkcatalog.py fehlgeschlagen."
    ok "Katalog erzeugt."

    # --- Container-Build-Skript --------------------------------------------
    cat > "$PROJDIR/build_in_container.sh" << 'INNEREOF'
#!/bin/bash
set -e
set -o pipefail

BASE="$(pwd)"
STAGEDIR="$BASE/stage"
rm -rf "$STAGEDIR"
mkdir -p "$STAGEDIR"

LIBS_OPT="${EUAE_LIBS:--lamiga}"
CFLAGS_OPT="${EUAE_CFLAGS:--g -O2 -fcommon -fgnu89-inline}"
JOBS="${JOBS:-2}"

command -v ppc-amigaos-gcc >/dev/null || { echo "!!! ppc-amigaos-gcc nicht gefunden"; exit 1; }
# Host-Compiler für die Generatoren (gencpu & Co. laufen im Container selbst)
HOSTCC=""
for c in gcc cc; do command -v $c >/dev/null && { HOSTCC=$c; break; }; done
if [ -z "$HOSTCC" ]; then
    echo ">>> Kein nativer gcc im Image - installiere build-essential..."
    (apt-get update -q && apt-get install -y -q build-essential) >/dev/null 2>&1 \
        || { echo "!!! Kein nativer gcc verfügbar (wird für gencpu/gencomp/... gebraucht)"; exit 1; }
    HOSTCC=gcc
fi
echo ">>> Ziel-Compiler: $(ppc-amigaos-gcc --version | head -n1)"
echo ">>> Host-Compiler: $($HOSTCC --version | head -n1)"

# Sauberer Stand (falls Quellbaum schon konfiguriert war)
make distclean >/dev/null 2>&1 || true
rm -f config.log config.status config.defs

# --- 1. configure (Cross) ------------------------------------------------------
echo ">>> configure ..."
# shellcheck disable=SC2086
sh ./configure --host=ppc-amigaos \
    CC=ppc-amigaos-gcc \
    CC_FOR_BUILD="$HOSTCC" CFLAGS_FOR_BUILD="-O1" \
    CFLAGS="$CFLAGS_OPT" \
    LIBS="$LIBS_OPT" \
    $EUAE_CONFIG_OPTS 2>&1 | tee configure.out \
    || { echo "!!! configure fehlgeschlagen - letzte Zeilen config.log:"; tail -n 40 config.log; exit 1; }

echo ">>> Zusammenfassung:"
sed -n '/^configure: =====/,/^configure: =====/p' configure.out | sed 's/^/    /' || true

# --- 2. Generatoren auf dem Host (seriell) ------------------------------------
echo ">>> Generatoren (gencpu, gencomp, genblitter, genlinetoscr, build68k) ..."
make -C src cpudefs.c cpuemu_0.c compstbl.c blit.h blitfunc.h blitfunc.c \
    blittable.c linetoscr.c 2>&1 | tee gen.log | grep -v -i "warning" | tail -n 25 \
    || { echo "!!! Generatoren fehlgeschlagen - siehe gen.log"; tail -n 40 gen.log; exit 1; }

# Big-Endian-Kontrolle: linetoscr muss mit -b erzeugt worden sein
if grep -q "TARGET_BIGENDIAN_TRUE@" src/Makefile 2>/dev/null; then
    echo "!!! src/Makefile enthält ungelöste @TARGET_BIGENDIAN_TRUE@-Marker"; exit 1
fi
grep -n "^genlinetoscr_args" src/Makefile | sed 's/^/    /' || true

# --- 3. Hauptbuild --------------------------------------------------------------
echo ">>> make -j$JOBS ..."
if ! make -j"$JOBS" > build.log 2>&1; then
    echo ">>> Paralleler Lauf fehlgeschlagen - wiederhole seriell für lesbare Fehlermeldung..."
    if ! make > build_serial.log 2>&1; then
        echo "!!! Build fehlgeschlagen. Letzte Zeilen:"
        grep -n -E "error|Error|undefined reference|No such file" build_serial.log | head -n 40
        echo "--- Ende von build_serial.log ---"
        tail -n 30 build_serial.log
        exit 1
    fi
fi
grep -c -i "warning" build.log 2>/dev/null | sed 's/^/    Warnungen: /' || true

[ -f src/uae ] || { echo "!!! src/uae wurde nicht erzeugt"; exit 1; }

cp src/uae "$STAGEDIR/uae"
ppc-amigaos-strip --strip-unneeded -o "$STAGEDIR/uae.stripped" src/uae 2>/dev/null || true
for t in readdisk make_hdf; do
    [ -f "src/$t" ] && cp "src/$t" "$STAGEDIR/$t"
done
{
    echo "libs=$LIBS_OPT"
    echo "cflags=$CFLAGS_OPT"
    echo "config_opts=$EUAE_CONFIG_OPTS"
    echo "file: $(file src/uae 2>/dev/null || true)"
} > "$STAGEDIR/buildinfo.txt"
echo ">>> Fertig: $STAGEDIR/uae"
INNEREOF
    chmod +x "$PROJDIR/build_in_container.sh"

    if [ -n "$OS4_PATCH_ONLY" ]; then
        ok "OS4_PATCH_ONLY gesetzt - Stopp vor Docker. Quelle: $PROJDIR"
        exit 0
    fi

    info "━━━ Baue im Docker-Container ━━━"
    docker run --rm \
        -e EUAE_LIBS="$EUAE_LIBS" \
        -e EUAE_CFLAGS="$EUAE_CFLAGS" \
        -e EUAE_CONFIG_OPTS="$EUAE_CONFIG_OPTS" \
        -e JOBS="$JOBS" \
        -v "$WORKDIR:/opt/code" \
        -w "/opt/code/$PROJ_DIR_NAME" \
        "$DOCKER_IMAGE" \
        "/opt/code/$PROJ_DIR_NAME/build_in_container.sh" \
        || die "Build im Container fehlgeschlagen – siehe Ausgabe oben (Logs: $PROJDIR/*.log)."

    [ -f "$PROJDIR/stage/uae" ] || die "Kein Binary in $PROJDIR/stage."
    ok "Build erfolgreich: $PROJDIR/stage/uae"
    cat "$PROJDIR/stage/buildinfo.txt" 2>/dev/null || true
fi

# ─── SCHRITT 3: Distributions-Ordner erstellen ───────────────────────────────
if [ -n "$DO_DIST" ]; then
    info "━━━ Erstelle Distributions-Ordner ━━━"
    SRC="$WORKDIR/$PROJ_DIR_NAME"
    DIST="$WORKDIR/dist/E-UAE"
    [ -f "$SRC/stage/uae" ] || die "$SRC/stage/uae fehlt – zuerst Option 2 ausführen."

    rm -rf "$DIST" 2>/dev/null || sudo rm -rf "$DIST"
    mkdir -p "$DIST/docs" "$DIST/configs" "$DIST/Roms" "$DIST/Floppies" \
             "$DIST/HardFiles" "$DIST/Isos" "$DIST/SaveStates" "$DIST/Screenshots"
    if [ -f "$SRC/stage/uae.stripped" ]; then
        cp "$SRC/stage/uae.stripped" "$DIST/E-UAE"
    else
        cp "$SRC/stage/uae" "$DIST/E-UAE"
    fi
    for t in readdisk make_hdf; do
        [ -f "$SRC/stage/$t" ] && cp "$SRC/stage/$t" "$DIST/$t"
    done
    cp "$SRC"/docs/*.txt "$DIST/docs/" 2>/dev/null || true
    for f in E-UAE.readme README README_original LIESMICH.txt COPYING CHANGES_original; do
        [ -f "$SRC/$f" ] && cp "$SRC/$f" "$DIST/"
    done
    # Locale-Kataloge (PROGDIR:Catalogs/<Sprache>/E-UAE.catalog)
    if [ -d "$SRC/Catalogs" ]; then
        for d in "$SRC"/Catalogs/*/; do
            [ -f "${d}E-UAE.catalog" ] || continue
            mkdir -p "$DIST/Catalogs/$(basename "$d")"
            cp "${d}E-UAE.catalog" "$DIST/Catalogs/$(basename "$d")/"
        done
    fi
    # Icons aus package/ (ADF-files.info, config.info) – wie make_lha
    [ -d "$SRC/package" ] && cp -a "$SRC"/package/. "$DIST/" 2>/dev/null || true

    ok "Distributions-Ordner erstellt: $DIST"
    info "Kickstart-ROM und ADFs/HDFs selbst dazulegen; Start: 'E-UAE' (ReAction-Konfigurations-GUI)."
fi
