#!/usr/bin/env python3
"""Sync Pastime's external-emulator preset table from Daijishou.

Daijishou (https://github.com/TapiocaFox/Daijishou, MIT) maintains a
community-curated set of Android emulator launch templates as JSON under
`platforms/*.json`.  This script pulls them, projects each entry to a small
struct that fits Pastime's launch model, and emits a generated C header.

Run:
    python3 pastime/tools/sync_external_presets.py [--sha <commit>]

The header records the source commit SHA so the snapshot is reproducible.
Re-run + commit when an emulator has renamed its activity.
"""

import argparse
import json
import re
import shlex
import sys
import urllib.request
from pathlib import Path

REPO = "TapiocaFox/Daijishou"
RAW = "https://raw.githubusercontent.com/" + REPO
API = "https://api.github.com/repos/" + REPO

OUT_PATH = Path(__file__).resolve().parent.parent / "pastime_external_presets.h"

# Hand-curated shortname overrides.  Daijishou's `name` field for some
# packages is just the package name itself ("atomiswave - com.flycast.emulator")
# rather than the human app name, which would generate a noisy shortname
# like `comflycastemulator`.  Map by package -> preferred shortname; the
# sync script logs whenever an override is applied so we can spot when an
# upstream JSON cleanup makes the override redundant.  Only add packages
# the user is realistically going to type — long-tail emulators can keep
# the noisy default until someone files a request.
SHORTNAME_OVERRIDES = {
    "com.flycast.emulator":            "flycast",
    "io.recompiled.redream":           "redream",
    "com.seleuco.mame4droid":          "mame4droid",
    "org.mupen64plusae.v3.alpha":      "mupen64plus",
    "org.mupen64plusae.v3.fzurita":    "mupen64plusfz",
    "com.PceEmu":                      "pceemu",
    "com.fastemulator.gba":            "myboy",       # FastEmulator's GBA app
    "com.fastemulator.gbc":            "mygbc",
    "com.explusalpha.GbaEmu":          "gbaemu",
    "com.explusalpha.GbcEmu":          "gbcemu",
    "com.explusalpha.LynxEmu":         "lynxemu",
    "com.explusalpha.MdEmu":           "mdemu",
    "com.explusalpha.NesEmu":          "nesemu",
    "com.explusalpha.Snes9xPlus":      "snes9xplus",
    "com.explusalpha.SwanEmu":         "swanemu",
}


def fetch(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=30) as r:
        return r.read()


def fetch_json(url: str):
    return json.loads(fetch(url))


def resolve_sha(ref: str) -> str:
    """Resolve a ref (branch name or SHA) to a full commit SHA."""
    if re.fullmatch(r"[0-9a-f]{40}", ref):
        return ref
    data = fetch_json(f"{API}/commits/{ref}")
    return data["sha"]


def list_platform_files(sha: str) -> list[str]:
    """Enumerate platforms/*.json at the given SHA. Skips *.deprecated."""
    data = fetch_json(f"{API}/contents/platforms?ref={sha}")
    return sorted(x["name"] for x in data
                  if x["name"].endswith(".json") and "deprecated" not in x["name"])


def parse_am_args(arg_str: str) -> dict | None:
    """Parse Daijishou's amStartArguments string into a dict.

    Returns None if the entry is unsupported (raw POSIX path, multiple ROM
    extras, boolean extras, etc.). Caller treats None as "skip this entry".
    """
    # `am start` flag syntax: shell-tokenised, with option/value pairs.
    try:
        tokens = shlex.split(arg_str.replace("\n", " "))
    except ValueError:
        return None

    result = {
        "component": None, "action": None, "category": None,
        "extra_key": None, "mime_type": None,
    }
    rom_token_count = 0  # how many tokens equal {file.uri}
    raw_path_seen = False
    bool_extra_seen = False

    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t == "-n" and i + 1 < len(tokens):
            result["component"] = tokens[i + 1]; i += 2
        elif t == "-a" and i + 1 < len(tokens):
            result["action"] = tokens[i + 1]; i += 2
        elif t == "-c" and i + 1 < len(tokens):
            result["category"] = tokens[i + 1]; i += 2
        elif t == "-t" and i + 1 < len(tokens):
            result["mime_type"] = tokens[i + 1]; i += 2
        elif t == "-d" and i + 1 < len(tokens):
            val = tokens[i + 1]
            if val == "{file.path}":
                raw_path_seen = True
            elif val == "{file.uri}":
                result["extra_key"] = None  # setData
                rom_token_count += 1
            i += 2
        elif t in ("-e", "--es", "--eu") and i + 2 < len(tokens):
            # `am start` synonyms: -e and --es are string extras; --eu is
            # a Uri extra. From our perspective they're identical — the
            # Java helper always passes the URI's toString anyway.
            key, val = tokens[i + 1], tokens[i + 2]
            if val == "{file.path}":
                raw_path_seen = True
            elif val == "{file.uri}":
                # Conflicting ROM-extra and ROM-data is unlikely; keep the
                # extra form since custom extras are how most modern emus
                # consume the path.
                result["extra_key"] = key
                rom_token_count += 1
            i += 3
        elif t == "--ez" and i + 2 < len(tokens):
            bool_extra_seen = True; i += 3
        elif t == "--ei" and i + 2 < len(tokens):
            i += 3  # int extra; ignore
        elif t.startswith("--activity-") or t == "--receiver-foreground":
            # Caller (Java helper) always sets NEW_TASK; ignore.
            i += 1
        else:
            # Unknown flag.  If the next token is a ROM placeholder, log
            # a warning — silently dropping such an entry would silently
            # lose an emulator the next time we resync.
            if i + 1 < len(tokens) and tokens[i + 1] in ("{file.uri}",
                                                        "{file.path}"):
                print(f"WARNING: unknown flag {t!r} carries ROM placeholder; "
                      f"entry may be misparsed", file=sys.stderr)
            i += 1

    if raw_path_seen:        return None  # v1: URI-only
    if bool_extra_seen:      return None  # v1: no boolean extras
    if rom_token_count != 1: return None  # need exactly one ROM placeholder
    if not result["component"]:
        return None
    return result


