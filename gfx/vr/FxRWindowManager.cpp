/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FxRWindowManager.h"

#include "FxROutputHandler.h"
#include <dwmapi.h>
#include "mozilla/Preferences.h"
#include "WinMessages.h"
#include "WinUtils.h"
#include "mozilla/Assertions.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/MouseEvents.h"
#include "mozilla/WidgetUtils.h"
#include "mozilla/dom/MouseEventBinding.h"
#include "mozilla/dom/WheelEventBinding.h"
#include "mozilla/gfx/GPUChild.h"
#include "mozilla/gfx/GPUProcessManager.h"
#include "nsDebug.h"
#include "mozilla/Services.h"
#include "nsIObserverService.h"
#include "nsPIDOMWindow.h"
#include "nsString.h"
#include "nsThreadUtils.h"
#include "nsIWidget.h"
#include "nsWindow.h"

// windowsx.h can't be included here: its GetWindowID/GetNextSibling macros
// collide with Gecko method names. This is the only thing we needed from it.
#define FXR_POINTTOPOINTS(pt) (MAKELONG((short)((pt).x), (short)((pt).y)))

mozilla::LazyLogModule gFxrWinLog("FxRWindowManager");

#define FXR_LOG(args) MOZ_LOG(gFxrWinLog, mozilla::LogLevel::Info, args)

static mozilla::StaticAutoPtr<FxRWindowManager> sFxrWinMgrInstance;

FxRWindowManager* FxRWindowManager::GetInstance() {
  if (sFxrWinMgrInstance == nullptr) {
    sFxrWinMgrInstance = new FxRWindowManager();
    ClearOnShutdown(&sFxrWinMgrInstance);
  }

  return sFxrWinMgrInstance;
}

FxRWindowManager::FxRWindowManager()
    : mWindow(nullptr),
      mOverlayHandle(vr::k_ulOverlayHandleInvalid),
      mHwndWidget(0),
      mVirtualKeyboardVisible(false),
      mLastMousePt({0, 0}) {
  ::InitializeCriticalSection(&mEventsCS);
}

FxRWindowManager::~FxRWindowManager() {
  DestroyOverlay();
  ::DeleteCriticalSection(&mEventsCS);
}

bool FxRWindowManager::VRinit() { return true; }

bool FxRWindowManager::CreateOverlayForWindow() {
  if (!mWindow) {
    return true;
  }

  nsCOMPtr<nsIWidget> newWidget =
      mozilla::widget::WidgetUtils::DOMWindowToWidget(mWindow);
  if (!newWidget) {
    return true;
  }

  // Cache the widget's HWND while we're still on the main thread. The overlay
  // input pump runs on the Renderer thread and can't resolve it there.
  HWND hwnd = (HWND)newWidget->GetNativeData(NS_NATIVE_WINDOW);
  mHwndWidget = (uintptr_t)hwnd;

  newWidget->RequestFxrOutput();
  mozilla::Preferences::SetBool("fxr.desktop-window-visible", true);
  StartRefreshTimer();
  printf_stderr(
      "[FxR-Modern] RequestFxrOutput dispatched to window widget (hwnd=0x%p)\n",
      hwnd);

  return true;
}

void FxRWindowManager::DestroyOverlay() { StopRefreshTimer(); }

