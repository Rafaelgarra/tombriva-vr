# Licensing and distribution review

Date: 2026-09-20. Scope: source publication of Tombriva VR, not a signed binary release or Steam Store listing.

## Source code

The fork retains the upstream `LICENSE`, `toolkit/content/license.html`, component license files and copyright notices. Changes to MPL-covered files remain under MPL 2.0. New Tombriva code uses MPL 2.0 unless a file explicitly states otherwise. No claim is made that every dependency is MPL: individual component notices govern.

The complete upstream license collection is preserved in the source tree and packaged internal page. Navigation to `about:license` failed in the embedded FxR panel test; the panel instead links to the public README license section. The README includes an index of the entries in that collection; conditional components do not necessarily ship in a Windows build. Release-specific third-party notices must be checked against the actual packaged binaries, not inferred from this source inventory.

OpenVR's BSD-style three-clause license permits source and binary redistribution while retaining its notices, disclaimer, and non-endorsement condition. SteamVR and the PlayStation VR2 App are separately installed dependencies; their proprietary runtimes/installers are not included here. No codecs, downloaded videos, user profiles, Steam credentials or platform installers are part of the Tombriva source import.

## Marks and binary distribution

Tombriva VR is an independent project based on Firefox/Gecko with adaptations from Firefox Reality. Mozilla, Valve, Sony and Google do not endorse it. Attribution and upstream source identifiers must remain; product branding is a separate issue from code licensing.

A public binary release still requires completing branding across executable metadata, installer, settings and update configuration. Publishing this fork does not certify the existing development executable as distribution-ready. Use of modified Firefox binaries under Mozilla marks requires satisfying Mozilla's distribution policy or authorization. Do not ship this development build as an official Firefox release.

## Artwork

The current cat logo and the historical logo are covered separately by the maintainer's CC0 dedication; see `tombriva/assets/LICENSE.md`. The current cat design uses a user-provided cat photo as reference. No source photo is published. This is not a trademark clearance.

The original controller tutorial images were generated from a third-party photograph at heypoorplayer.com. No redistribution license for the source photograph was established. Those images, and the derived transparent sprite, are excluded from the public source import. The public tutorial uses a separately constructed generic trigger diagram, without tracing the supplied photograph.

## Behavior and services

The start page defaults to `https://www.google.com/`, configurable through `vulpis.startup.homepage`, rather than a local test server or an assumed legally required landing page. Native `about:home` remained blank in the FxR embedded browser test and was not retained. No homepage requirement was identified in the reviewed MPL or Mozilla distribution policy.

YouTube playback uses media available to the loaded page; this review does not grant rights to redistribute videos or waive service terms. Service/API keys, remote experiments, telemetry defaults, updater endpoints and redistributed codec/runtime dependencies require a separate release audit. There is no claim of blanket legal clearance for every possible use or future binary package.

## Primary references

- [Mozilla MPL 2.0](https://www.mozilla.org/en-US/MPL/2.0/), especially source, executable and notice obligations in section 3.
- [Mozilla MPL FAQ](https://www.mozilla.org/en-US/MPL/2.0/FAQ/), questions 9 and 10.
- [Mozilla distribution policy](https://www.mozilla.org/en-US/foundation/trademarks/distribution-policy/).
- [Mozilla trademark policy](https://www.mozilla.org/en-US/foundation/trademarks/policy/).
- [OpenVR license](https://github.com/ValveSoftware/openvr/blob/master/LICENSE); the vendored copy in this fork governs that copy.

The upstream error-page illustration `toolkit/themes/shared/illustrations/no-connection.svg` has an MPL 2.0 header. It is preserved with that notice; this is not a grant to brand a modified binary as Firefox/Nightly.

## Review result for this source-only submission

Reviewed on 2026-09-20: 51 modified source files and 20 added source/assets files. The original bytes of all 51 modified files were matched against the official Git baseline before import. A targeted check found no removed copyright/MPL header notices in those modified files. Upstream component licenses are retained; this was not a fresh legal audit of all 468,619 upstream files.

The new code carries MPL notices or SPDX identifiers. The directory artwork notice explicitly covers the custom SVGs without headers. The media-track development record identifies three such icons as project-created and four others as copied from MPL-covered Firefox Reality assets. Logo raster data is covered by the CC0 dedication. Photo-derived tutorial images and the unused downloaded Firefox logo are excluded from the import.

No source-publication licensing blocker was identified within this reviewed scope after recording these notices and exclusions. This conclusion is limited to the source snapshot; it does not certify trademark clearance or distribution readiness of a future executable. Public source attribution to Firefox/Mozilla is retained and must not be confused with product endorsement.

**The maintainer has completed functional testing and explicitly authorized this source-only publication. No installer or executable is part of this submission.**
