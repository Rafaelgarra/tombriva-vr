# Changelog

All notable changes to Tombriva VR are recorded here. Versions are source-preview milestones on top of the Firefox ESR baseline; they are not Firefox version numbers.

## [0.1.0-preview] - 2026-09-21

First public source preview. Source only: no installer, executable or signed update channel.

### Added

- SteamVR browser panel for PSVR2 on Windows: dashboard panel and detached ("free") window with drag controls, rendered offscreen by Gecko/WebRender and presented through an OpenVR overlay.
- Immersive playback of compatible 360° and VR180 web video using the page's own media element and a dedicated WebXR renderer (ERP, validated EAC layouts, supported VR180 metadata/mesh handling).
- PSVR2 Sense controller input: panel pointing, trigger to recenter the video, R1/grip to leave immersive playback and return to the docked or detached panel.
- Repaired browser-to-VR paths in the modern Gecko codebase: D3D11 shared-texture transport, WebXR session and OpenVR submission, GPU/VR process coordination.
- One-time onboarding bubble in English and Brazilian Portuguese (Fluent), with a generic animated trigger diagram and reduced-motion support.
- Configurable start page (`vulpis.startup.homepage`, default Google) replacing the old localhost test harness.
- Publication records: source import manifest with SHA-256 per imported file, licensing review, build notes, architecture notes and branding manifest.

### Changed

- Project renamed from Vulpis VR to **Tombriva VR** with a new CC0 cat logo. Internal preference, app and overlay keys (`vulpis.*`, `firefox.reality.*`, `mozilla.firefoxreality`) are kept so existing profiles and SteamVR registrations keep working.
- README now credits Mozilla and Firefox Reality contributors and states project independence explicitly.

### Known limitations

- SteamVR Home analog-stick locomotion is unavailable while the browser is open as an interactive detached window. Planned for V2 investigation; see [INPUT_COEXISTENCE_V2.md](INPUT_COEXISTENCE_V2.md).
- Tested only on the maintainer's PSVR2 setup. Not every YouTube video, stereo layout, fisheye or mesh format is supported.
- This checkout has not yet been rebuilt on a clean second PC.
- Inherited executable/installer branding (`firefox.exe`, native about/settings surfaces) is not yet replaced; see [RELEASE_TODO.md](RELEASE_TODO.md).
- Older custom shell labels are not fully localized; no language packs are installed.

[0.1.0-preview]: https://github.com/Rafaelgarra/tombriva-vr/releases/tag/tombriva-v0.1.0-preview
