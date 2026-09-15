#!/usr/bin/env python3
"""Platform package identity, static verification and Actions measurements."""

import hashlib
import json
import os
import re
from datetime import datetime, timedelta, timezone
from pathlib import Path
import struct
import subprocess
import sys
import time
import urllib.parse
import urllib.request


DIST = Path("dist")
PLATFORM = os.environ.get("CI_PLATFORM", "")
# Compiler/build cache prefix follows files that change compile or package
# layout. Verification scripts stay out of `source` so a finished package can
# be reused; they must not rotate this prefix.
WINDOWS_BUILD_RECIPE = (
    "scripts/build/build-win.ps1",
    "scripts/build/provision-qt.ps1",
    "scripts/build/windows-toolchain.psd1",
)
MACOS_BUILD_RECIPE = (
    "scripts/build/build-macos-ci.sh",
    "scripts/build/package-mac.sh",
    "scripts/build/thin-macos-app.sh",
)
MACOS_MEDIA_RECIPE = (
    "scripts/ffmpeg/ensure-macos-ffmpeg.sh",
    "scripts/ffmpeg/ensure-macos-ffmpeg-dev.sh",
)
WINDOWS_MEDIA_RECIPE = (
    "scripts/ffmpeg/ensure-windows-ffmpeg.ps1",
    "scripts/ffmpeg/ensure-windows-ffmpeg-dev.ps1",
    "scripts/ffmpeg/trim",
    "scripts/build/windows-toolchain.psd1",
)
# These change CI checks and job text, not the 7z payload.
SOURCE_EXCLUDE = (
    "scripts/build/ci-package.py",
    "scripts/build/verify-win-package.ps1",
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def tree(*paths, exclude=(), revision="HEAD"):
    # Git object IDs cover LFS pointers and submodule commits before downloads.
    # Generated SDKs/build outputs never enter the source identity.
    data = subprocess.check_output(["git", "ls-tree", "-r", "-z", revision, "--", *paths])
    if not exclude:
        return data
    skipped = set(exclude)
    kept = []
    for entry in data.split(b"\0"):
        if not entry:
            continue
        path = entry.split(b"\t", 1)[1].decode()
        if any(path == skip or path.startswith(skip + "/") for skip in skipped):
            continue
        kept.append(entry)
    return b"\0".join(kept) + b"\0" if kept else b""


def output(**values):
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as stream:
        for key, value in values.items():
            if "\n" in value:
                stream.write(f"{key}<<CACHE_KEYS\n{value}\nCACHE_KEYS\n")
            else:
                stream.write(f"{key}={value}\n")


def summary(message):
    with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as stream:
        stream.write(message + "\n")


def windows_section(name, revision):
    data = subprocess.check_output([
        "git", "show", f"{revision}:scripts/build/windows-toolchain.psd1"])
    match = re.search(rb"(?ms)^    " + name.encode() + rb" = @\{.*?^    \}", data)
    if match is None:
        raise RuntimeError(f"Missing Windows toolchain section: {name}")
    return match.group()


def cache_inputs(revision="HEAD"):
    if PLATFORM.startswith("windows-"):
        media_paths = WINDOWS_MEDIA_RECIPE[:-1]
        if PLATFORM == "windows-arm64":
            media_paths = media_paths[:-1]  # The trim toolchain builds x64 only.
        media = tree(*media_paths, revision=revision) + windows_section("FFmpeg", revision)
        build = tree("scripts/build/build-win.ps1", "scripts/build/provision-qt.ps1",
                     revision=revision)
        build += windows_section("Qt", revision) + windows_section("Toolchains", revision)
    elif PLATFORM == "macos-arm64":
        media = tree(*MACOS_MEDIA_RECIPE, revision=revision)
        build = tree(*MACOS_BUILD_RECIPE, revision=revision)
    else:
        media = tree("scripts/ffmpeg", revision=revision)
        build = tree("scripts/build", revision=revision)
    return digest(media), digest(build)


def legacy_cache_keys(recipe, build_recipe, image):
    # Migrate compatible caches from the two pre-isolation recipes.
    # Compare actual dependency/build inputs before admitting a legacy key.
    media_keys, build_keys = [], []
    for revision in ("a269c7700a6b7832e34a13fb148b2cbf6cd49840",
                     "24e2ebc42ae8bf29c838335ebaa639a009f97a5a"):
        exists = subprocess.run(["git", "cat-file", "-e", revision],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if exists.returncode:
            fetched = subprocess.run(["git", "fetch", "--no-tags", "--depth=1", "origin", revision],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if fetched.returncode:
                continue
        old_media, old_build = cache_inputs(revision)
        if old_media != recipe:
            continue
        old_recipe = digest(tree("scripts/ffmpeg", "scripts/build/windows-toolchain.psd1",
                                 revision=revision))
        media_key = f"platform-v1-media-{PLATFORM}-{os.environ['CI_MEDIA_VERSION']}-{old_recipe}"
        if media_key not in media_keys:
            media_keys.append(media_key)
        if old_build != build_recipe:
            continue
        paths = (("scripts/build/build-win.ps1", "scripts/build/package-win.ps1",
                  "scripts/build/provision-qt.ps1", "scripts/build/windows-toolchain.psd1")
                 if PLATFORM.startswith("windows-") else MACOS_BUILD_RECIPE)
        old_toolchain = digest((old_recipe + digest(tree(*paths, revision=revision)) + image).encode())
        build_key = f"platform-v1-build-{PLATFORM}-{old_toolchain}-"
        if build_key not in build_keys:
            build_keys.append(build_key)
    return "\n".join(media_keys), "\n".join(build_keys)


def inputs():
    excluded = SOURCE_EXCLUDE
    if PLATFORM.startswith("windows-"):
        excluded += MACOS_BUILD_RECIPE + MACOS_MEDIA_RECIPE
        if PLATFORM == "windows-arm64":
            excluded += ("scripts/ffmpeg/trim",)
    elif PLATFORM == "macos-arm64":
        excluded += WINDOWS_BUILD_RECIPE + WINDOWS_MEDIA_RECIPE + ("scripts/build/package-win.ps1",)
    source = digest(tree("CMakeLists.txt", "CMakePresets.json", "cmake", "src",
                         "resources", "assets", "translations", "templates",
                         "third_party", "scripts", "licenses", "LICENSE",
                         "LICENSE_SCOPE.md", "THIRD_PARTY_NOTICES.md",
                         ".github/workflows/package.yml", ".gitmodules", ".gitattributes",
                         exclude=excluded))
    recipe, build_recipe = cache_inputs()
    image = os.environ.get("ImageOS", "") + os.environ.get("ImageVersion", "")
    toolchain = digest((recipe + build_recipe + image).encode())
    media_restore, build_restore = legacy_cache_keys(recipe, build_recipe, image)
    output(source=source, recipe=recipe, toolchain=toolchain,
           media_restore=media_restore, build_restore=build_restore)


def archive():
    archives = list(DIST.glob("*.7z"))
    if len(archives) != 1:
        raise RuntimeError(f"Expected one archive, found {archives}")
    return archives[0]


def execute(*args):
    subprocess.run([str(arg) for arg in args], check=True, timeout=120)


def verify_caches():
    prefix = "platform-v1-"
    expected = {
        f"{prefix}qt-macos-arm64-6.11.1-quick3d",
        f"{prefix}media-macos-arm64-ffmpeg8.1.2-{os.environ['CI_RECIPE']}",
        f"{prefix}compiler-macos-arm64-{os.environ['CI_TOOLCHAIN']}-{os.environ['CI_SOURCE']}",
        f"{prefix}build-macos-arm64-{os.environ['CI_TOOLCHAIN']}-{os.environ['CI_SOURCE']}",
    }
    page = 1
    found = set()
    while True:
        query = urllib.parse.urlencode(dict(ref=os.environ["GITHUB_REF"], key=prefix,
                                           per_page=100, page=page))
        url = (f"{os.environ['GITHUB_API_URL']}/repos/{os.environ['GITHUB_REPOSITORY']}"
               f"/actions/caches?{query}")
        request = urllib.request.Request(url, headers={
            "Authorization": f"Bearer {os.environ['GH_TOKEN']}",
            "Accept": "application/vnd.github+json",
        })
        with urllib.request.urlopen(request, timeout=30) as response:
            caches = json.load(response)["actions_caches"]
        found.update(c["key"] for c in caches if c["size_in_bytes"] > 0)
        if expected <= found or len(caches) < 100:
            break
        page += 1
    if expected - found:
        raise RuntimeError(f"Saved caches missing: {sorted(expected - found)}")


def verify():
    package = archive()
    folder = package.with_suffix("")
    checks = []
    if PLATFORM.startswith("windows-"):
        app = folder / "app"
        ffmpeg = app / "ffmpeg/ffmpeg.exe"
        machine = 0x8664 if PLATFORM == "windows-x64" else 0xAA64
        for binary in [folder / "MiaCode.exe", app / "MiaCode.exe", ffmpeg,
                       *app.glob("avcodec-*.dll")]:
            with binary.open("rb") as stream:
                stream.seek(0x3C)
                offset = struct.unpack("<I", stream.read(4))[0]
                stream.seek(offset)
                signature, actual = struct.unpack("<4sH", stream.read(6))
            if signature != b"PE\0\0" or actual != machine:
                raise RuntimeError(f"Unexpected PE architecture: {binary}")
        checks.extend(["PE 架构", "Windows 包结构与依赖检查"])
    else:
        app = folder / "MiaCode.app/Contents/MacOS"
        ffmpeg = app / "ffmpeg/ffmpeg"
        execute("lipo", app / "MiaCode", "-verify_arch", "arm64")
        checks.extend(["arm64 架构", "macOS 包依赖与部署版本检查"])
    version = subprocess.check_output([str(ffmpeg), "-version"], text=True).splitlines()[0]
    expected = "7.1" if PLATFORM == "windows-x64" else "8.1"
    if expected not in version:
        raise RuntimeError(f"Unexpected FFmpeg version: {version}")
    media = DIST / "validation/media.mp4"
    media.parent.mkdir(parents=True, exist_ok=True)
    execute(ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
            "-f", "lavfi", "-i", "color=c=black:s=64x64:r=10:d=0.3",
            "-f", "lavfi", "-i", "sine=frequency=440:duration=0.3",
            "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", media)
    execute(ffmpeg, "-hide_banner", "-loglevel", "error", "-i", media, "-f", "null", "-")
    checks.extend(["FFmpeg 版本", "H.264/AAC 编码与解码"])
    record = dict(platform=PLATFORM, source=os.environ["CI_SOURCE"], archive=package.name,
                  bytes=package.stat().st_size, sha256=digest(package.read_bytes()),
                  ffmpeg=version, checks=checks, verified_run=os.environ["GITHUB_RUN_ID"])
    (DIST / "verification.json").write_text(json.dumps(record, ensure_ascii=False, indent=2),
                                           encoding="utf-8")


def check():
    package = archive()
    record = json.loads((DIST / "verification.json").read_text(encoding="utf-8"))
    if (record["platform"] != PLATFORM or record["source"] != os.environ["CI_SOURCE"]
            or record["archive"] != package.name
            or record["sha256"] != digest(package.read_bytes())
            or record["bytes"] != package.stat().st_size):
        raise RuntimeError("Package identity mismatch")
    if PLATFORM == "windows-x64" and package.stat().st_size >= 80 * 1048576:
        raise RuntimeError("Windows x64 archive must be below 80 MiB")
    output(name=package.name)


def format_elapsed(elapsed):
    total = max(0, int(round(elapsed)))
    hours, rem = divmod(total, 3600)
    minutes, seconds = divmod(rem, 60)
    if hours:
        return f"{hours} 小时 {minutes} 分"
    if minutes and seconds:
        return f"{minutes} 分 {seconds:02d} 秒"
    if minutes:
        return f"{minutes} 分钟"
    return f"{seconds} 秒"


def format_trigger():
    raw = os.environ.get("CI_TRIGGERED_AT", "").strip()
    if raw:
        moment = datetime.fromisoformat(raw.replace("Z", "+00:00"))
    else:
        moment = datetime.fromtimestamp(int(os.environ["CI_STARTED_AT"]), tz=timezone.utc)
    local = moment.astimezone(timezone(timedelta(hours=8))).strftime("%Y-%m-%d %H:%M")
    event = {"push": "推送", "workflow_dispatch": "手动"}.get(os.environ.get("CI_EVENT", ""))
    return f"{local}（{event}）" if event else local


def report():
    record = json.loads((DIST / "verification.json").read_text(encoding="utf-8"))
    elapsed = time.time() - int(os.environ["CI_STARTED_AT"])
    summary("\n".join((
        f"耗时 {format_elapsed(elapsed)}",
        f"大小 {record['bytes'] / 1048576:.2f} MiB",
        f"触发 {format_trigger()}",
    )))
    if os.environ.get("CI_PACKAGE_HIT") == "true" and elapsed >= 60:
        raise RuntimeError(f"Cached package delivery exceeded 60 seconds: {elapsed:.1f}s")


if __name__ == "__main__":
    commands = {"inputs": inputs, "verify": verify, "check": check, "report": report,
                "verify-caches": verify_caches}
    commands[sys.argv[1]]()
