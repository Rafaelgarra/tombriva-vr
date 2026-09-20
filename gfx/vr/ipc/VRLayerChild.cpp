#ifndef VRSUBMIT_LOG_DEFINED
#  define VRSUBMIT_LOG_DEFINED
#  include "mozilla/Logging.h"
// Diagnostico da submissao de quadros WebXR. MOZ_LOG e nao fopen: a macro
// artesanal anterior gravava direto num caminho absoluto, o que o sandbox do
// processo de conteudo bloqueia -- o log ficava cego justamente no processo que
// mais precisavamos observar. MOZ_LOG funciona em todos os processos.
static mozilla::LazyLogModule gVRSubmitLog("VRSubmit");
#  define VRSUBMIT_LOG(...) MOZ_LOG(gVRSubmitLog, mozilla::LogLevel::Info, (__VA_ARGS__))
#  define VRSUBMIT_LOG_VERBOSE(...) MOZ_LOG(gVRSubmitLog, mozilla::LogLevel::Verbose, (__VA_ARGS__))
#endif
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "VRLayerChild.h"

#include "mozilla/dom/HTMLCanvasElement.h"
#include "mozilla/layers/ImageBridgeChild.h"
#include "mozilla/layers/LayersMessages.h"  // for TimedTexture
#include "mozilla/layers/SyncObject.h"      // for SyncObjectClient
#include "mozilla/StaticPrefs_dom.h"
#include "mozilla/StaticPrefs_webgl.h"

#include "ClientWebGLContext.h"
#include "gfxPlatform.h"
#include "GLContext.h"
#include "GLScreenBuffer.h"
#include "SharedSurface.h"    // for SharedSurface
#include "SharedSurfaceGL.h"  // for SharedSurface

