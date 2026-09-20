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

#include "VRSession.h"
#include <stdio.h>

#include "moz_external_vr.h"

#include "mozilla/ipc/FileDescriptor.h"

#if defined(XP_WIN)
#  include <d3d11.h>
#endif  // defined(XP_WIN)

#if defined(MOZILLA_INTERNAL_API)
#  if defined(XP_WIN)
#    include "mozilla/gfx/Logging.h"
#  endif
#else
#  define NS_WARNING(s)
#endif

using namespace mozilla::gfx;

VRSession::VRSession()
    : mShouldQuit(false)
#ifdef XP_WIN
      ,
      mDevice(nullptr),
      mContext(nullptr),
      mDeviceContextState(nullptr)
#endif
{
}

#ifdef XP_WIN
VRSession::~VRSession() {
  if (mDevice != nullptr) {
    mDevice->Release();
    mDevice = nullptr;
  }

  if (mContext != nullptr) {
    mContext->Release();
    mContext = nullptr;
  }

  if (mDeviceContextState != nullptr) {
    mDeviceContextState->Release();
    mDeviceContextState = nullptr;
  }
}
#endif

#if defined(XP_WIN)
bool VRSession::CreateD3DContext(ID3D11Device* aDevice) {
  if (!mDevice) {
    if (!aDevice) {
      NS_WARNING("VRSession::CreateD3DObjects failed to get a D3D11Device");
      return false;
    }
    if (FAILED(aDevice->QueryInterface(IID_PPV_ARGS(&mDevice)))) {
      NS_WARNING("VRSession::CreateD3DObjects failed to get a D3D11Device1");
      return false;
    }
  }
  if (!mContext) {
    mDevice->GetImmediateContext1(&mContext);
    if (!mContext) {
      NS_WARNING(
          "VRSession::CreateD3DObjects failed to get an immediate context");
      return false;
    }
  }
  if (!mDeviceContextState) {
    D3D_FEATURE_LEVEL featureLevels[]{D3D_FEATURE_LEVEL_11_1,
                                      D3D_FEATURE_LEVEL_11_0};
    mDevice->CreateDeviceContextState(0, featureLevels, 2, D3D11_SDK_VERSION,
                                      __uuidof(ID3D11Device1), nullptr,
                                      &mDeviceContextState);
  }
  if (!mDeviceContextState) {
    NS_WARNING(
        "VRSession::CreateD3DObjects failed to get a D3D11DeviceContextState");
    return false;
  }
  return true;
}

ID3D11Device1* VRSession::GetD3DDevice() { return mDevice; }

ID3D11DeviceContext1* VRSession::GetD3DDeviceContext() { return mContext; }

ID3DDeviceContextState* VRSession::GetD3DDeviceContextState() {
  return mDeviceContextState;
}

#endif  // defined(XP_WIN)

