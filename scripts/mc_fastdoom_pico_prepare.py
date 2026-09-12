#!/usr/bin/env python3
"""
Prepare the generated FastDoom source overlay for the MicroConsole Pico target.

This is the single canonical Pico source-transform pass.  It contains:
  * Cortex-M33/GCC portability fixes used by the base Pico port;
  * E5/E6 gameplay compatibility needed by SIGIL and SIGIL II;
  * known-PWAD frontend namespace isolation;
  * a multi-range flat namespace for PWADs with additional F_START/F_END pairs;
  * visible campaign/episode menus for SIGIL, SIGIL II, NERVE and Master Levels.

It intentionally does not add custom SIGIL music tables.  The Pico target uses
mc_fastdoom_sound_mw.c instead of FastDoom's s_sound.c, and the current product
requirement is to keep the existing safe episode-4-style music fallback.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


def replace_exact(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(
            f"{label}: expected exactly one source match; found {count}"
        )
    return text.replace(old, new, 1)


def find_function(text: str, signature_regex: str):
    """Return (start, open_brace, close_brace) for one C function definition."""
    match = re.search(signature_regex, text, flags=re.MULTILINE)
    if not match:
        raise RuntimeError(f"function definition not found: {signature_regex}")

    brace = text.find("{", match.start(), match.end())
    if brace < 0:
        raise RuntimeError(f"function opening brace not found: {signature_regex}")

    depth = 0
    state = "code"
    i = brace

    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if state == "code":
            if ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            elif ch == "/" and nxt == "/":
                state = "line_comment"
                i += 1
            elif ch == "/" and nxt == "*":
                state = "block_comment"
                i += 1
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return match.start(), brace, i

        elif state == "string":
            if ch == "\\":
                i += 1
            elif ch == '"':
                state = "code"

        elif state == "char":
            if ch == "\\":
                i += 1
            elif ch == "'":
                state = "code"

        elif state == "line_comment":
            if ch == "\n":
                state = "code"

        elif state == "block_comment":
            if ch == "*" and nxt == "/":
                state = "code"
                i += 1

        i += 1

    raise RuntimeError(f"unterminated function: {signature_regex}")


def replace_function(text: str, signature_regex: str, replacement: str) -> str:
    start, _, close = find_function(text, signature_regex)
    end = close + 1
    while end < len(text) and text[end] in "\r\n":
        end += 1
    return text[:start] + replacement.rstrip() + "\n\n" + text[end:]


# ---------------------------------------------------------------------------
# Base Pico portability pass (kept equivalent to main).
# ---------------------------------------------------------------------------

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

    text = replace_exact(text, old, new, "options.h Pico mode name")
    path.write_text(text, encoding="utf-8", newline="\n")
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
        text = replace_exact(text, old, new, "d_main.c event queue sequencing")

    path.write_text(text, encoding="utf-8", newline="\n")
    print("Pico portability: d_main.c event queue sequencing=2")


def patch_g_game_portability(fd: Path) -> None:
    path = fd / "g_game.c"
    text = path.read_text(encoding="utf-8")

    text = replace_exact(
        text,
        "memset(mousebuttons, 0, sizeof(mousebuttons));",
        "memset(mousebuttons, 0, NUMMOUSEBUTTONS * sizeof(*mousebuttons));",
        "g_game.c mouse button clear",
    )

    path.write_text(text, encoding="utf-8", newline="\n")
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
    text = replace_exact(
        text,
        "char secrettext[5];",
        "char secrettext[16];",
        "hu_stuff.c secret text buffer",
    )
    path.write_text(text, encoding="utf-8", newline="\n")
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

    for old, new in (
        (
            "extern unsigned int ticcount_hr;",
            "extern volatile unsigned int ticcount_hr;",
        ),
        (
            "extern unsigned int ticcount;",
            "extern volatile unsigned int ticcount;",
        ),
    ):
        text = replace_exact(text, old, new, "i_ibm.h volatile timer counter")

    path.write_text(text, encoding="utf-8", newline="\n")
    print("Pico portability: i_ibm.h volatile timer counters=2")


def patch_wi_identifier(fd: Path) -> None:
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


# ---------------------------------------------------------------------------
# E5/E6 gameplay compatibility.  Music stays in mc_fastdoom_sound_mw.c.
# ---------------------------------------------------------------------------

def patch_episode56_g_game(fd: Path) -> None:
    path = fd / "g_game.c"
    text = path.read_text(encoding="utf-8")

    old_clamp = """    if (gamemode == retail)
    {
        if (episode > 4)
            episode = 4;
    }
"""
    new_clamp = """    if (gamemode == retail)
    {
#if defined(MC_FASTDOOM_PICO)
        /* SIGIL and SIGIL II use conventional Doom-format E5Mx/E6Mx maps. */
        if (episode > 6)
            episode = 6;
#else
        if (episode > 4)
            episode = 4;
#endif
    }
"""
    text = replace_exact(text, old_clamp, new_clamp, "g_game.c retail episode clamp")

    old_sky = """        case 4: // Special Edition sky
            skytexture = R_TextureNumForName("SKY4");
            break;
        }
"""
    new_sky = """        case 4: // Special Edition sky
            skytexture = R_TextureNumForName("SKY4");
            break;
