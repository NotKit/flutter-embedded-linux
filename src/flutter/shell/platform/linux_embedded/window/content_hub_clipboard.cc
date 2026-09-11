// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux_embedded/window/content_hub_clipboard.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "flutter/shell/platform/linux_embedded/logger.h"

namespace flutter {

namespace {
constexpr char kServiceName[] = "com.lomiri.content.dbus.Service";
constexpr char kServicePath[] = "/";
constexpr char kServiceInterface[] = "com.lomiri.content.dbus.Service";

// Wayland clients have no Mir surface id to offer, see the class comment.
constexpr char kEmptySurfaceId[] = "";

constexpr char kMimeTypeText[] = "text/plain";

// The hub holds the pastes in memory, so answers come back right away. Keep
// the wait short anyway: these calls block the platform thread.
constexpr int kCallTimeoutMs = 2000;

// As in content-hub's src/com/lomiri/content/utils.cpp.
constexpr int32_t kMaxFormatCount = 16;

int32_t ReadInt(const uint8_t* blob, size_t index) {
  int32_t value;
  std::memcpy(&value, blob + index * sizeof(int32_t), sizeof(int32_t));
  return value;
}

// A paste is a serialized QMimeData: an int32 format count, then four int32 per
// format (format offset, format size, data offset, data size), then the bytes
// they point at. Nothing in it is Qt-specific.
std::vector<uint8_t> SerializePaste(const std::string& text) {
  const std::string format = kMimeTypeText;
  const size_t header_size = sizeof(int32_t) * 5;

  std::vector<uint8_t> blob(header_size + format.size() + text.size());
  int32_t header[] = {
      1,
      static_cast<int32_t>(header_size),
      static_cast<int32_t>(format.size()),
      static_cast<int32_t>(header_size + format.size()),
      static_cast<int32_t>(text.size()),
  };
  std::memcpy(blob.data(), header, header_size);
  std::memcpy(blob.data() + header[1], format.data(), format.size());
  std::memcpy(blob.data() + header[3], text.data(), text.size());
  return blob;
}

bool ParsePaste(const uint8_t* blob, size_t size, std::string* text) {
  if (size < sizeof(int32_t)) {
    return false;
  }

  const int32_t count = std::min(ReadInt(blob, 0), kMaxFormatCount);
  if (count <= 0 || size < sizeof(int32_t) * (1 + 4 * static_cast<size_t>(count))) {
    return false;
  }

  bool found = false;
  for (int32_t i = 0; i < count; i++) {
    const int32_t format_offset = ReadInt(blob, i * 4 + 1);
    const int32_t format_size = ReadInt(blob, i * 4 + 2);
    const int32_t data_offset = ReadInt(blob, i * 4 + 3);
    const int32_t data_size = ReadInt(blob, i * 4 + 4);
    if (format_offset < 0 || format_size < 0 || data_offset < 0 ||
        data_size < 0 ||
        static_cast<size_t>(format_offset) + format_size > size ||
        static_cast<size_t>(data_offset) + data_size > size) {
      return false;
    }

    const std::string format(reinterpret_cast<const char*>(blob) + format_offset,
                             format_size);
    const std::string charset_prefix = std::string(kMimeTypeText) + ";";
    if (format != kMimeTypeText && format.rfind(charset_prefix, 0) != 0) {
      continue;
    }

    text->assign(reinterpret_cast<const char*>(blob) + data_offset, data_size);
    // A charset variant will do, but keep looking for plain text/plain.
    if (format == kMimeTypeText) {
      return true;
    }
    found = true;
  }

  return found;
}

// content-hub labels a paste with the id of the app that made it. A click app
// gets one from the launcher; its AppArmor profile carries the same string, and
// is what the hub compares against.
std::string AppId() {
  const char* app_id = std::getenv("APP_ID");
  if (app_id && *app_id) {
    return app_id;
  }

  gchar* label = nullptr;
  if (!g_file_get_contents("/proc/self/attr/current", &label, nullptr, nullptr)) {
    return "";
  }

  std::string profile(label);
  g_free(label);
  // The label reads "profile (mode)" while a mode is set.
  const size_t mode = profile.find(" (");
  if (mode != std::string::npos) {
    profile.resize(mode);
  }
  return profile == "unconfined" ? "" : profile;
}
}  // namespace

ContentHubClipboard::~ContentHubClipboard() {
  if (connection_) {
    g_object_unref(connection_);
  }
}

GDBusConnection* ContentHubClipboard::Connection() {
  if (connection_ || connection_failed_) {
    return connection_;
  }

  GError* error = nullptr;
  connection_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!connection_) {
    ELINUX_LOG(ERROR) << "Failed to connect to the session bus: "
                      << error->message;
    g_error_free(error);
    connection_failed_ = true;
  }
  return connection_;
}

bool ContentHubClipboard::SetText(const std::string& text) {
  GDBusConnection* connection = Connection();
  if (!connection) {
    return false;
  }

  const std::vector<uint8_t> blob = SerializePaste(text);
  GVariant* mime_data = g_variant_new_fixed_array(
      G_VARIANT_TYPE_BYTE, blob.data(), blob.size(), sizeof(uint8_t));
  const char* types[] = {kMimeTypeText, nullptr};
  const std::string app_id = AppId();

  GError* error = nullptr;
  GVariant* reply = g_dbus_connection_call_sync(
      connection, kServiceName, kServicePath, kServiceInterface, "CreatePaste",
      g_variant_new("(ss@ay^as)", app_id.c_str(), kEmptySurfaceId, mime_data,
                    types),
      G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr,
      &error);
  if (!reply) {
    ELINUX_LOG(ERROR) << "Failed to publish the clipboard to content-hub: "
                      << error->message;
    g_error_free(error);
    return false;
  }

  gboolean created = FALSE;
  g_variant_get(reply, "(b)", &created);
  g_variant_unref(reply);
  if (!created) {
    // Either the hub does not know about callers without a surface id, or it
    // does and we are not the focused app.
    ELINUX_LOG(ERROR) << "content-hub refused the clipboard contents.";
  }
  return created;
}

bool ContentHubClipboard::GetText(std::string* text) {
  GDBusConnection* connection = Connection();
  if (!connection) {
    return false;
  }

  GError* error = nullptr;
  GVariant* reply = g_dbus_connection_call_sync(
      connection, kServiceName, kServicePath, kServiceInterface,
      "GetLatestPasteData", g_variant_new("(s)", kEmptySurfaceId),
      G_VARIANT_TYPE("(ay)"), G_DBUS_CALL_FLAGS_NONE, kCallTimeoutMs, nullptr,
      &error);
  if (!reply) {
    ELINUX_LOG(ERROR) << "Failed to read the clipboard from content-hub: "
                      << error->message;
    g_error_free(error);
    return false;
  }

  GVariant* blob = g_variant_get_child_value(reply, 0);
  gsize size = 0;
  auto data = static_cast<const uint8_t*>(
      g_variant_get_fixed_array(blob, &size, sizeof(uint8_t)));
  const bool parsed = data && ParsePaste(data, size, text);
  g_variant_unref(blob);
  g_variant_unref(reply);
  return parsed;
}

}  // namespace flutter
