# Tombriva VR identity change

Approved by the maintainer on 2026-09-20. The public project name is now **Tombriva VR**, with a gray-and-white cat inspired by Tom wearing a cyan/violet VR visor. PNG transparency is preserved; the dashboard uses a generated 128x128 RGBA representation of the same image.

Updated surfaces: repository title/description/links, README, current project documentation, browser window/settings labels, SteamVR overlay display names and dashboard artwork. The initial SOURCE_MANIFEST.json remains the immutable record of the original source import; its old paths/hashes describe that earlier snapshot, not this branding commit.

Compatibility: `firefox.reality.*` and `mozilla.firefoxreality` app/overlay keys, `vulpis.startup.homepage`, the onboarding-seen preference and internal CSS/Fluent/resource identifiers are retained. These preserve profiles, dashboard registrations and tutorial state. Internal identifiers are not user-facing product names. The local checkout folder and historical branch may also retain the old name.

The name had no exact occurrences found in the preliminary web searches performed before selection. No comprehensive trademark-register search or legal clearance was obtained. AI generation and CC0 do not eliminate third-party rights.

The project remains a source-only preview. Complete upstream executable/installer branding is still a separate release task; this change does not claim that every inherited Firefox/Nightly surface is rebranded.

## Validation

The existing Windows development build passed `mach build binaries` and `mach build faster`. The first link attempt was blocked by the running executable; after requesting normal window closure, the retry succeeded with zero compiler warnings. Gecko chrome automation verified the Tombriva window title, packaged 512px PNG decoding, English/Portuguese onboarding, Google homepage and persistent dismissal. This is not a new immersive headset test. Media/input code paths were not changed.
