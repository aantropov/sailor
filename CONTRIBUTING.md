# Contributing to Sailor

[Project overview](README.md) · [Build and run](BUILDING.md)

Bug reports, feature discussions and pull requests are welcome. For runtime bugs,
include the engine version, platform, GPU and reproduction steps. The repository's
[agent and coding rules](AGENTS.md) describe its implementation and review conventions.

## Continuous integration

[![CI / Windows](https://github.com/aantropov/sailor/actions/workflows/ci-windows.yml/badge.svg)](https://github.com/aantropov/sailor/actions/workflows/ci-windows.yml)
[![CI / Linux](https://github.com/aantropov/sailor/actions/workflows/ci-linux.yml/badge.svg)](https://github.com/aantropov/sailor/actions/workflows/ci-linux.yml)
[![Release / Desktop](https://github.com/aantropov/sailor/actions/workflows/release-desktop.yml/badge.svg)](https://github.com/aantropov/sailor/actions/workflows/release-desktop.yml)


**CI / Windows** and **CI / Linux** select independent engine/SDK and editor/tools
jobs from the complete pull-request diff. Native C++, shaders, content and native
tests run engine/SDK checks. Editor UI and tool changes run managed builds and
editor contract tests. Protocol, interop, workspace/export services, version and
CI configuration changes run both components. Documentation-only changes skip
the component builds; manual CI runs always select both.

The shared routing rules are in `.github/ci-paths.json`. Unclassified inputs run
both components so new build dependencies retain coverage. Engine jobs include
headless editor contracts and native SDK integration without building the MAUI
or GTK4 editor application. **Release / Desktop** builds and validates complete
packages for all three platforms before publication.

## Preparing a desktop release

`VERSION` is the shared `major.minor.patch` version for the engine, editor, SDK,
application metadata and archives. Change it in a reviewed commit and merge it to
`main` before publishing a new version.

Open **Actions → Release / Desktop**, select `main`, and choose an **Action**:

| Action | Result |
| --- | --- |
| **Build artifacts** (default) | Build archives for review without publishing. |
| **Publish prerelease** | Publish a GitHub Release marked as a prerelease. |
| **Publish release** | Publish a regular GitHub Release. |

Both publication actions require the repository owner. The workflow builds and
tests Windows x64, macOS arm64 and Linux x64, verifies the extracted packages and
SDK consumers, then creates `v<VERSION>` at the tested commit. All three platforms
must succeed. Archives and SHA-256 checksums are attached to the release, and
existing tags/releases are preserved.

Pull requests that change release tooling validate packages without publishing.
Pushes and tags do not publish releases automatically. macOS signing limitations
and first-launch instructions are in the [installation guide](BUILDING.md#macos-first-launch).