#if defined(MC_FASTDOOM_PICO)
        case 5:
            skytexture = R_CheckTextureNumForName("SKY5");
            if (skytexture < 0)
                skytexture = R_TextureNumForName("SKY4");
            break;
        case 6:
            skytexture = R_CheckTextureNumForName("SKY6");
            if (skytexture < 0)
                skytexture = R_TextureNumForName("SKY4");
            break;
#endif
        }
"""
    text = replace_exact(text, old_sky, new_sky, "g_game.c E5/E6 sky")

    old_secret_return = """            case 4:
                wminfo.next = 2;
                break;
            }
"""
    new_secret_return = """            case 4:
                wminfo.next = 2;
                break;
#if defined(MC_FASTDOOM_PICO)
            /* SIGIL: E5M9 -> E5M7; SIGIL II: E6M9 -> E6M4. */
            case 5:
                wminfo.next = 6;
                break;
            case 6:
                wminfo.next = 3;
                break;
#endif
            }
"""
    text = replace_exact(
        text,
        old_secret_return,
        new_secret_return,
        "g_game.c E5/E6 secret return",
    )

    old_par = """    else
        wminfo.partime = Mul175(pars[gameepisode - 1][gamemap - 1]);
"""
    new_par = """    else
    {
#if defined(MC_FASTDOOM_PICO)
        if (gameepisode <= 4)
            wminfo.partime = Mul175(pars[gameepisode - 1][gamemap - 1]);
        else
            wminfo.partime = 0;
#else
        wminfo.partime = Mul175(pars[gameepisode - 1][gamemap - 1]);
#endif
    }
"""
    text = replace_exact(text, old_par, new_par, "g_game.c E5/E6 par time")

    path.write_text(text, encoding="utf-8", newline="\n")
    print("Pico PWAD: g_game.c E5/E6 gameplay compatibility")


def patch_episode56_wi(fd: Path) -> None:
    path = fd / "wi_stuff.c"
    text = path.read_text(encoding="utf-8")

    old_bg = (
        '\tif (gamemode == retail)\n'
        '\t{\n'
        '\t\tif (wbs->epsd == 3)\n'
        '\t\t{\n'
        '\t\t\tstrcpy(name, "INTERPIC");\n'
        '\t\t}\n'
        '\t}\n'
    )
    new_bg = """    if (gamemode == retail)
    {
#if defined(MC_FASTDOOM_PICO)
        if (wbs->epsd >= 3)
#else
        if (wbs->epsd == 3)
#endif
        {
            strcpy(name, "INTERPIC");
        }
    }
"""
    text = replace_exact(text, old_bg, new_bg, "wi_stuff.c E5/E6 background")

    old_lname = (
        '\t\tfor (i = 0; i < NUMMAPS; i++)\n'
        '\t\t{\n'
        '\t\t\tsprintf(name, "WILV%d%d", wbs->epsd, i);\n'
        '\t\t\tlnames[i] = W_CacheLumpName(name, PU_STATIC);\n'
        '\t\t}\n'
    )
    new_lname = """        for (i = 0; i < NUMMAPS; i++)
        {
            sprintf(name, "WILV%d%d", wbs->epsd, i);
#if defined(MC_FASTDOOM_PICO)
            if (wbs->epsd > 3 && W_GetNumForName(name) < 0)
                sprintf(name, "WILV3%d", i);
#endif
            lnames[i] = W_CacheLumpName(name, PU_STATIC);
        }
"""
    text = replace_exact(text, old_lname, new_lname, "wi_stuff.c E5/E6 level names")

    path.write_text(text, encoding="utf-8", newline="\n")
    print("Pico PWAD: wi_stuff.c E5/E6 intermission fallback")


def patch_episode56_finale(fd: Path) -> None:
    path = fd / "f_finale.c"
    text = path.read_text(encoding="utf-8")

    old = """\t\tcase 4:
\t\t\tfinaleflat = "MFLR8_3";
\t\t\tI_LoadTextProgram(50);
\t\t\tmoveText = 1;
\t\t\tbreak;
\t\t}
"""
    new = """\t\tcase 4:
