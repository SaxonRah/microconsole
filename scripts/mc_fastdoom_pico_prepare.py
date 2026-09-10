#!/usr/bin/env python3
"""
Apply Pico-only source-compatibility fixes to the generated FastDoom overlay.

The canonical scripts/mc_fastdoom_prepare.py remains shared with the validated
Raylib build. This second pass changes only constructs that modern GCC rejects
when compiling the original DOS-oriented C for Cortex-M33.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


def patch_am_map(fd: Path) -> None:
    path = fd / "am_map.c"
    text = path.read_text(encoding="utf-8")

    replacements = (
        ("register outcode1 = 0;", "register int outcode1 = 0;"),
        ("register outcode2 = 0;", "register int outcode2 = 0;"),
        ("register outside;", "register int outside;"),
    )

    changed = 0
    for old, new in replacements:
        count = text.count(old)
        if count != 1:
            raise RuntimeError(
                f"am_map.c: expected exactly one {old!r}; found {count}"
            )
        text = text.replace(old, new, 1)
        changed += 1

    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Pico portability: am_map.c explicit-int declarations={changed}")


def patch_options_name(fd: Path) -> None:
    path = fd / "options.h"
    text = path.read_text(encoding="utf-8")

    old = (
        '#if defined(MC_FASTDOOM_RAYLIB)\n'
        '#define FD_MODE_NAME "MicroConsole Raylib 320x200 256 colors"\n'
        '#elif defined(MODE_Y)\n'
    )
    new = (
        '#if defined(MC_FASTDOOM_PICO)\n'
        '#define FD_MODE_NAME "MicroConsole Pico 320x200 256 colors"\n'
        '#elif defined(MC_FASTDOOM_RAYLIB)\n'
        '#define FD_MODE_NAME "MicroConsole Raylib 320x200 256 colors"\n'
        '#elif defined(MODE_Y)\n'
    )

    count = text.count(old)
    if count != 1:
        raise RuntimeError(
            "options.h: expected exactly one generated Raylib FD_MODE_NAME "
            f"block; found {count}"
        )

    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print("Pico portability: options.h mode name=MicroConsole Pico")



def patch_d_main(fd: Path) -> None:
    path = fd / "d_main.c"
    text = path.read_text(encoding="utf-8")

    replacements = (
        (
            "eventhead = (++eventhead) & (MAXEVENTS - 1);",
            "eventhead = (eventhead + 1) & (MAXEVENTS - 1);",
        ),
        (
            "for (; eventtail != eventhead; eventtail = (++eventtail) & (MAXEVENTS - 1))",
            "for (; eventtail != eventhead; eventtail = (eventtail + 1) & (MAXEVENTS - 1))",
        ),
    )

    for old, new in replacements:
        count = text.count(old)
        if count != 1:
            raise RuntimeError(
                f"d_main.c: expected exactly one {old!r}; found {count}"
            )
        text = text.replace(old, new, 1)

    path.write_text(text, encoding="utf-8", newline="\n")
    print("Pico portability: d_main.c event queue sequencing=2")


def patch_g_game(fd: Path) -> None:
    path = fd / "g_game.c"
    text = path.read_text(encoding="utf-8")

    old = "memset(mousebuttons, 0, sizeof(mousebuttons));"
    new = "memset(mousebuttons, 0, NUMMOUSEBUTTONS * sizeof(*mousebuttons));"

    count = text.count(old)
    if count != 1:
        raise RuntimeError(
            "g_game.c: expected exactly one mousebuttons memset; "
            f"found {count}"
        )

    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print("Pico portability: g_game.c mouse button clear=NUMMOUSEBUTTONS")


def patch_descriptor_io(fd: Path) -> None:
    read_count = 0
    write_count = 0

    for path in sorted(fd.glob("*.c")):
        text = path.read_text(encoding="utf-8")
        new_text = text

        rc = new_text.count("_read(")
        wc = new_text.count("_write(")

        if rc:
            new_text = new_text.replace("_read(", "mc_fd_read(")
            read_count += rc
        if wc:
            new_text = new_text.replace("_write(", "mc_fd_write(")
            write_count += wc

        if new_text != text:
            path.write_text(new_text, encoding="utf-8", newline="\n")

    if read_count == 0:
        raise RuntimeError(
            "Pico descriptor I/O: no generated _read() calls were found"
        )

    print(
        "Pico portability: descriptor I/O "
        f"read={read_count} write={write_count}"
    )


def patch_hu_stuff(fd: Path) -> None:
    path = fd / "hu_stuff.c"
    text = path.read_text(encoding="utf-8")

    old = "char secrettext[5];"
    new = "char secrettext[16];"

    count = text.count(old)
    if count != 1:
        raise RuntimeError(
            "hu_stuff.c: expected exactly one secrettext[5] declaration; "
            f"found {count}"
        )

    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print("Pico portability: hu_stuff.c secret text buffer=16")


def replace_c_identifier(text: str, old: str, new: str) -> tuple[str, int]:
    """Replace one C identifier outside strings, chars, and comments."""
    out = []
    i = 0
    n = len(text)
    count = 0
    state = "code"

    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if state == "code":
            if ch == '"':
                out.append(ch)
                state = "string"
                i += 1
                continue
            if ch == "'":
                out.append(ch)
                state = "char"
                i += 1
                continue
            if ch == "/" and nxt == "/":
                out.extend((ch, nxt))
                state = "line_comment"
                i += 2
                continue
            if ch == "/" and nxt == "*":
                out.extend((ch, nxt))
                state = "block_comment"
                i += 2
                continue

            if ch == "_" or ch.isalpha():
                j = i + 1
                while j < n and (text[j] == "_" or text[j].isalnum()):
                    j += 1
                ident = text[i:j]
                if ident == old:
                    out.append(new)
                    count += 1
                else:
                    out.append(ident)
                i = j
                continue

            out.append(ch)
            i += 1
            continue

        if state == "string":
            out.append(ch)
            i += 1
            if ch == "\\" and i < n:
                out.append(text[i])
                i += 1
            elif ch == '"':
                state = "code"
            continue

        if state == "char":
            out.append(ch)
            i += 1
            if ch == "\\" and i < n:
                out.append(text[i])
                i += 1
            elif ch == "'":
                state = "code"
            continue

        if state == "line_comment":
            out.append(ch)
            i += 1
            if ch == "\n":
                state = "code"
            continue

        if state == "block_comment":
            out.append(ch)
            i += 1
            if ch == "*" and i < n and text[i] == "/":
                out.append("/")
                i += 1
                state = "code"
            continue

    return "".join(out), count



def patch_timer_globals(fd: Path) -> None:
    path = fd / "i_ibm.h"
    text = path.read_text(encoding="utf-8")

    replacements = (
        (
            "extern unsigned int ticcount_hr;",
            "extern volatile unsigned int ticcount_hr;",
        ),
        (
            "extern unsigned int ticcount;",
            "extern volatile unsigned int ticcount;",
        ),
    )

    changed = 0
    for old, new in replacements:
        count = text.count(old)
        if count != 1:
            raise RuntimeError(
                f"i_ibm.h: expected exactly one {old!r}; found {count}"
            )
        text = text.replace(old, new, 1)
        changed += 1

    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Pico portability: i_ibm.h volatile timer counters={changed}")



def patch_wi_stuff(fd: Path) -> None:
    path = fd / "wi_stuff.c"
    text = path.read_text(encoding="utf-8")

    if "static patch_t *time;" not in text:
        raise RuntimeError(
            "wi_stuff.c: expected the original static patch_t *time declaration"
        )

    new_text, count = replace_c_identifier(text, "time", "wi_time_patch")

    if count < 2:
        raise RuntimeError(
            "wi_stuff.c: expected multiple code identifiers named time; "
            f"found {count}"
        )

    path.write_text(new_text, encoding="utf-8", newline="\n")
    print(
        "Pico portability: wi_stuff.c time sprite rename="
        f"{count} identifier(s)"
    )

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--src",
        required=True,
        type=Path,
        help="Generated FASTDOOM directory, e.g. build-fastdoom-src/FASTDOOM",
    )
    args = ap.parse_args()

    fd = args.src.resolve()

    if not (fd / "doomdef.h").is_file():
        print(f"ERROR: generated FastDoom source not found: {fd}", file=sys.stderr)
        return 1

    try:
        patch_am_map(fd)
        patch_options_name(fd)
        patch_d_main(fd)
        patch_g_game(fd)
        patch_descriptor_io(fd)
        patch_hu_stuff(fd)
        patch_timer_globals(fd)
        patch_wi_stuff(fd)
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    print(f"Pico portability prepared {fd}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
