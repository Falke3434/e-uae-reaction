#!/usr/bin/env python3
# ================================================================
# mkcatalog.py  -  build the E-UAE locale files
#
# Run from the main E-UAE folder (the one with Makefile):
#
#     python3 tools/mkcatalog.py
#
# Input (single source of truth for all texts):
#     Catalogs/E-UAE.cd     English texts + fixed message IDs
#     Catalogs/<language>.ct       one translation per language,
#                                  e.g. Catalogs/deutsch.ct
#
# Output:
#     src/gui-reaction/locale_ids.h             MSG_xxx constants (included by the program)
#     src/gui-reaction/locale_defs.c            built-in English text table
#     Catalogs/<language>/E-UAE.catalog
#                                  binary catalog for locale.library
#
# The .cd / .ct files use the FlexCat file format (## header lines,
# "NAME (ID//)" + one text line + ";"), so FlexCat can be used instead
# of this script if you prefer - the program itself only needs the
# generated .h/.c files and the finished .catalog files.
#
# Text escapes understood: \n \t \\ \" \xHH (hex byte).
# .ct files are ISO-8859-1 (Latin-1), like AmigaOS itself.
# No third-party modules needed (Python 3.6+).
# ================================================================

import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAT_DIR = os.path.join(ROOT, "Catalogs")
SRC_DIR = os.path.join(ROOT, "src", "gui-reaction")
CD_FILE = os.path.join(CAT_DIR, "E-UAE.cd")
CATALOG_NAME = "E-UAE.catalog"

ENTRY_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*(\d*)\s*/[^/]*/[^)]*\)\s*$")
NAME_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*$")
FMT_RE = re.compile(r"%[-+ #0-9.]*(?:l|h|ll)?[a-zA-Z%]")


def unescape(text):
    """Text from a .cd/.ct line -> bytes (without terminating NUL)."""
    out = bytearray()
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c != "\\":
            out += c.encode("latin-1")
            i += 1
            continue
        i += 1
        if i >= n:
            raise ValueError("text ends with a lone backslash")
        c = text[i]
        i += 1
        if c == "n":
            out.append(10)
        elif c == "t":
            out.append(9)
        elif c == "\\":
            out.append(92)
        elif c == '"':
            out.append(34)
        elif c == "x":
            hx = text[i:i + 2]
            if len(hx) != 2 or any(h not in "0123456789abcdefABCDEF" for h in hx):
                raise ValueError("bad \\x escape")
            out.append(int(hx, 16))
            i += 2
        else:
            raise ValueError("unknown escape \\%s" % c)
    return bytes(out)


def read_lines(path):
    with open(path, "r", encoding="latin-1", newline=None) as f:
        return f.read().split("\n")


def parse_header(line, hdr):
    m = re.match(r"^##\s*(\w+)\s+(.*)$", line)
    if m:
        hdr[m.group(1).lower()] = m.group(2).strip()


def parse_cd(path):
    hdr = {}
    entries = []            # (name, id, bytes)
    cur = None
    seen_ids = {}
    seen_names = set()
    lines = read_lines(path)
    for ln, line in enumerate(lines, 1):
        line = line.rstrip("\r")
        if line.startswith("##"):
            parse_header(line, hdr)
            continue
        if line.startswith(";"):
            cur = None
            continue
        if not line.strip():
            continue
        m = ENTRY_RE.match(line)
        if m:
            name, sid = m.group(1), m.group(2)
            if not sid:
                raise SystemExit("%s:%d: %s has no explicit ID" % (path, ln, name))
            sid = int(sid)
            if name in seen_names:
                raise SystemExit("%s:%d: duplicate name %s" % (path, ln, name))
            if sid in seen_ids:
                raise SystemExit("%s:%d: ID %d used by %s and %s"
                                 % (path, ln, sid, seen_ids[sid], name))
            seen_names.add(name)
            seen_ids[sid] = name
            cur = [name, sid, None]
            entries.append(cur)
            continue
        if cur is None:
            raise SystemExit("%s:%d: unexpected line: %r" % (path, ln, line))
        if cur[2] is not None:
            raise SystemExit("%s:%d: %s has more than one text line"
                             % (path, ln, cur[0]))
        try:
            cur[2] = unescape(line)
        except ValueError as e:
            raise SystemExit("%s:%d: %s" % (path, ln, e))
    for e in entries:
        if e[2] is None:
            raise SystemExit("%s: %s has no text" % (path, e[0]))
    return hdr, [tuple(e) for e in entries]


def parse_ct(path):
    hdr = {}
    trans = {}
    cur = None
    for ln, line in enumerate(read_lines(path), 1):
        line = line.rstrip("\r")
        if line.startswith("##"):
            parse_header(line, hdr)
            continue
        if line.startswith(";"):
            cur = None
            continue
        if not line.strip():
            continue
        m = NAME_RE.match(line)
        if m and cur is None:
            cur = m.group(1)
            if cur in trans:
                raise SystemExit("%s:%d: duplicate entry %s" % (path, ln, cur))
            trans[cur] = None
            continue
        if cur is None:
            raise SystemExit("%s:%d: unexpected line: %r" % (path, ln, line))
        if trans[cur] is not None:
            raise SystemExit("%s:%d: %s has more than one text line" % (path, ln, cur))
        try:
            trans[cur] = unescape(line)
        except ValueError as e:
            raise SystemExit("%s:%d: %s" % (path, ln, e))
    return hdr, trans


def c_string(b):
    out = []
    for ch in b:
        if ch == 10:
            out.append("\\n")
        elif ch == 9:
            out.append("\\t")
        elif ch == 34:
            out.append('\\"')
        elif ch == 92:
            out.append("\\\\")
        elif 32 <= ch < 127:
            out.append(chr(ch))
        else:
            out.append("\\%03o" % ch)
    return '"' + "".join(out) + '"'


