# Before a binary release

The source preview is not an approved installer. Complete these items before tagging downloadable binaries:

1. Build from the published checkout on a clean Windows machine using documented toolchains and a dedicated clean profile; verify no local absolute path is required by launch/dashboard helpers.
2. Complete Tombriva branding in executable metadata, installer, icons, native about/settings UI and localized product names. Preserve copyright/attribution notices and upstream technical identifiers. Upstream MPL illustrations are not automatically unlicensed; review their use separately from product marks.
3. Recheck `about:license` and notices against the actual binaries and bundled dependencies; include corresponding source/tag and build configuration. Do not bundle SteamVR, PlayStation installers, user profiles, downloaded media or external service credentials.
4. Review permission-testing preferences, sandbox settings, telemetry/experiments, service keys and privacy/settings links. Define a product permission flow rather than relying on development auto-allow behavior.
5. Finish Fluent localization of older custom shell labels and ship matching browser language resources. The onboarding currently supports English and Brazilian Portuguese through native locale selection; it does not install language packs.
6. Test dashboard launch, detach/reattach, desktop visibility, controller pointing, video entry, recentering, R1 exit, close/reopen, quality switching, prolonged sessions and runtime restart. Confirm 360 and supported VR180 sources across supported GPUs. Keep a known-good build for rollback.
7. Provide a relocatable launcher, SteamVR registration/unregistration, and clear dependency instructions. Source publication alone does not register an installed app for users.
8. Keep stock Mozilla updates disabled until a separately signed Tombriva update channel is implemented and tested. Each update needs a corresponding source tag, hashes, verification, staged rollout and recovery behavior.
9. Add maintainer screenshots with appropriate rights and no private account data. Do not imply affiliation or support beyond tested devices/sites.

## V2: shared controller input

Investigate SteamVR Home locomotion alongside an interactive detached browser without click-through. Keep current behavior for the first source submission. See [input coexistence study](INPUT_COEXISTENCE_V2.md).
