// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_CONTENT_HUB_CLIPBOARD_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_CONTENT_HUB_CLIPBOARD_H_

#include <gio/gio.h>

#include <string>

namespace flutter {

// Copy and paste through Ubuntu Touch's content-hub, the clipboard the Qt apps
// on the system use. The Wayland selection only ever reaches other Wayland
// clients, so without this the app has a clipboard of its own.
//
// content-hub only serves the client owning the focused window, named by a Mir
// persistent surface id. A Wayland client cannot ask for one, so it sends an
// empty id and the hub checks the calling process instead. Hubs without that
// support refuse us, and the caller is expected to fall back to Wayland.
class ContentHubClipboard {
 public:
  ContentHubClipboard() = default;
  ~ContentHubClipboard();

  // Publishes |text| as the system-wide paste. False if content-hub is not
  // there or would not take it.
  bool SetText(const std::string& text);

  // Reads the newest paste into |text|. False if there is nothing to read.
  bool GetText(std::string* text);

 private:
  // The session bus, connected to on first use. Null while unavailable.
  GDBusConnection* Connection();

  GDBusConnection* connection_ = nullptr;
  bool connection_failed_ = false;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_CONTENT_HUB_CLIPBOARD_H_
