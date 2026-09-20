#ifndef VRSUBMIT_LOG_DEFINED
#  define VRSUBMIT_LOG_DEFINED
#  include "mozilla/Logging.h"
static mozilla::LazyLogModule gVRSubmitLog("VRSubmit");
#  define VRSUBMIT_LOG(...) MOZ_LOG(gVRSubmitLog, mozilla::LogLevel::Info, (__VA_ARGS__))
#endif
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "VRProcessChild.h"

#include "mozilla/BackgroundHangMonitor.h"
#include "mozilla/GeckoArgs.h"
#include "mozilla/ipc/ProcessUtils.h"
#include "mozilla/StaticPrefs_dom.h"

using namespace mozilla;
using namespace mozilla::gfx;

StaticRefPtr<VRParent> sVRParent;

VRProcessChild::~VRProcessChild() { sVRParent = nullptr; }

/*static*/
VRParent* VRProcessChild::GetVRParent() {
  MOZ_ASSERT(sVRParent);
  return sVRParent;
}

bool VRProcessChild::Init(int aArgc, char* aArgv[]) {
  // Controle positivo (ADR-18): se o processo VR existe, isto executou.
  // Sem esta linha nos logs, o instrumento esta cego neste processo e
  // qualquer conclusao tirada da ausencia de log e invalida.
  VRSUBMIT_LOG("[VRProcessChild::Init] processo VR iniciou, pid=%lu",
               (unsigned long)base::GetCurrentProcId());
  Maybe<const char*> parentBuildID =
      geckoargs::sParentBuildID.Get(aArgc, aArgv);
  if (parentBuildID.isNothing()) {
    return false;
  }

  if (!ProcessChild::InitPrefs(aArgc, aArgv)) {
    return false;
  }

  sVRParent = new VRParent();
  sVRParent->Init(TakeInitialEndpoint(), *parentBuildID);

  return true;
}

void VRProcessChild::CleanUp() {
  sVRParent = nullptr;
  NS_ShutdownXPCOM(nullptr);
}
