/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Buttons for the transport-controls overlay. Each one calls straight into
// ChromeUtils.setFxrProjectionMode, which hops to the GPU process where the
// overlay lives (ADR-02) and applies the OpenVR flags there.
//
// Differences from the legacy version:
//  - no play/pause: ChromeUtils.setFxrPlayMediaState does not exist in ESR 153;
//  - the two VR180 modes are present, which the legacy UI never offered.

window.addEventListener(
  "DOMContentLoaded",
  () => {
    setupTransportButtons();
  },
  { once: true }
);

function setupTransportButtons() {
  const buttons = {
    eExitFullScreen: "exit",
    eProjection2D: "2d",
    eProjection360: "360",
    eProjection360Stereo: "360-stereo",
    eProjection3D: "3d",
    eProjection180SBS: "180-sbs",
    eProjection180TB: "180-tb",
  };

  for (const [id, mode] of Object.entries(buttons)) {
    const elem = document.getElementById(id);
    if (!elem) {
      continue;
    }
    elem.addEventListener("click", () => {
      try {
        ChromeUtils.setFxrProjectionMode(mode);
      } catch (e) {
        // Keep the overlay usable even if one call fails.
        console.error("setFxrProjectionMode(" + mode + ") falhou: " + e);
      }
    });
  }
}
