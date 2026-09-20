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

#include "VRLayerParent.h"
#include "VRManager.h"
#include "mozilla/layers/CompositorThread.h"

namespace mozilla {
using namespace layers;
namespace gfx {

VRLayerParent::VRLayerParent(uint32_t aVRDisplayID, const uint32_t aGroup)
    : mDestroyed(false), mGroup(aGroup) {}

VRLayerParent::~VRLayerParent() {
  Destroy();
  MOZ_COUNT_DTOR(VRLayerParent);
}

mozilla::ipc::IPCResult VRLayerParent::RecvDestroy() {
  Destroy();
  return IPC_OK();
}

void VRLayerParent::Destroy() {
  if (!mDestroyed) {
    VRManager* vm = VRManager::Get();
    vm->RemoveLayer(this);
    mDestroyed = true;
  }

  if (CanSend()) {
    (void)PVRLayerParent::Send__delete__(this);
  }
}

mozilla::ipc::IPCResult VRLayerParent::RecvSubmitFrame(
    const layers::SurfaceDescriptor& aTexture, const uint64_t& aFrameId,
    const gfx::Rect& aLeftEyeRect, const gfx::Rect& aRightEyeRect) {
  VRSUBMIT_LOG_VERBOSE("[VRLayerParent::RecvSubmitFrame] frameId=%llu, mDestroyed=%d",
         (unsigned long long)aFrameId, (int)mDestroyed);
  if (!mDestroyed) {
    VRManager* vm = VRManager::Get();
    vm->SubmitFrame(this, aTexture, aFrameId, aLeftEyeRect, aRightEyeRect);
  }

  return IPC_OK();
}

}  // namespace gfx
}  // namespace mozilla
