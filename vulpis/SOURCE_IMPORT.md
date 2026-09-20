# Source origin and import record

Prepared 2026-09-20. Status: source-only publication approved by the maintainer after functional testing on 2026-09-20. No installer or executable is included.

## Baseline and lineage

- Actual base repository: https://github.com/mozilla-firefox/firefox
- Release tag: `FIREFOX_153_2_0esr_RELEASE`
- Git commit: `feec67e62a5148b41fd017ccbbc463e8a6f9e83d`
- Source archive: `firefox-153.2.0esr.source.tar.xz` (807705672 bytes)
- Archive SHA-256: `c443c99704aadaf60fb267d709c42bc283e49e82292084f976369c8327ad833c`
- Source Mercurial stamp: `92c5bf513a3e4e39fa70df2df6a565a1049a9920`
- Legacy reference: https://github.com/MozillaReality/gecko-dev, inspected checkout `f0d4809d080a45e464f9664ed501e7c82e06a041`.
- Destination fork: https://github.com/Rafaelgarra/vulpis-vr ; local development branch `vulpis-vr`.

Development occurred outside Git. The publication checkout starts at the official tag, preserving that lineage; it does not fabricate individual historical commits for earlier experiments. Firefox Reality informed the adaptations but is not misrepresented as the modern engine baseline.

## Import scope and exclusions

The completed archive comparison examined 468,619 base files and found 51 modified files, one missing local file and 1,723 new local files. Only 51 modified and 20 selected new source/assets files were imported. [SOURCE_MANIFEST.json](SOURCE_MANIFEST.json) records each of these 71 paths and its SHA-256. Documentation and licensing notices added for publication are separate from those 71 files.

For each modified file, the original archive hash was verified against the corresponding official Git blob before the local version was copied. The 20 new paths were absent from that baseline. This proves snapshot correspondence, not correctness of every change or a clean rebuild.

Excluded: Python caches, object directories, runtime logs, personal profiles, an actor backup, an unused downloaded Firefox logo, and tutorial images derived from a third-party photograph. The isolated deletion of `dom/base/crashtests/607222.html` was not imported; the upstream test remains intact. No downloaded videos or proprietary Steam/Sony installers are included.

## Changes by subsystem

- `gfx/vr`, `gfx/ipc`, `gfx/webrender_bindings`: OpenVR overlay, dashboard/free-panel lifecycle, offscreen rendering, controller input, shared-texture and process/session integration.
- `widget/windows`: native window/input/compositor integration supporting the VR panel.
- `dom/vr`, `dom/canvas`, WebIDL and ChromeUtils: WebXR session and media integration, including browser-to-VR frame delivery.
- `browser/actors`: privileged page-media bridge and supported 360/VR180 rendering paths.
- `browser/fxr`: browser controls, projection UI, settings, branding assets, Fluent onboarding and a configurable Google homepage replacing the test-server startup URL.
- Build manifests register the FxR and actor resources. See [build notes](BUILD.md) for the distinction between the existing development build and a fresh build of this checkout.

## Validation and limits

Previous headset observations cover spatial browsing and compatible immersive playback. The latest onboarding/homepage changes were packaged into the existing development build and checked with Gecko chrome automation: English and Portuguese text, both controller diagrams, dismissal persistence, layout and Google navigation. The maintainer subsequently confirmed that the build works, reporting the detached-panel locomotion limitation documented in INPUT_COEXISTENCE_V2.md. There has been no independent clean build of this publication checkout.

No runtime code changed during this documentation/licensing finalization. The selected source hashes were checked again against the import manifest. Review [LICENSING_REVIEW.md](../LICENSING_REVIEW.md) for MPL, component notices, CC0 artwork and source-only scope.

## Publication approval

The maintainer tested the functionality, reported that it works, accepted deferring controller-input coexistence to V2, and explicitly authorized uploading the source. Installer packaging and binary distribution are outside this first submission.
