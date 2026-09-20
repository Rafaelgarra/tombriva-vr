/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FxRDashboardIcon.h"
#include "FxROutputHandler.h"
#include <vector>
#include <cstdlib>
#include <cmath>
#include <string>
#include <cstdio>
#include <cstring>

#include <string.h>

#include "FxROverlayInputEvent.h"
#include "FxRWindowManager.h"
#include "mozilla/Assertions.h"
#include "mozilla/gfx/GPUParent.h"
#include "nsDebug.h"
#include "nsThreadUtils.h"
#include "nsXULAppAPI.h"
#include "openvr.h"

// Hands one overlay input event to whoever can turn it into a Gecko event.
//
// The overlay is polled on the Renderer thread. When compositing runs in the
// GPU process (the normal case) the event has to cross to the UI process, and
// PGPU's child-side messages are received on the UI process's main thread --
// exactly where nsWindow::DispatchMouseEvent needs to run. When compositing
// runs in the parent process, FxRWindowManager is right here and can be poked
// directly; it only takes a lock and PostMessage, so any thread will do.
static void ForwardOverlayInput(const FxROverlayInputEvent& aEvent) {
  if (XRE_IsParentProcess()) {
    FxRWindowManager::GetInstance()->OnOverlayInputEvent(aEvent);
    return;
  }

  auto send = [aEvent]() {
    mozilla::gfx::GPUParent* parent = mozilla::gfx::GPUParent::GetSingleton();
    if (parent == nullptr) {
      return;
    }

    mozilla::gfx::FxrOverlayInputEvent msg;
    msg.type() = aEvent.mType;
    msg.x() = aEvent.mX;
    msg.y() = aEvent.mY;
    msg.button() = aEvent.mButton;
    msg.scrollYDelta() = aEvent.mScrollYDelta;
    msg.overlayW() = aEvent.mOverlayW;
    msg.overlayH() = aEvent.mOverlayH;
    msg.focused() = aEvent.mFocused;
    msg.keyboardInput().AppendElements(
        reinterpret_cast<const uint8_t*>(aEvent.mKeyboardInput),
        sizeof(aEvent.mKeyboardInput));

    // Input is best-effort: a dropped event just means one missed laser
    // sample, and there is nothing useful to do if the actor is going away.
    (void)parent->SendNotifyFxrOverlayInput(msg);
  };

  if (NS_IsMainThread()) {
    send();
  } else {
    NS_DispatchToMainThread(
        NS_NewRunnableFunction("FxrForwardOverlayInput", std::move(send)));
  }
}

static void ForwardWindowState(uint32_t aState) {
  NS_DispatchToMainThread(NS_NewRunnableFunction("FxRWindowState", [aState]() {
    if (XRE_IsParentProcess()) {
      FxRWindowManager::GetInstance()->OnWindowState(aState);
    } else if (auto* parent = mozilla::gfx::GPUParent::GetSingleton()) {
      (void)parent->SendNotifyFxrWindowState(aState);
    }
  }));
}

// Only one FxR window is supported, so a single active handler is enough to
// route virtual-keyboard requests from the UI thread back to the Renderer
// thread that owns the overlay.
static mozilla::Atomic<FxROutputHandler*> sActiveHandler(nullptr);
static mozilla::Atomic<int> sPendingWindowDocked(0);

bool FxROutputHandler::HasActiveHandler() { return sActiveHandler != nullptr; }

FxROutputHandler::FxROutputHandler()
    : m_pHMD(nullptr),
      mOverlayHandle(vr::k_ulOverlayHandleInvalid),
      mSwapChain(nullptr),
      mOverlayWidth(0),
      mOverlayHeight(0),
      mPendingKeyboard(0),
      mPendingVisibility(0),
      mPendingProjection(0),
      mCurrentProjection(0),
      mControlsHandle(vr::k_ulOverlayHandleInvalid),
      mControlsHover(-1),
      mTitleBarHandle(vr::k_ulOverlayHandleInvalid),
      mDragDevice(vr::k_unTrackedDeviceIndexInvalid),
      mIconScale(1.0f),
      mIconScaleTarget(1.0f),
      mVr180SrcWidth(0),
      mVr180SrcHeight(0),
      mVr180Mode(0),
      mInputPumpActive(false) {
  sPendingWindowDocked = 0;
  sActiveHandler = this;
}

FxROutputHandler::~FxROutputHandler() {
  sActiveHandler.compareExchange(this, nullptr);

  // Must happen before the overlay is destroyed: the pump touches the handle.
  StopInputPump();
  DestroyControlsOverlay();
  if (vr::VROverlay()) {
    if (mDashboardHandle) vr::VROverlay()->DestroyOverlay(mDashboardHandle);
    if (mDashboardIcon) vr::VROverlay()->DestroyOverlay(mDashboardIcon);
  }

  if (mTitleBarHandle != vr::k_ulOverlayHandleInvalid) {
    if (vr::VROverlay() != nullptr) {
      vr::VROverlay()->DestroyOverlay(mTitleBarHandle);
    }
    mTitleBarHandle = vr::k_ulOverlayHandleInvalid;
  }

  if (mOverlayHandle != vr::k_ulOverlayHandleInvalid) {
    if (vr::VROverlay() != nullptr) {
      vr::VROverlay()->DestroyOverlay(mOverlayHandle);
    }
    mOverlayHandle = vr::k_ulOverlayHandleInvalid;
  }
  if (m_pHMD != nullptr) {
    vr::VR_Shutdown();
    m_pHMD = nullptr;
  }
  mSwapChain = nullptr;
}

// SteamVR derives an app key from the executable, so by default the overlay
// (this process) and the VR process -- both firefox.exe -- land on the same
// system.generated.firefox.exe. When the VR process then connects as a scene
// app, SteamVR moves that key to it and the scene dies about a second later.
//
// Measured, same build, back-to-back sessions:
//   distinct keys (overlay=mozilla.firefoxreality.overlay, scene=system.*)
//       -> 4017 frames
//   shared key   (both system.generated.firefox.exe)
//       -> 4 frames
//
// So the fix is simply to make the keys differ. The manifest deliberately uses
// launch_type "url" and declares NO binary path: SteamVR matches keys to
// connecting processes by binary, and declaring firefox.exe made the VR
// process inherit this very key, which just renamed the collision. With no
// binary to match, the only way into this key is IdentifyApplication() below,
// which is by PID and therefore binds this process alone.
#define FXR_OVERLAY_APPKEY "mozilla.firefoxreality.overlay"

void FxROutputHandler::IdentifyOverlayApplication() {
  if (vr::VRApplications() == nullptr) {
    return;
  }

  char tempDir[MAX_PATH] = {0};
  if (::GetTempPathA(MAX_PATH, tempDir) == 0) {
    return;
  }
  std::string manifestPath = std::string(tempDir) + "fxr-overlay.vrmanifest";

  const char* manifest =
      "{"
      "\n"
      "  \"source\" : \"builtin\","
      "\n"
      "  \"applications\" : [{"
      "\n"
      "    \"app_key\" : \"" FXR_OVERLAY_APPKEY
      "\","
      "\n"
      "    \"launch_type\" : \"url\","
      "\n"
      "    \"url\" : \"https://mixedreality.mozilla.org/FirefoxRealityPC/\","
      "\n"
      "    \"is_dashboard_overlay\" : true,"
      "\n"
      "    \"strings\" : { \"en_us\" : { \"name\" : \"Tombriva VR\" } }"
      "\n"
      "  }]"
      "\n"
      "}"
      "\n";

  FILE* f = ::fopen(manifestPath.c_str(), "wb");
  if (f == nullptr) {
    return;
  }
  ::fwrite(manifest, 1, ::strlen(manifest), f);
  ::fclose(f);

  // A previous run may have left a manifest registered under this same key
  // (a temporary manifest survives until SteamVR restarts).
  // AddApplicationManifest would then return AppKeyAlreadyExists and the stale
  // entry would win, so drop it first and make this run's registration the one
  // in effect.
  vr::VRApplications()->RemoveApplicationManifest(manifestPath.c_str());

  vr::EVRApplicationError err =
      vr::VRApplications()->AddApplicationManifest(manifestPath.c_str(), true);
  printf_stderr(
      "[FxR-Modern-GPU] AddApplicationManifest -> %d"
      "\n",
      (int)err);

  err = vr::VRApplications()->IdentifyApplication(::GetCurrentProcessId(),
                                                  FXR_OVERLAY_APPKEY);
  printf_stderr("[FxR-Modern-GPU] IdentifyApplication(" FXR_OVERLAY_APPKEY
                ") -> %d"
                "\n",
                (int)err);
}

