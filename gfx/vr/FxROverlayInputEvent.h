/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_VR_FXR_OVERLAY_INPUT_EVENT_H
#define GFX_VR_FXR_OVERLAY_INPUT_EVENT_H

#include <cstdint>

// A single OpenVR overlay input event, flattened out of vr::VREvent_t so that
// it can be handed from the thread that polls the overlay (the Renderer thread,
// in FxROutputHandler::UpdateOutput) to the UI thread, where FxRWindowManager
// turns it into a real Gecko widget event.
//
// This deliberately stays a POD with no OpenVR types in it: the polling side and
// the dispatching side live in different threads (and, under the PGPU fallback,
// different processes), and nothing here should require openvr.h to be included.
struct FxROverlayInputEvent {
  // vr::EVREventType value (VREvent_MouseMove, VREvent_MouseButtonDown, ...).
  uint32_t mType = 0;

  // Position in overlay texture pixels, OpenVR's bottom-left origin.
  float mX = 0.0f;
  float mY = 0.0f;

  // vr::EVRMouseButton bitmask.
  uint32_t mButton = 0;

  // VREvent_ScrollDiscrete delta, in detents.
  float mScrollYDelta = 0.0f;

  // Size of the overlay texture, i.e. the space mX/mY are expressed in. Needed
  // to flip Y into Windows' top-left origin.
  uint32_t mOverlayW = 0;
  uint32_t mOverlayH = 0;

  // VREvent_KeyboardCharInput payload (vr::VREvent_Keyboard_t::cNewInput).
  // UTF-8, not necessarily NUL-terminated when all 8 bytes are used.
  char mKeyboardInput[8] = {0};

  // For VREvent_OverlayFocusChanged: whether the overlay that gained focus is
  // ours. Resolved on the side that owns the overlay handle.
  bool mFocused = false;
};

#endif  // GFX_VR_FXR_OVERLAY_INPUT_EVENT_H
