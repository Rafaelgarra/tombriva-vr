/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Creates a modal container, if it doesn't exist, and adds the provided
// content element to it
function showModalContainer(content) {
  var container = document.getElementById("eModalContainer");
  if (container == null) {
    container = document.createElement("div");
    container.id = "eModalContainer";
    container.classList.add("modal_container");

    var mask = document.createElement("div");
    mask.id = "eModalMask";
    mask.classList.add("modal_mask");

    document.body.appendChild(mask);
    document.body.appendChild(container);
  } else {
    container.hidden = false;
    document.getElementById("eModalMask").hidden = false;
  }

  container.appendChild(content);
  if (content.classList.contains("modal_hide")) {
    content.classList.replace("modal_hide", "modal_content");
  } else {
    content.classList.add("modal_content");
  }
}

// Hides the modal container, and returns the contents back to the caller.
// The caller can choose to use the return value to move the contents to
// another part of the DOM, or ignore the return value so that the nodes
// can be garbage collected.
function clearModalContainer() {
  var container = document.getElementById("eModalContainer");
  container.hidden = true;
  document.getElementById("eModalMask").hidden = true;

  var content = container.firstElementChild;
  container.removeChild(content);
  content.classList.replace("modal_content", "modal_hide");

  return content;
}

//
// FxRMediaStub - Media Projection Interface & Logging Stub
// Bridge between Firefox Reality UI and C++ Projection Subsystem.
// Approved vocabulary: "2d", "360", "360-stereo", "3d", "180-sbs", "180-tb", "exit"
//

const FxRMediaStub = {
  _currentMode: "2d",

  get currentMode() {
    return this._currentMode;
  },

  setProjectionMode(aMode) {
    const validModes = [
      "2d",
      "360",
      "360-stereo",
      "3d",
      "3d-sbs",
      "180-sbs",
      "180-tb",
      "curved",
      "exit",
    ];
    let mode = typeof aMode === "string" ? aMode.toLowerCase().trim() : "2d";
    if (mode === "3d-sbs") {
      mode = "3d";
    }

    if (!validModes.includes(mode)) {
      console.warn(
        `[FxRMediaStub] Unrecognized projection mode: "${aMode}", defaulting to "2d"`
      );
      mode = "2d";
    }

    if (mode !== "exit") {
      this._currentMode = mode;
    }

    const timestamp = new Date().toISOString();
    console.log(
      `[FxRMediaStub ${timestamp}] setProjectionMode("${mode}") invoked.`
    );

    // Call native C++ implementation if present in ChromeUtils:
    if (
      typeof ChromeUtils !== "undefined" &&
      typeof ChromeUtils.setFxrProjectionMode === "function"
    ) {
      try {
        ChromeUtils.setFxrProjectionMode(mode);
        console.log(
          `[FxRMediaStub] Dispatched to ChromeUtils.setFxrProjectionMode("${mode}")`
        );
      } catch (err) {
        console.error(
          `[FxRMediaStub] Exception calling ChromeUtils.setFxrProjectionMode:`,
          err
        );
      }
    } else {
      console.log(
        `[FxRMediaStub] ChromeUtils.setFxrProjectionMode not yet implemented in C++ (stub logging mode)`
      );
    }

    // Notify UI listeners of projection change
    try {
      if (typeof window !== "undefined" && window.dispatchEvent) {
        window.dispatchEvent(
          new CustomEvent("fxr-projection-changed", { detail: { mode } })
        );
      }
    } catch (e) {
      // ignore
    }

    return mode;
  },
};

globalThis.FxRMediaStub = FxRMediaStub;