// static
void FxROutputHandler::RequestShowKeyboard(bool aShow) {
  FxROutputHandler* handler = sActiveHandler;
  if (handler != nullptr) {
    handler->mPendingKeyboard = aShow ? 1 : -1;
  }
}

// static
void FxROutputHandler::RequestSetVisible(bool aVisible) {
  FxROutputHandler* handler = sActiveHandler;
  if (handler != nullptr) {
    handler->mPendingVisibility = aVisible ? 1 : -1;
  }
}

void FxROutputHandler::RequestDocked(bool aDocked) {
  if (sActiveHandler) {
    sPendingWindowDocked = aDocked ? 1 : -1;
  }
}

vr::VROverlayHandle_t FxROutputHandler::InputOverlay() const {
  return (mDocked || mDashboardVisible) ? mDashboardHandle : mOverlayHandle;
}

void FxROutputHandler::UpdatePanelVisibility() {
  auto* overlay = vr::VROverlay();
  const bool active = !mImmersiveSessionActive && !mClosing;
  for (auto h : {mOverlayHandle, mDashboardHandle, mTitleBarHandle}) {
    if (!h) continue;
    const bool shown =
        active && (h == mDashboardHandle
                       ? (mDocked || (mDashboardVisible && mWasDashboardActive))
                       : (!mDocked && !mDashboardVisible));
    overlay->SetOverlayInputMethod(h, shown ? vr::VROverlayInputMethod_Mouse
                                            : vr::VROverlayInputMethod_None);
    if (shown)
      overlay->ShowOverlay(h);
    else
      overlay->HideOverlay(h);
  }
  if (mControlsHandle) overlay->HideOverlay(mControlsHandle);
}

void FxROutputHandler::AlignTitleBar() {
  if (!mTitleBarHandle || mDragDevice != vr::k_unTrackedDeviceIndexInvalid)
    return;
  vr::ETrackingUniverseOrigin origin;
  vr::HmdMatrix34_t panel;
  if (vr::VROverlay()->GetOverlayTransformAbsolute(
          mOverlayHandle, &origin, &panel) != vr::VROverlayError_None)
    return;
  float width = 2.0f;
  vr::VROverlay()->GetOverlayWidthInMeters(mOverlayHandle, &width);
  const float height =
      mOverlayWidth ? width * mOverlayHeight / mOverlayWidth : width;
  // The chrome titlebar occupies the first 8% of the panel height.
  const float offset = height * 0.46f;
  vr::VROverlay()->SetOverlayWidthInMeters(mTitleBarHandle,
                                           height * 0.06f * mIconScale);
  for (int r = 0; r < 3; ++r) {
    panel.m[r][3] += panel.m[r][1] * offset + panel.m[r][2] * 0.002f;
  }
  vr::VROverlay()->SetOverlayTransformAbsolute(mTitleBarHandle, origin, &panel);
}

void FxROutputHandler::RecenterPanel() {
  if (m_pHMD) {
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    m_pHMD->GetDeviceToAbsoluteTrackingPose(
        vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
    if (poses[0].bPoseIsValid) {
      const auto& head = poses[0].mDeviceToAbsoluteTracking;
      const float yaw = atan2f(head.m[0][2], head.m[2][2]);
      const float c = cosf(yaw), t = sinf(yaw);
      vr::HmdMatrix34_t panel = {{{c, 0, t, head.m[0][3] - 1.8f * t},
                                  {0, 1, 0, head.m[1][3]},
                                  {-t, 0, c, head.m[2][3] - 1.8f * c}}};
      vr::VROverlay()->SetOverlayTransformAbsolute(
          mOverlayHandle, vr::TrackingUniverseStanding, &panel);
    }
  }
  AlignTitleBar();
}

void FxROutputHandler::SetDocked(bool aDocked) {
  if (mClosing || mImmersiveSessionActive) return;
  if (aDocked && !mDashboardHandle) {
    ForwardWindowState(3);
    return;
  }
  EndDragPanel();
  vr::VROverlay()->HideKeyboard();
  if (aDocked && mCurrentProjection != 0) ApplyProjectionMode(0);
  const bool wasDocked = mDocked;
  mDocked = aDocked;
  mWasDashboardActive =
      mDashboardHandle &&
      vr::VROverlay()->IsActiveDashboardOverlay(mDashboardHandle);
  // Drop queued coordinates before transferring the interactive surface.
  vr::VREvent_t stale;
  for (auto h : {mOverlayHandle, mDashboardHandle}) {
    if (h)
      while (vr::VROverlay()->PollNextOverlayEvent(h, &stale, sizeof(stale))) {
        if (stale.eventType == vr::VREvent_Quit) RequestAppQuit();
      }
  }
  if (mClosing) return;
  if (!aDocked && wasDocked) RecenterPanel();
  AlignTitleBar();
  UpdatePanelVisibility();
  if (aDocked && mDashboardTextureReady) {
    vr::VROverlay()->ShowDashboard("firefox.reality.dashboard");
    mDashboardOpened = true;
  }
  ForwardWindowState(aDocked ? 0 : 1);
  printf_stderr("[FxR window] %s\n", aDocked ? "dashboard" : "detached");
}

void FxROutputHandler::RequestAppQuit() {
  if (mClosing) return;
  mClosing = true;
  EndDragPanel();
  vr::VROverlay()->HideKeyboard();
  UpdatePanelVisibility();
  m_pHMD->AcknowledgeQuit_Exiting();
  ForwardWindowState(2);
  printf_stderr("[FxR window] application quit requested\n");
}

// static
void FxROutputHandler::RequestProjectionMode(uint32_t aMode) {
  FxROutputHandler* handler = sActiveHandler;
  if (handler != nullptr) {
    handler->mPendingProjection = aMode + 1;
  }
}

// Applies a projection by toggling the OpenVR overlay flags and repositioning
// the panel. This is the part the legacy Firefox Reality had
// (FxRWindowManager::ChangeProjectionMode in the Gecko 84 tree), ported to the
// GPU process because that is where the overlay now lives (ADR-02).
//
// On VR180: OpenVR has no 180 flag, which is exactly why the legacy enum
// declared VIDEO_PROJECTION_180* and never wired them up. What is applied here
// is an APPROXIMATION, not true VR180 -- 180 side-by-side borrows the flat
// stereo flag (stereo, but not spherical) and 180 top-bottom borrows the stereo
// panorama flag (spherical and stereo, but stretched across 360 degrees
// instead of 180). Getting it right needs the source remapped before
// submission; see SPIKE3_MEDIA_VR180.md and ADR-27.
// Number of buttons on the controls strip, left to right:
//   0 = 2D (exit projection)   1 = 360        2 = 360 stereo
//   3 = 3D SBS                 4 = 180 SBS    5 = 180 TB
// Tres botoes, por decisao do usuario em 11/09/2026: sair, esfera e tela
// grande. Os modos estereo e VR180 do overlay continuam existindo na API
// (ChromeUtils.setFxrProjectionMode), mas sairam da faixa: eles dependem
// de fonte equirretangular crua, que na pratica nao chega pela web, e o
// caminho correto para midia imersiva passou a ser o player WebXR.
static const uint32_t kControlsButtons = 3;
static const uint32_t kControlsWidth = 1200;
static const uint32_t kControlsHeight = 120;

// Creates the controls overlay and paints it once. The strip is drawn as flat
// coloured blocks with a gap between them: no text, because SetOverlayRaw takes
// raw pixels and there is no glyph rasteriser available here. The leftmost
// block is red (leave projection); the others follow the order above.
// Paints the strip. Each button carries a geometric glyph rather than text:
// SetOverlayRaw takes raw pixels and there is no glyph rasteriser available
// here, but a shape reads at a glance and survives the overlay's resampling.
//
//   0 exit (X)   1 circle (360)   2 two circles (360 stereo)
//   3 two rects (3D SBS)   4 two half-discs side by side (180 SBS)
//   5 two half-discs stacked (180 TB)
//
// aHighlight is the button under the laser, or -1. SteamVR does not reliably
// draw its cursor over our overlays, so the strip provides its own feedback.
void FxROutputHandler::PaintControls(int32_t aHighlight) {
  if (mControlsHandle == vr::k_ulOverlayHandleInvalid) {
    return;
  }

  static const uint8_t kColors[kControlsButtons][3] = {
      {200, 60, 60},   // 0 sair / 2D
      {60, 140, 220},  // 1 esfera 360
      {120, 200, 90},  // 2 tela curva grande
  };

  std::vector<uint8_t> pixels(kControlsWidth * kControlsHeight * 4, 0);
  const int32_t slot = (int32_t)(kControlsWidth / kControlsButtons);
  const int32_t gap = 10;

  for (int32_t y = 0; y < (int32_t)kControlsHeight; ++y) {
    for (int32_t x = 0; x < (int32_t)kControlsWidth; ++x) {
      const int32_t idx = x / slot;
      const int32_t wx = x % slot;  // x dentro do botao
      uint8_t* px = &pixels[(y * kControlsWidth + x) * 4];

      const bool inButton = idx < (int32_t)kControlsButtons && wx > gap &&
                            wx < slot - gap && y > gap &&
                            y < (int32_t)kControlsHeight - gap;
      if (!inButton) {
        px[0] = px[1] = px[2] = 20;
        px[3] = 230;
        continue;
      }

      // Fundo do botao, mais claro quando sob o laser.
      const bool hot = (idx == aHighlight);
      for (int c = 0; c < 3; ++c) {
        int v = kColors[idx][c];
        px[c] = (uint8_t)(hot ? (v + (255 - v) / 2) : v);
      }
      px[3] = 255;

      // Glifo em branco, centrado no botao.
      const int32_t cx = slot / 2, cy = (int32_t)kControlsHeight / 2;
      const int32_t dx = wx - cx, dy = y - cy;
      const int32_t r = (int32_t)kControlsHeight / 5;
      const int32_t d2 = dx * dx + dy * dy;
      const int32_t ring = r * r / 4;  // espessura do traco
      bool ink = false;

      switch (idx) {
        case 0:  // X: sair
          ink = (abs(abs(dx) - abs(dy)) < 5) && abs(dx) < r && abs(dy) < r;
          break;
        case 1:  // circulo: esfera 360
          ink = abs(d2 - r * r) < ring;
          break;
        case 2: {  // arco: tela curva
          const int32_t rr = r + 4;
          ink = abs(d2 - rr * rr) < ring && dy < r / 2 && dy > -r;
          break;
        }
        default:
          break;
      }

      if (ink) {
        px[0] = px[1] = px[2] = 255;
      }
    }
  }

  vr::VROverlay()->SetOverlayRaw(mControlsHandle, pixels.data(), kControlsWidth,
                                 kControlsHeight, 4);
}

// Repaints only when the laser crosses into a different button.
void FxROutputHandler::HandleControlsHover(float aX) {
  const int32_t slot = (int32_t)(kControlsWidth / kControlsButtons);
  int32_t idx = (int32_t)(aX / (float)slot);
  if (idx < 0 || idx >= (int32_t)kControlsButtons) {
    idx = -1;
  }
  if (idx != mControlsHover) {
    mControlsHover = idx;
    PaintControls(idx);
  }
}

// Botao quadrado de mover. Pintado uma unica vez: a animacao de pressionar
// e feita escalando o overlay, nao reenviando a textura.
static const uint32_t kTitleBarWidth = 96;
static const uint32_t kTitleBarHeight = 96;
// Largura fisica do botao; a animacao de pressionar escala isto.
static const float kBarWidthMeters = 0.26f;

// Rigid-transform helpers. HmdMatrix34_t is a 3x4 affine matrix whose implied
// last row is (0,0,0,1), so multiplication and inversion can be done in closed
// form -- no general matrix library needed.
static vr::HmdMatrix34_t MatMul(const vr::HmdMatrix34_t& a,
                                const vr::HmdMatrix34_t& b) {
  vr::HmdMatrix34_t out = {};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      out.m[r][c] =
          a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c];
    }
    out.m[r][3] = a.m[r][0] * b.m[0][3] + a.m[r][1] * b.m[1][3] +
                  a.m[r][2] * b.m[2][3] + a.m[r][3];
  }
  return out;
}