// Firefox composites only when it thinks something changed. That is right for a
// desktop window and wrong for a VR overlay: the headset needs a fresh frame at
// a steady rate, and SteamVR just keeps displaying the last texture it got.
// Measured before this existed: input flowing at 64 events/s while the image
// sat frozen for over 5 seconds at a time.
//
// Invalidating on a timer forces the paint -> transaction -> composite chain
// that ends in FxROutputHandler::UpdateOutput submitting a new texture.
void FxRWindowManager::StartRefreshTimer() {
  if (mRefreshTimer || !mWindow) {
    return;
  }

  // ~60 Hz. Cheap when nothing changed (WebRender skips unchanged scenes) and
  // enough to keep the panel feeling live.
  NS_NewTimerWithFuncCallback(
      getter_AddRefs(mRefreshTimer),
      [](nsITimer*, void*) {
        FxRWindowManager* self = FxRWindowManager::GetInstance();
        if (self == nullptr || self->mWindow == nullptr) {
          return;
        }
        nsCOMPtr<nsIWidget> widget =
            mozilla::widget::WidgetUtils::DOMWindowToWidget(self->mWindow);
        if (widget) {
          // Last-resort release: neither MouseButtonUp nor ButtonUnpress
          // arrived. A button held this long is a lost event, not a real
          // press, and leaving it down kills hover and clicking page-wide.
          {
            static uint32_t sTicks = 0;
            if (++sTicks % 120 == 0) {  // ~2 s at 16 ms
              FXR_LOG(
                  ("refresh tick: buttonDown=%d", (int)self->mMouseButtonDown));
            }
          }
          if (self->mMouseButtonDown && !self->mMouseDownTime.IsNull() &&
              (mozilla::TimeStamp::Now() - self->mMouseDownTime)
                      .ToMilliseconds() > 1500.0) {
            self->ReleaseStuckMouseButton(static_cast<nsWindow*>(widget.get()));
          }
          const bool visible =
              !self->mWindowRuntimeReady ||
              !mozilla::Preferences::GetBool("fxr.offscreen-render", false) ||
              mozilla::Preferences::GetBool("fxr.desktop-window-visible", true);
          if (visible != self->mDesktopWindowVisible) {
            const BOOL cloak = !visible;
            HWND hwnd = (HWND)widget->GetNativeData(NS_NATIVE_WINDOW);
            HRESULT hr = ::DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak,
                                                 sizeof(cloak));
            if (SUCCEEDED(hr))
              self->mDesktopWindowVisible = visible;
            else
              mozilla::Preferences::SetBool("fxr.desktop-window-visible", true);
            FXR_LOG(
                ("desktop visible=%d hr=0x%08lx", visible, (unsigned long)hr));
          }
          widget->Invalidate(widget->GetBounds());
        }
      },
      nullptr, 16, nsITimer::TYPE_REPEATING_SLACK, "FxROverlayRefresh"_ns);

  printf_stderr("[FxR-Modern] overlay refresh timer started (~60 Hz)\n");
}

void FxRWindowManager::StopRefreshTimer() {
  if (mRefreshTimer) {
    mRefreshTimer->Cancel();
    mRefreshTimer = nullptr;
  }
}

void FxRWindowManager::AddWindow(nsPIDOMWindowOuter* aWindow) {
  if (mWindow != nullptr) {
    MOZ_CRASH("Only one window is supported");
  }

  mWindow = aWindow;
}

bool FxRWindowManager::IsFxRWindow(uint64_t aOuterWindowID) {
  return mWindow != nullptr && mWindow->WindowID() == aOuterWindowID;
}

bool FxRWindowManager::IsFxRWindow(const nsWindow* aWindow) const {
  if (mWindow == nullptr) {
    return false;
  }

  nsCOMPtr<nsIWidget> widget =
      mozilla::widget::WidgetUtils::DOMWindowToWidget(mWindow);
  return widget.get() == (const nsIWidget*)aWindow;
}

uint64_t FxRWindowManager::GetWindowID() const {
  if (mWindow == nullptr) {
    return 0;
  }

  return mWindow->WindowID();
}

/* OpenVR overlay input */

// Runs on the thread that polls the OpenVR overlay (the Renderer thread, from
// FxROutputHandler::UpdateOutput). Widget events can't be dispatched from
// another thread, so queue the event here and post MOZ_WM_OPENVR_EVENT so that
// the UI thread drains the queue in ProcessOverlayEvents.
void FxRWindowManager::OnOverlayInputEvent(const FxROverlayInputEvent& aEvent) {
  HWND hwnd = (HWND)(uintptr_t)mHwndWidget;
  if (hwnd == nullptr) {
    return;
  }

  ::EnterCriticalSection(&mEventsCS);

  const bool initiallyEmpty = mEventQueue.empty();

  // Coalesce consecutive mouse moves: only the most recent position matters,
  // and the laser can produce them faster than the UI thread drains them.
  if (aEvent.mType == vr::VREvent_MouseMove && !mEventQueue.empty() &&
      mEventQueue.back().mType == vr::VREvent_MouseMove) {
    mEventQueue.back() = aEvent;
  } else {
    mEventQueue.emplace_back(aEvent);
  }

  ::LeaveCriticalSection(&mEventsCS);

  if (initiallyEmpty) {
    ::PostMessageW(hwnd, MOZ_WM_OPENVR_EVENT, 0, 0);
  }
}

