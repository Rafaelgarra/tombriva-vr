/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#pragma once
#include <cstdint>
#include <windows.h>
#include <vector>
#include "mozilla/Atomics.h"
#include "mozilla/TimeStamp.h"
#include "openvr.h"
#include "FxROverlayInputEvent.h"
#include "nsCOMPtr.h"
#include "nsITimer.h"

class nsPIDOMWindowOuter;
class nsWindow;

// How the overlay maps the browser texture into the headset. The values that
// map to a native OpenVR overlay flag are the ones the legacy Firefox Reality
// supported; the 180 entries did not exist there beyond the enum, because
// OpenVR has no 180 flag (see SPIKE3_MEDIA_VR180.md and ADR-27).
enum class FxRProjectionMode : uint32_t {
  Mode2D = 0,         // flat rectangle, the normal browser panel
  Mode360 = 1,        // 360 mono      -> VROverlayFlags_Panorama
  Mode360Stereo = 2,  // 360 stereo    -> VROverlayFlags_StereoPanorama
  Mode3D = 3,         // 3D side-by-side -> VROverlayFlags_SideBySide_Parallel
  Mode180SBS = 4,     // VR180 left/right  -- no native OpenVR flag
  Mode180TB = 5,      // VR180 top/bottom  -- no native OpenVR flag
  ModeCurved = 6,     // tela curva grande, sem reprojetar a imagem
};

// FxRWindowManager is a singleton that is responsible for tracking all of
// the top-level windows created for Firefox Reality on Desktop. Only a
// single window is initially supported.
//
// It also owns the UI-thread half of the OpenVR overlay input path. Overlay
// events are polled on the Renderer thread (see FxROutputHandler::UpdateOutput)
// and handed here via OnOverlayInputEvent; that queues them and pokes the
// widget with MOZ_WM_OPENVR_EVENT. nsWindow::ProcessMessageInternal then calls
// ProcessOverlayEvents on the UI thread, which converts them into real Gecko
// widget events via nsWindow::DispatchMouseEvent and friends.
//
// Note what this deliberately does NOT do: it never moves the Windows desktop
// cursor and never fakes WM_MOUSEMOVE/WM_LBUTTONDOWN messages. Modern Gecko
// drops synthetic pointer messages that don't match a real OS pointer state, so
// those never produce :hover or a real click.
class FxRWindowManager final {
 public:
  static FxRWindowManager* GetInstance();
  ~FxRWindowManager();

  bool VRinit();
  bool CreateOverlayForWindow();
  void DestroyOverlay();
  vr::VROverlayHandle_t GetOverlayHandle() const { return mOverlayHandle; }

  void AddWindow(nsPIDOMWindowOuter* aWindow);
  bool IsFxRWindow(uint64_t aOuterWindowID);
  bool IsFxRWindow(const nsWindow* aWindow) const;
  uint64_t GetWindowID() const;

  // Called from the thread that polls the OpenVR overlay. Queues the event and
  // posts MOZ_WM_OPENVR_EVENT to the widget so it gets drained on the UI
  // thread.
  void OnOverlayInputEvent(const FxROverlayInputEvent& aEvent);

  // Called on the UI thread in response to MOZ_WM_OPENVR_EVENT.
  void ProcessOverlayEvents(nsWindow* aWindow);

  // A VR overlay is a continuously-presented surface, unlike a desktop window
  // which Firefox only repaints when something changes. Without this the
  // headset keeps showing whatever frame was submitted last -- measured stalls
  // of 5+ seconds while the laser was actively moving.
  void StartRefreshTimer();
  void StopRefreshTimer();

  // Called from OSKVRManager when an editable field gains/loses focus, to raise
  // or dismiss the SteamVR virtual keyboard.
  void NotifyEditableFocus(bool aFocused);

  // Media projection, driven from chrome JS via
  // ChromeUtils.setFxrProjectionMode (browser/fxr/content/fxr-fullScreen.js).
  // Accepts the vocabulary agreed with the media track: "2d", "360",
  // "360-stereo", "3d"/"3d-sbs", "180-sbs", "180-tb" and "exit". Runs on the
  // parent process and hops to the GPU process, which owns the overlay
  // (ADR-02).
  void SetProjectionMode(const nsAString& aMode);
  void SetWindowDocked(bool aDocked);
  void OnWindowState(uint32_t aState);

 private:
  FxRWindowManager();

  void HandleMouseEvent(nsWindow* aWindow, const FxROverlayInputEvent& aEvent);
  void ReleaseStuckMouseButton(nsWindow* aWindow);
  void HandleScrollEvent(nsWindow* aWindow, const FxROverlayInputEvent& aEvent,
                         bool& aHasScrolled);
  void HandleKeyboardEvent(nsWindow* aWindow,
                           const FxROverlayInputEvent& aEvent);

  // Only a single window is supported for tracking. Support for multiple
  // windows will require a data structure to collect windows as they are
  // created.
  nsPIDOMWindowOuter* mWindow;
  bool mDesktopWindowVisible = true;
  bool mWindowRuntimeReady = false;

  // The overlay itself is created and owned by FxROutputHandler, on whichever
  // process hosts the compositor; this is only kept for diagnostics.
  vr::VROverlayHandle_t mOverlayHandle;

  // Cached on the main thread in CreateOverlayForWindow. The overlay polling
  // thread must not touch XPCOM to resolve the widget, so it posts
  // MOZ_WM_OPENVR_EVENT to this HWND directly.
  mozilla::Atomic<uintptr_t> mHwndWidget;

  // Guards mEventQueue, which is filled on the polling thread and drained on
  // the UI thread.
  CRITICAL_SECTION mEventsCS;
  std::vector<FxROverlayInputEvent> mEventQueue;

  mozilla::Atomic<bool> mVirtualKeyboardVisible;

  nsCOMPtr<nsITimer> mRefreshTimer;

  // True between a dispatched eMouseDown and its eMouseUp. If the up is ever
  // lost, Gecko keeps believing the button is held, which silently kills
  // :hover and clicking across the whole page.
  bool mMouseButtonDown = false;
  mozilla::TimeStamp mMouseDownTime;

  // Last pointer position in client coordinates, kept so that scroll events
  // (which carry no position of their own) land where the laser is pointing.
  POINT mLastMousePt;
};
