/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "OSKVRManager.h"

#include "FxRWindowManager.h"

namespace mozilla {
namespace widget {

// The upstream implementation signalled IME state to an external vrhost process
// over VRShMem. The standalone FxR desktop shell owns the OpenVR overlay itself,
// so route this to FxRWindowManager, which raises SteamVR's own keyboard for
// that overlay.

// static
void OSKVRManager::ShowOnScreenKeyboard() {
#if defined(NIGHTLY_BUILD) || defined(MOZ_FXR_DESKTOP)
  FxRWindowManager::GetInstance()->NotifyEditableFocus(true);
#endif  // NIGHTLY_BUILD || MOZ_FXR_DESKTOP
}

// static
void OSKVRManager::DismissOnScreenKeyboard() {
#if defined(NIGHTLY_BUILD) || defined(MOZ_FXR_DESKTOP)
  FxRWindowManager::GetInstance()->NotifyEditableFocus(false);
#endif  // NIGHTLY_BUILD || MOZ_FXR_DESKTOP
}

}  // namespace widget
}  // namespace mozilla