def package_from_component(component: str) -> str | None:
    """`-n com.foo.bar/.MainActivity` → `com.foo.bar`."""
    if "/" not in component:
        return None
    pkg = component.split("/", 1)[0]
    return pkg if "." in pkg else None


def normalize_component(component: str, package: str) -> str:
    """Strip the `<package>/` prefix; keep what `am start` accepts.

    Returns either a relative class (`.MainActivity`) or a fully-qualified one
    (`org.foo.SomeActivity`). The Java side reconstructs the ComponentName.
    """
    return component[len(package) + 1:]


def is_retroarch_player(unique_id: str, component: str) -> bool:
    """Drop RetroArch players — we are RetroArch."""
    if ".ra64." in unique_id or ".ra32." in unique_id or ".ra." in unique_id:
        return True
    if component and component.startswith("com.retroarch"):
        return True
    return False


def derive_shortname(name: str) -> str:
    """Daijishou's `name` field is structured as "<platform-prefix> - <App>"
    (e.g. "psx - Duckstation", "gba - Skyemu", "n64 - Mupen64Plus FZ").
    The user-facing shortname should be just the app side, so we split on
    " - " and take the rightmost segment, then sanitize to [a-z0-9]+.

    Naturally distinguishes free-vs-paid SKUs ("ppsspp" vs "ppssppgold")
    and handles odd capitalisation ("ePSXe" → "epsxe").  The full catalog
    (94 deduped entries today) yields zero collisions; the sync script
    warns if a future regen introduces one.
    """
    if not name:
        return ""
    # rsplit handles "name - with - dashes - in - app - name" by always
    # taking the last segment, which is the most specific.
    app = name.rsplit(" - ", 1)[-1]
    return re.sub(r"[^a-z0-9]", "", app.lower())


def collect_presets(sha: str) -> tuple[list[dict], dict]:
    files = list_platform_files(sha)
    presets: dict[str, dict] = {}  # package → spec
    skipped = {"retroarch": 0, "raw_path_or_bool": 0, "no_package": 0,
               "duplicate": 0, "no_shortname": 0}

    for fname in files:
        platform_data = fetch_json(f"{RAW}/{sha}/platforms/{fname}")
        for player in platform_data.get("playerList", []):
            unique_id = player.get("uniqueId", "")
            am_args = player.get("amStartArguments", "")
            name = player.get("name", "")

            parsed = parse_am_args(am_args)
            if not parsed:
                skipped["raw_path_or_bool"] += 1
                continue
            if is_retroarch_player(unique_id, parsed["component"]):
                skipped["retroarch"] += 1
                continue

            pkg = package_from_component(parsed["component"])
            if not pkg:
                skipped["no_package"] += 1
                continue

            shortname = SHORTNAME_OVERRIDES.get(pkg) or derive_shortname(name)
            if not shortname:
                # No name in the JSON, or only non-alphanumerics; falling
                # back to the full-package marker is fine, just lose the
                # readable form.
                skipped["no_shortname"] += 1

            spec = {
                "package":    pkg,
                "shortname":  shortname or None,
                "component":  normalize_component(parsed["component"], pkg),
                "action":     parsed["action"],
                "category":   parsed["category"],
                "extra_key":  parsed["extra_key"],
                "mime_type":  parsed["mime_type"],
                "kill_first": bool(player.get("killPackageProcesses", False)),
                "_source":    f"{fname}#{unique_id}",
            }

            if pkg in presets:
                # Prefer the candidate with a cleaner shortname.
                # Daijishou's per-platform entries vary in quality —
                # "atomiswave - com.flycast.emulator" yields the noisy
                # shortname `comflycastemulator` while
                # "dreamcast - Flycast" yields the clean `flycast`.
                # Tie-breaker: shorter shortname wins (flycast < comflycastemulator).
                # Only matters when both candidates are shortname-bearing.
                cur = presets[pkg]
                cur_sn = cur.get("shortname") or ""
                new_sn = spec.get("shortname") or ""
                cur_is_pkg = cur_sn == re.sub(r"[^a-z0-9]", "", pkg.lower())
                new_is_pkg = new_sn == re.sub(r"[^a-z0-9]", "", pkg.lower())
                better = False
                if new_sn and not cur_sn:
                    better = True
                elif new_sn and cur_is_pkg and not new_is_pkg:
                    # Current shortname is just the sanitised package
                    # (the noisy form); new one is something cleaner.
                    better = True
                elif new_sn and cur_sn and not cur_is_pkg and not new_is_pkg \
                        and len(new_sn) < len(cur_sn):
                    # Both are "clean", prefer shorter (more memorable).
                    better = True
                if better:
                    presets[pkg] = spec
                else:
                    skipped["duplicate"] += 1
                continue
            presets[pkg] = spec

    # Shortname collision check.  Today (with 94 deduped presets) this should
    # report zero; if a future sync introduces a duplicate the warning lets us
    # decide whether to disambiguate at sync time or to add runtime install-
    # check fallback in the C lookup.
    by_shortname: dict[str, list[str]] = {}
    for spec in presets.values():
        sn = spec["shortname"]
        if sn:
            by_shortname.setdefault(sn, []).append(spec["package"])
    collisions = {k: v for k, v in by_shortname.items() if len(v) > 1}
    if collisions:
        print(f"WARNING: {len(collisions)} shortname collision(s) — "
              f"users typing one of these will get the first-listed package; "
              f"consider hand-curating an alias override.", file=sys.stderr)
        for sn, pkgs in sorted(collisions.items()):
            print(f"  {sn!r}: {pkgs}", file=sys.stderr)

    return list(presets.values()), skipped


