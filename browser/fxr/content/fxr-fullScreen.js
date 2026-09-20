/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// fxr-fullScreen.js is a provisional, stripped-down clone of
//   browser\base\content\browser-fullScreenAndPointerLock.js
// that is adapted for Firefox Reality on Desktop.

// Defensive fallback definition in case common.js was not in scope
if (typeof globalThis.FxRMediaStub === "undefined") {
  globalThis.FxRMediaStub = {
    _currentMode: "2d",
    get currentMode() {
      return this._currentMode;
    },
    setProjectionMode(aMode) {
      let mode = typeof aMode === "string" ? aMode.toLowerCase().trim() : "2d";
      if (mode !== "exit") this._currentMode = mode;
      console.log(
        `[FxRMediaStub ${new Date().toISOString()}] setProjectionMode("${mode}") invoked.`
      );
      if (
        typeof ChromeUtils !== "undefined" &&
        typeof ChromeUtils.setFxrProjectionMode === "function"
      ) {
        try {
          ChromeUtils.setFxrProjectionMode(mode);
        } catch (e) {}
      }
      return mode;
    },
  };
}

var FullScreen = {
  init() {
    // Called when the Firefox window goes into fullscreen.
    addEventListener("fullscreen", this, true);

    if (window.fullScreen) {
      this.toggle();
    }
  },

  toggle() {
    var enterFS = window.fullScreen;
    if (enterFS) {
      document.documentElement.setAttribute("inFullscreen", true);
    } else {
      document.documentElement.removeAttribute("inFullscreen");
      // Reset projection mode to 2d when exiting fullscreen
      if (typeof FxRMediaStub !== "undefined") {
        FxRMediaStub.setProjectionMode("2d");
      }
    }
  },

  handleEvent(event) {
    if (event.type === "fullscreen") {
      this.toggle();
    }
  },

  enterDomFullscreen(aBrowser, aActor) {
    if (!document.fullscreenElement) {
      return;
    }

    // Inspect URL query parameters for mozVideoProjection or fxrProjection
    let currentFullscreenURI = null;
    if (document.fullscreenElement && document.fullscreenElement.currentURI) {
      currentFullscreenURI = document.fullscreenElement.currentURI;
    } else if (aBrowser && aBrowser.currentURI) {
      currentFullscreenURI = aBrowser.currentURI;
    }

    if (currentFullscreenURI && currentFullscreenURI.query) {
      let searchParams = new URLSearchParams(currentFullscreenURI.query);
      const projectionKeys = [
        "mozVideoProjection",
        "fxrProjection",
        "projection",
      ];
      let detectedParam = null;
      for (let k of projectionKeys) {
        if (searchParams.has(k)) {
          detectedParam = searchParams.get(k);
          break;
        }
      }

      if (detectedParam) {
        let mode = "2d";
        switch (detectedParam.toLowerCase().trim()) {
          case "360_auto":
          case "360":
            mode = "360";
            break;
          case "360s_auto":
          case "360s":
          case "360-stereo":
            mode = "360-stereo";
            break;
          case "3d_auto":
          case "3d":
          case "3d-sbs":
            mode = "3d";
            break;
          case "180_auto":
          case "180":
          case "180s_auto":
          case "180lr_auto":
          case "180-sbs":
            mode = "180-sbs";
            break;
          case "180tb_auto":
          case "180tb":
          case "180-tb":
            mode = "180-tb";
            break;
          case "curved":
          case "cinema":
            mode = "curved";
            break;
          default:
            mode = "2d";
            break;
        }

        console.log(
          `[fxr-fullScreen] Detected projection parameter: "${detectedParam}" -> mode: "${mode}"`
        );
        if (typeof FxRMediaStub !== "undefined") {
          FxRMediaStub.setProjectionMode(mode);
        }
      }
    }

    // If it is a remote browser, send a message to ask the content
    // to enter fullscreen state.
    if (aBrowser && aBrowser.isRemoteBrowser) {
      aActor.sendAsyncMessage("DOMFullscreen:Entered", {});
    }

    document.documentElement.setAttribute("inDOMFullscreen", true);
  },

  cleanupDomFullscreen(aActor) {
    if (aActor) {
      aActor.sendAsyncMessage("DOMFullscreen:CleanUp", {});
    }
    document.documentElement.removeAttribute("inDOMFullscreen");

    // Reset projection mode to 2d upon cleaning up DOM fullscreen
    if (typeof FxRMediaStub !== "undefined") {
      FxRMediaStub.setProjectionMode("2d");
    }
  },
};
