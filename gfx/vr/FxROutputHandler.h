/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#pragma once

#include <cstdint>
#include <windows.h>
#include <d3d11_1.h>
#include "mozilla/Atomics.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/RefPtr.h"
#include "nsCOMPtr.h"
#include "nsIThread.h"

struct ID3D11Texture2D;
struct IDXGISwapChain;
struct ID3D11DeviceContext;
struct ID3D11Device;

namespace vr {
class IVRSystem;
typedef uint64_t VROverlayHandle_t;
// Mesmos tipos do openvr.h, repetidos aqui para nao arrastar o header
// inteiro (que puxa windows.h) para quem inclui este arquivo.
typedef uint32_t TrackedDeviceIndex_t;
}  // namespace vr

// Owns the SteamVR overlay that the browser window is presented on: creates it,
// submits the compositor's backbuffer to it, and pumps its input events.
//
// Two threads are involved:
//  - The Renderer thread calls TryInitialize/UpdateOutput and owns the texture
//    submission. It does NOT poll input.
//  - A dedicated input pump thread drains the overlay's event queue. This has
//    to be separate: UpdateOutput only runs when WebRender composites a frame,
//    so on a static page input would sit in SteamVR's queue for tens of seconds
//    and then arrive in a burst at stale coordinates. (The original Firefox
//    Reality code used a dedicated thread for exactly this reason.) OpenVR also
//    requires that only one thread polls, so the poll lives here alone.
//
// It does not turn overlay input into Gecko events itself -- it hands each
// event to FxRWindowManager, which marshals it to the UI thread and dispatches
// it through nsWindow. In particular it must never synthesize Windows mouse
// messages or move the desktop cursor: modern Gecko drops synthetic pointer
// input that doesn't match a real OS pointer state, so that approach produces
// neither :hover nor a real click.
class FxROutputHandler final {
 public:
  FxROutputHandler();
  ~FxROutputHandler();

  bool TryInitialize(IDXGISwapChain* aSwapChain, ID3D11Device* aDevice);

  // Offscreen path: the compositor rendered into a texture it owns and passes
  // it in each frame, so the overlay no longer depends on a window's swapchain.
  bool TryInitializeOffscreen(ID3D11Texture2D* aTexture, uint32_t aWidth,
                              uint32_t aHeight);

  void UpdateOutput(ID3D11DeviceContext* aCtx);

  // Called from the UI thread (FxRWindowManager::NotifyEditableFocus) to raise
  // or dismiss the SteamVR virtual keyboard. The request is picked up by the
  // input pump thread, since only it may touch the overlay handle.
  static void RequestShowKeyboard(bool aShow);

  // Hides the overlay for the duration of an immersive WebXR session, and
  // restores it on exit. Called by VRManager::Start/StopPresentation, which
  // runs in this same process, so no IPC is involved.
  //
  // This is not cosmetic. The overlay and the VR process are separate
  // processes of the same firefox.exe, so SteamVR derives the same appkey
  // (system.generated.firefox.exe) for both. When the VR process connects as
  // VRApplication_Scene, SteamVR reassigns that appkey to it and the scene
  // dies moments later: measured 4 frames with the overlay up, 4019 without.
  //
  // Like the keyboard request, this is serviced by the input pump thread,
  // the only one allowed to touch the overlay handle.
  static void RequestSetVisible(bool aVisible);
  static bool HasActiveHandler();
  static void RequestDocked(bool aDocked);

  // Media projection (flat / 360 / stereo). Value is FxRProjectionMode.
  // Reaches here from chrome JS via FxRWindowManager and PGPU, and like
  // the others is applied by the input pump thread.
  static void RequestProjectionMode(uint32_t aMode);

 private:
  bool EnsureOverlay(uint32_t aWidth, uint32_t aHeight);
  void SetDocked(bool aDocked);
  void UpdatePanelVisibility();
  void AlignTitleBar();
  void RecenterPanel();
  vr::VROverlayHandle_t InputOverlay() const;
  void RequestAppQuit();

  // Claims a SteamVR app key distinct from the VR process's (ADR-23).
  void IdentifyOverlayApplication();

  void ApplyProjectionMode(uint32_t aMode);

  // Controls overlay, shown while a non-2D projection is active.
  //
  // Drawn straight into a pixel buffer and handed to SteamVR with
  // SetOverlayRaw, deliberately NOT a second browser window. A window would
  // mean a second compositor, a second handler and input crossing to the
  // parent process -- and that routing is what broke the main panel's input on
  // the first attempt. Here everything stays in the GPU process: the click is
  // resolved from the overlay's own coordinates and applied by calling
  // ApplyProjectionMode directly.
  void EnsureControlsOverlay();
  void DestroyControlsOverlay();
  void HandleControlsClick(float aX);