// Inverse of a rotation+translation: transpose the rotation, and rotate the
// negated translation by it.
static vr::HmdMatrix34_t MatInvertRigid(const vr::HmdMatrix34_t& m) {
  vr::HmdMatrix34_t out = {};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      out.m[r][c] = m.m[c][r];
    }
  }
  for (int r = 0; r < 3; ++r) {
    out.m[r][3] = -(out.m[r][0] * m.m[0][3] + out.m[r][1] * m.m[1][3] +
                    out.m[r][2] * m.m[2][3]);
  }
  return out;
}

// Flat strip with a grip pattern, brighter while held.
// Paints the bar. aScale shrinks the move icon for the pressed effect; the
// icon sits centred, and only its box grabs.
// Desenha o botao de mover: cruz com pontas em seta, sobre fundo arredondado.
// Chamado uma vez; o efeito de pressionar vem da escala do overlay.
void FxROutputHandler::PaintTitleBar(float) {
  if (mTitleBarHandle == vr::k_ulOverlayHandleInvalid) {
    return;
  }

  std::vector<uint8_t> pixels(kTitleBarWidth * kTitleBarHeight * 4, 0);
  const int32_t c = (int32_t)kTitleBarWidth / 2;
  for (int32_t y = 0; y < (int32_t)kTitleBarHeight; ++y) {
    for (int32_t x = 0; x < (int32_t)kTitleBarWidth; ++x) {
      uint8_t* px = &pixels[(y * kTitleBarWidth + x) * 4];
      const int32_t dx = x - c, dy = y - c;
      const int32_t ax = abs(dx), ay = abs(dy);
      const int32_t cornerX = std::max(ax - 32, 0);
      const int32_t cornerY = std::max(ay - 32, 0);
      if (cornerX * cornerX + cornerY * cornerY > 12 * 12) continue;
      px[0] = 48;
      px[1] = 67;
      px[2] = 83;
      px[3] = 255;
      const bool vertical =
          ax <= 12 && ay >= 17 && ay <= 31 && abs(ax + ay - 29) <= 3;
      const bool horizontal =
          ay <= 12 && ax >= 17 && ax <= 31 && abs(ax + ay - 29) <= 3;
      if (vertical || horizontal) {
        px[0] = 233;
        px[1] = 243;
        px[2] = 248;
      }
    }
  }

  vr::VROverlay()->SetOverlayRaw(mTitleBarHandle, pixels.data(), kTitleBarWidth,
                                 kTitleBarHeight, 4);
}

void FxROutputHandler::EnsureTitleBar() {
  if (vr::VROverlay() == nullptr ||
      mTitleBarHandle != vr::k_ulOverlayHandleInvalid) {
    return;
  }
  if (vr::VROverlay()->CreateOverlay("firefox.reality.titlebar",
                                     "Tombriva VR Title Bar", &mTitleBarHandle) !=
      vr::VROverlayError_None) {
    mTitleBarHandle = vr::k_ulOverlayHandleInvalid;
    return;
  }

  PaintTitleBar(mIconScale);
  vr::VROverlay()->SetOverlayWidthInMeters(mTitleBarHandle, kBarWidthMeters);
  vr::VROverlay()->SetOverlayInputMethod(mTitleBarHandle,
                                         vr::VROverlayInputMethod_Mouse);
  vr::VROverlay()->SetOverlayFlag(
      mTitleBarHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible,
      true);
  vr::HmdVector2_t scale = {{(float)kTitleBarWidth, (float)kTitleBarHeight}};
  vr::VROverlay()->SetOverlayMouseScale(mTitleBarHandle, &scale);
  vr::VROverlay()->SetOverlaySortOrder(mTitleBarHandle, 150);

  // Centrado logo acima da borda superior do painel. O painel tem 2 m de
  // largura em 1280x720, logo 1.125 m de altura, e seu topo fica em 1.8125.
  const float kPanelTopY = 1.25f + (2.0f * 720.0f / 1280.0f) / 2.0f;
  vr::HmdMatrix34_t t = {
      {{1.0f, 0.0f, 0.0f, 0.0f},
       {0.0f, 1.0f, 0.0f, kPanelTopY + kBarWidthMeters / 2.0f},
       {0.0f, 0.0f, 1.0f, -1.8f}}};
  vr::VROverlay()->SetOverlayTransformAbsolute(
      mTitleBarHandle, vr::TrackingUniverseStanding, &t);
  // Etapa 1C-d: mesma regra da barra de controles -- nada interativo aparece
  // durante a sessao imersiva.
  if (!mImmersiveSessionActive && !mDocked) {
    vr::VROverlay()->ShowOverlay(mTitleBarHandle);
  }
  printf_stderr("[FxR-Modern-GPU] title bar criada \n");
}