// Runs on the UI thread, from nsWindow::ProcessMessageInternal's
// MOZ_WM_OPENVR_EVENT case.
void FxRWindowManager::ProcessOverlayEvents(nsWindow* aWindow) {
  MOZ_ASSERT(NS_IsMainThread());

  if (aWindow == nullptr) {
    return;
  }

  std::vector<FxROverlayInputEvent> events;

  ::EnterCriticalSection(&mEventsCS);
  mEventQueue.swap(events);
  ::LeaveCriticalSection(&mEventsCS);

  // See the note on HandleScrollEvent: only one synthesized scroll can be
  // processed per pass through the message loop.
  bool hasScrolled = false;

  for (const FxROverlayInputEvent& event : events) {
    switch (event.mType) {
      case vr::VREvent_MouseMove:
      case vr::VREvent_MouseButtonDown:
      case vr::VREvent_MouseButtonUp:
        HandleMouseEvent(aWindow, event);
        break;

      case vr::VREvent_ScrollDiscrete:
        HandleScrollEvent(aWindow, event, hasScrolled);
        break;

      case vr::VREvent_KeyboardCharInput:
        HandleKeyboardEvent(aWindow, event);
        break;

      case vr::VREvent_KeyboardDone:
      case vr::VREvent_KeyboardClosed:
        mVirtualKeyboardVisible = false;
        break;

      case vr::VREvent_ButtonUnpress:
        // The trigger was let go. If SteamVR never sent the MouseButtonUp,
        // this is our only chance to end the click.
        ReleaseStuckMouseButton(aWindow);
        break;

      case vr::VREvent_OverlayFocusChanged:
        // Keep Firefox's idea of this window's focus in sync with the
        // overlay's, so that clicking a text field gets a caret and can raise
        // the keyboard.
        //
        // Skip it while the virtual keyboard is up: the keyboard is itself
        // another overlay, so it steals focus, and Firefox should not react.
        if (!mVirtualKeyboardVisible) {
          FXR_LOG(("Overlay focus: %s", event.mFocused ? "true" : "false"));
          // Losing focus mid-drag means the matching MouseButtonUp will never
          // arrive: SteamVR simply stops sending us events. Release the button
          // ourselves, or Gecko stays stuck in a drag forever and no further
          // hover or click works anywhere on the page.
          if (!event.mFocused) {
            ReleaseStuckMouseButton(aWindow);
          }
          aWindow->DispatchFocusToTopLevelWindow(event.mFocused);
        }
        break;

      default:
        break;
    }
  }

  aWindow->DispatchPendingEvents();
}

