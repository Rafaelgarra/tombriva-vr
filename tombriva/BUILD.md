# Building Tombriva VR from source

This is a development source fork, not an installer release. The code baseline is the official `FIREFOX_153_2_0esr_RELEASE` tag (`feec67e62a5148b41fd017ccbbc463e8a6f9e83d`). The repository preserves upstream history through the GitHub fork network.

## Windows x64

Use a short checkout path and enable Git's long-path support before cloning. Follow the build prerequisites for the checked-out Gecko revision, including MozillaBuild, Visual Studio C++/Windows SDK and the toolchains selected by `mach bootstrap`. Versions must match the revision; copying an arbitrary current compiler is not a reproducible setup.

The original development configuration used a separate object directory and these options:

```sh
mk_add_options MOZ_OBJDIR=@TOPSRCDIR@/obj-tombriva
ac_add_options --enable-application=browser
ac_add_options --target=x86_64-pc-windows-msvc
ac_add_options --disable-tests
ac_add_options --disable-updater
ac_add_options --disable-crashreporter
ac_add_options --enable-fxr-desktop
```

Save as `.mozconfig`, then run `./mach build` from the configured MozillaBuild environment. `./mach build faster` packages frontend-only edits after a complete build; it cannot replace a native/WebIDL/IPDL rebuild. Keep one build process per object directory.

The build output contains `dist/bin/firefox.exe`; the inherited executable filename is not a claim of official Mozilla affiliation. Full executable/installer branding remains a release task. Do not enable official branding to ship a modified Firefox binary.

## VR development launch

SteamVR and the PlayStation VR2 PC setup must already work. Launch the built executable with `--fxr -no-remote -profile <dedicated-profile>`; keep personal browsing profiles separate. There is no dependency on the old `127.0.0.1:8765` test server for the browser homepage.

The tested VR configuration also uses preferences for OpenVR/WebXR and offscreen rendering. Review the current development launcher and source defaults before building a distributable configuration: this source import is not a promise that a clean installation has passed the headset acceptance suite. In particular, permission-testing preferences and the GPU sandbox configuration require a product/security review before general-purpose browsing distribution.

The updater is disabled. Do not point this build at Mozilla's binary update service or automatically install a stock build over the VR modifications. Future Tombriva releases need corresponding source tags, checksums, their own signed update channel, and validation of the complete SteamVR enter/play/exit lifecycle.

## Validation status

The maintainer has tested navigation, docked/free panels, compatible 360/VR180 playback and R1 return on PSVR2. The import has not been independently rebuilt from this new checkout on another PC. UI-only updates for onboarding/homepage were packaged in the existing development build and checked using actual Gecko chrome automation. Headset confirmation of those final UI changes remains separate.

References: [Gecko source build documentation](https://firefox-source-docs.mozilla.org/setup/windows_build.html), [license review](../LICENSING_REVIEW.md).