// Parent the panel (and the bar) to the controller holding it, preserving the
// current offset, so both follow the hand rigidly.
void FxROutputHandler::BeginDragPanel(vr::TrackedDeviceIndex_t aDevice) {
  if (mDocked || mImmersiveSessionActive || mClosing || m_pHMD == nullptr ||
      aDevice == vr::k_unTrackedDeviceIndexInvalid) {
    return;
  }

  vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
  m_pHMD->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f,
                                          poses, vr::k_unMaxTrackedDeviceCount);
  if (aDevice >= vr::k_unMaxTrackedDeviceCount ||
      !poses[aDevice].bPoseIsValid) {
    return;
  }
  const vr::HmdMatrix34_t invCtl =
      MatInvertRigid(poses[aDevice].mDeviceToAbsoluteTracking);

  vr::ETrackingUniverseOrigin origin;
  vr::HmdMatrix34_t abs34;
  for (vr::VROverlayHandle_t h : {mOverlayHandle, mTitleBarHandle}) {
    if (h == vr::k_ulOverlayHandleInvalid) {
      continue;
    }
    if (vr::VROverlay()->GetOverlayTransformAbsolute(h, &origin, &abs34) !=
        vr::VROverlayError_None) {
      continue;
    }
    vr::HmdMatrix34_t rel = MatMul(invCtl, abs34);
    vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(h, aDevice, &rel);
  }

  mDragDevice = aDevice;
  mIconScaleTarget = 0.88f;
  printf_stderr("[FxR-Modern-GPU] arraste iniciado pelo dispositivo %u \n",
                aDevice);
}

// Freeze wherever the hand left it: read back the relative transform, compose
// it with the controller's current pose, and re-anchor in room space.
void FxROutputHandler::EndDragPanel() {
  if (mDragDevice == vr::k_unTrackedDeviceIndexInvalid || m_pHMD == nullptr) {
    return;
  }

  vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
  m_pHMD->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f,
                                          poses, vr::k_unMaxTrackedDeviceCount);

  for (vr::VROverlayHandle_t h : {mOverlayHandle, mTitleBarHandle}) {
    if (h == vr::k_ulOverlayHandleInvalid) {
      continue;
    }
    vr::TrackedDeviceIndex_t dev = vr::k_unTrackedDeviceIndexInvalid;
    vr::HmdMatrix34_t rel;
    if (vr::VROverlay()->GetOverlayTransformTrackedDeviceRelative(
            h, &dev, &rel) != vr::VROverlayError_None ||
        dev >= vr::k_unMaxTrackedDeviceCount || !poses[dev].bPoseIsValid) {
      continue;
    }
    vr::HmdMatrix34_t abs34 = MatMul(poses[dev].mDeviceToAbsoluteTracking, rel);
    vr::VROverlay()->SetOverlayTransformAbsolute(
        h, vr::TrackingUniverseStanding, &abs34);
  }

  mDragDevice = vr::k_unTrackedDeviceIndexInvalid;
  mIconScaleTarget = 1.0f;
  printf_stderr("[FxR-Modern-GPU] arraste encerrado \n");
}

void FxROutputHandler::EnsureControlsOverlay() {
  if (vr::VROverlay() == nullptr) {
    return;
  }

  if (mControlsHandle == vr::k_ulOverlayHandleInvalid) {
    vr::EVROverlayError err = vr::VROverlay()->CreateOverlay(
        "firefox.reality.controls", "Tombriva VR Controls", &mControlsHandle);
    if (err != vr::VROverlayError_None) {
      printf_stderr("[FxR-Modern-GPU] CreateOverlay(controls) falhou: %d \n",
                    (int)err);
      mControlsHandle = vr::k_ulOverlayHandleInvalid;
      return;
    }

    vr::VROverlay()->SetOverlayWidthInMeters(mControlsHandle, 1.2f);
    vr::VROverlay()->SetOverlayInputMethod(mControlsHandle,
                                           vr::VROverlayInputMethod_Mouse);
    // Sem isto o SteamVR entrega os eventos mas NAO desenha o cursor do laser
    // sobre o overlay: o clique funciona e a mira nao aparece. A flag liga o
    // "system-wide laser mouse mode" enquanto o overlay estiver visivel
    // (openvr.h:3646). O painel principal sempre a teve; aqui faltava.
    vr::VROverlay()->SetOverlayFlag(
        mControlsHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible,
        true);
    vr::HmdVector2_t mouseScale = {
        {(float)kControlsWidth, (float)kControlsHeight}};
    vr::VROverlay()->SetOverlayMouseScale(mControlsHandle, &mouseScale);
    // Above everything else, including a panorama sphere.
    vr::VROverlay()->SetOverlaySortOrder(mControlsHandle, 200);

    mControlsHover = -1;
    PaintControls(-1);
    printf_stderr("[FxR-Modern-GPU] controls overlay criado \n");
  }

  // Low and close, tracked to the head so it follows the user inside a
  // panorama instead of being left behind on the sphere.
  vr::HmdMatrix34_t transform = {{{1.0f, 0.0f, 0.0f, 0.0f},
                                  {0.0f, 1.0f, 0.0f, -0.45f},
                                  {0.0f, 0.0f, 1.0f, -1.3f}}};
  vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(
      mControlsHandle, vr::k_unTrackedDeviceIndex_Hmd, &transform);
  // Etapa 1C-d: durante a sessao imersiva a barra pode ser criada, mas nao
  // aparece -- reexibi-la devolveria a entrada dos controles ao laser do
  // sistema. Ela volta no fim da sessao, com o painel.
  if (!mImmersiveSessionActive) {
    vr::VROverlay()->ShowOverlay(mControlsHandle);
  }
}

void FxROutputHandler::DestroyControlsOverlay() {
  if (mControlsHandle == vr::k_ulOverlayHandleInvalid) {
    return;
  }
  if (vr::VROverlay() != nullptr) {
    vr::VROverlay()->DestroyOverlay(mControlsHandle);
  }
  mControlsHandle = vr::k_ulOverlayHandleInvalid;
  printf_stderr("[FxR-Modern-GPU] controls overlay destruido \n");
}

// Which block was hit, from the overlay's own mouse coordinates. No IPC and no
// window involved: the mode is applied right here.
void FxROutputHandler::HandleControlsClick(float aX) {
  const uint32_t slot = kControlsWidth / kControlsButtons;
  uint32_t idx = (uint32_t)(aX / (float)slot);
  if (idx >= kControlsButtons) {
    idx = kControlsButtons - 1;
  }
  // A faixa tem 3 botoes, mas os modos internos continuam os mesmos: o
  // terceiro botao aciona a tela curva, que e o modo 6.
  static const uint32_t kButtonToMode[kControlsButtons] = {0, 1, 6};
  const uint32_t mode = kButtonToMode[idx];
  printf_stderr("[FxR-Modern-GPU] controls: botao %u -> modo %u \n", idx, mode);
  ApplyProjectionMode(mode);
}

