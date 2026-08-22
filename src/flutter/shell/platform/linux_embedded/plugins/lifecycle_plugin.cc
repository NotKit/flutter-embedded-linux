// Copyright 2021 Sony Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux_embedded/plugins/lifecycle_plugin.h"

#include "flutter/shell/platform/linux_embedded/logger.h"

namespace flutter {

namespace {
constexpr char kChannelName[] = "flutter/lifecycle";
constexpr char kInactive[] = "AppLifecycleState.inactive";
constexpr char kResumed[] = "AppLifecycleState.resumed";
constexpr char kPaused[] = "AppLifecycleState.paused";
constexpr char kDetached[] = "AppLifecycleState.detached";
}  // namespace

LifecyclePlugin::LifecyclePlugin(BinaryMessenger* messenger)
    : messenger_(messenger) {}

// flutter/lifecycle is a StringCodec channel: the payload is the bare UTF-8
// string with no envelope, so send it through the messenger directly. Wrapping
// it in a message channel adds the codec's type and length bytes, and the
// framework then fails to parse the state and throws.
void LifecyclePlugin::SendState(const std::string& state) const {
  messenger_->Send(kChannelName,
                   reinterpret_cast<const uint8_t*>(state.data()),
                   state.size());
}

void LifecyclePlugin::OnInactive() const {
  ELINUX_LOG(DEBUG) << "App lifecycle changed to inactive state.";
  SendState(kInactive);
}

void LifecyclePlugin::OnResumed() const {
  ELINUX_LOG(DEBUG) << "App lifecycle changed to resumed state.";
  SendState(kResumed);
}

void LifecyclePlugin::OnPaused() const {
  ELINUX_LOG(DEBUG) << "App lifecycle changed to paused state.";
  SendState(kPaused);
}

void LifecyclePlugin::OnDetached() const {
  ELINUX_LOG(DEBUG) << "App lifecycle changed to detached state.";
  SendState(kDetached);
}

}  // namespace flutter
