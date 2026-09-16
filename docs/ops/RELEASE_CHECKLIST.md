# Release Checklist

This checklist documents the local and GitHub Actions release flow.

## Version

- [ ] Update `MIACODE_VERSION_MAJOR`, `MIACODE_VERSION_MINOR`, `MIACODE_VERSION_PATCH`, and `MIACODE_VERSION_PRERELEASE` in [CMakeLists.txt](../../CMakeLists.txt).
- [ ] Confirm the About dialog and package filename use the same version.
- [ ] Add release notes to [CHANGELOG.md](../../CHANGELOG.md).

## Clean Build

- [ ] Start from a clean clone or a clean working tree.
- [ ] Confirm required Qt modules are available: `Core`, `Gui`, `Widgets`, `OpenGL`, `Qml`, `Quick`, `Multimedia`.
- [ ] Confirm FFmpeg runtime/dev SDK provisioning scripts still point at the intended versions.

## Windows Package

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build\build-win.ps1 -Toolchain msvc -BuildDir build-msvc              # x64
powershell -ExecutionPolicy Bypass -File .\scripts\build\build-win.ps1 -Toolchain msvc-arm64 -BuildDir build-msvc-arm64 # arm64
```

- [ ] Confirm the toolchain data file (`scripts/build/windows-toolchain.psd1`) still
      points at the intended Qt version, FFmpeg component versions and package contents.
- [ ] Verify `dist/MiaCode_<version>_win_x64` and `dist/MiaCode_<version>_win_arm64`.
- [ ] Verify `dist/MiaCode_<version>_win_x64.7z` and `dist/MiaCode_<version>_win_arm64.7z`.
- [ ] Launch the packaged app from `dist/`, not from the build tree.
- [ ] Confirm the arm64 package starts on a machine without the VC++ runtime installed
      (the redistributable DLLs ship inside `app\`).
- [ ] Test opening a sample chart, preview playback, video background decode, SFX playback, and export.

## Checksums

Generate checksums for uploaded artifacts:

```powershell
Get-FileHash .\dist\MiaCode_*_win_x64.7z, .\dist\MiaCode_*_win_arm64.7z -Algorithm SHA256
```

```bash
shasum -a 256 dist/*.zip
```

## License And Notices

- [ ] Review [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md).
- [ ] Review [LICENSE_SCOPE.md](../../LICENSE_SCOPE.md).
- [ ] Confirm package contents match the non-commercial distribution notes.
- [ ] Confirm BASS files are present only in the intended non-commercial release package.
- [ ] Confirm FFmpeg build flags and license notes match the packaged binaries.

## Publish

- [ ] Create a signed tag if signing is part of the release process.
- [ ] Upload release zip files and checksums.
- [ ] Include platform support, known issues, non-commercial positioning, and license notes in release notes.
- [ ] Mark the release as prerelease.
- [ ] Refresh the update manifest by hand — see below. Skipping this is silent: the
      client reports a failed check, not a missing one.

## Update Manifest

The in-app update check reads one fixed URL per major version and channel:

```
https://github.com/fanfaredash/MiaCode/releases/download/channel-manifest/<major>-<channel>.json
```

`<channel>` is `stable` or `beta`. The assets hang off a permanent `channel-manifest`
release, which **must stay marked prerelease** so GitHub's "Latest" badge keeps pointing
at a real build.

The client accepts `schema` 1 and ignores anything higher. It requires `schema`, `major`
and `version` to be whole numbers / a parseable semver, a `releasePageUrl` that is valid
**https**, and a `platforms` entry for its own key that is an object carrying a non-empty
`file` and `url`. Anything else is treated as "nothing to offer", so a malformed manifest
is silently inert rather than loud.

```json
{
  "schema": 1,
  "channel": "beta",
  "major": 2,
  "version": "2.1.0-beta.3",
  "releasedAt": "2026-09-20",
  "releasePageUrl": "https://github.com/fanfaredash/MiaCode/releases/tag/v2.1.0-beta.3",
  "mandatory": false,
  "notes": { "zh_CN": "...", "en_US": "..." },
  "platforms": {
    "macos-arm64":   { "file": "...7z", "bytes": 0, "sha256": "...", "url": "https://..." },
    "windows-x64":   { "file": "...7z", "bytes": 0, "sha256": "...", "url": "https://..." },
    "windows-arm64": { "file": "...7z", "bytes": 0, "sha256": "...", "url": "https://..." }
  }
}
```

Platform keys must match `.github/workflows/package.yml`'s matrix exactly
(`macos-arm64`, `windows-x64`, `windows-arm64`); a key the client cannot match reads as
"no package for this platform".

**This step is manual today.** Generating it from the release pipeline was planned, but
that plan reads a per-platform `dist/verification.json` for each package's size, hash and
tag, and the packaging rewrite that consolidated everything into
`scripts/build/package.py` removed that artifact. `package.py artifact` now emits only the
archive filename and `package.py report` only a human-readable summary, so no step
currently produces the size and hash a manifest needs. Automating this requires deciding
where those values come from first — either a new `package.py` subcommand or computing
them from the uploaded asset.
