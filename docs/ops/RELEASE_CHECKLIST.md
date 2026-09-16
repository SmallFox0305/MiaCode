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

Pushing a `v*` tag runs [release.yml](../../.github/workflows/release.yml): it builds the
three platform packages, creates the Release with them attached, and refreshes the update
manifests. A tag carrying a prerelease identifier (`v2.1.0-beta.3`) publishes as a
pre-release; `v2.1.0` publishes as a full release.

- [ ] Confirm `MIACODE_VERSION_*` in CMakeLists.txt already names the version you are
      about to tag. The `tag` job checks this before the build starts, because a tag
      ahead of CMakeLists.txt packages the old version under the new name and every
      client then keeps offering itself the same update forever.
- [ ] Add the `## <version>` section to CHANGELOG.md before tagging — the manifest lifts
      its `zh_CN` release notes from that section. Other languages need `--notes`.
- [ ] Create a signed tag if signing is part of the release process.
- [ ] Push the tag, then watch the run: three `.7z` assets uploaded, and the manifest
      assets refreshed (or deliberately not — see below).
- [ ] Add platform support, known issues, non-commercial positioning, and license notes
      to the release body. `--generate-notes` only writes the commit list.

### Rehearsing without publishing

Push whatever you want to test to the `ci/release-rehearsal` branch:

```bash
git push --force-with-lease origin HEAD:ci/release-rehearsal
```

It builds the three packages and generates the manifests exactly as a tag push would —
same `release` channel, so the archive names and hashes are the ones a release would
carry — then prints them into the log and uploads them as the `rehearsed-manifests` run
artifact. Nothing is created: no Release, no tag, no manifest upload. **Only a tag
publishes**; a branch push never does, whatever it is named.

It rehearses `v` + the version in CMakeLists.txt, which is the only tag the guard would
accept for that commit anyway.

Once this workflow reaches the default branch, Actions → Release → Run workflow offers
the same thing with an explicit tag. It is a branch push rather than that because GitHub
only offers `workflow_dispatch` for workflows already on the default branch, and the
rehearsal has to work from the branch that changes the workflow.

The rehearsal also warms the package cache under the release key, so a real tag push at
the same commit skips the build.

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

### How it is produced

[release.yml](../../.github/workflows/release.yml) generates the manifests with
[scripts/release/update-manifest.py](../../scripts/release/update-manifest.py). Size and
hash are computed from the archives the run is about to publish, so no separate record
can disagree with the assets.

**A channel only ever moves forward**, because a client has no way to downgrade:

| Tag | stable | beta |
| --- | --- | --- |
| Release, newer than both | refreshed | refreshed |
| Release, older than the newest beta | refreshed | left alone |
| Prerelease, newer than both | left alone | refreshed |
| Anything not newer than what a channel publishes | left alone | left alone |

So `beta` always holds the higher of (stable, beta), and a beta user lands on a release
as soon as the release outranks the newest beta. A run that moves no channel says
`no channel moved forward` and publishes nothing — that is a success, not a failure.

The release job fails rather than publishing a manifest when a platform has no package
(pass `--allow-partial` if a one-platform release is intended), when an archive name does
not match the tagged version, or when the manifest would exceed the 64 KiB the client
accepts.

### Refreshing it by hand

Only needed when a release was published outside the workflow, or when a manifest has to
be corrected without a new tag:

```bash
python3 scripts/release/update-manifest.py generate \
  --tag v2.1.0-beta.3 --packages dist --out manifests \
  --released-at "$(date -u +%F)" --changelog CHANGELOG.md \
  --current-stable-version 2.0.0 --current-beta-version 2.1.0-beta.2
gh release upload channel-manifest manifests/*.json --clobber
```

`python3 scripts/release/update-manifest.py selfcheck` asserts the version rules against
the same cases as `src/tools/update/UpdateVersionSpec.cpp`; run it after touching either
side, since the C++ and Python comparisons are separate implementations.