bool VRSession::SubmitFrame(
    const mozilla::gfx::VRLayer_Stereo_Immersive& aLayer) {
#if defined(XP_WIN)
  bool success = false;
  if (aLayer.textureType ==
      VRLayerTextureType::LayerTextureType_D3D10SurfaceDescriptor) {
    ID3D11Texture2D* dxTexture = nullptr;
    // Upstream form: the texture is shared as an NT handle (see
    // SharedSurfaceANGLE.cpp), so it must be opened with OpenSharedResource1
    // and its lifetime managed by UniquePlatformHandle. The legacy
    // OpenSharedResource fails on an NT handle.
    mozilla::ipc::FileDescriptor::UniquePlatformHandle handle(
        aLayer.textureHandle);
    if (!mDevice) {
      fprintf(stderr, "[VRSession::SubmitFrame] ERROR: mDevice is NULL!\n");
      fflush(stderr);
      return false;
    }

    VRSUBMIT_LOG_VERBOSE("[VRSession] SubmitFrame: handle=%p, mDevice=%p", (void*)handle.get(), mDevice);
    HRESULT hr =
        mDevice->OpenSharedResource1(handle.get(), IID_PPV_ARGS(&dxTexture));
    VRSUBMIT_LOG_VERBOSE("[VRSession] OpenSharedResource1 hr=0x%08lx, dxTexture=%p", (unsigned long)hr, dxTexture);
    if (SUCCEEDED(hr) && dxTexture != nullptr) {
      IDXGIKeyedMutex* mutex = nullptr;
      HRESULT hrMutex = dxTexture->QueryInterface(IID_PPV_ARGS(&mutex));
      if (SUCCEEDED(hrMutex) && mutex != nullptr) {
        const auto waitStart = TimeStamp::Now();
        hr = mutex->AcquireSync(0, 1000);
        const double waitMs = (TimeStamp::Now() - waitStart).ToMilliseconds();
        static uint32_t waits = 0, slowWaits = 0;
        static double worstWaitMs = 0;
        if (waitMs > worstWaitMs) worstWaitMs = waitMs;
        if (waitMs > 8.0) ++slowWaits;
        if (++waits % 300 == 0) {
          VRSUBMIT_LOG("[FxR perf] VR mutex last300 maxMs=%.3f over8ms=%u",
                       worstWaitMs, slowWaits);
          worstWaitMs = 0;
          slowWaits = 0;
        }
#  ifdef MOZILLA_INTERNAL_API
        if (hr == WAIT_TIMEOUT) {
          gfxDevCrash(LogReason::D3DLockTimeout) << "D3D lock mutex timeout";
        } else if (hr == WAIT_ABANDONED) {
          gfxCriticalNote << "GFX: D3D11 lock mutex abandoned";
        }
#  endif
        if (hr == S_OK) {
          success = SubmitFrame(aLayer, dxTexture);
          HRESULT hrRelease = mutex->ReleaseSync(0);
          if (FAILED(hrRelease)) {
            success = false;
            fprintf(stderr, "[VRSession::SubmitFrame] Failed to unlock keyed mutex: 0x%08lx\n", (unsigned long)hrRelease);
            fflush(stderr);
          }
        } else {
          fprintf(stderr, "[VRSession::SubmitFrame] Failed to acquire keyed mutex: 0x%08lx (timeout=%d)\n", (unsigned long)hr, (hr == (HRESULT)WAIT_TIMEOUT));
          fflush(stderr);
        }

        mutex->Release();
        mutex = nullptr;
      } else {
        // Texture does not support KeyedMutex (e.g. fence-based or standard shared texture)
        // Etapa 1B: com a intermediaria sincronizada do VRManager, este ramo nao
        // deve mais ser percorrido. Se for, a submissao segue sem sincronizacao
        // nenhuma -- avisar, com limite, para que isso apareca no teste.
        static int sUnsyncedSubmits = 0;
        if (++sUnsyncedSubmits <= 3 || sUnsyncedSubmits % 900 == 0) {
          fprintf(stderr, "[VRSession::SubmitFrame] AVISO: textura sem keyed mutex submetida sem sincronizacao (%d)\n", sUnsyncedSubmits);
          fflush(stderr);
        }
        hr = S_OK;
        success = SubmitFrame(aLayer, dxTexture);
      }

      dxTexture->Release();
      dxTexture = nullptr;
    } else {
      static int sOpenFailCount = 0;
      if (sOpenFailCount++ < 10) {
        fprintf(stderr, "[VRSession::SubmitFrame] Failed to open shared texture: hr=0x%08lx, handle=%p\n", (unsigned long)hr, (void*)handle.get());
        fflush(stderr);
      }
    }

    static int sSubmitCount = 0;
    if (++sSubmitCount % 90 == 1) {
      fprintf(stderr, "[VRSession::SubmitFrame] Frame #%d submitted (success=%d, hr=0x%08lx)\n", sSubmitCount, (int)success, (unsigned long)hr);
      fflush(stderr);
    }

    return SUCCEEDED(hr) && success;
  }

#elif defined(XP_MACOSX)

  if (aLayer.textureType == VRLayerTextureType::LayerTextureType_MacIOSurface) {
    return SubmitFrame(aLayer, aLayer.textureHandle);
  }

#endif

  return false;
}

void VRSession::UpdateTrigger(VRControllerState& aState, uint32_t aButtonIndex,
                              float aValue, float aThreshold) {
  // For OpenVR, the threshold value of ButtonPressed and ButtonTouched is 0.55.
  // We prefer to let developers to set their own threshold for the adjustment.
  // Therefore, we don't check ButtonPressed and ButtonTouched with ButtonMask
  // here. we just check the button value is larger than the threshold value or
  // not.
  uint64_t mask = (1ULL << aButtonIndex);
  aState.triggerValue[aButtonIndex] = aValue;
  if (aValue > aThreshold) {
    aState.buttonPressed |= mask;
    aState.buttonTouched |= mask;
  } else {
    aState.buttonPressed &= ~mask;
    aState.buttonTouched &= ~mask;
  }
}

void VRSession::SetControllerSelectionAndSqueezeFrameId(
    VRControllerState& controllerState, uint64_t aFrameId) {
  // The 1st button, trigger, is its selection action.
  const bool selectionPressed = controllerState.buttonPressed & 1ULL;
  if (selectionPressed && controllerState.selectActionStopFrameId >=
                              controllerState.selectActionStartFrameId) {
    controllerState.selectActionStartFrameId = aFrameId;
  } else if (!selectionPressed && controllerState.selectActionStartFrameId >
                                      controllerState.selectActionStopFrameId) {
    controllerState.selectActionStopFrameId = aFrameId;
  }
  // The 2nd button, squeeze, is its squeeze action.
  const bool squeezePressed = controllerState.buttonPressed & (1ULL << 1);
  if (squeezePressed && controllerState.squeezeActionStopFrameId >=
                            controllerState.squeezeActionStartFrameId) {
    controllerState.squeezeActionStartFrameId = aFrameId;
  } else if (!squeezePressed && controllerState.squeezeActionStartFrameId >
                                    controllerState.squeezeActionStopFrameId) {
    controllerState.squeezeActionStopFrameId = aFrameId;
  }
}

bool VRSession::ShouldQuit() const { return mShouldQuit; }
