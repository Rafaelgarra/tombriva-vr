# Detached panel and SteamVR Home locomotion

Date: 2026-09-20. Decision: V2 investigation; no input or rendering changes in this source submission.

## Observed limitation

The maintainer confirmed that the tested build works, but SteamVR Home analog-stick locomotion is unavailable while the browser is open as an interactive detached window. This is a controller-input limitation, not a claim that physical headset tracking stops. Simultaneous room locomotion and detached-window interaction is not supported in the current tested configuration.

## Code and documentation evidence

`gfx/vr/FxROutputHandler.cpp` sets `VROverlayInputMethod_Mouse` and `VROverlayFlags_MakeOverlaysInteractiveIfVisible` on the detached panel and its drag overlay. `UpdatePanelVisibility` enables mouse input for shown panels and disables it for hidden panels. The same relevant settings are present in the active development source.

The vendored OpenVR header explicitly documents that this flag activates the system-wide laser mouse mode whenever an interactive overlay is visible. This is strong evidence for input arbitration as the explanation for the report. We did not collect SteamVR Home action/focus telemetry or perform a controlled flag-off comparison, so the exact runtime path suppressing locomotion remains unconfirmed. It should not be described merely as a deliberate per-button anti-click-through rule implemented by Vulpis.

The reviewed panel code uses automatic overlay mouse events, not an action-set mechanism selectively capturing only pointer/trigger inputs. Removing the flag alone risks losing the native laser and working overlay events; passing all input onward risks unintended scene actions during browser clicks.

## V2 feasibility

Coexistence is worth prototyping, but it is not a verified one-line option in the existing input path. The vendored OpenVR header documents experimental global action-set priorities intended to override part of a controller without taking all scene input. That version also requires an experimental SteamVR developer setting. Confirm current runtime support before relying on this for end users.

Investigate a dedicated SteamVR Input action set that binds only browser interaction sources and leaves the locomotion stick unbound. This requires an action manifest/bindings, ray intersection and cursor behavior, and careful press/release ownership. Binding priority alone does not establish that automatic system laser mode can remain active simultaneously. An alternative is explicit interaction/navigation focus switching; it must preserve a reliable way to re-engage the panel and is not equivalent to simultaneous operation.

Do not inject synthetic locomotion events into SteamVR Home or modify its private bindings as a shortcut.

## Acceptance checks before changing the default

1. Record current baseline in Home with panel hidden, docked and detached, including both Sense sticks, focus state and pointer events.
2. Navigate using the configured Home locomotion control while the detached browser remains visible.
3. Click, scroll and drag without activating objects behind the panel or duplicating trigger actions.
4. Test pointer outside the panel, both controllers, disconnect/reconnect and no stuck pressed state.
5. Preserve dashboard transitions, video entry, recentering and R1 exit for 360/VR180.
6. Verify any experimental-setting dependency; retain the existing mode if selective coexistence is unreliable.

Current workaround to try: dock the browser, close the SteamVR dashboard, move in Home, then open/detach the browser again. The complete workaround has not yet been validated by the maintainer; it is not a promised fix.

References: [Valve overlay overview](https://github.com/ValveSoftware/openvr/wiki/IVROverlay_Overview), [overlay mouse input](https://github.com/ValveSoftware/openvr/wiki/IVROverlay%3A%3ASetOverlayInputMethod), and the version-specific comments for `MakeOverlaysInteractiveIfVisible`, `VRActiveActionSet_t` and global overlay priorities in [the vendored header](../gfx/vr/service/openvr/headers/openvr.h).