void FxROutputHandler::ApplyProjectionMode(uint32_t aMode) {
  if (mOverlayHandle == vr::k_ulOverlayHandleInvalid ||
      vr::VROverlay() == nullptr) {
    return;
  }

  if (mClosing || mImmersiveSessionActive) return;
  if (aMode != 0 && mDocked) SetDocked(false);
  EndDragPanel();

  // Modo 1 deixou de ser esfera (VROverlayFlags_Panorama) e passou a ser uma
  // TELA CURVA GRANDE.
  //
  // Motivo, observado em hardware: os players de video na web (YouTube entre
  // eles) ja projetam o 360 internamente e entregam na tela uma vista plana.
  // Aplicar Panorama sobre isso e projetar o que ja foi projetado -- a imagem
  // deforma, sobretudo perto do chao, onde o mapeamento equirretangular mais
  // estica. A esfera so esta correta quando a fonte e equirretangular crua, o
  // que praticamente nao acontece pela web.
  //
  // Uma superficie curva envolve o usuario sem reprojetar nada: a imagem
  // continua sendo exibida como e, apenas em escala de cinema.
  const bool isPanorama = (aMode == 1);  // 360 mono, esfera
  const bool isCurvedCinema =
      (aMode == 6);  // tela curva grande, sem reprojetar
  // Both VR180 modes go through RemapVr180, which rebuilds them into the
  // above/below 360-wide layout StereoPanorama expects -- so the flag is the
  // same for 2, 4 and 5. Before the remap existed, 180-SBS borrowed the flat
  // stereo flag and 180-TB borrowed this one, which is why the two pairs were
  // indistinguishable in the headset (ADR-27).
  const bool isStereoPanorama = (aMode == 2) || (aMode == 4) || (aMode == 5);
  const bool isSideBySide = (aMode == 3);  // flat 3D only
  const bool isSpherical = isPanorama || isStereoPanorama;

  vr::VROverlay()->SetOverlayFlag(mOverlayHandle, vr::VROverlayFlags_Panorama,
                                  isPanorama);
  vr::VROverlay()->SetOverlayFlag(
      mOverlayHandle, vr::VROverlayFlags_StereoPanorama, isStereoPanorama);
  vr::VROverlay()->SetOverlayFlag(
      mOverlayHandle, vr::VROverlayFlags_SideBySide_Parallel, isSideBySide);

  // Curvatura: 0 e plano, 1 e cilindro fechado. Para 6 m de largura a ~3,8 m de
  // raio (openvr.h:3846: curvatura = largura / (2 PI r)).
  vr::VROverlay()->SetOverlayCurvature(mOverlayHandle,
                                       isCurvedCinema ? 0.25f : 0.0f);

  if (isCurvedCinema) {
    // Tela de cinema: larga, um pouco acima da linha dos olhos e afastada o
    // suficiente para caber no campo de visao. Presa a sala, nao a cabeca, para
    // o usuario poder olhar em volta.
    vr::VROverlay()->SetOverlayWidthInMeters(mOverlayHandle, 6.0f);
    vr::HmdMatrix34_t cinema = {{{1.0f, 0.0f, 0.0f, 0.0f},
                                 {0.0f, 1.0f, 0.0f, 1.7f},
                                 {0.0f, 0.0f, 1.0f, -3.4f}}};
    vr::VROverlay()->SetOverlayTransformAbsolute(
        mOverlayHandle, vr::TrackingUniverseStanding, &cinema);
  } else if (isSpherical) {
    // Wide and close, so the sphere fills the field of view. Tracked to the
    // head, so looking around moves through the content instead of off it.
    vr::VROverlay()->SetOverlayWidthInMeters(mOverlayHandle, 6.0f);
    vr::HmdMatrix34_t headRelative = {{{1.0f, 0.0f, 0.0f, 0.0f},
                                       {0.0f, 1.0f, 0.0f, 0.0f},
                                       {0.0f, 0.0f, 1.0f, -2.1f}}};
    vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(
        mOverlayHandle, vr::k_unTrackedDeviceIndex_Hmd, &headRelative);
  } else {
    // Back to the browsing panel: 2m wide at eye level, fixed in the room.
    // Stereo side-by-side sits further out, since the fused image reads as
    // closer than a flat one.
    vr::VROverlay()->SetOverlayWidthInMeters(mOverlayHandle,
                                             isSideBySide ? 4.0f : 2.0f);
    vr::HmdMatrix34_t roomFixed = {
        {{1.0f, 0.0f, 0.0f, 0.0f},
         {0.0f, 1.0f, 0.0f, 1.25f},
         {0.0f, 0.0f, 1.0f, isSideBySide ? -4.0f : -1.8f}}};
    vr::VROverlay()->SetOverlayTransformAbsolute(
        mOverlayHandle, vr::TrackingUniverseStanding, &roomFixed);
  }

  mCurrentProjection = aMode;
  AlignTitleBar();

  // Projection controls now live in the browser's media bar.
  DestroyControlsOverlay();
  printf_stderr(
      "[FxR-Modern-GPU] projection mode %u applied "
      "(panorama=%d stereoPanorama=%d sbs=%d)"
      "\n",
      aMode, (int)isPanorama, (int)isStereoPanorama, (int)isSideBySide);
}

