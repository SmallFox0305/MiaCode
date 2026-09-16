#!/usr/bin/env python3
"""Generate the update-check manifests for the packages a release publishes.

The client only ever compares "manifest version vs its own version". Deciding
*which* version a channel points at happens here, where the whole release
context exists.

Size and hash come from the archives themselves. The packaging pipeline
(scripts/build/package.py) emits only the archive name, and hashing the bytes
that are actually uploaded leaves one fewer record that can disagree with the
Release.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys


SCHEMA = 1
REPO = "fanfaredash/MiaCode"
# SemanticVersion.cpp holds core numbers and numeric prerelease identifiers in
# an int, and refuses anything wider rather than comparing it wrong.
INT_MAX = 2147483647
# Mirrors kMaxPayloadBytes in src/app/services/update/NetworkUpdateFetcher.cpp:
# a manifest past this is refused by every client, so publishing one is worse
# than failing the release step.
MAX_MANIFEST_BYTES = 64 * 1024
# Release notes are a summary, not the changelog. The dialog links to the
# release page for the rest.
NOTES_CHAR_LIMIT = 2000
# Package names are <name>_<version>[_<channel>]_<os>_<arch>, so the trailing
# two segments are the only part that says which platform an archive is for.
# The keys must match currentPlatformKey() in
# src/app/services/update/UpdateManifest.cpp and the matrix in
# .github/workflows/package.yml.
PLATFORM_SUFFIXES = {
    "_mac_arm64.7z": "macos-arm64",
    "_win_x64.7z": "windows-x64",
    "_win_arm64.7z": "windows-arm64",
}
PLATFORM_KEYS = ("windows-x64", "windows-arm64", "macos-arm64")

IDENTIFIER = re.compile(r"[0-9A-Za-z-]+")
NUMERIC = re.compile(r"0|[1-9][0-9]*")


def is_digits(text: str) -> bool:
    """ASCII digits only -- str.isdigit() also accepts full-width forms."""
    return bool(text) and all("0" <= character <= "9" for character in text)


def parse_number(text: str):
    """A semver numeric identifier: no leading zeros, and it fits in an int."""
    if not NUMERIC.fullmatch(text):
        return None
    value = int(text)
    return value if value <= INT_MAX else None


def parse_version(text: str):
    """A semver subset mirroring src/app/services/update/SemanticVersion.cpp.

    Returns (major, minor, patch, prerelease_identifiers) or None. Every
    rejection has a matching case in src/tools/update/UpdateVersionSpec.cpp.
    """
    body = text.strip()
    if body[:1] in ("v", "V"):
        body = body[1:]
    # Build metadata takes no part in ordering.
    body = body.split("+", 1)[0]
    core, separator, prerelease_text = body.partition("-")
    if separator and not prerelease_text:
        # "2.0.0-" carries an empty prerelease, which is not the same as none.
        return None
    parts = core.split(".")
    if len(parts) != 3:
        return None
    numbers = []
    for part in parts:
        value = parse_number(part)
        if value is None:
            return None
        numbers.append(value)
    prerelease = []
    if prerelease_text:
        prerelease = prerelease_text.split(".")
        for identifier in prerelease:
            if not IDENTIFIER.fullmatch(identifier):
                return None
            if is_digits(identifier) and parse_number(identifier) is None:
                # A leading zero, or a number the C++ side cannot hold: both
                # would order wrong rather than merely look odd.
                return None
    return (numbers[0], numbers[1], numbers[2], prerelease)


def compare_identifier(left: str, right: str) -> int:
    left_numeric = is_digits(left)
    right_numeric = is_digits(right)
    if left_numeric and right_numeric:
        return (int(left) > int(right)) - (int(left) < int(right))
    if left_numeric != right_numeric:
        return -1 if left_numeric else 1
    return (left > right) - (left < right)


def compare_versions(left, right) -> int:
    """-1 / 0 / +1 over parse_version() tuples."""
    for index in range(3):
        if left[index] != right[index]:
            return -1 if left[index] < right[index] else 1
    left_pre, right_pre = left[3], right[3]
    if bool(left_pre) != bool(right_pre):
        # A release outranks any prerelease of the same core version.
        return 1 if not left_pre else -1
    for left_id, right_id in zip(left_pre, right_pre):
        ordering = compare_identifier(left_id, right_id)
        if ordering != 0:
            return ordering
    if len(left_pre) == len(right_pre):
        return 0
    return -1 if len(left_pre) < len(right_pre) else 1


def strip_tag(tag: str) -> str:
    return tag[1:] if tag[:1] in ("v", "V") else tag


def outranks(parsed, published: str) -> bool:
    """Is `parsed` strictly newer than the version a published manifest names?

    An absent or unreadable baseline counts as "nothing published": a manifest
    somebody hand-edited into an unparseable state must not freeze its channel
    forever.
    """
    text = (published or "").strip()
    if not text:
        return True
    existing = parse_version(text)
    if existing is None:
        print(f"warning: published version {text!r} does not parse; treating the channel as empty")
        return True
    return compare_versions(parsed, existing) > 0


def platform_for(name: str):
    for suffix, key in PLATFORM_SUFFIXES.items():
        if name.endswith(suffix):
            return key
    return None


def names_version(name: str, version_text: str) -> bool:
    """Does this archive actually contain the version the tag claims?

    Nightlies carry a channel segment after the version, releases do not, so
    both shapes share the same prefix.
    """
    return name.startswith(f"MiaCode_{version_text}_")


def sha256_of(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_platform_entries(packages_dir: str, tag: str, version_text: str) -> dict:
    """Hash every published archive and key it by the platform it targets.

    download-artifact nests each archive in a directory named after the
    artifact, so this walks rather than lists.
    """
    root = pathlib.Path(packages_dir)
    archives = sorted(path for path in root.rglob("*.7z") if path.is_file())
    if not archives:
        raise SystemExit(f"no .7z package found under {root}")
    entries: dict = {}
    for archive in archives:
        key = platform_for(archive.name)
        if key is None:
            # Guessing would point a platform at somebody else's download.
            raise SystemExit(f"cannot tell which platform {archive.name} is built for")
        if not names_version(archive.name, version_text):
            # The tag moved but MIACODE_VERSION in CMakeLists.txt did not. Left
            # alone, every client would download a package that reports the old
            # version and so keeps offering itself the same update forever.
            raise SystemExit(
                f"{archive.name} is not a {version_text} package -- the tag and "
                "MIACODE_VERSION in CMakeLists.txt disagree")
        if key in entries:
            raise SystemExit(
                f"two packages claim {key}: {entries[key]['file']} and {archive.name}")
        entries[key] = {
            "file": archive.name,
            "bytes": archive.stat().st_size,
            "sha256": sha256_of(archive),
            "url": f"https://github.com/{REPO}/releases/download/{tag}/{archive.name}",
        }
    return entries


def changelog_notes(path: pathlib.Path, version_text: str) -> dict:
    """Lift the `## <version>` section out of CHANGELOG.md.

    The changelog is written in Chinese, so it can only fill zh_CN; pass
    --notes to supply the other languages.
    """
    if not path.is_file():
        print(f"warning: {path} does not exist; the manifest carries no notes")
        return {}
    body: list[str] = []
    collecting = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("## "):
            if collecting:
                break
            collecting = line[3:].strip() == version_text
            continue
        if collecting:
            body.append(line)
    text = "\n".join(body).strip()
    if not text:
        print(f"warning: CHANGELOG.md has no '## {version_text}' section; the manifest carries no notes")
        return {}
    if len(text) > NOTES_CHAR_LIMIT:
        text = text[:NOTES_CHAR_LIMIT].rstrip() + "…"
    return {"zh_CN": text}


def build_manifest(tag: str, channel: str, entries: dict, notes: dict, released_at: str) -> dict:
    version_text = strip_tag(tag)
    parsed = parse_version(version_text)
    if parsed is None:
        raise SystemExit(f"tag is not a usable version: {tag}")
    return {
        "schema": SCHEMA,
        "channel": channel,
        "major": parsed[0],
        "version": version_text,
        "releasedAt": released_at,
        "releasePageUrl": f"https://github.com/{REPO}/releases/tag/{tag}",
        "mandatory": False,
        "notes": notes,
        "platforms": entries,
    }


def plan_channels(parsed, current_stable: str, current_beta: str) -> list:
    """Which channels this version is allowed to move forward.

    A client cannot downgrade, so a channel that goes backwards strands
    everyone already on the newer build. Both rules follow from that:

    * stable only ever takes a release, and only a newer one;
    * beta holds the highest of (stable, beta), which is how a beta user ends
      up on a release once the release outranks the newest beta.
    """
    channels = []
    beats_stable = outranks(parsed, current_stable)
    if not parsed[3] and beats_stable:
        channels.append("stable")
    if beats_stable and outranks(parsed, current_beta):
        channels.append("beta")
    return channels


def cmake_version(path: pathlib.Path) -> str:
    """The version CMakeLists.txt bakes into the build, e.g. 2.0.0-alpha."""
    text = path.read_text(encoding="utf-8")
    parts = []
    for field in ("MAJOR", "MINOR", "PATCH"):
        found = re.search(rf'set\(MIACODE_VERSION_{field}\s+"([^"]*)"', text)
        if found is None:
            raise SystemExit(f"{path} has no MIACODE_VERSION_{field}")
        parts.append(found.group(1))
    version = ".".join(parts)
    found = re.search(r'set\(MIACODE_VERSION_PRERELEASE\s+"([^"]*)"', text)
    if found is not None and found.group(1):
        version += f"-{found.group(1)}"
    return version


def command_version(args) -> int:
    """Print the version this commit builds, so a rehearsal can name its tag."""
    print(cmake_version(pathlib.Path(args.cmakelists)))
    return 0


def command_verify(args) -> int:
    """Does the tag name the version this commit actually builds?

    Cheap to answer and expensive to get wrong: a tag ahead of CMakeLists.txt
    produces packages that report the old version, so every client would keep
    offering itself the same update forever. Run this before the build, not
    after.
    """
    version_text = strip_tag(args.tag)
    if parse_version(version_text) is None:
        raise SystemExit(f"tag is not a usable version: {args.tag}")
    built = cmake_version(pathlib.Path(args.cmakelists))
    if built != version_text:
        raise SystemExit(
            f"tag {args.tag} builds version {built} -- bump MIACODE_VERSION_* in "
            f"{args.cmakelists} to {version_text}, or retag")
    print(f"{args.tag} matches the {built} this commit builds")
    return 0


def command_published(args) -> int:
    """Print the version a published manifest names, or an empty line.

    Anything unreadable prints nothing, because an absent channel and a channel
    somebody hand-edited into nonsense are the same input to plan_channels().
    """
    version = ""
    try:
        data = json.loads(pathlib.Path(args.manifest).read_text(encoding="utf-8"))
        if isinstance(data, dict) and isinstance(data.get("version"), str):
            version = data["version"]
    except (OSError, ValueError):
        version = ""
    print(version)
    return 0


def command_generate(args) -> int:
    version_text = strip_tag(args.tag)
    parsed = parse_version(version_text)
    if parsed is None:
        raise SystemExit(f"tag is not a usable version: {args.tag}")

    channels = plan_channels(parsed, args.current_stable_version, args.current_beta_version)
    if not channels:
        print(f"skip: {version_text} does not outrank what the channels already publish")
        print(json.dumps({"written": []}))
        return 0

    entries = load_platform_entries(args.packages, args.tag, version_text)
    missing = [key for key in PLATFORM_KEYS if key not in entries]
    if missing:
        if not args.allow_partial:
            raise SystemExit(
                "no package for " + ", ".join(missing)
                + " -- those clients would see no update at all. Pass --allow-partial if that is intended.")
        print(f"warning: publishing without {', '.join(missing)}")

    if args.notes:
        notes = json.loads(pathlib.Path(args.notes).read_text(encoding="utf-8"))
    elif args.changelog:
        notes = changelog_notes(pathlib.Path(args.changelog), version_text)
    else:
        notes = {}

    output_dir = pathlib.Path(args.out)
    output_dir.mkdir(parents=True, exist_ok=True)
    written = []
    for channel in channels:
        manifest = build_manifest(args.tag, channel, entries, notes, args.released_at)
        payload = json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
        encoded = payload.encode("utf-8")
        if len(encoded) > MAX_MANIFEST_BYTES:
            raise SystemExit(
                f"the {channel} manifest is {len(encoded)} bytes, past the {MAX_MANIFEST_BYTES} "
                "the client accepts -- shorten the release notes")
        path = output_dir / f"{parsed[0]}-{channel}.json"
        path.write_text(payload, encoding="utf-8")
        written.append(str(path))

    print(json.dumps({"written": written}, ensure_ascii=False))
    return 0


def command_selfcheck(_args) -> int:
    """Assertions mirroring src/tools/update/UpdateVersionSpec.cpp."""
    failures = []

    def check(condition, message):
        if not condition:
            failures.append(message)

    # ---- Parsing ----
    check(parse_version("2.1.0") == (2, 1, 0, []), "a release version parses with an empty prerelease")
    check(parse_version("2.0.0-beta.3") == (2, 0, 0, ["beta", "3"]), "prerelease identifiers split on dots")
    check(parse_version("v2.0.0-alpha") == (2, 0, 0, ["alpha"]), "a leading v is accepted so release tags parse")
    check(parse_version("2.0.0-alpha+20260915") == (2, 0, 0, ["alpha"]), "build metadata is discarded")
    check(parse_version("V2.0.0") == (2, 0, 0, []), "an uppercase leading V is accepted just like lowercase v")
    check(parse_version("2.0.0-2147483647") == (2, 0, 0, ["2147483647"]), "a numeric identifier at INT_MAX still parses")
    check(parse_version("2.0.0-0") == (2, 0, 0, ["0"]), "a bare zero numeric identifier is legal")

    for bad in ("", "2.0", "2.0.0.1", "2.0.x", "2.0.0-", "2.0.0-beta..1", "-1.0.0", "nightly",
                "2.0.0-99999999999999999999", "2.0.0-2147483648", "2.01.0", "2.0.0-01", "2.0.0-０"):
        check(parse_version(bad) is None, f"malformed version is rejected: {bad!r}")

    # ---- Ordering ----
    for left, right, expected in (
        ("2.1.0", "2.0.0", 1),
        ("2.0.10", "2.0.9", 1),
        ("10.0.0", "9.9.9", 1),
        ("2.0.0", "2.0.0", 0),
        ("2.0.0-alpha", "2.0.0", -1),
        ("2.0.0", "2.0.0-alpha", 1),
        ("2.0.0-alpha", "1.9.9", 1),
        ("2.0.0-alpha", "2.0.0-beta.1", -1),
        ("2.0.0-beta.2", "2.0.0-beta.10", -1),
        ("2.0.0-beta.1", "2.0.0-beta", 1),
        ("2.0.0-1", "2.0.0-alpha", -1),
        ("2.0.0-beta.1", "2.0.0-alpha", 1),
        ("2.0.0-alpha", "2.0.0-alpha", 0),
    ):
        actual = compare_versions(parse_version(left), parse_version(right))
        check(actual == expected, f"compare({left}, {right}) = {actual}, expected {expected}")

    # ---- Channel rules ----
    for tag, stable, beta, expected in (
        ("v2.1.0", "", "", ["stable", "beta"]),
        ("v2.1.0", "2.0.0", "2.0.5-beta.1", ["stable", "beta"]),
        ("v2.1.0", "2.1.0", "", []),
        ("v2.1.0", "2.2.0", "", []),
        # A release older than the newest beta still owns stable, but must not
        # drag beta users backwards.
        ("v2.1.0", "2.0.0", "2.2.0-beta.1", ["stable"]),
        ("v2.2.0-beta.2", "2.1.0", "2.2.0-beta.1", ["beta"]),
        ("v2.2.0-beta.1", "2.1.0", "2.2.0-beta.1", []),
        ("v2.0.5-beta.1", "2.1.0", "", []),
        ("v2.2.0-beta.1", "", "", ["beta"]),
    ):
        actual = plan_channels(parse_version(strip_tag(tag)), stable, beta)
        check(actual == expected, f"plan_channels({tag}, stable={stable!r}, beta={beta!r}) = {actual}, expected {expected}")

    # ---- Package naming ----
    for name, expected in (
        ("MiaCode_2.1.0_mac_arm64.7z", "macos-arm64"),
        ("MiaCode_2.1.0_win_x64.7z", "windows-x64"),
        ("MiaCode_2.1.0_win_arm64.7z", "windows-arm64"),
        ("MiaCode_2.1.0-beta.3_mac_arm64.7z", "macos-arm64"),
        # A nightly carries a channel segment, and must still resolve.
        ("MiaCode_2.1.0_nightly_win_x64.7z", "windows-x64"),
        ("MiaCode_2.1.0_linux_x64.7z", None),
        ("MiaCode_2.1.0_mac_arm64.zip", None),
    ):
        actual = platform_for(name)
        check(actual == expected, f"platform_for({name}) = {actual}, expected {expected}")

    for name, version_text, expected in (
        ("MiaCode_2.1.0_mac_arm64.7z", "2.1.0", True),
        ("MiaCode_2.1.0_nightly_win_x64.7z", "2.1.0", True),
        ("MiaCode_2.0.0-alpha_mac_arm64.7z", "2.1.0", False),
        ("MiaCode_2.1.0-beta.1_mac_arm64.7z", "2.1.0", False),
    ):
        actual = names_version(name, version_text)
        check(actual == expected, f"names_version({name}, {version_text}) = {actual}, expected {expected}")

    # ---- Manifest shape ----
    entries = {"macos-arm64": {"file": "a.7z", "bytes": 1, "sha256": "x", "url": "https://x"}}
    release = build_manifest("v2.1.0", "stable", entries, {}, "2026-09-20")
    check(release["major"] == 2 and release["version"] == "2.1.0", "build_manifest lost the version")
    check(release["schema"] == SCHEMA, "build_manifest wrote the wrong schema")
    check(release["releasePageUrl"] == f"https://github.com/{REPO}/releases/tag/v2.1.0",
          "build_manifest produced the wrong release page url")

    if failures:
        for line in failures:
            print(f"FAIL: {line}", file=sys.stderr)
        return 1
    print("update_manifest_script ok")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    generate = sub.add_parser("generate", help="write the channel manifests")
    generate.add_argument("--tag", required=True, help="release tag, e.g. v2.1.0-beta.3")
    generate.add_argument("--packages", required=True,
                          help="directory holding the published .7z archives, at any depth")
    generate.add_argument("--out", required=True, help="output directory")
    generate.add_argument("--released-at", required=True, help="YYYY-MM-DD")
    generate.add_argument("--notes", help="path to a JSON object of language token -> notes")
    generate.add_argument("--changelog", help="CHANGELOG.md to lift the zh_CN notes from")
    generate.add_argument("--current-stable-version", default="",
                          help="version in the published stable manifest, if any")
    generate.add_argument("--current-beta-version", default="",
                          help="version in the published beta manifest, if any")
    generate.add_argument("--allow-partial", action="store_true",
                          help="publish even though some platform has no package")
    generate.set_defaults(func=command_generate)

    published = sub.add_parser("published", help="print the version a downloaded manifest names")
    published.add_argument("--manifest", required=True, help="path to a downloaded channel manifest")
    published.set_defaults(func=command_published)

    version = sub.add_parser("version", help="print the version CMakeLists.txt builds")
    version.add_argument("--cmakelists", default="CMakeLists.txt", help="path to CMakeLists.txt")
    version.set_defaults(func=command_version)

    verify = sub.add_parser("verify", help="check the tag against the version the build bakes in")
    verify.add_argument("--tag", required=True, help="release tag, e.g. v2.1.0-beta.3")
    verify.add_argument("--cmakelists", default="CMakeLists.txt", help="path to CMakeLists.txt")
    verify.set_defaults(func=command_verify)

    selfcheck = sub.add_parser("selfcheck", help="run the built-in assertions")
    selfcheck.set_defaults(func=command_selfcheck)

    args = parser.parse_args()
    result = args.func(args)
    sys.exit(result if isinstance(result, int) else 0)


if __name__ == "__main__":
    main()