void FxRWindowManager::HandleMouseEvent(nsWindow* aWindow,
                                        const FxROverlayInputEvent& aEvent) {
  if (aEvent.mOverlayW == 0 || aEvent.mOverlayH == 0) {
    return;
  }

  // OpenVR reports the pointer in overlay-texture pixels with a bottom-left
  // origin; Gecko wants client-area pixels with a top-left origin. The texture
  // is the compositor's backbuffer, which is not necessarily the same size as
  // the widget's client area (e.g. 1280x720 texture in a 1296x759 client), so
  // scale rather than assuming 1:1.
  mozilla::LayoutDeviceIntSize clientSize = aWindow->GetClientSize();
  const double clientW = clientSize.width > 0 ? (double)clientSize.width
                                              : (double)aEvent.mOverlayW;
  const double clientH = clientSize.height > 0 ? (double)clientSize.height
                                               : (double)aEvent.mOverlayH;

  const double scaleX = clientW / (double)aEvent.mOverlayW;
  const double scaleY = clientH / (double)aEvent.mOverlayH;

  LONG x = (LONG)((double)aEvent.mX * scaleX + 0.5);
  LONG y =
      (LONG)(((double)aEvent.mOverlayH - (double)aEvent.mY) * scaleY + 0.5);

  const LONG maxX = (LONG)clientW - 1;
  const LONG maxY = (LONG)clientH - 1;
  mLastMousePt.x = x < 0 ? 0 : (maxX > 0 && x > maxX ? maxX : x);
  mLastMousePt.y = y < 0 ? 0 : (maxY > 0 && y > maxY ? maxY : y);

  // The secondary button is reserved; don't translate it into a right-click.
  if (aEvent.mButton == vr::VRMouseButton_Right) {
    return;
  }

  mozilla::EventMessage eMsg;
  if (aEvent.mType == vr::VREvent_MouseMove) {
    eMsg = mozilla::eMouseMove;
  } else if (aEvent.mType == vr::VREvent_MouseButtonDown) {
    eMsg = mozilla::eMouseDown;
    mMouseButtonDown = true;
    mMouseDownTime = mozilla::TimeStamp::Now();
  } else {
    MOZ_ASSERT(aEvent.mType == vr::VREvent_MouseButtonUp);
    // The click was already completed on the down. A late Up from SteamVR (if
    // one ever arrives) would be a second, spurious release.
    if (!mMouseButtonDown) {
      return;
    }
    eMsg = mozilla::eMouseUp;
    mMouseButtonDown = false;
  }

  FXR_LOG(
      ("HandleMouseEvent type=%u raw=(%.1f, %.1f) overlay=%ux%u "
       "client=%dx%d -> (%ld, %ld)",
       aEvent.mType, aEvent.mX, aEvent.mY, aEvent.mOverlayW, aEvent.mOverlayH,
       clientSize.width, clientSize.height, mLastMousePt.x, mLastMousePt.y));

  // A down that is never released is not just a lost click, it is a hang.
  // SteamVR does not reliably send VREvent_MouseButtonUp for overlay input, and
  // Gecko reacts to "button held while the pointer moves" by starting a drag --
  // which on Windows runs a modal loop that starves the main thread. Timers
  // stop, so any deferred release we schedule can never run: the freeze blocks
  // its own cure. Measured: the last main-thread tick lands in the same second
  // as the click, then nothing for minutes while the process still answers
  // Windows messages.
  //
  // So a trigger pull is emitted as a complete click here and now. Dragging is
  // given up deliberately; it is not reachable with an unreliable button-up
  // anyway, and a wedged browser is far worse than no drag.

  // wParam stays 0, as the original Firefox Reality code does: it is a
  // key-state mask, and MK_LBUTTON there would claim the button is held during
  // every move, which suppresses :hover and starts text selection.
  auto dispatch = [&](mozilla::EventMessage aMsg) {
    aWindow->DispatchMouseEvent(
        aMsg, 0, FXR_POINTTOPOINTS(mLastMousePt),
        /* aIsContextMenuKey */ false, mozilla::MouseButton::ePrimary,
        mozilla::dom::MouseEvent_Binding::MOZ_SOURCE_MOUSE,
        /* aPointerInfo */ nullptr);
  };

  dispatch(eMsg);

  if (eMsg == mozilla::eMouseDown) {
    dispatch(mozilla::eMouseUp);
    mMouseButtonDown = false;
    FXR_LOG(("Completed click at (%ld, %ld)", mLastMousePt.x, mLastMousePt.y));
  }
}

// Synthesises the eMouseUp that SteamVR owes us, at the last known position.
void FxRWindowManager::ReleaseStuckMouseButton(nsWindow* aWindow) {
  if (!mMouseButtonDown || aWindow == nullptr) {
    return;
  }
  mMouseButtonDown = false;

  FXR_LOG(("Releasing stuck mouse button at (%ld, %ld)", mLastMousePt.x,
           mLastMousePt.y));

  aWindow->DispatchMouseEvent(
      mozilla::eMouseUp, 0, FXR_POINTTOPOINTS(mLastMousePt),
      /* aIsContextMenuKey */ false, mozilla::MouseButton::ePrimary,
      mozilla::dom::MouseEvent_Binding::MOZ_SOURCE_MOUSE,
      /* aPointerInfo */ nullptr);
}