// Brings up SteamVR and the overlay itself. Shared by both render paths: the
// overlay does not care whether its texture later comes from a swapchain
// backbuffer or from a texture we allocated ourselves.
bool FxROutputHandler::EnsureOverlay(uint32_t aWidth, uint32_t aHeight) {
  if (m_pHMD == nullptr) {
    vr::EVRInitError eError = vr::VRInitError_None;
    m_pHMD = vr::VR_Init(&eError, vr::VRApplication_Overlay);
    if (eError != vr::VRInitError_None || m_pHMD == nullptr) {
      printf_stderr("[FxR-Modern-GPU] VR_Init failed: %s\n",
                    vr::VR_GetVRInitErrorAsEnglishDescription(eError));
      m_pHMD = nullptr;
      return false;
    }

    IdentifyOverlayApplication();
    printf_stderr(
        "[FxR-Modern-GPU] VR_Init success as VRApplication_Overlay\n");
  }

  if (mOverlayHandle != vr::k_ulOverlayHandleInvalid) {
    return true;
  }

  if (vr::VROverlay() == nullptr) {
    printf_stderr("[FxR-Modern-GPU] VROverlay interface is null\n");
    return false;
  }

  vr::EVROverlayError err = vr::VROverlay()->CreateOverlay(
      "firefox.reality.overlay", "Tombriva VR", &mOverlayHandle);

  if (err == vr::VROverlayError_KeyInUse) {
    vr::VROverlayHandle_t existing = vr::k_ulOverlayHandleInvalid;
    if (vr::VROverlay()->FindOverlay("firefox.reality.overlay", &existing) ==
        vr::VROverlayError_None) {
      vr::VROverlay()->DestroyOverlay(existing);
    }
    err = vr::VROverlay()->CreateOverlay("firefox.reality.overlay", "Tombriva VR",
                                         &mOverlayHandle);
  }

  if (err != vr::VROverlayError_None) {
    printf_stderr("[FxR-Modern-GPU] CreateOverlay failed: %d\n", (int)err);
    mOverlayHandle = vr::k_ulOverlayHandleInvalid;
    return false;
  }

  printf_stderr("[FxR-Modern-GPU] CreateOverlay success, handle=0x%llx\n",
                (unsigned long long)mOverlayHandle);

  // Width: 2.0 meters wide
  vr::VROverlay()->SetOverlayWidthInMeters(mOverlayHandle, 2.0f);

  // Elevated to 1.25m eye level, 1.8m in front in standing tracking universe
  vr::HmdMatrix34_t transform = {{{1.0f, 0.0f, 0.0f, 0.0f},
                                  {0.0f, 1.0f, 0.0f, 1.25f},
                                  {0.0f, 0.0f, 1.0f, -1.8f}}};
  vr::VROverlay()->SetOverlayTransformAbsolute(
      mOverlayHandle, vr::TrackingUniverseStanding, &transform);

  // Enable laser pointer interaction and mouse input from VR controllers.
  // Without these, SteamVR emits no pointer events for the overlay at all.
  vr::VROverlay()->SetOverlayInputMethod(mOverlayHandle,
                                         vr::VROverlayInputMethod_Mouse);
  vr::VROverlay()->SetOverlayFlag(
      mOverlayHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible,
      true);
  vr::VROverlay()->SetOverlayFlag(
      mOverlayHandle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
  vr::VROverlay()->SetOverlaySortOrder(mOverlayHandle, 100);

  err = vr::VROverlay()->CreateDashboardOverlay("firefox.reality.dashboard",
                                                "Tombriva VR", &mDashboardHandle,
                                                &mDashboardIcon);
  if (err != vr::VROverlayError_None) {
    if (mDashboardHandle) vr::VROverlay()->DestroyOverlay(mDashboardHandle);
    if (mDashboardIcon) vr::VROverlay()->DestroyOverlay(mDashboardIcon);
    mDashboardHandle = mDashboardIcon = 0;
    mDocked = false;
    printf_stderr("[FxR window] dashboard creation failed: %d\n", int(err));
  } else {
    vr::VROverlay()->SetOverlayRaw(
        mDashboardIcon, const_cast<uint8_t*>(kFxRDashboardIconRGBA),
        kFxRDashboardIconSize, kFxRDashboardIconSize, 4);
    vr::VROverlay()->SetOverlayWidthInMeters(mDashboardHandle, 2.0f);
    vr::VROverlay()->SetOverlayInputMethod(mDashboardHandle,
                                           vr::VROverlayInputMethod_Mouse);
    vr::VROverlay()->SetOverlayFlag(
        mDashboardHandle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
  }
  SetMouseScale(aWidth, aHeight);
  UpdatePanelVisibility();
  printf_stderr(
      "[FxR-Modern-GPU] ShowOverlay active at eye level (y=1.25m, z=-1.8m), "
      "mouse scale %ux%u\n",
      mOverlayWidth, mOverlayHeight);

  EnsureTitleBar();
  AlignTitleBar();
  UpdatePanelVisibility();
  StartInputPump();
  return true;
}

// Report pointer coordinates in texture pixels. FxRWindowManager scales these
// to the widget's client area when it dispatches, so this just has to match
// whatever texture we are actually submitting.
void FxROutputHandler::SetMouseScale(uint32_t aWidth, uint32_t aHeight) {
  if (aWidth == 0 || aHeight == 0 ||
      mOverlayHandle == vr::k_ulOverlayHandleInvalid) {
    return;
  }
  mOverlayWidth = aWidth;
  mOverlayHeight = aHeight;
  vr::HmdVector2_t vecWindowSize = {(float)aWidth, (float)aHeight};
  vr::VROverlay()->SetOverlayMouseScale(mOverlayHandle, &vecWindowSize);
  if (mDashboardHandle) {
    vr::VROverlay()->SetOverlayMouseScale(mDashboardHandle, &vecWindowSize);
  }

  // A mascara de intersecao foi REMOVIDA em 10/09/2026. Fora adicionada para
  // fazer o cursor do laser aparecer em todo o painel e nunca teve efeito
  // comprovado: o SteamVR a aceitava e o sintoma continuava. Como ela restringe
  // onde o SteamVR considera haver intersecao, e o sintoma era exatamente o
  // cursor faltando em partes do painel, ela era a suspeita mais provavel.
  // Sem mascara o overlay inteiro e interativo, que e o comportamento padrao.
}

bool FxROutputHandler::TryInitialize(IDXGISwapChain* aSwapChain,
                                     ID3D11Device* aDevice) {
  if (mSwapChain == nullptr) {
    uint32_t width = 0;
    uint32_t height = 0;
    ID3D11Texture2D* texInit = nullptr;
    if (SUCCEEDED(aSwapChain->GetBuffer(0, IID_PPV_ARGS(&texInit))) &&
        texInit) {
      D3D11_TEXTURE2D_DESC desc;
      texInit->GetDesc(&desc);
      width = desc.Width;
      height = desc.Height;
      texInit->Release();
    }

    if (!EnsureOverlay(width, height)) {
      return false;
    }

    mSwapChain = aSwapChain;
  } else {
    MOZ_ASSERT(aSwapChain == mSwapChain);
  }

  return mSwapChain != nullptr && aSwapChain == mSwapChain;
}

// Offscreen path: the compositor renders into a texture it owns and hands it
// here each frame. The texture is replaced on resize, so re-check it every
// time rather than latching it once.
bool FxROutputHandler::TryInitializeOffscreen(ID3D11Texture2D* aTexture,
                                              uint32_t aWidth,
                                              uint32_t aHeight) {
  if (aTexture == nullptr) {
    return false;
  }

  if (!EnsureOverlay(aWidth, aHeight)) {
    return false;
  }

  if (aWidth != mOverlayWidth || aHeight != mOverlayHeight) {
    SetMouseScale(aWidth, aHeight);
  }

  mOffscreenTexture = aTexture;
  return true;
}

void FxROutputHandler::StartInputPump() {
  if (mInputPumpThread != nullptr) {
    return;
  }

  mInputPumpActive = true;

  nsCOMPtr<nsIThread> thread;
  nsresult rv = NS_NewNamedThread("FxROverlayInput", getter_AddRefs(thread));
  if (NS_FAILED(rv) || !thread) {
    printf_stderr("[FxR-Modern-GPU] failed to create overlay input thread\n");
    mInputPumpActive = false;
    return;
  }

  mInputPumpThread = thread;
  mInputPumpThread->Dispatch(
      NS_NewRunnableFunction("FxROverlayInputPump",
                             [this]() { RunInputPump(); }),
      nsIEventTarget::DISPATCH_NORMAL);

  printf_stderr("[FxR-Modern-GPU] overlay input pump started\n");
}

void FxROutputHandler::StopInputPump() {
  if (mInputPumpThread == nullptr) {
    return;
  }

  // Drop out of RunInputPump's loop, then wait for it to finish before the
  // caller tears the overlay down underneath it.
  mInputPumpActive = false;
  mInputPumpThread->Shutdown();
  mInputPumpThread = nullptr;

  printf_stderr("[FxR-Modern-GPU] overlay input pump stopped\n");
}

// Runs on the dedicated input thread. Polling has to be off the Renderer
// thread: UpdateOutput only runs when WebRender composites a frame, so on an
// idle page the overlay's event queue would go undrained for tens of seconds
// and then flush in a burst at coordinates the user has long since moved away
// from. This is also the only place allowed to poll -- OpenVR does not support
// concurrent PollNextOverlayEvent from several threads.
void FxROutputHandler::RunInputPump() {
  ForwardWindowState(mDocked ? 0 : 3);
  while (mInputPumpActive) {
    if (mOverlayHandle == vr::k_ulOverlayHandleInvalid ||
        vr::VROverlay() == nullptr) {
      break;
    }

    vr::VREvent_t sysEvent;
    while (m_pHMD->PollNextEvent(&sysEvent, sizeof(sysEvent))) {
      if (sysEvent.eventType == vr::VREvent_Quit) RequestAppQuit();
      if (sysEvent.eventType == vr::VREvent_DashboardActivated) EndDragPanel();
    }
    const int visibility = mPendingVisibility.exchange(0);
    const bool returningFromImmersive =
        visibility > 0 && mImmersiveSessionActive;
    if (visibility) {
      EndDragPanel();
      vr::VROverlay()->HideKeyboard();
      mImmersiveSessionActive = visibility < 0;
      UpdatePanelVisibility();
    }
    const int dock = sPendingWindowDocked.exchange(0);
    if (dock) SetDocked(dock > 0);
    const uint32_t projection = mPendingProjection.exchange(0);
    if (projection) ApplyProjectionMode(projection - 1);
    if (returningFromImmersive && !mClosing) {
      if (mDocked) {
        mDashboardOpened = false;
      } else {
        RecenterPanel();
      }
    }
    if (mDocked && !mDashboardOpened && mDashboardTextureReady &&
        !mImmersiveSessionActive && !mClosing) {
      vr::VROverlay()->ShowDashboard("firefox.reality.dashboard");
      mDashboardOpened = true;
      ForwardWindowState(0);
    }
    const bool dashboardActive =
        mDashboardHandle &&
        vr::VROverlay()->IsActiveDashboardOverlay(mDashboardHandle);
    const bool dashboardVisible = vr::VROverlay()->IsDashboardVisible();
    if (dashboardVisible != mDashboardVisible ||
        dashboardActive != mWasDashboardActive) {
      EndDragPanel();
      vr::VROverlay()->HideKeyboard();
      mDashboardVisible = dashboardVisible;
      mWasDashboardActive = dashboardActive;
      UpdatePanelVisibility();
    }

    const int keyboard = mPendingKeyboard.exchange(0);
    if (keyboard == 1 && !mClosing && !mImmersiveSessionActive) {
      vr::VROverlay()->ShowKeyboardForOverlay(
          InputOverlay(), vr::k_EGamepadTextInputModeNormal,
          vr::k_EGamepadTextInputLineModeSingleLine, vr::KeyboardFlag_Minimal,
          "Tombriva VR", 256, "", 0);
      vr::HmdRect2_t rect = {{0.0f, (float)mOverlayHeight},
                             {(float)mOverlayWidth, 0.0f}};
      vr::VROverlay()->SetKeyboardPositionForOverlay(InputOverlay(), rect);
    } else if (keyboard == -1) {
      vr::VROverlay()->HideKeyboard();
    }

    // Ease the move icon toward its target size. The pump already ticks about
    // every 4 ms, so a 0.18 step lands the animation in roughly 60 ms -- quick
    // enough to feel like a button press, slow enough to be seen.
    // Anima o botao encolhendo o PROPRIO OVERLAY, nao a textura.
    //
    // A versao anterior repintava com SetOverlayRaw a cada passo e piscava em
    // qualquer taxa -- inclusive limitada a 30 Hz -- porque cada upload faz o
    // SteamVR reconstruir a textura do overlay. SetOverlayWidthInMeters mexe
    // so na transformacao, entao a animacao fica continua e sem flash.
    if (mTitleBarHandle != vr::k_ulOverlayHandleInvalid &&
        fabsf(mIconScale - mIconScaleTarget) > 0.005f) {
      mIconScale += (mIconScaleTarget - mIconScale) * 0.25f;
      if (fabsf(mIconScale - mIconScaleTarget) <= 0.005f) {
        mIconScale = mIconScaleTarget;
      }
      vr::VROverlay()->SetOverlayWidthInMeters(
          mTitleBarHandle,
          ([&]() {
            float width = 2.0f;
            vr::VROverlay()->GetOverlayWidthInMeters(mOverlayHandle, &width);
            return mOverlayWidth
                       ? width * mOverlayHeight / mOverlayWidth * 0.06f
                       : 0.06f;
          })() *
              mIconScale);
    }

    // Title bar: grab/release drives the panel move. Its own queue, resolved
    // here, with no effect on the panel's input path.
    if (mTitleBarHandle != vr::k_ulOverlayHandleInvalid) {
      vr::VREvent_t barEvent;
      while (vr::VROverlay()->PollNextOverlayEvent(mTitleBarHandle, &barEvent,
                                                   sizeof(barEvent))) {
        if (barEvent.eventType == vr::VREvent_MouseButtonDown) {
          // SÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â³ o
          // ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â­cone arrasta: o resto da
          // barra ÃƒÆ’Ã†â€™Ãƒâ€ Ã¢â‚¬â„¢ÃƒÆ’Ã¢â‚¬Å¡Ãƒâ€šÃ‚Â© moldura.
          if (barEvent.data.mouse.button == vr::VRMouseButton_Left)
            BeginDragPanel(barEvent.trackedDeviceIndex);
        } else if (barEvent.eventType == vr::VREvent_MouseButtonUp) {
          EndDragPanel();
        }
      }
    }

    // Controls overlay: its own queue, drained here and resolved here. Kept
    // strictly separate from the panel's queue below -- nothing about the
    // existing path changes, which is the point of this design.
    if (mControlsHandle != vr::k_ulOverlayHandleInvalid) {
      vr::VREvent_t ctlEvent;
      while (vr::VROverlay()->PollNextOverlayEvent(mControlsHandle, &ctlEvent,
                                                   sizeof(ctlEvent))) {
        if (ctlEvent.eventType == vr::VREvent_MouseMove) {
          HandleControlsHover(ctlEvent.data.mouse.x);
        } else if (ctlEvent.eventType == vr::VREvent_MouseButtonUp) {
          HandleControlsClick(ctlEvent.data.mouse.x);
          break;  // ApplyProjectionMode pode destruir este overlay
        }
      }
    }

    // Drain the overlay's event queue. Everything interesting is forwarded to
    // FxRWindowManager, which hops to the UI thread and dispatches it through
    // nsWindow so that Gecko sees genuine mouse/keyboard events.
    uint32_t drained = 0;
    vr::VREvent_t vrEvent;
    for (auto inputHandle : {mOverlayHandle, mDashboardHandle}) {
      if (!inputHandle) continue;
      while (vr::VROverlay()->PollNextOverlayEvent(inputHandle, &vrEvent,
                                                   sizeof(vrEvent))) {
        if (vrEvent.eventType == vr::VREvent_Quit) {
          RequestAppQuit();
          continue;
        }
        if (mClosing || mImmersiveSessionActive ||
            inputHandle != InputOverlay())
          continue;
        switch (vrEvent.eventType) {
          case vr::VREvent_MouseMove:
          case vr::VREvent_MouseButtonDown:
          case vr::VREvent_MouseButtonUp:
          case vr::VREvent_ScrollDiscrete:
          case vr::VREvent_KeyboardCharInput:
          case vr::VREvent_KeyboardDone:
          case vr::VREvent_KeyboardClosed:
          case vr::VREvent_OverlayFocusChanged:
            break;
          case vr::VREvent_ButtonUnpress:
            // Forwarded only as a safety net for releasing the mouse button.
            // SteamVR does not always follow a VREvent_MouseButtonDown with the
            // matching Up, which leaves Gecko stuck mid-drag. ButtonPress is
            // still ignored: it carries no overlay position, so it cannot start
            // a click, but a release does not need one.
            break;
          default:
            continue;
        }

        FxROverlayInputEvent event;
        event.mType = vrEvent.eventType;
        event.mX = vrEvent.data.mouse.x;
        event.mY = vrEvent.data.mouse.y;
        event.mButton = vrEvent.data.mouse.button;
        event.mScrollYDelta = vrEvent.data.scroll.ydelta;
        event.mOverlayW = mOverlayWidth;
        event.mOverlayH = mOverlayHeight;
        memcpy(event.mKeyboardInput, vrEvent.data.keyboard.cNewInput,
               sizeof(event.mKeyboardInput));

        // Resolve focus here, where the overlay handle is meaningful; only a
        // bool needs to reach the UI process.
        if (vrEvent.eventType == vr::VREvent_OverlayFocusChanged) {
          event.mFocused = vrEvent.data.overlay.overlayHandle == InputOverlay();
        }

        ForwardOverlayInput(event);
        drained++;
      }
    }

    // Heartbeat: proves whether the pump is still polling. If this stops after
    // a click, the pump died and that alone explains every event vanishing.
    {
      static uint32_t sTicks = 0;
      static uint32_t sEvents = 0;
      sEvents += drained;
      if (++sTicks % 1250 == 0) {  // ~5 s at 4 ms
        printf_stderr("[FxR-Modern-GPU] pump alive: %u events in last 5s\n",
                      sEvents);
        sEvents = 0;
      }
    }

    // ~250 Hz. Smooth for the laser pointer without spinning a core; the
    // original code yielded with Sleep(0), which is needlessly hot.
    ::Sleep(4);
  }
}

// Rebuilds a VR180 frame into the above/below, 360-degree-wide layout that
// VROverlayFlags_StereoPanorama expects. Plain region copies, no shader: the
// operation is a reposition, not a resample.
//
//   180-SBS source            destination (StereoPanorama)
//   +--------+--------+       +-----------------+
//   |   L    |   R    |  ->   | black| L |black |   <- upper half, left eye
//   +--------+--------+       +-----------------+
//                             | black| R |black |   <- lower half, right eye
//                             +-----------------+
//
// The content sits in the middle half horizontally, so 180 degrees of image
// occupy 180 degrees of sphere in front of the viewer, with nothing behind.
ID3D11Texture2D* FxROutputHandler::RemapVr180(ID3D11DeviceContext* aCtx,
                                              ID3D11Texture2D* aSource) {
  if (aCtx == nullptr || aSource == nullptr) {
    return nullptr;
  }

  D3D11_TEXTURE2D_DESC srcDesc = {};
  aSource->GetDesc(&srcDesc);

  // Each eye ends up covering 360 degrees, so it needs twice the width of the
  // 180 degrees it actually has.
  uint32_t eyeW = 0, eyeH = 0, dstW = 0, dstH = 0;
  // Regiao da fonte a copiar, e onde ela cai no destino.
  uint32_t srcX = 0, srcY = 0, srcW = srcDesc.Width, srcH = srcDesc.Height;

  if (mCurrentProjection == 1 || mCurrentProjection == 2) {
    // 360: equirretangular e 2:1 (360 graus por 180). A janela do navegador e
    // 16:9, entao entregar a textura inteira ao Panorama estica a imagem na
    // vertical -- o resultado e a imagem "retorcida". Recorta-se a maior faixa
    // 2:1 centrada, que e onde o video fica quando exibido em tela cheia com
    // barras. Para 360 estereo o conteudo e duas faixas dessas empilhadas, ou
    // seja 1:1 no total.
    // mono e 2:1; estereo sao duas faixas 2:1 empilhadas, logo 1:1
    const double wantAspect = (mCurrentProjection == 1) ? 2.0 : 1.0;
    uint32_t cropW = srcDesc.Width;
    uint32_t cropH = (uint32_t)(srcDesc.Width / wantAspect);
    if (cropH > srcDesc.Height) {
      cropH = srcDesc.Height;
      cropW = (uint32_t)(srcDesc.Height * wantAspect);
    }
    srcX = (srcDesc.Width - cropW) / 2;
    srcY = (srcDesc.Height - cropH) / 2;
    srcW = cropW;
    srcH = cropH;
    dstW = cropW;
    dstH = cropH;
    eyeW = cropW;
    eyeH = cropH;
  } else if (mCurrentProjection == 4) {  // VR180 side by side
    eyeW = srcDesc.Width / 2;
    eyeH = srcDesc.Height;
    dstW = eyeW * 2;
    dstH = eyeH * 2;
  } else if (mCurrentProjection == 5) {  // VR180 top and bottom
    eyeW = srcDesc.Width;
    eyeH = srcDesc.Height / 2;
    dstW = eyeW * 2;
    dstH = eyeH * 2;
  } else {
    return nullptr;
  }
  if (eyeW == 0 || eyeH == 0 || dstW == 0 || dstH == 0) {
    return nullptr;
  }

  if (!mVr180Texture || mVr180SrcWidth != srcDesc.Width ||
      mVr180SrcHeight != srcDesc.Height || mVr180Mode != mCurrentProjection) {
    mVr180Texture = nullptr;

    RefPtr<ID3D11Device> device;
    aCtx->GetDevice(getter_AddRefs(device));
    if (!device) {
      return nullptr;
    }

    // MISC_SHARED is mandatory: vrcompositor.exe opens this texture from
    // another process. Leaving it out is what crashed the offscreen path
    // earlier in this project (ADR-14).
    CD3D11_TEXTURE2D_DESC desc(
        srcDesc.Format, dstW, dstH, 1, 1,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
        D3D11_USAGE_DEFAULT, 0, 1, 0, D3D11_RESOURCE_MISC_SHARED);
    RefPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, getter_AddRefs(tex)))) {
      printf_stderr("[FxR-Modern-GPU] VR180: CreateTexture2D falhou (%ux%u)\n",
                    dstW, dstH);
      return nullptr;
    }
    mVr180Texture = tex;
    mVr180SrcWidth = srcDesc.Width;
    mVr180SrcHeight = srcDesc.Height;
    mVr180Mode = mCurrentProjection;
    printf_stderr(
        "[FxR-Modern-GPU] VR180: destino %ux%u para fonte %ux%u (modo %u)\n",
        dstW, dstH, srcDesc.Width, srcDesc.Height,
        uint32_t(mCurrentProjection));
  }

  // Clear to black every frame: the regions outside the 180 degrees of content
  // must stay empty, and the texture is reused across frames.
  RefPtr<ID3D11Device> device;
  aCtx->GetDevice(getter_AddRefs(device));
  RefPtr<ID3D11RenderTargetView> rtv;
  if (device && SUCCEEDED(device->CreateRenderTargetView(
                    mVr180Texture, nullptr, getter_AddRefs(rtv)))) {
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    aCtx->ClearRenderTargetView(rtv, black);
  }

  if (mCurrentProjection == 1 || mCurrentProjection == 2) {
    // 360: so o recorte, sem padding -- o conteudo ja cobre a volta inteira.
    D3D11_BOX box = {srcX, srcY, 0, srcX + srcW, srcY + srcH, 1};
    aCtx->CopySubresourceRegion(mVr180Texture, 0, 0, 0, 0, aSource, 0, &box);
  } else {
    // VR180: metade da largura do destino, centrada, e o resto preto.
    const uint32_t xOff = eyeW / 2;

    D3D11_BOX leftBox = {}, rightBox = {};
    if (mCurrentProjection == 4) {  // SBS: olhos lado a lado na fonte
      leftBox = {0, 0, 0, eyeW, eyeH, 1};
      rightBox = {eyeW, 0, 0, eyeW * 2, eyeH, 1};
    } else {  // TB: olhos empilhados na fonte
      leftBox = {0, 0, 0, eyeW, eyeH, 1};
      rightBox = {0, eyeH, 0, eyeW, eyeH * 2, 1};
    }

    aCtx->CopySubresourceRegion(mVr180Texture, 0, xOff, 0, 0, aSource, 0,
                                &leftBox);
    aCtx->CopySubresourceRegion(mVr180Texture, 0, xOff, eyeH, 0, aSource, 0,
                                &rightBox);
  }

  return mVr180Texture;
}