\t\t\tfinaleflat = "MFLR8_3";
\t\t\tI_LoadTextProgram(50);
\t\t\tmoveText = 1;
\t\t\tbreak;
#if defined(MC_FASTDOOM_PICO)
\t\tcase 5:
\t\tcase 6:
\t\t\t/* FastDoom has no built-in E5/E6 text database; use safe E4 art/text. */
\t\t\tfinaleflat = "MFLR8_3";
\t\t\tI_LoadTextProgram(50);
\t\t\tmoveText = 1;
\t\t\tbreak;
#endif
\t\t}
"""
    text = replace_exact(text, old, new, "f_finale.c E5/E6 safe finale")

    path.write_text(text, encoding="utf-8", newline="\n")
    print("Pico PWAD: f_finale.c E5/E6 safe finale fallback")

# ---------------------------------------------------------------------------
# Known-PWAD frontend namespace isolation.
# ---------------------------------------------------------------------------

PWAD_NAMESPACE_HELPER = r'''
#if defined(MC_FASTDOOM_PICO)
/* MC_FASTDOOM_PICO_PWAD_CAMPAIGN_PROFILES */
#define MC_FD_CAMPAIGN_NONE         0
#define MC_FD_CAMPAIGN_SIGIL        1
#define MC_FD_CAMPAIGN_SIGIL2       2
#define MC_FD_CAMPAIGN_NERVE        3
#define MC_FD_CAMPAIGN_MASTERLEVELS 4

int mc_fd_pwad_campaign = MC_FD_CAMPAIGN_NONE;

typedef struct mc_fd_lump_rename_s
{
    const char *old_name;
    const char *new_name;
} mc_fd_lump_rename_t;

typedef struct mc_fd_namespace_profile_s
{
    const char *filename;
    int campaign;
    const mc_fd_lump_rename_t *renames;
    unsigned int rename_count;
} mc_fd_namespace_profile_t;

static int MC_FD_FileBasenameIs(const char *filename, const char *wanted)
{
    const char *base;
    const char *p;

    if (!filename || !wanted)
        return 0;

    base = filename;
    for (p = filename; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }

    return !strcasecmp(base, wanted);
}

static void MC_FD_SetLumpName8(lumpinfo_t *lump, const char *name)
{
    unsigned int i;

    for (i = 0u; i < 8u; ++i)
        lump->name[i] = '\0';

    for (i = 0u; i < 8u && name[i]; ++i)
        lump->name[i] = name[i];
}

static unsigned int MC_FD_RenameLumpRange(int startlump,
                                          int endlump,
                                          const char *old_name,
                                          const char *new_name)
{
    int i;
    unsigned int changed = 0u;

    for (i = startlump; i < endlump; ++i)
    {
        if (!strncasecmp(lumpinfo[i].name, old_name, 8))
        {
            MC_FD_SetLumpName8(&lumpinfo[i], new_name);
            ++changed;
        }
    }

    return changed;
}

static const mc_fd_lump_rename_t mc_fd_sigil_renames[] = {
    {"CREDIT",   "SIGCREDI"},
    {"HELP1",    "SIGHELP1"},
    {"TITLEPIC", "SIGTITLE"},
    {"DEHACKED", "SIG_DEH"},
    {"DEMO1",    "SIGDEMO1"},
    {"DEMO2",    "SIGDEMO2"},
    {"DEMO3",    "SIGDEMO3"},
    {"DEMO4",    "SIGDEMO4"},
    {"D_INTER",  "D_SIGINT"},
    {"D_INTRO",  "D_SIGTIT"},
};

static const mc_fd_lump_rename_t mc_fd_sigil2_renames[] = {
    {"CREDIT",   "SG2CREDI"},
    {"HELP1",    "SG2HELP1"},
    {"TITLEPIC", "SG2TITLE"},
    {"SIGILEND", "SGL2END"},
    {"DEHACKED", "SG2_DEH"},
    {"DEMO1",    "SG2DEMO1"},
    {"DEMO2",    "SG2DEMO2"},
    {"DEMO3",    "SG2DEMO3"},
    {"DEMO4",    "SG2DEMO4"},
    {"D_INTER",  "D_SG2INT"},
    {"D_INTRO",  "D_SG2TIT"},
};

static const mc_fd_lump_rename_t mc_fd_nerve_renames[] = {
    {"TITLEPIC", "NERVEPIC"},
    {"INTERPIC", "NERVEINT"},
    {"M_DOOM",   "M_DOOM_N"},
    {"DEMO1",    "DEMO1N"},
    {"DEMO2",    "DEMO2N"},
    {"DEMO3",    "DEMO3N"},
    {"D_DM2TTL", "D_NERVTL"},
};

static const mc_fd_lump_rename_t mc_fd_master_renames[] = {
    {"TITLEPIC", "MASTRPIC"},
    {"INTERPIC", "MASTRINT"},
    {"M_DOOM",   "M_DOOM_M"},
    {"DEMO1",    "DEMO1M"},
    {"DEMO2",    "DEMO2M"},
    {"DEMO3",    "DEMO3M"},
    {"D_DM2TTL", "D_MASTTL"},
};

static const mc_fd_namespace_profile_t mc_fd_namespace_profiles[] = {
    {
        "sigil.wad", MC_FD_CAMPAIGN_SIGIL,
        mc_fd_sigil_renames,
        (unsigned int)(sizeof(mc_fd_sigil_renames) /
                       sizeof(mc_fd_sigil_renames[0]))
    },
    {
        "sigil2.wad", MC_FD_CAMPAIGN_SIGIL2,
        mc_fd_sigil2_renames,
        (unsigned int)(sizeof(mc_fd_sigil2_renames) /
                       sizeof(mc_fd_sigil2_renames[0]))
    },
    {
        "nerve.wad", MC_FD_CAMPAIGN_NERVE,
        mc_fd_nerve_renames,
        (unsigned int)(sizeof(mc_fd_nerve_renames) /
                       sizeof(mc_fd_nerve_renames[0]))
    },
    {
        "masterlevels.wad", MC_FD_CAMPAIGN_MASTERLEVELS,
        mc_fd_master_renames,
        (unsigned int)(sizeof(mc_fd_master_renames) /
                       sizeof(mc_fd_master_renames[0]))
    },
};

static void MC_FD_ApplyPwadNamespaceProfile(const char *filename,
                                            int startlump,
                                            int endlump)
{
    unsigned int p;

    for (p = 0u;
         p < (unsigned int)(sizeof(mc_fd_namespace_profiles) /
                            sizeof(mc_fd_namespace_profiles[0]));
         ++p)
    {
        const mc_fd_namespace_profile_t *profile =
            &mc_fd_namespace_profiles[p];
        unsigned int i;
        unsigned int changed = 0u;

        if (!MC_FD_FileBasenameIs(filename, profile->filename))
            continue;

        mc_fd_pwad_campaign = profile->campaign;

        for (i = 0u; i < profile->rename_count; ++i)
        {
            changed += MC_FD_RenameLumpRange(
                startlump,
                endlump,
                profile->renames[i].old_name,
                profile->renames[i].new_name);
        }

        printf("MCFDOOM1 pwad_campaign=%d file=%s renamed=%u range=%d..%d\n",
               mc_fd_pwad_campaign,
               profile->filename,
               changed,
               startlump,
               endlump - 1);
        return;
    }
}
#endif

'''


def patch_pwad_namespace(fd: Path) -> None:
    path = fd / "w_wad.c"
    text = path.read_text(encoding="utf-8")

    marker = "MC_FASTDOOM_PICO_PWAD_CAMPAIGN_PROFILES"
    if marker in text:
        raise RuntimeError("w_wad.c: PWAD namespace profile unexpectedly pre-applied")

    section = text.find("//\n// W_AddFile\n")
    if section < 0:
        raise RuntimeError("w_wad.c: W_AddFile section marker not found")

    text = text[:section] + PWAD_NAMESPACE_HELPER + text[section:]

    sig = r"void\s+W_AddFile\s*\(\s*char\s*\*filename\s*\)\s*\{"
    start, _, close = find_function(text, sig)
    end = close + 1
    body = text[start:end]

    # The shared FastDoom prepare pass normally rewrites close() to _close()
    # before this Pico-specific pass runs.  Match the reload cleanup structurally
    # instead of depending on which spelling is present.
    reload_cleanup = re.compile(
        r"(?m)^    if \(reloadname\)\n"
        r"        (?P<close>_?close\(handle\);)\n"
    )
    matches = list(reload_cleanup.finditer(body))
    if len(matches) != 1:
        raise RuntimeError(
            "w_wad.c: expected one reload cleanup inside W_AddFile; "
            f"found {len(matches)}"
        )

    match = matches[0]
    original_cleanup = match.group(0)
    replacement = (
        "#if defined(MC_FASTDOOM_PICO)\n"
        "    MC_FD_ApplyPwadNamespaceProfile(filename, startlump, numlumps);\n"
        "#endif\n\n"
        + original_cleanup
    )
    body = body[:match.start()] + replacement + body[match.end():]
    text = text[:start] + body + text[end:]

    path.write_text(text, encoding="utf-8", newline="\n")
    print("Pico PWAD: w_wad.c known campaign namespace profiles")


# ---------------------------------------------------------------------------
# Multi-range flat namespace.  Required by SIGIL II and useful for PWADs in
# general when more than one F_START/F_END namespace is present.
# ---------------------------------------------------------------------------

def patch_flat_r_state(fd: Path) -> None:
    path = fd / "r_state.h"
    text = path.read_text(encoding="utf-8")

    old = """extern int firstflat;