def write_c_files(entries, hdr):
    ids = sorted(entries, key=lambda e: e[1])
    maxid = ids[-1][1]
    with open(os.path.join(SRC_DIR, "locale_ids.h"), "w", newline="\n") as f:
        f.write("/* GENERATED by tools/mkcatalog.py from Catalogs/E-UAE.cd - do not edit. */\n")
        f.write("#ifndef LOCALE_IDS_H\n#define LOCALE_IDS_H\n\n")
        for name, sid, _ in ids:
            f.write("#define %-26s %d\n" % (name, sid))
        f.write("\n#define MSG_MAX_ID                 %d\n\n" % maxid)
        f.write("/* built-in English texts, indexed by message ID (see locale_support.c) */\n")
        f.write("extern const char * const g_MsgDefault[MSG_MAX_ID + 1];\n\n")
        f.write("#endif /* LOCALE_IDS_H */\n")
    table = {sid: b for _, sid, b in entries}
    with open(os.path.join(SRC_DIR, "locale_defs.c"), "w", newline="\n") as f:
        f.write("/* GENERATED by tools/mkcatalog.py from Catalogs/E-UAE.cd - do not edit. */\n")
        f.write('#include "locale_ids.h"\n\n')
        f.write("const char * const g_MsgDefault[MSG_MAX_ID + 1] = {\n")
        for i in range(maxid + 1):
            if i in table:
                name = [e[0] for e in entries if e[1] == i][0]
                f.write("    /* %-24s */ %s,\n" % (name, c_string(table[i])))
            else:
                f.write("    /* (unused %d) */ \"\",\n" % i)
        f.write("};\n")


def chunk(tag, data):
    pad = b"\0" if len(data) % 2 else b""
    return tag + struct.pack(">I", len(data)) + data + pad


def build_catalog(lang, ver, entries, trans):
    """Binary IFF CTLG catalog (see AmigaOS locale.library docs)."""
    strs = bytearray()
    used = 0
    for name, sid, default in sorted(entries, key=lambda e: e[1]):
        text = trans.get(name)
        if text is None or text == b"":
            continue                      # missing: program falls back to English
        if text == default:
            continue                      # identical to the built-in text: not needed
        data = text + b"\0"
        data += b"\0" * ((-len(data)) % 4)
        strs += struct.pack(">II", sid, len(data)) + data
        used += 1
    body = b"CTLG"
    body += chunk(b"FVER", ver.encode("latin-1") + b"\0")
    body += chunk(b"LANG", lang.encode("latin-1") + b"\0")
    body += chunk(b"CSET", struct.pack(">8I", 0, 0, 0, 0, 0, 0, 0, 0))
    body += chunk(b"STRS", bytes(strs))
    return b"FORM" + struct.pack(">I", len(body)) + body, used


def read_catalog(data):
    """Tiny reader used for the self-test after writing."""
    assert data[:4] == b"FORM" and data[8:12] == b"CTLG"
    assert struct.unpack(">I", data[4:8])[0] == len(data) - 8
    pos = 12
    out = {}
    lang = None
    while pos < len(data):
        tag = data[pos:pos + 4]
        size = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        pos += 8 + size + (size % 2)
        if tag == b"LANG":
            lang = body.split(b"\0")[0].decode("latin-1")
        if tag == b"STRS":
            p = 0
            while p < len(body):
                sid, ssz = struct.unpack(">II", body[p:p + 8])
                s = body[p + 8:p + 8 + ssz]
                out[sid] = s.split(b"\0")[0]
                p += 8 + ssz
    return lang, out


def check_placeholders(name, en, tr, path):
    a = FMT_RE.findall(en.decode("latin-1"))
    b = FMT_RE.findall(tr.decode("latin-1"))
    if a != b:
        print("WARNING: %s: %s: placeholders differ: %s vs %s" % (path, name, a, b))


def main():
    hdr, entries = parse_cd(CD_FILE)
    write_c_files(entries, hdr)
    print("wrote src/gui-reaction/locale_ids.h, src/gui-reaction/locale_defs.c (%d messages)" % len(entries))
    ver = hdr.get("version", "$VER: E-UAE.catalog 1.0")

    known = {e[0] for e in entries}
    defaults = {e[0]: e[2] for e in entries}
    for fn in sorted(os.listdir(CAT_DIR)):
        if not fn.endswith(".ct"):
            continue
        path = os.path.join(CAT_DIR, fn)
        thdr, trans = parse_ct(path)
        lang = thdr.get("language", fn[:-3])
        for name in trans:
            if name not in known:
                print("WARNING: %s: %s is not in the .cd (ignored)" % (fn, name))
        missing = [n for n in sorted(known) if not trans.get(n)]
        for name, tr in trans.items():
            if name in defaults and tr:
                check_placeholders(name, defaults[name], tr, fn)
        data, used = build_catalog(lang, thdr.get("version", ver), entries, trans)
        outdir = os.path.join(CAT_DIR, lang)
        os.makedirs(outdir, exist_ok=True)
        outpath = os.path.join(outdir, CATALOG_NAME)
        with open(outpath, "wb") as f:
            f.write(data)
        # self-test: read it back
        rl, back = read_catalog(data)
        ids = {e[0]: e[1] for e in entries}
        for name, tr in trans.items():
            if name in ids and tr and tr != defaults[name]:
                assert back[ids[name]] == tr, name
        print("wrote %s  (%d strings, %d bytes)%s"
              % (os.path.relpath(outpath, ROOT), used, len(data),
                 ("  - untranslated, using English: " + ", ".join(missing)) if missing else ""))


if __name__ == "__main__":
    sys.exit(main())