void FxRWindowManager::HandleScrollEvent(nsWindow* aWindow,
                                         const FxROverlayInputEvent& aEvent,
                                         bool& aHasScrolled) {
  // The legacy code went through
  // MouseScrollHandler::SynthesizeNativeMouseScrollEvent, but that path's
  // MOUSESCROLL_SEND_TO_WIDGET / MOUSESCROLL_POINT_IN_WINDOW_COORD flags are
  // gone in modern Gecko, and it round-trips through a native WM_MOUSEWHEEL --
  // exactly the kind of synthetic native message this whole change exists to
  // avoid. Dispatch a real wheel event instead.
  //
  // Only one scroll per drain: coalescing them keeps a flick from turning into
  // a burst of separate scrolls.
  if (aHasScrolled) {
    return;
  }

  if (aEvent.mScrollYDelta == 0.0f) {
    return;
  }

  // OpenVR reports scroll in detents; Gecko's line delta convention is 3 lines
  // per detent. Y is inverted: scrolling the pad up should move content up.
  const double lines = -(double)aEvent.mScrollYDelta * 3.0;

  FXR_LOG(("HandleScrollEvent lines=%f at (%ld, %ld)", lines, mLastMousePt.x,
           mLastMousePt.y));

  mozilla::WidgetWheelEvent wheelEvent(true, mozilla::eWheel, aWindow);
  wheelEvent.mRefPoint =
      mozilla::LayoutDeviceIntPoint(mLastMousePt.x, mLastMousePt.y);
  wheelEvent.mDeltaMode = mozilla::dom::WheelEvent_Binding::DOM_DELTA_LINE;
  wheelEvent.mDeltaY = lines;
  wheelEvent.mLineOrPageDeltaY =
      (int32_t)(lines > 0 ? ceil(lines) : floor(lines));
  wheelEvent.mInputSource = mozilla::dom::MouseEvent_Binding::MOZ_SOURCE_MOUSE;

  aWindow->DispatchInputEvent(&wheelEvent);

  aHasScrolled = true;
}

void FxRWindowManager::HandleKeyboardEvent(nsWindow* aWindow,
                                           const FxROverlayInputEvent& aEvent) {
  HWND hwnd = (HWND)(uintptr_t)mHwndWidget;
  if (hwnd == nullptr) {
    return;
  }

  size_t inputLength =
      strnlen_s(aEvent.mKeyboardInput, ARRAYSIZE(aEvent.mKeyboardInput));
  if (inputLength == 0) {
    return;
  }

  wchar_t msgChar = aEvent.mKeyboardInput[0];

  if (inputLength > 1) {
    // The event can contain multi-byte UTF8 characters. Convert them to a
    // single wide character to send to Gecko.
    wchar_t convertedChar[ARRAYSIZE(aEvent.mKeyboardInput)] = {0};
    int convertedReturn = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, aEvent.mKeyboardInput, (int)inputLength,
        convertedChar, ARRAYSIZE(convertedChar));

    if (convertedReturn < 1) {
      return;
    }

    msgChar = convertedChar[0];
  } else if (msgChar == L'\n' || msgChar == L'\r') {
    // Make new line
    msgChar = VK_RETURN;
  }

  FXR_LOG(("HandleKeyboardEvent char=0x%x", (unsigned)msgChar));

  switch (msgChar) {
    // These characters need to be mapped to key presses rather than char so
    // that they map to actions instead.
    case VK_BACK:
    case VK_TAB:
    case VK_RETURN:
    case VK_ESCAPE: {
      MSG nativeMsgDown =
          mozilla::widget::WinUtils::InitMSG(WM_KEYDOWN, msgChar, 0, hwnd);
      aWindow->ProcessKeyDownMessage(nativeMsgDown, nullptr);

      MSG nativeMsgUp =
          mozilla::widget::WinUtils::InitMSG(WM_KEYUP, msgChar, 0, hwnd);
      aWindow->ProcessKeyUpMessage(nativeMsgUp, nullptr);
      break;
    }

    default: {
      MSG nativeMsg =
          mozilla::widget::WinUtils::InitMSG(WM_CHAR, msgChar, 0, hwnd);
      aWindow->ProcessCharMessage(nativeMsg, nullptr);
      break;
    }
  }
}

void FxRWindowManager::NotifyEditableFocus(bool aFocused) {
  if (aFocused == (bool)mVirtualKeyboardVisible) {
    return;
  }
  mVirtualKeyboardVisible = aFocused;

  FXR_LOG(("NotifyEditableFocus(%s)", aFocused ? "true" : "false"));

  // Only the process that created the overlay may talk to it. When compositing
  // runs in the GPU process, hop over there; otherwise FxROutputHandler is in
  // this process and can be poked directly.
  mozilla::gfx::GPUProcessManager* gpm = mozilla::gfx::GPUProcessManager::Get();
  if (gpm != nullptr) {
    if (mozilla::gfx::GPUChild* child = gpm->GetGPUChild()) {
      child->SendShowFxrVirtualKeyboard(aFocused);
      return;
    }
  }

  FxROutputHandler::RequestShowKeyboard(aFocused);
}