def c_str(s: str | None) -> str:
    if s is None:
        return "NULL"
    # Strings from Daijishou are well-behaved (ASCII identifiers, action
    # strings, MIME types). Belt-and-braces escape just in case.
    escaped = s.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def emit_header(presets: list[dict], sha: str, skipped: dict) -> str:
    presets_sorted = sorted(presets, key=lambda p: p["package"])
    rows = []
    for p in presets_sorted:
        rows.append(
            f"   {{  /* {p['_source']} */\n"
            f"      {c_str(p['package'])},\n"
            f"      {c_str(p['shortname'])},\n"
            f"      {c_str(p['component'])},\n"
            f"      {c_str(p['action'])},\n"
            f"      {c_str(p['category'])},\n"
            f"      {c_str(p['extra_key'])},\n"
            f"      {c_str(p['mime_type'])},\n"
            f"      {'true' if p['kill_first'] else 'false'}\n"
            f"   }},"
        )
    body = "\n".join(rows)
    return (
        "/* GENERATED FILE - DO NOT EDIT.\n"
        " *\n"
        " * Regenerate via:\n"
        " *   python3 pastime/tools/sync_external_presets.py\n"
        " *\n"
        f" * Source:    https://github.com/{REPO}\n"
        f" * Commit:    {sha}\n"
        f" * Filtered:  {skipped['retroarch']} RetroArch players,\n"
        f" *            {skipped['raw_path_or_bool']} raw-path/bool-extra entries,\n"
        f" *            {skipped['no_package']} malformed,\n"
        f" *            {skipped['duplicate']} package duplicates,\n"
        f" *            {skipped['no_shortname']} without a derivable shortname.\n"
        f" * Kept:      {len(presets_sorted)} entries.\n"
        " *\n"
        " * Daijishou is MIT-licensed (Copyright (c) 2022 TapiocaFox / Yves Chen).\n"
        " */\n"
        "\n"
        "#ifndef PASTIME_EXTERNAL_PRESETS_H\n"
        "#define PASTIME_EXTERNAL_PRESETS_H\n"
        "\n"
        "#include \"pastime_external.h\"\n"
        "\n"
        "static const pastime_external_spec_t pastime_external_presets[] = {\n"
        f"{body}\n"
        "};\n"
        "\n"
        "static const size_t pastime_external_presets_count =\n"
        "   sizeof(pastime_external_presets) / sizeof(pastime_external_presets[0]);\n"
        "\n"
        "#endif /* PASTIME_EXTERNAL_PRESETS_H */\n"
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sha", default="main",
                    help="Daijishou commit/branch to pin to (default: main)")
    ap.add_argument("--out", default=str(OUT_PATH),
                    help="Output header path")
    args = ap.parse_args()

    if args.sha == "main":
        print("WARNING: --sha defaulted to 'main' (mutable ref). The "
              "regenerated header diff is your trust gate; review before "
              "committing. Pin a SHA explicitly to make re-runs reproducible.",
              file=sys.stderr)
    sha = resolve_sha(args.sha)
    print(f"Pinned to {REPO}@{sha}", file=sys.stderr)

    presets, skipped = collect_presets(sha)
    print(f"Kept {len(presets)} entries; skipped {skipped}", file=sys.stderr)

    header = emit_header(presets, sha, skipped)
    Path(args.out).write_text(header)
    print(f"Wrote {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