// for global animation
extern int *flattranslation;
"""
    new = """extern int firstflat;

/* Logical flat number -> physical WAD lump number. */
extern int *flatlumpnums;

// for global animation
extern int *flattranslation;
"""
    text = replace_exact(text, old, new, "r_state.h flat lump table declaration")
    path.write_text(text, encoding="utf-8", newline="\n")


def patch_flat_r_data(fd: Path) -> None:
    path = fd / "r_data.c"
    text = path.read_text(encoding="utf-8")

    old_globals = """int firstflat;
int lastflat;
int numflats;
"""
    new_globals = """int firstflat;
int lastflat;
int numflats;

/* MicroConsole Pico: logical flat number -> physical WAD lump number. */
int *flatlumpnums;
"""
    text = replace_exact(text, old_globals, new_globals, "r_data.c flat globals")

    old_init = """//
// R_InitFlats
//
void R_InitFlats(void)
{
    int i;

    firstflat = W_GetNumForName("F_START") + 1;
    lastflat = W_GetNumForName("F_END") - 1;
    numflats = lastflat - firstflat + 1;

    // Create translation table for global animation.
    flattranslation = Z_MallocUnowned((numflats + 1) * 4, PU_STATIC);

    for (i = 0; i < numflats; i++)
        flattranslation[i] = i;
}
"""

    new_init = r"""//
// R_InitFlats
//

/*
 * MicroConsole Pico: multi-range flat namespace.
 *
 * Base IWAD flats keep their logical slot. Later PWAD flats with the same
 * name override only the physical lump used by that slot; new names append.
 */
static boolean R_IsFlatStartMarker(char *name)
{
    return !strncasecmp(name, "F_START", 8) ||
           !strncasecmp(name, "FF_START", 8) ||
           !strncasecmp(name, "F1_START", 8) ||
           !strncasecmp(name, "F2_START", 8) ||
           !strncasecmp(name, "F3_START", 8) ||
           !strncasecmp(name, "F4_START", 8) ||
           !strncasecmp(name, "F5_START", 8) ||
           !strncasecmp(name, "F6_START", 8) ||
           !strncasecmp(name, "F7_START", 8) ||
           !strncasecmp(name, "F8_START", 8) ||
           !strncasecmp(name, "F9_START", 8);
}