// Maps the string vocabulary agreed with the media track onto FxRProjectionMode
// and forwards it to whoever owns the overlay. The mapping accepts the legacy
// Firefox Reality spellings too, so pages and shims written for the old
// mozVideoProjection convention keep working.
void FxRWindowManager::SetProjectionMode(const nsAString& aMode) {
  FxRProjectionMode mode = FxRProjectionMode::Mode2D;

  if (aMode.EqualsLiteral("360")) {
    mode = FxRProjectionMode::Mode360;
  } else if (aMode.EqualsLiteral("360-stereo")) {
    mode = FxRProjectionMode::Mode360Stereo;
  } else if (aMode.EqualsLiteral("3d") || aMode.EqualsLiteral("3d-sbs")) {
    mode = FxRProjectionMode::Mode3D;
  } else if (aMode.EqualsLiteral("180-sbs")) {
    mode = FxRProjectionMode::Mode180SBS;
  } else if (aMode.EqualsLiteral("180-tb")) {
    mode = FxRProjectionMode::Mode180TB;
  } else if (aMode.EqualsLiteral("curved") || aMode.EqualsLiteral("cinema")) {
    mode = FxRProjectionMode::ModeCurved;
  } else if (aMode.EqualsLiteral("exit")) {
    // Leaving fullscreen is a window operation, not an overlay one, and it
    // belongs to this process either way. The modern tree keeps the DOM window,
    // so go through the widget the same way CreateOverlayForWindow does.
    if (mWindow != nullptr) {
      nsCOMPtr<nsIWidget> widget =
          mozilla::widget::WidgetUtils::DOMWindowToWidget(mWindow);
      if (widget) {
        widget->MakeFullScreen(false);
      }
    }
    return;
  }

  FXR_LOG(("SetProjectionMode(%s) -> %u", NS_ConvertUTF16toUTF8(aMode).get(),
           (unsigned)mode));

  // Same hop as NotifyEditableFocus: only the process that created the overlay
  // may talk to it (ADR-02).
  mozilla::gfx::GPUProcessManager* gpm = mozilla::gfx::GPUProcessManager::Get();
  if (gpm != nullptr) {
    if (mozilla::gfx::GPUChild* child = gpm->GetGPUChild()) {
      child->SendSetFxrProjectionMode((uint32_t)mode);
      return;
    }
  }

  FxROutputHandler::RequestProjectionMode((uint32_t)mode);
}

namespace mozilla::gfx {

// Thin entry point for dom/base/ChromeUtils.cpp. It exists so that DOM code
// does not have to include FxRWindowManager.h, which pulls in windows.h and
// openvr.h -- those have broken unified builds in this tree before.
void FxRSetProjectionMode(const nsAString& aMode) {
  FxRWindowManager::GetInstance()->SetProjectionMode(aMode);
}

}  // namespace mozilla::gfx

void FxRWindowManager::SetWindowDocked(bool aDocked) {
  MOZ_ASSERT(NS_IsMainThread());
  if (!mWindow) return;
  if (auto* gpm = mozilla::gfx::GPUProcessManager::Get()) {
    if (auto* child = gpm->GetGPUChild()) {
      (void)child->SendSetFxrWindowDocked(aDocked);
      return;
    }
  }
  FxROutputHandler::RequestDocked(aDocked);
}

void FxRWindowManager::OnWindowState(uint32_t aState) {
  MOZ_ASSERT(NS_IsMainThread());
  if (!mWindow || aState > 3) return;
  mWindowRuntimeReady = aState < 2;
  if (nsCOMPtr<nsIObserverService> observers =
          mozilla::services::GetObserverService()) {
    const char16_t* state = aState == 0   ? u"docked"
                            : aState == 1 ? u"detached"
                            : aState == 2 ? u"quit"
                                          : u"unavailable";
    observers->NotifyObservers(nullptr, "fxr-window-state", state);
  }
}

namespace mozilla::gfx {
void FxRSetWindowDocked(bool aDocked) {
  FxRWindowManager::GetInstance()->SetWindowDocked(aDocked);
}
}  // namespace mozilla::gfx