namespace mozilla::gfx {

VRLayerChild::VRLayerChild() { MOZ_COUNT_CTOR(VRLayerChild); }

VRLayerChild::~VRLayerChild() {
  ClearSurfaces();

  MOZ_COUNT_DTOR(VRLayerChild);
}

void VRLayerChild::Initialize(dom::HTMLCanvasElement* aCanvasElement,
                              const gfx::Rect& aLeftEyeRect,
                              const gfx::Rect& aRightEyeRect) {
  MOZ_ASSERT(aCanvasElement);
  mLeftEyeRect = aLeftEyeRect;
  mRightEyeRect = aRightEyeRect;
  mCanvasElement = aCanvasElement;
}

void VRLayerChild::SetXRFramebuffer(WebGLFramebufferJS* fb) {
  mFramebuffer = fb;
}

static constexpr bool kIsAndroid =
#if defined(MOZ_WIDGET_ANDROID)
    true;
#else
    false;
#endif

void VRLayerChild::SubmitFrame(const VRDisplayInfo& aDisplayInfo) {
  VRSUBMIT_LOG_VERBOSE("[VRLayerChild::SubmitFrame] frameId=%llu, lastSubmitted=%llu",
         (unsigned long long)aDisplayInfo.GetFrameId(), (unsigned long long)mLastSubmittedFrameId);
  uint64_t frameId = aDisplayInfo.GetFrameId();

  // aFrameId will not increment unless the previuosly submitted
  // frame was received by the VR thread and submitted to the VR
  // compositor.  We early-exit here in the event that SubmitFrame
  // was called twice for the same aFrameId.
  if (!mCanvasElement || frameId == mLastSubmittedFrameId) {
    VRSUBMIT_LOG_VERBOSE("[4/5] VRLayerChild::SubmitFrame BAILOU: canvas=%p frameId=%llu "
                 "lastSubmitted=%llu",
                 (void*)mCanvasElement.get(), (unsigned long long)frameId,
                 (unsigned long long)mLastSubmittedFrameId);
    return;
  }

  const auto& webgl = mCanvasElement->GetWebGLContext();
  if (!webgl) {
    VRSUBMIT_LOG("[5] VRLayerChild::SubmitFrame BAILOU: sem WebGLContext");
    return;
  }

  // Keep the SharedSurfaceTextureClient alive long enough for
  // 1 extra frame, accomodating overlapped asynchronous rendering.
  mLastFrameTextureDesc = mThisFrameTextureDesc;

  bool getNewFrame = true;
  if (kIsAndroid) {
    /**
     * Do not blit WebGL to a SurfaceTexture until the last submitted frame is
     * already processed and the new frame poses are ready. SurfaceTextures need
     * to be released in the VR render thread in order to allow to be used again
     * in the WebGLContext GLScreenBuffer producer. Not doing so causes some
     * freezes, crashes or other undefined behaviour.
     */
    getNewFrame = (!mThisFrameTextureDesc ||
                   aDisplayInfo.mDisplayState.lastSubmittedFrameId ==
                       mLastSubmittedFrameId);
  }
  if (getNewFrame) {
    const RefPtr<layers::ImageBridgeChild> imageBridge =
        layers::ImageBridgeChild::GetSingleton();

    auto texType = layers::TextureType::Unknown;
    if (imageBridge) {
      texType = layers::PreferredCanvasTextureType(imageBridge);
    }
    if (kIsAndroid && StaticPrefs::webgl_enable_surface_texture()) {
      texType = layers::TextureType::AndroidNativeWindow;
    }

#ifdef XP_WIN
    // ChooseTextureType() (behind PreferredCanvasTextureType) lost its Windows
    // branch upstream: it now answers only for macOS and Android and returns
    // TextureType::Unknown everywhere else. Gecko 84, the last version where
    // this path was known to work on Windows, returned TextureType::D3D11 here.
    //
    // Unknown is not harmless. InitSwapChain() cannot build a factory for it and
    // silently falls back to SurfaceFactory_Basic, whose surfaces have no
    // shareable descriptor. ToSurfaceDescriptor() then yields nothing,
    // GetFrontBuffer() returns an empty descriptor, and SendSubmitFrame() is
    // never reached -- so the VR compositor receives no texture at all and the
    // headset sits on "up next" forever, while the frame loop drops to the
    // watchdog rate. Measured: texType=0 and an empty descriptor on every one of
    // the 13 frames of a 45 s session.
    //
    // Only this VR path is corrected; ChooseTextureType() is shared with all of
    // canvas and is deliberately left alone.
    if (texType == layers::TextureType::Unknown && imageBridge &&
        !imageBridge->UsingSoftwareWebRender()) {
      texType = layers::TextureType::D3D11;
    }
#endif

    VRSUBMIT_LOG_VERBOSE("[texType] imageBridge=%p texType=%d framebuffer=%p",
                 (void*)imageBridge.get(), (int)texType, (void*)mFramebuffer);
    webgl->Present(mFramebuffer, texType, true);
    mThisFrameTextureDesc = webgl->GetFrontBuffer(mFramebuffer, true);
  }

  mLastSubmittedFrameId = frameId;

  if (!mThisFrameTextureDesc) {
    VRSUBMIT_LOG("[6] VRLayerChild::SubmitFrame BAILOU: GetFrontBuffer devolveu "
                 "descritor vazio");
    gfxCriticalError() << "ToSurfaceDescriptor failed in "
                          "VRLayerChild::SubmitFrame";
    return;
  }

  VRSUBMIT_LOG_VERBOSE("[7] VRLayerChild::SubmitFrame -> SendSubmitFrame frameId=%llu",
               (unsigned long long)frameId);
  SendSubmitFrame(*mThisFrameTextureDesc, frameId, mLeftEyeRect, mRightEyeRect);
}

void VRLayerChild::ClearSurfaces() {
  mThisFrameTextureDesc = Nothing();
  mLastFrameTextureDesc = Nothing();
  const auto& webgl = mCanvasElement->GetWebGLContext();
  if (!mFramebuffer && webgl) {
    webgl->ClearVRSwapChain();
  }
}

// static
already_AddRefed<VRLayerChild> VRLayerChild::CreateIPDLActor() {
  if (!StaticPrefs::dom_vr_enabled() && !StaticPrefs::dom_vr_webxr_enabled()) {
    return nullptr;
  }

  return MakeAndAddRef<VRLayerChild>();
}

}  // namespace mozilla::gfx
