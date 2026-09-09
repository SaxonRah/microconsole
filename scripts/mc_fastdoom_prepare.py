#!/usr/bin/env python3
"""
Prepare a generated, portable source copy for the MicroConsole FastDoom
Raylib target.

The pinned third_party/fastdoom checkout is never modified.  We copy
FASTDOOM/ into build-fastdoom-src/FASTDOOM and perform a small, deterministic
set of portability transforms there.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import shutil
import sys

PINNED_SHA = "5580b5e61fc0ad3ba3a0338c264e7e4a0427c3df"

OLD_IWAD_ALLOC = "iwadfile = Z_Malloc(strlen(src) + 1, PU_STATIC, NULL);"
NEW_IWAD_ALLOC = "iwadfile = Z_MallocUnowned(strlen(src) + 1, PU_STATIC);"

KEX_DOOM_SIZE = 12996515
KEX_DOOM2_SIZE = 14951361

IO_CALLS = ("open", "close", "read", "write", "lseek", "tell", "filelength", "access")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _update_block_comment_state(line: str, in_block_comment: bool) -> bool:
    """Track C block comments well enough to avoid rewriting commented pragmas."""
    i = 0
    n = len(line)

    while i < n:
        if in_block_comment:
            end = line.find("*/", i)
            if end < 0:
                return True
            in_block_comment = False
            i = end + 2
            continue

        line_comment = line.find("//", i)
        block_start = line.find("/*", i)

        if line_comment >= 0 and (block_start < 0 or line_comment < block_start):
            return False
        if block_start < 0:
            return False

        in_block_comment = True
        i = block_start + 2

    return in_block_comment


def strip_watcom_pragma_aux(text: str) -> str:
    """
    Remove active Watcom #pragma aux bodies while retaining C prototypes.

    FastDoom's std_func.h also contains an old #pragma aux example inside a
    /* ... */ comment.  That text must be preserved; otherwise stripping the
    pragma continuation also strips the closing */ and leaves the generated
    header with an unterminated comment.
    """
    out: list[str] = []
    skipping = False
    in_block_comment = False

    for line in text.splitlines(keepends=True):
        stripped = line.lstrip()

        if skipping:
            skipping = line.rstrip().endswith("\\")
            continue

        if not in_block_comment and stripped.startswith("#pragma aux"):
            skipping = line.rstrip().endswith("\\")
            continue

        out.append(line)
        in_block_comment = _update_block_comment_state(line, in_block_comment)

    return "".join(out)


def rewrite_io_calls(text: str) -> str:
    # Replace actual call tokens only.  Enum constants called open/close are
    # deliberately untouched.
    for name in IO_CALLS:
        text = re.sub(rf"(?<![A-Za-z0-9_]){name}(\s*)\(", rf"_{name}\1(", text)
    return text


def patch_file(path: Path) -> list[str]:
    original = path.read_text(encoding="utf-8")
    text = original
    changes: list[str] = []

    stripped = strip_watcom_pragma_aux(text)
    if stripped != text:
        text = stripped
        changes.append("strip Watcom #pragma aux")

    if path.suffix.lower() == ".c":
        rewritten = rewrite_io_calls(text)
        if rewritten != text:
            text = rewritten
            changes.append("rewrite POSIX-ish CRT calls to MSVC underscore names")

    if path.name in ("r_main.c", "p_setup.c"):
        rewritten = text.replace("#include <math.h>\n", "")
        if rewritten != text:
            text = rewritten
            changes.append("remove system <math.h> include shadowed by FastDoom math.h")

    if path.name == "r_segs.c":
        text, rsegs_changes = patch_r_segs_null_backsector(text)
        changes.extend(rsegs_changes)

    if path.name == "fastmath.h":
        rewritten = text.replace(
            "inline fixed_t FixedInterpolate(fixed_t a, fixed_t b, fixed_t frac)",
            "static inline fixed_t FixedInterpolate(fixed_t a, fixed_t b, fixed_t frac)",
        )
        if rewritten != text:
            text = rewritten
            changes.append("make FixedInterpolate header-local")

    if path.name == "options.h" and "MC_FASTDOOM_RAYLIB" not in text:
        needle = '#if defined(MODE_Y)\n#define FD_MODE_NAME "VGA 320x200 256 colors"'
        replacement = (
            '#if defined(MC_FASTDOOM_RAYLIB)\n'
            '#define FD_MODE_NAME "MicroConsole Raylib 320x200 256 colors"\n'
            '#elif defined(MODE_Y)\n'
            '#define FD_MODE_NAME "VGA 320x200 256 colors"'
        )
        if needle not in text:
            raise RuntimeError("options.h no longer matches expected FD_MODE_NAME block")
        text = text.replace(needle, replacement, 1)
        changes.append("name MicroConsole Raylib video mode")

    if text != original:
        path.write_text(text, encoding="utf-8", newline="\n")

    return changes


def patch_kex_iwads(text: str) -> tuple[str, list[str]]:
    """Recognize KEX Doom/Doom II IWADs and skip incompatible attract demos."""
    changes: list[str] = []

    const_needle = "#define TNTWADSIZE2 18654796\n"
    if const_needle not in text:
        raise RuntimeError("d_main.c no longer matches IWAD size constants")
    const_repl = (
        const_needle
        + f"#define DOOMKEXWADSIZE {KEX_DOOM_SIZE}\n"
        + f"#define DOOM2KEXWADSIZE {KEX_DOOM2_SIZE}\n"
    )
    text = text.replace(const_needle, const_repl, 1)
    changes.append("add KEX Doom/Doom II IWAD sizes")

    load_needle = """void LoadExternalIWAD(void)
{
    int i;

    if (access(iwadfile, R_OK))
        I_Error(1);

    for (i = 0; i < NUMIWADS; i++)
"""
    load_repl = """void LoadExternalIWAD(void)
{
    int i;
    long iwadsize = -1;
    FILE *iwadfp;

    if (access(iwadfile, R_OK))
        I_Error(1);

    /*
     * The current DOOM + DOOM II KEX rerelease ships IWADs whose
     * base game data is usable here but whose file sizes and attract
     * demos differ from the DOS-era IWADs FastDoom knows about.
     */
    iwadfp = fopen(iwadfile, "rb");
    if (iwadfp != NULL)
    {
        if (fseek(iwadfp, 0, SEEK_END) == 0)
            iwadsize = ftell(iwadfp);
        fclose(iwadfp);
    }

    if (!strcasecmp(iwadname, "doom.wad") && iwadsize == DOOMKEXWADSIZE)
    {
        complevel = COMPLEVEL_ULTIMATE_DOOM;
        gamemode = retail;
        gamemission = doom;
        strcpy(savegamename, SAVEGAMENAME_DOOMU);
        disableDemo = true;
        D_AddFile(iwadfile);
        printf("Detected DOOM KEX IWAD (%ld bytes); using Ultimate Doom mode and disabling incompatible attract demos.\\n", iwadsize);
        return;
    }

    if (!strcasecmp(iwadname, "doom2.wad") && iwadsize == DOOM2KEXWADSIZE)
    {
        complevel = COMPLEVEL_DOOM;
        gamemode = commercial;
        gamemission = doom2;
        strcpy(savegamename, SAVEGAMENAME_DOOM2);
        disableDemo = true;
        D_AddFile(iwadfile);
        printf("Detected DOOM II KEX IWAD (%ld bytes); disabling incompatible attract demos.\\n", iwadsize);
        return;
    }

    for (i = 0; i < NUMIWADS; i++)
"""
    if load_needle not in text:
        raise RuntimeError("d_main.c no longer matches LoadExternalIWAD prologue")
    text = text.replace(load_needle, load_repl, 1)
    changes.append("recognize KEX Doom/Doom II IWADs")

    demo_cases = ((1, "demo1"), (3, "demo2"), (5, "demo3"), (6, "demo4"))
    for case_num, demo in demo_cases:
        old = f"""    case {case_num}:
        G_DeferedPlayDemo("{demo}");
        break;
"""
        new = f"""    case {case_num}:
        if (disableDemo)
        {{
            D_AdvanceDemo();
            break;
        }}
        G_DeferedPlayDemo("{demo}");
        break;
"""
        if old not in text:
            raise RuntimeError(f"d_main.c no longer matches attract demo case {case_num}")
        text = text.replace(old, new, 1)
    changes.append("skip attract demos when disableDemo is set")

    return text, changes


def patch_raylib_bringup_defaults(text: str) -> tuple[str, list[str]]:
    """Use conservative desktop defaults while the level path is validated."""
    changes: list[str] = []

    no_melt_needle = "boolean noMelt;"
    if no_melt_needle not in text:
        raise RuntimeError("d_main.c no longer matches noMelt declaration")
    text = text.replace(
        no_melt_needle,
        "boolean noMelt = true; /* MicroConsole: -melt re-enables it. */",
        1,
    )
    changes.append("disable melt by default for MicroConsole bring-up")

    demo_needle = '    disableDemo = M_CheckParm("-disabledemo");\n'
    demo_repl = (
        '    /* MicroConsole: keep the title/menu stable during bring-up. */\n'
        '#if defined(MC_FASTDOOM_RAYLIB)\n'
        '    disableDemo = !M_CheckParm("-enabledemo");\n'
        '    if (M_CheckParm("-disabledemo"))\n'
        '        disableDemo = true;\n'
        '#else\n'
        '    disableDemo = M_CheckParm("-disabledemo");\n'
        '#endif\n'
    )
    if demo_needle not in text:
        raise RuntimeError("d_main.c no longer matches disableDemo assignment")
    text = text.replace(demo_needle, demo_repl, 1)
    changes.append("disable attract demos by default; add -enabledemo override")

    return text, changes


def patch_raylib_deferred_autostart(text: str) -> tuple[str, list[str]]:
    """Route Raylib command-line autostart through G_DoNewGame().

    The initial Raylib bring-up sets singletics=true before graphics are ready.
    Menu-started games call G_DeferedInitNew(), then G_DoNewGame() on the first
    game tick, which resets singletics=false.  FastDoom's command-line autostart
    normally calls G_InitNew() immediately, bypassing that reset.  The result is
    one game tic per rendered frame on -warp/-skill/-episode, which feels like
    large input latency with the portable C renderer and also makes menu input
    pathological.

    Keep demo recording on the original immediate path because G_BeginRecording()
    runs at D_DoomLoop entry and needs the game parameters initialized already.
    """
    changes: list[str] = []

    old = """    if (gameaction != ga_loadgame)
    {
        if (autostart)
            G_InitNew(startskill, startepisode, startmap);
        else
            D_StartTitle(); // start up intro loop
    }

    D_DoomLoop(); // never returns
"""

    new = """    if (gameaction != ga_loadgame)
    {
        if (autostart)
        {
#if defined(MC_FASTDOOM_RAYLIB)
            /* Match the normal menu-start path.  G_DoNewGame() resets the
               temporary Raylib bring-up singletics mode before gameplay. */
            if (!demorecording)
                G_DeferedInitNew(startskill, startepisode, startmap);
            else
                G_InitNew(startskill, startepisode, startmap);
#else
            G_InitNew(startskill, startepisode, startmap);
#endif
        }
        else
            D_StartTitle(); // start up intro loop
    }

    D_DoomLoop(); // never returns
"""

    matches = text.count(old)
    if matches != 1:
        raise RuntimeError(
            "d_main.c no longer matches final autostart block while applying "
            f"Raylib deferred-start patch (matches={matches})"
        )

    text = text.replace(old, new, 1)
    changes.append("defer Raylib command-line autostart through G_DoNewGame")
    return text, changes


def patch_r_segs_null_backsector(text: str) -> tuple[str, list[str]]:
    """Avoid dereferencing NULL backsector on one-sided walls.

    FastDoom's interpolation-era R_StoreWallRange() computes cached back-sector
    heights before its existing `if (!backsector)` split.  That is tolerated by
    its DOS environment but faults immediately on Win32 where page zero is
    unmapped.  Preserve the two-sided behavior and initialize the unused cached
    values to zero for one-sided walls.
    """
    changes: list[str] = []

    old = """	if (highResTimer) {
		frontsector_floorheight = FixedInterpolate(frontsector->prevfloorheight, frontsector->floorheight, interpolation_weight);
		frontsector_ceilingheight = FixedInterpolate(frontsector->prevceilingheight, frontsector->ceilingheight, interpolation_weight);
		backsector_floorheight = FixedInterpolate(backsector->prevfloorheight, backsector->floorheight, interpolation_weight);
		backsector_ceilingheight = FixedInterpolate(backsector->prevceilingheight, backsector->ceilingheight, interpolation_weight);
	} else {
		frontsector_floorheight = frontsector->floorheight;
		frontsector_ceilingheight = frontsector->ceilingheight;
		backsector_floorheight = backsector->floorheight;
		backsector_ceilingheight = backsector->ceilingheight;
	}
"""

    new = """	if (highResTimer) {
		frontsector_floorheight = FixedInterpolate(frontsector->prevfloorheight, frontsector->floorheight, interpolation_weight);
		frontsector_ceilingheight = FixedInterpolate(frontsector->prevceilingheight, frontsector->ceilingheight, interpolation_weight);
	} else {
		frontsector_floorheight = frontsector->floorheight;
		frontsector_ceilingheight = frontsector->ceilingheight;
	}

	/*
	 * One-sided segs intentionally have backsector == NULL.  FastDoom's
	 * DOS build can get away with reading through address zero, but Win32
	 * maps the null page inaccessible.  These cached values are only used
	 * by the two-sided branch below, so do not touch backsector until it is
	 * known to exist.
	 */
	if (backsector) {
		if (highResTimer) {
			backsector_floorheight = FixedInterpolate(backsector->prevfloorheight, backsector->floorheight, interpolation_weight);
			backsector_ceilingheight = FixedInterpolate(backsector->prevceilingheight, backsector->ceilingheight, interpolation_weight);
		} else {
			backsector_floorheight = backsector->floorheight;
			backsector_ceilingheight = backsector->ceilingheight;
		}
	} else {
		backsector_floorheight = 0;
		backsector_ceilingheight = 0;
	}
"""

    matches = text.count(old)
    if matches != 1:
        raise RuntimeError(
            "r_segs.c no longer matches R_StoreWallRange sector-height prologue "
            f"(matches={matches})"
        )

    text = text.replace(old, new, 1)
    changes.append("guard one-sided R_StoreWallRange backsector before dereference")
    return text, changes


def patch_static_title_when_demos_disabled(text: str) -> tuple[str, list[str]]:
    """Keep TITLEPIC stable while attract demos are disabled."""
    changes: list[str] = []
    old = """void D_PageTicker(void)
{
    if (--pagetic < 0)
        D_AdvanceDemo();
}
"""
    new = """void D_PageTicker(void)
{
    /* During Raylib bring-up, disabling attract demos should leave the title
       page stable rather than cycling rapidly through the remaining pages. */
    if (disableDemo)
        return;

    if (--pagetic < 0)
        D_AdvanceDemo();
}
"""
    matches = text.count(old)
    if matches != 1:
        raise RuntimeError(
            "d_main.c no longer matches D_PageTicker while applying stable-title patch "
            f"(matches={matches})"
        )
    text = text.replace(old, new, 1)
    changes.append("keep TITLEPIC stable while attract demos are disabled")
    return text, changes

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fastdoom", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    args = ap.parse_args()

    fastdoom = args.fastdoom.resolve()
    out = args.out.resolve()
    src_tree = fastdoom / "FASTDOOM"
    original_d_main = src_tree / "d_main.c"

    if not original_d_main.is_file():
        print(f"ERROR: FastDoom source not found: {original_d_main}", file=sys.stderr)
        return 1

    original_text = original_d_main.read_text(encoding="utf-8")
    occurrences = original_text.count(OLD_IWAD_ALLOC)
    if occurrences != 1:
        print(
            "ERROR: pinned FastDoom d_main.c no longer matches the expected "
            f"-iwad allocation site (matches={occurrences}).",
            file=sys.stderr,
        )
        print("Refusing to guess at a source transformation.", file=sys.stderr)
        return 1

    if out.exists():
        shutil.rmtree(out)

    dst_tree = out / "FASTDOOM"
    shutil.copytree(src_tree, dst_tree)

    # Apply semantic d_main.c transforms before the generic CRT-call rewrite.
    # v8 did this in the opposite order, so access()/fopen() had already
    # become _access()/_fopen() and the KEX matcher rejected the source.
    d_main = dst_tree / "d_main.c"
    text = d_main.read_text(encoding="utf-8")
    if OLD_IWAD_ALLOC not in text:
        print("ERROR: generated d_main.c lost expected -iwad allocation site", file=sys.stderr)
        return 1
    text = text.replace(OLD_IWAD_ALLOC, NEW_IWAD_ALLOC, 1)
    try:
        text, kex_changes = patch_kex_iwads(text)
        text, bringup_changes = patch_raylib_bringup_defaults(text)
        text, title_changes = patch_static_title_when_demos_disabled(text)
        text, autostart_changes = patch_raylib_deferred_autostart(text)
    except RuntimeError as exc:
        print(f"ERROR: d_main.c: {exc}", file=sys.stderr)
        return 1
    d_main.write_text(text, encoding="utf-8", newline="\n")

    changed: list[tuple[str, list[str]]] = [(
        "d_main.c",
        ["use Z_MallocUnowned for explicit -iwad path"]
        + kex_changes
        + bringup_changes
        + title_changes
        + autostart_changes,
    )]

    for path in sorted(dst_tree.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in (".c", ".h"):
            continue
        try:
            changes = patch_file(path)
        except RuntimeError as exc:
            print(f"ERROR: {path.name}: {exc}", file=sys.stderr)
            return 1
        if changes:
            changed.append((str(path.relative_to(dst_tree)), changes))

    marker = out / "README.generated.txt"
    marker_lines = [
        "Generated by scripts/mc_fastdoom_prepare.py",
        f"FastDoom pin: {PINNED_SHA}",
        f"Original d_main.c SHA256: {sha256(original_d_main)}",
        "",
        "Portability transforms:",
    ]
    for rel, changes in changed:
        marker_lines.append(f"- {rel}: " + "; ".join(changes))
    marker.write_text("\n".join(marker_lines) + "\n", encoding="utf-8", newline="\n")

    print(f"prepared {dst_tree}")
    print(f"portable source files changed: {len(changed)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