static boolean R_IsFlatEndMarker(char *name)
{
    return !strncasecmp(name, "F_END", 8) ||
           !strncasecmp(name, "FF_END", 8) ||
           !strncasecmp(name, "F1_END", 8) ||
           !strncasecmp(name, "F2_END", 8) ||
           !strncasecmp(name, "F3_END", 8) ||
           !strncasecmp(name, "F4_END", 8) ||
           !strncasecmp(name, "F5_END", 8) ||
           !strncasecmp(name, "F6_END", 8) ||
           !strncasecmp(name, "F7_END", 8) ||
           !strncasecmp(name, "F8_END", 8) ||
           !strncasecmp(name, "F9_END", 8);
}

static int R_FindFlatLogicalIndex(char *name)
{
    int i;

    for (i = 0; i < numflats; i++)
    {
        if (!strncasecmp(lumpinfo[flatlumpnums[i]].name, name, 8))
            return i;
    }

    return -1;
}

void R_InitFlats(void)
{
    int i;
    int logical;
    int namespace_depth;

    flatlumpnums = Z_MallocUnowned(numlumps * sizeof(*flatlumpnums), PU_STATIC);
    numflats = 0;
    namespace_depth = 0;

    for (i = 0; i < numlumps; i++)
    {
        if (R_IsFlatStartMarker(lumpinfo[i].name))
        {
            namespace_depth++;
            continue;
        }

        if (R_IsFlatEndMarker(lumpinfo[i].name))
        {
            if (namespace_depth > 0)
                namespace_depth--;
            continue;
        }

        if (namespace_depth <= 0 || lumpinfo[i].size != 4096)
            continue;

        logical = R_FindFlatLogicalIndex(lumpinfo[i].name);
        if (logical >= 0)
            flatlumpnums[logical] = i;
        else
            flatlumpnums[numflats++] = i;
    }

    if (numflats > 0)
    {
        firstflat = flatlumpnums[0];
        lastflat = flatlumpnums[numflats - 1];
    }
    else
    {
        firstflat = 0;
        lastflat = -1;
    }

    flattranslation = Z_MallocUnowned((numflats + 1) * 4, PU_STATIC);
    for (i = 0; i < numflats; i++)
        flattranslation[i] = i;
}
"""
    text = replace_exact(text, old_init, new_init, "r_data.c multi-range R_InitFlats")

    old_lookup = """//
// R_FlatNumForName
// Retrieval, get a flat number for a flat name.
//
short R_FlatNumForName(char *name)
{
    short i;

    i = W_GetNumForName(name);
    return i - firstflat;
}
"""
    new_lookup = r"""//
// R_FlatNumForName
// Retrieval, get a flat number for a flat name.
//
short R_FlatNumForName(char *name)
{
    int logical;

    logical = R_FindFlatLogicalIndex(name);
    if (logical >= 0)
        return (short)logical;

    /* Never turn a missing flat into a negative array index. */
    return 0;
}
"""
    text = replace_exact(text, old_lookup, new_lookup, "r_data.c logical flat lookup")

    old_sector_marks = """    for (i = 0; i < numsectors; i++)
    {
        flatpresent[sectors[i].floorpic] = 1;
        flatpresent[sectors[i].ceilingpic] = 1;
    }
"""
    new_sector_marks = r"""    for (i = 0; i < numsectors; i++)
    {
        if (sectors[i].floorpic >= 0 && sectors[i].floorpic < numflats)
            flatpresent[sectors[i].floorpic] = 1;

        if (sectors[i].ceilingpic >= 0 && sectors[i].ceilingpic < numflats)
            flatpresent[sectors[i].ceilingpic] = 1;
    }