  // Repaints the controls strip, highlighting the button under the laser.
  // SteamVR does not reliably draw its cursor splat over our overlays, so the
  // strip draws its own hover feedback instead of chasing that.
  void PaintControls(int32_t aHighlight);

  // Title bar above the panel: grab it with the trigger to move the panel
  // around the room. While held, the panel is parented to the controller, so
  // it follows the hand; on release it is re-anchored where it ended up.
  void EnsureTitleBar();
  void PaintTitleBar(float aScale);
  void BeginDragPanel(vr::TrackedDeviceIndex_t aDevice);
  void EndDragPanel();
  void HandleControlsHover(float aX);

  // Rebuilds a VR180 source into the layout StereoPanorama expects.
  //
  // OpenVR has no 180 flag: StereoPanorama wants the two eyes stacked
  // above/below, each covering a full 360 degrees. A VR180 source covers half
  // that, so the content is centred horizontally and the rest left black --
  // which is what makes the image land where the user is actually looking
  // instead of being stretched across the whole sphere (ADR-27, ADR-29).
  //
  // Returns the remapped texture, or nullptr to submit the source unchanged.
  ID3D11Texture2D* RemapVr180(ID3D11DeviceContext* aCtx,
                              ID3D11Texture2D* aSource);
  void SetMouseScale(uint32_t aWidth, uint32_t aHeight);
  void StartInputPump();
  void StopInputPump();
  void RunInputPump();

  vr::IVRSystem* m_pHMD;
  vr::VROverlayHandle_t mOverlayHandle;
  vr::VROverlayHandle_t mDashboardHandle = 0;
  vr::VROverlayHandle_t mDashboardIcon = 0;
  mozilla::Atomic<bool> mDashboardTextureReady{false};
  bool mDashboardOpened = false;
  bool mWasDashboardActive = false;
  bool mDashboardVisible = false;
  bool mDocked = true;
  bool mClosing = false;
  RefPtr<IDXGISwapChain> mSwapChain;

  // Set only in offscreen mode; owned by RenderCompositorANGLE, which replaces
  // it on resize.
  RefPtr<ID3D11Texture2D> mOffscreenTexture;

  // Size of the overlay texture, which is also the mouse-coordinate space that
  // OpenVR reports pointer events in (see SetOverlayMouseScale).
  uint32_t mOverlayWidth;
  uint32_t mOverlayHeight;

  // 1 = show keyboard, -1 = hide, 0 = nothing pending.
  mozilla::Atomic<int> mPendingKeyboard;

  // 1 = show overlay, -1 = hide, 0 = nothing pending.
  mozilla::Atomic<int> mPendingVisibility;
  // Etapa 1C-d. Verdadeiro entre o pedido de ocultar (inicio da sessao
  // imersiva) e o de mostrar (fim). Tocado so pela thread de pump. Enquanto
  // verdadeiro, nenhum overlay interativo e reexibido: a barra de arrastar e a
  // de controles usam VROverlayFlags_MakeOverlaysInteractiveIfVisible, que
  // liga o laser do sistema e tira a entrada dos controles da cena WebXR
  // (medido na 1C-c: IsInputAvailable=0 e bActive=0 durante a sessao inteira).
  bool mImmersiveSessionActive = false;

  // FxRProjectionMode + 1, so that 0 can mean "nothing pending".
  mozilla::Atomic<uint32_t> mPendingProjection;

  // Projecao em vigor, para saber se ha de onde sair. Tocado so pela
  // thread de pump.
  mozilla::Atomic<uint32_t> mCurrentProjection;

  // Separate overlay for the controls. Lives only while a projection is
  // active; zero means it does not exist.
  vr::VROverlayHandle_t mControlsHandle;

  // Destination for the VR180 remap. Allocated on demand and reused;
  // needs MISC_SHARED because vrcompositor.exe is a separate process.
  // Button currently under the laser, -1 for none. Avoids repainting the
  // strip on every mouse-move sample.
  int32_t mControlsHover;

  // Title bar overlay and drag state. mDragDevice is the controller
  // currently holding the panel, or k_unTrackedDeviceIndexInvalid.
  vr::VROverlayHandle_t mTitleBarHandle;
  vr::TrackedDeviceIndex_t mDragDevice;

  // Press animation for the move icon: mIconScale eases toward
  // mIconScaleTarget on the pump thread, which already ticks ~250 Hz.
  float mIconScale;
  float mIconScaleTarget;

  RefPtr<ID3D11Texture2D> mVr180Texture;
  uint32_t mVr180SrcWidth;
  uint32_t mVr180SrcHeight;
  uint32_t mVr180Mode;

  // Frame-submission diagnostics: the overlay only updates when WebRender
  // composites, so a stall here means a frozen image in the headset even
  // while input keeps flowing.
  mozilla::TimeStamp mLastSubmit;

  nsCOMPtr<nsIThread> mInputPumpThread;
  mozilla::Atomic<bool> mInputPumpActive;
};
