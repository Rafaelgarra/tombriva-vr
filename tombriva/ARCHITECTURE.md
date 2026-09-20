# Target device and integration

Tombriva VR targets **PSVR2 on Windows PC through SteamVR**, with Sense controllers. The initial problem was practical: the maintainer could use the headset in SteamVR but did not have the desired immersive web-video workflow in the tested browser setup. SteamVR connectivity alone does not integrate a site's player, browser graphics pipeline, projection interpretation and controller UI.

Sony documents Steam, SteamVR and the PlayStation VR2 App as PC dependencies. This is not evidence that every other browser is incapable of driving PSVR2, nor that the hardware intrinsically forbids web video. Tombriva addresses the integration demonstrated in this project; support beyond the tested configuration remains experimental.

## Spatial browsing

The FxR shell is rendered by Gecko/WebRender through an offscreen D3D11 surface. The OpenVR overlay owner presents it as a dashboard panel or a detached spatial window. Input is delivered back to browser UI instead of relying on a desktop mouse. The detached panel and its drag controls share positioning/lifecycle handling.

## Immersive video

The goggles controls communicate with a content-side JSWindowActor. The renderer uses the page's actual video element rather than reprojecting a screenshot of the whole browser. It selects supported mappings for delivered media, including ERP, the validated EAC layouts and supported VR180 metadata/mesh handling. Unsupported formats are not automatically equivalent to a half-sphere SBS video.

WebXR supplies the headset views and projection matrices. The adapted Gecko VR service and D3D11 texture-sharing path submit frames through OpenVR to SteamVR. The work includes restoring compatible texture selection, process-local shared-handle transport and session/overlay coordination in the modern codebase. The current production path described here is OpenVR/SteamVR; the earlier OpenXR handshake experiment does not mean the whole renderer was migrated to OpenXR.

## Controls and return

The trigger recenters the video view; the grip/squeeze action (R1 in the tested mapping) exits immersive playback. The browser panel returns according to its docked or detached state, so the user can continue controlling the page. These are application controls, distinct from the headset/runtime's own system recenter command.

The tutorial illustrates the front trigger and side grip using original generic drawings. It follows the browser's application locale through Fluent, and its dismissal is stored once per profile.

## Distribution boundary

No Sony driver, headset firmware, SteamVR runtime, downloaded media or third-party photo-derived tutorial asset is bundled in the source import. This is a browser fork, not yet an extension that enables arbitrary browsers. An installer/update channel and broad hardware compatibility need separate implementation and validation.

Reference: [Sony's PSVR2 PC preparation guide](https://www.playstation.com/en-us/support/hardware/pc-prepare-ps-vr2/).