"""
    text = replace_exact(
        text,
        old_sector_marks,
        new_sector_marks,
        "r_data.c flat precache bounds",
    )
    text = replace_exact(
        text,
        "            lump = firstflat + i;",
        "            lump = flatlumpnums[i];",
        "r_data.c physical flat precache lookup",
    )

    path.write_text(text, encoding="utf-8", newline="\n")


def patch_flat_r_plane(fd: Path) -> None:
    path = fd / "r_plane.c"
    text = path.read_text(encoding="utf-8")

    old_expr = "W_CacheLumpNum(firstflat + flattranslation[pl->picnum], PU_CACHE)"
    new_expr = "W_CacheLumpNum(flatlumpnums[flattranslation[pl->picnum]], PU_CACHE)"
    count = text.count(old_expr)
    if count == 0:
        raise RuntimeError(
            "r_plane.c: expected original firstflat flat-cache expressions"
        )

    text = text.replace(old_expr, new_expr)
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Pico PWAD: r_plane.c logical->physical flat lookups={count}")


def patch_flat_namespace(fd: Path) -> None:
    patch_flat_r_state(fd)
    patch_flat_r_data(fd)
    patch_flat_r_plane(fd)
    print("Pico PWAD: multi-range flat namespace + bounds")

# ---------------------------------------------------------------------------
# PWAD campaign menus.
# ---------------------------------------------------------------------------

MENU_GLOBALS = r'''
#if defined(MC_FASTDOOM_PICO)
/* MC_FASTDOOM_PICO_PWAD_CAMPAIGN_MENU */
#define MC_FD_CAMPAIGN_NONE         0
#define MC_FD_CAMPAIGN_SIGIL        1
#define MC_FD_CAMPAIGN_SIGIL2       2
#define MC_FD_CAMPAIGN_NERVE        3
#define MC_FD_CAMPAIGN_MASTERLEVELS 4
extern int mc_fd_pwad_campaign;

void MC_FD_Episode5(int choice);
void MC_FD_Episode6(int choice);
int MC_FD_MenuTextWidth2x(char *string);
void MC_FD_WriteMenuText2x(int x, int y, char *string);
#endif

'''

EPISODE_BLOCK = r'''//
// EPISODE SELECT
//
#define ep1 0
#define ep2 1
#define ep3 2
#define ep4 3
#define ep5 4
#define ep6 5
#define ep_end 6

menuitem_t EpisodeMenu[] =
    {
        {1, "M_EPI1", "Knee-Deep in the Dead", M_Episode},
        {1, "M_EPI2", "The Shores of Hell", M_Episode},
        {1, "M_EPI3", "Inferno", M_Episode},
        {1, "M_EPI4", "Thy Flesh Consumed", M_Episode},
        {1, "M_EPI5", "SIGIL", MC_FD_Episode5},
        {1, "M_EPI6", "SIGIL II", MC_FD_Episode6}};

menuitem_t EpisodeMenuSII[] =
    {
        {1, "M_EPI1", "Knee-Deep in the Dead", M_Episode},
        {1, "M_EPI2", "The Shores of Hell", M_Episode},
        {1, "M_EPI3", "Inferno", M_Episode},
        {1, "M_EPI4", "Thy Flesh Consumed", M_Episode},
        {1, "M_EPI6", "SIGIL II", MC_FD_Episode6}};

menuitem_t PwadCampaignMenu[] =
    {
        {1, "", "", M_Episode}};

menu_t EpiDef =
    {
        4,
        &MainDef,
        EpisodeMenu,
        M_DrawEpisode,
        48, 63,
        ep1
};

'''

NEWGAME_FUNCTIONS = r'''static void MC_FD_ConfigureEpisodeMenu(void)
{
    EpiDef.lastOn = 0;
    EpiDef.x = 48;
    EpiDef.y = 63;
    EpiDef.menuitems = EpisodeMenu;

#if defined(MC_FASTDOOM_PICO)
    switch (mc_fd_pwad_campaign)
    {
    case MC_FD_CAMPAIGN_SIGIL:
        EpiDef.numitems = 5;
        return;

    case MC_FD_CAMPAIGN_SIGIL2:
        EpiDef.menuitems = EpisodeMenuSII;
        EpiDef.numitems = 5;
        return;

    case MC_FD_CAMPAIGN_NERVE:
        EpiDef.menuitems = PwadCampaignMenu;
        EpiDef.numitems = 1;
        EpiDef.x = 24;
        EpiDef.y = 82;
        strcpy(PwadCampaignMenu[0].text, "No Rest for the Living");
        return;

    case MC_FD_CAMPAIGN_MASTERLEVELS:
        EpiDef.menuitems = PwadCampaignMenu;
        EpiDef.numitems = 1;
        EpiDef.x = 92;
        EpiDef.y = 82;
        strcpy(PwadCampaignMenu[0].text, "Master Levels");
        return;

    default:
        break;
    }
#endif

    if (gamemode == shareware)
        EpiDef.numitems = 1;
    else if (gamemode == registered)
        EpiDef.numitems = 3;
    else
        EpiDef.numitems = 4;
}

void M_NewGame(int choice)
{
    (void)choice;
    MC_FD_ConfigureEpisodeMenu();

#if defined(MC_FASTDOOM_PICO)
    if (gamemode == commercial &&
        (mc_fd_pwad_campaign == MC_FD_CAMPAIGN_NERVE ||
         mc_fd_pwad_campaign == MC_FD_CAMPAIGN_MASTERLEVELS))
    {
        NewDef.prevMenu = &EpiDef;
        M_SetupNextMenu(&EpiDef);
        return;
    }
#endif

    if (gamemode == commercial)
    {
        NewDef.prevMenu = &MainDef;
        M_SetupNextMenu(&NewDef);
    }
    else
    {
        NewDef.prevMenu = &EpiDef;
        M_SetupNextMenu(&EpiDef);
    }
}
'''

DRAW_EPISODE_FUNCTION = r'''void M_DrawEpisode(void)
{
#if defined(MC_FASTDOOM_PICO)
    if (mc_fd_pwad_campaign == MC_FD_CAMPAIGN_NERVE ||
        mc_fd_pwad_campaign == MC_FD_CAMPAIGN_MASTERLEVELS)
    {
#if defined(MODE_T4025) || defined(MODE_T4050)
        V_WriteTextDirect(10, 4, "SELECT CAMPAIGN");
#endif
#if defined(MODE_T8025) || defined(MODE_MDA) || defined(MODE_VT100) || defined(MODE_COLOR_MDA)
        V_WriteTextDirect(25, 4, "SELECT CAMPAIGN");
#endif
#if defined(MODE_T8050) || defined(MODE_T8043)
        V_WriteTextDirect(25, 9, "SELECT CAMPAIGN");
#endif
#if defined(MODE_X) || defined(MODE_Y) || defined(MODE_Y_HALF) || defined(USE_BACKBUFFER) || defined(MODE_VBE2_DIRECT)
#if defined(USE_BACKBUFFER)
        MC_FD_WriteMenuText2x(
            160 - MC_FD_MenuTextWidth2x("SELECT CAMPAIGN") / 2,
            38,
            "SELECT CAMPAIGN");
#else
        M_WriteText(160 - M_StringWidth("SELECT CAMPAIGN") / 2,
                    46,
                    "SELECT CAMPAIGN");
#endif
#endif
        return;
    }
#endif

#if defined(MODE_T4025) || defined(MODE_T4050)
    V_WriteTextDirect(13, 4, "WHICH EPISODE?");
#endif
#if defined(MODE_T8025) || defined(MODE_MDA) || defined(MODE_VT100) || defined(MODE_COLOR_MDA)
    V_WriteTextDirect(27, 4, "WHICH EPISODE?");
#endif
#if defined(MODE_T8050) || defined(MODE_T8043)
    V_WriteTextDirect(27, 9, "WHICH EPISODE?");
#endif
#if defined(MODE_X) || defined(MODE_Y) || defined(MODE_Y_HALF) || defined(USE_BACKBUFFER) || defined(MODE_VBE2_DIRECT)
    V_DrawPatchDirectCentered(54, 38, W_CacheLumpName("M_EPISOD", PU_CACHE));
#endif
}
'''

EPISODE_FUNCTIONS = r'''#if defined(MC_FASTDOOM_PICO)
void MC_FD_Episode5(int choice)
{
    (void)choice;
    M_Episode(4);
}

void MC_FD_Episode6(int choice)
{
    (void)choice;
    M_Episode(5);
}
#endif

void M_Episode(int choice)
{
    epi = choice;
    M_SetupNextMenu(&NewDef);
}
'''

MENU_TEXT_HELPERS = r'''#if defined(MC_FASTDOOM_PICO) && defined(USE_BACKBUFFER)
static void MC_FD_DrawPatch2x(int x, int y, patch_t *patch)
{
    int col;
    int base_x;
    int base_y;

    if (!patch)
        return;

    base_x = CENTERING_OFFSET_X + x - patch->leftoffset * 2;
    base_y = CENTERING_OFFSET_Y + y - patch->topoffset * 2;

    for (col = 0; col < patch->width; ++col)
    {
        column_t *column =
            (column_t *)((byte *)patch + patch->columnofs[col]);

        while (column->topdelta != 0xff)
        {
            const byte *source = (const byte *)column + 3;
            int row;

            for (row = 0; row < column->length; ++row)
            {
                int dx = base_x + col * 2;
                int dy = base_y + (column->topdelta + row) * 2;
                byte pixel = source[row];

                if (dx >= 0 && dx + 1 < SCREENWIDTH &&
                    dy >= 0 && dy + 1 < SCREENHEIGHT)
                {
                    byte *dest = backbuffer + dy * SCREENWIDTH + dx;
                    dest[0] = pixel;
                    dest[1] = pixel;
                    dest[SCREENWIDTH] = pixel;
                    dest[SCREENWIDTH + 1] = pixel;
                }
            }

            column =
                (column_t *)((byte *)column + column->length + 4);
        }
    }
}

int MC_FD_MenuTextWidth2x(char *string)
{
    int i;
    int width = 0;

    if (!string)
        return 0;

    for (i = 0; string[i]; ++i)
    {
        int c = toupper(string[i]) - HU_FONTSTART;

        if (c < 0 || c >= HU_FONTSIZE)
            width += 8;
        else
            width += hu_font[c]->width * 2;
    }

    return width;
}

void MC_FD_WriteMenuText2x(int x, int y, char *string)
{
    int i;
    int cx = x;

    if (!string)
        return;

    for (i = 0; string[i]; ++i)
    {
        int c = toupper(string[i]) - HU_FONTSTART;

        if (c < 0 || c >= HU_FONTSIZE)
        {
            cx += 8;
            continue;
        }

        MC_FD_DrawPatch2x(cx, y, hu_font[c]);
        cx += hu_font[c]->width * 2;

        if (cx >= SCREENWIDTH)
            break;
    }
}
#endif

'''

DRAWER_OLD = r'''        if (currentMenu->menuitems[i].name[0])
        {
#if defined(MODE_T4025) || defined(MODE_T4050)
            V_WriteTextDirect(x / 8, y / 8, currentMenu->menuitems[i].text);
#endif
#if defined(MODE_T8025) || defined(MODE_MDA) || defined(MODE_VT100) || defined(MODE_COLOR_MDA)
            V_WriteTextDirect(x / 4, y / 8, currentMenu->menuitems[i].text);
#endif
#if defined(MODE_T8050) || defined(MODE_T8043)
            V_WriteTextDirect(x / 4, y / 4, currentMenu->menuitems[i].text);
#endif
#if defined(MODE_X) || defined(MODE_Y) || defined(MODE_Y_HALF) || defined(USE_BACKBUFFER) || defined(MODE_VBE2_DIRECT)
            V_DrawPatchDirectCentered(x, y, W_CacheLumpName(currentMenu->menuitems[i].name, PU_CACHE));
#endif
        }

        y += LINEHEIGHT;
'''

DRAWER_NEW = r'''        if (currentMenu->menuitems[i].name[0]
#if defined(MC_FASTDOOM_PICO)
            && W_GetNumForName(currentMenu->menuitems[i].name) >= 0
#endif
           )
        {
#if defined(MODE_T4025) || defined(MODE_T4050)
            V_WriteTextDirect(x / 8, y / 8, currentMenu->menuitems[i].text);
#endif
#if defined(MODE_T8025) || defined(MODE_MDA) || defined(MODE_VT100) || defined(MODE_COLOR_MDA)
            V_WriteTextDirect(x / 4, y / 8, currentMenu->menuitems[i].text);
#endif
#if defined(MODE_T8050) || defined(MODE_T8043)
            V_WriteTextDirect(x / 4, y / 4, currentMenu->menuitems[i].text);
#endif
#if defined(MODE_X) || defined(MODE_Y) || defined(MODE_Y_HALF) || defined(USE_BACKBUFFER) || defined(MODE_VBE2_DIRECT)
            V_DrawPatchDirectCentered(x, y, W_CacheLumpName(currentMenu->menuitems[i].name, PU_CACHE));
#endif
        }
#if defined(MODE_X) || defined(MODE_Y) || defined(MODE_Y_HALF) || defined(USE_BACKBUFFER) || defined(MODE_VBE2_DIRECT)
        else if (currentMenu->menuitems[i].text[0])
        {
#if defined(MC_FASTDOOM_PICO) && defined(USE_BACKBUFFER)
            MC_FD_WriteMenuText2x(x, y, currentMenu->menuitems[i].text);
#else
            M_WriteText(x, y, currentMenu->menuitems[i].text);
#endif
        }
#endif

        y += LINEHEIGHT;
'''


def patch_campaign_menu(fd: Path) -> None:
    path = fd / "m_menu.c"
    text = path.read_text(encoding="utf-8")

    text = replace_exact(
        text,
        "    char text[22];",
        "    char text[32];",
        "m_menu.c menu text capacity",
    )

    extern_needle = "extern byte message_dontfuckwithme;\n"
    pos = text.find(extern_needle)
    if pos < 0:
        raise RuntimeError("m_menu.c: extern insertion point not found")
    pos += len(extern_needle)
    text = text[:pos] + MENU_GLOBALS + text[pos:]

    start = text.find("//\n// EPISODE SELECT\n//\n")
    end = text.find("//\n// NEW GAME\n//\n", start)
    if start < 0 or end < 0:
        raise RuntimeError("m_menu.c: episode menu definition region not found")
    text = text[:start] + EPISODE_BLOCK + text[end:]

    text = replace_function(
        text,
        r"void\s+M_NewGame\s*\(\s*int\s+choice\s*\)\s*\{",
        NEWGAME_FUNCTIONS,
    )

    text = replace_function(
        text,
        r"void\s+M_DrawEpisode\s*\(\s*void\s*\)\s*\{",
        DRAW_EPISODE_FUNCTION,
    )

    text = replace_function(
        text,
        r"void\s+M_Episode\s*\(\s*int\s+choice\s*\)\s*\{",
        EPISODE_FUNCTIONS,
    )

    marker = (
        "//\n"
        "// M_Drawer\n"
        "// Called after the view has been rendered,\n"
    )
    pos = text.find(marker)
    if pos < 0:
        raise RuntimeError("m_menu.c: M_Drawer insertion point not found")
    text = text[:pos] + MENU_TEXT_HELPERS + text[pos:]

    text = replace_exact(
        text,
        DRAWER_OLD,
        DRAWER_NEW,
        "m_menu.c generic menu item renderer",
    )

    path.write_text(text, encoding="utf-8", newline="\n")
    print("Pico PWAD: visible SIGIL/SIGIL II/NERVE/Master menus + 2x text")


# ---------------------------------------------------------------------------
# Driver.
# ---------------------------------------------------------------------------

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

    required = (
        "doomdef.h",
        "g_game.c",
        "m_menu.c",
        "r_data.c",
        "r_plane.c",
        "r_state.h",
        "w_wad.c",
        "wi_stuff.c",
        "f_finale.c",
    )
    missing = [name for name in required if not (fd / name).is_file()]
    if missing:
        print(
            "ERROR: generated FastDoom source is incomplete: "
            + ", ".join(missing),
            file=sys.stderr,
        )
        return 1

    try:
        patch_am_map(fd)
        patch_options_name(fd)
        patch_d_main(fd)
        patch_g_game_portability(fd)
        patch_descriptor_io(fd)
        patch_hu_stuff(fd)
        patch_timer_globals(fd)
        patch_wi_identifier(fd)

        patch_episode56_g_game(fd)
        patch_episode56_wi(fd)
        patch_episode56_finale(fd)
        patch_pwad_namespace(fd)
        patch_flat_namespace(fd)
        patch_campaign_menu(fd)
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    print(f"Pico portability + PWAD compatibility prepared {fd}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