void FxROutputHandler::UpdateOutput(ID3D11DeviceContext* aCtx) {
  if (mOverlayHandle == vr::k_ulOverlayHandleInvalid) {
    return;
  }

  // Submit the current D3D11 frame to the OpenVR overlay. In offscreen mode the
  // compositor owns the texture and handed it to us; otherwise we borrow the
  // swapchain's backbuffer and have to release our reference afterwards.
  ID3D11Texture2D* texOrig = nullptr;
  bool ownsReference = false;
  if (mOffscreenTexture) {
    texOrig = mOffscreenTexture;
  } else if (mSwapChain) {
    if (SUCCEEDED(mSwapChain->GetBuffer(0, IID_PPV_ARGS(&texOrig)))) {
      ownsReference = true;
    }
  }

  if (texOrig != nullptr) {
    // Only the two VR180 modes take the remap path; every other mode submits
    // exactly what it submitted before.
    ID3D11Texture2D* texToSubmit = texOrig;
    if (mCurrentProjection == 1 || mCurrentProjection == 2 ||
        mCurrentProjection == 4 || mCurrentProjection == 5) {
      if (ID3D11Texture2D* remapped = RemapVr180(aCtx, texOrig)) {
        texToSubmit = remapped;
      }
    }

    vr::Texture_t overlayTextureDX11 = {texToSubmit, vr::TextureType_DirectX,
                                        vr::ColorSpace_Auto};

    vr::EVROverlayError err =
        vr::VROverlay()->SetOverlayTexture(mOverlayHandle, &overlayTextureDX11);
    if (mDashboardHandle) {
      vr::Texture_t dashboardTexture = {texOrig, vr::TextureType_DirectX,
                                        vr::ColorSpace_Auto};
      const auto dashboardError = vr::VROverlay()->SetOverlayTexture(
          mDashboardHandle, &dashboardTexture);
      if (dashboardError == vr::VROverlayError_None)
        mDashboardTextureReady = true;
    }
    if (err != vr::VROverlayError_None) {
      static int sErrCount = 0;
      if (sErrCount++ < 5) {
        printf_stderr("[FxR-Modern-GPU] SetOverlayTexture failed: %d\n",
                      (int)err);
      }
    }

    if (ownsReference) {
      texOrig->Release();
    }

    // Frame pacing. UpdateOutput only runs when WebRender composites, so a long
    // gap means the headset is showing a stale image regardless of input.
    if (err == vr::VROverlayError_None) {
      mozilla::TimeStamp now = mozilla::TimeStamp::Now();
      if (!mLastSubmit.IsNull()) {
        double gapMs = (now - mLastSubmit).ToMilliseconds();
        if (gapMs > 500.0) {
          printf_stderr(
              "[FxR-Modern-GPU] frame stall: %.0f ms with no new frame\n",
              gapMs);
        }
      }
      mLastSubmit = now;
    }
  }
}
