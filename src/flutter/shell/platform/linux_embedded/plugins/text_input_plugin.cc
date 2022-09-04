// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux_embedded/plugins/text_input_plugin.h"

#include <linux/input-event-codes.h>

#include <iostream>
#include <map>

#include "flutter/shell/platform/common/json_method_codec.h"

// Avoids the following build error:
// ----------------------------------------------------------------
//  error: expected unqualified-id
//    result->Success(document);
//            ^
// /usr/include/X11/X.h:350:21: note: expanded from macro 'Success'
// #define Success            0    /* everything's okay */
// ----------------------------------------------------------------
#if defined(DISPLAY_BACKEND_TYPE_X11)
#undef Success
#endif

namespace Qt {
  // from QtCore/qnamespace.h to avoid Qt dependency
  enum Key {
    Key_Escape = 0x01000000,
    Key_Tab = 0x01000001,
    Key_Backspace = 0x01000003,
    Key_Return = 0x01000004,
    Key_Enter = 0x01000005,
    Key_Insert = 0x01000006,
    Key_Delete = 0x01000007,
    Key_Pause = 0x01000008,
    Key_Home = 0x01000010,
    Key_End = 0x01000011,
    Key_Left = 0x01000012,
    Key_Up = 0x01000013,
    Key_Right = 0x01000014,
    Key_Down = 0x01000015,
    Key_PageUp = 0x01000016,
    Key_PageDown = 0x01000017
  };
  enum Type {
    KeyPress = 6,
    KeyRelease = 7
  };
}

namespace flutter {

namespace {
constexpr char kChannelName[] = "flutter/textinput";

constexpr char kSetEditingStateMethod[] = "TextInput.setEditingState";
constexpr char kClearClientMethod[] = "TextInput.clearClient";
constexpr char kSetClientMethod[] = "TextInput.setClient";
constexpr char kShowMethod[] = "TextInput.show";
constexpr char kHideMethod[] = "TextInput.hide";

constexpr char kMultilineInputType[] = "TextInputType.multiline";

constexpr char kUpdateEditingStateMethod[] =
    "TextInputClient.updateEditingState";
constexpr char kPerformActionMethod[] = "TextInputClient.performAction";

constexpr char kTextInputAction[] = "inputAction";
constexpr char kTextInputType[] = "inputType";
constexpr char kTextInputTypeName[] = "name";
constexpr char kComposingBaseKey[] = "composingBase";
constexpr char kComposingExtentKey[] = "composingExtent";
constexpr char kSelectionAffinityKey[] = "selectionAffinity";
constexpr char kAffinityDownstream[] = "TextAffinity.downstream";
constexpr char kSelectionBaseKey[] = "selectionBase";
constexpr char kSelectionExtentKey[] = "selectionExtent";
constexpr char kSelectionIsDirectionalKey[] = "selectionIsDirectional";
constexpr char kTextKey[] = "text";

constexpr char kBadArgumentError[] = "Bad Arguments";
constexpr char kInternalConsistencyError[] = "Internal Consistency Error";

std::map<int, int> QtKeyToLinuxEvent = {
  {Qt::Key_Escape, KEY_ESC},
  {Qt::Key_Tab, KEY_TAB},
  {Qt::Key_Backspace, KEY_BACKSPACE},
  {Qt::Key_Return, KEY_ENTER},
  {Qt::Key_Enter, KEY_ENTER},
  {Qt::Key_Insert, KEY_INSERT},
  {Qt::Key_Delete, KEY_DELETE},
  {Qt::Key_Pause, KEY_PAUSE},
  {Qt::Key_Home, KEY_HOME},
  {Qt::Key_End, KEY_END},
  {Qt::Key_Left, KEY_LEFT},
  {Qt::Key_Up, KEY_UP},
  {Qt::Key_Right, KEY_RIGHT},
  {Qt::Key_Down, KEY_DOWN},
  {Qt::Key_PageUp, KEY_PAGEUP},
  {Qt::Key_PageDown, KEY_PAGEDOWN},
};
}  // namespace

void TextInputPlugin::OnKeyPressed(uint32_t keycode, uint32_t code_point) {
  if (!active_model_) {
    return;
  }

  bool changed = false;
  switch (keycode) {
    case KEY_LEFT:
      changed = active_model_->MoveCursorBack();
      break;
    case KEY_RIGHT:
      changed = active_model_->MoveCursorForward();
      break;
    case KEY_END:
      changed = active_model_->MoveCursorToEnd();
      break;
    case KEY_HOME:
      changed = active_model_->MoveCursorToBeginning();
      break;
    case KEY_BACKSPACE:
      changed = active_model_->Backspace();
      break;
    case KEY_DELETE:
      changed = active_model_->Delete();
      break;
    case KEY_ENTER:
      EnterPressed(active_model_.get());
      break;
    default:
      if (code_point) {
        active_model_->AddCodePoint(code_point);
        changed = true;
      }
      break;
  }
  if (changed) {
    SendStateUpdate(*active_model_);
  }
}

void TextInputPlugin::DispatchEvent() {
  g_main_context_iteration(glib_ctx_, FALSE);
}

TextInputPlugin::TextInputPlugin(BinaryMessenger* messenger,
                                 WindowBindingHandler* delegate)
    : channel_(std::make_unique<flutter::MethodChannel<rapidjson::Document>>(
          messenger,
          kChannelName,
          &flutter::JsonMethodCodec::GetInstance())),
      delegate_(delegate),
      active_model_(nullptr) {

  InitMaliitConnection();

  channel_->SetMethodCallHandler(
      [this](
          const flutter::MethodCall<rapidjson::Document>& call,
          std::unique_ptr<flutter::MethodResult<rapidjson::Document>> result) {
        HandleMethodCall(call, std::move(result));
      });
}

void TextInputPlugin::HandleMethodCall(
    const flutter::MethodCall<rapidjson::Document>& method_call,
    std::unique_ptr<flutter::MethodResult<rapidjson::Document>> result) {
  const std::string& method = method_call.method_name();

  if (method.compare(kShowMethod) == 0) {
    delegate_->UpdateVirtualKeyboardStatus(true);
    MaliitShowInputMethod();
  } else if (method.compare(kHideMethod) == 0) {
    delegate_->UpdateVirtualKeyboardStatus(false);
    MaliitHideInputMethod();
  } else if (method.compare(kClearClientMethod) == 0) {
    active_model_ = nullptr;
  } else if (method.compare(kSetClientMethod) == 0) {
    if (!method_call.arguments() || method_call.arguments()->IsNull()) {
      result->Error(kBadArgumentError, "Method invoked without args");
      return;
    }
    const rapidjson::Document& args = *method_call.arguments();

    // TODO(awdavies): There's quite a wealth of arguments supplied with this
    // method, and they should be inspected/used.
    const rapidjson::Value& client_id_json = args[0];
    const rapidjson::Value& client_config = args[1];
    if (client_id_json.IsNull()) {
      result->Error(kBadArgumentError, "Could not set client, ID is null.");
      return;
    }
    if (client_config.IsNull()) {
      result->Error(kBadArgumentError,
                    "Could not set client, missing arguments.");
      return;
    }
    client_id_ = client_id_json.GetInt();
    input_action_ = "";
    auto input_action_json = client_config.FindMember(kTextInputAction);
    if (input_action_json != client_config.MemberEnd() &&
        input_action_json->value.IsString()) {
      input_action_ = input_action_json->value.GetString();
    }
    input_type_ = "";
    auto input_type_info_json = client_config.FindMember(kTextInputType);
    if (input_type_info_json != client_config.MemberEnd() &&
        input_type_info_json->value.IsObject()) {
      auto input_type_json =
          input_type_info_json->value.FindMember(kTextInputTypeName);
      if (input_type_json != input_type_info_json->value.MemberEnd() &&
          input_type_json->value.IsString()) {
        input_type_ = input_type_json->value.GetString();
      }
    }
    active_model_ = std::make_unique<TextInputModel>();
  } else if (method.compare(kSetEditingStateMethod) == 0) {
    if (!method_call.arguments() || method_call.arguments()->IsNull()) {
      result->Error(kBadArgumentError, "Method invoked without args");
      return;
    }
    const rapidjson::Document& args = *method_call.arguments();

    if (active_model_ == nullptr) {
      result->Error(
          kInternalConsistencyError,
          "Set editing state has been invoked, but no client is set.");
      return;
    }
    auto text = args.FindMember(kTextKey);
    if (text == args.MemberEnd() || text->value.IsNull()) {
      result->Error(kBadArgumentError,
                    "Set editing state has been invoked, but without text.");
      return;
    }
    auto selection_base = args.FindMember(kSelectionBaseKey);
    auto selection_extent = args.FindMember(kSelectionExtentKey);
    if (selection_base == args.MemberEnd() || selection_base->value.IsNull() ||
        selection_extent == args.MemberEnd() ||
        selection_extent->value.IsNull()) {
      result->Error(kInternalConsistencyError,
                    "Selection base/extent values invalid.");
      return;
    }
    // Flutter uses -1/-1 for invalid; translate that to 0/0 for the model.
    int base = selection_base->value.GetInt();
    int extent = selection_extent->value.GetInt();
    if (base == -1 && extent == -1) {
      base = extent = 0;
    }
    active_model_->SetText(text->value.GetString());
    active_model_->SetSelection(TextRange(base, extent));
  } else {
    result->NotImplemented();
    return;
  }
  // All error conditions return early, so if nothing has gone wrong indicate
  // success.
  result->Success();
}

void TextInputPlugin::SendStateUpdate(const TextInputModel& model) {
  auto args = std::make_unique<rapidjson::Document>(rapidjson::kArrayType);
  auto& allocator = args->GetAllocator();
  args->PushBack(client_id_, allocator);

  TextRange selection = model.selection();
  rapidjson::Value editing_state(rapidjson::kObjectType);
  editing_state.AddMember(kComposingBaseKey, -1, allocator);
  editing_state.AddMember(kComposingExtentKey, -1, allocator);
  editing_state.AddMember(kSelectionAffinityKey, kAffinityDownstream,
                          allocator);
  editing_state.AddMember(kSelectionBaseKey, selection.base(), allocator);
  editing_state.AddMember(kSelectionExtentKey, selection.extent(), allocator);
  editing_state.AddMember(kSelectionIsDirectionalKey, false, allocator);
  editing_state.AddMember(
      kTextKey, rapidjson::Value(model.GetText(), allocator).Move(), allocator);
  args->PushBack(editing_state, allocator);

  channel_->InvokeMethod(kUpdateEditingStateMethod, std::move(args));
}

void TextInputPlugin::EnterPressed(TextInputModel* model) {
  if (input_type_ == kMultilineInputType) {
    model->AddCodePoint('\n');
    SendStateUpdate(*model);
  }
  auto args = std::make_unique<rapidjson::Document>(rapidjson::kArrayType);
  auto& allocator = args->GetAllocator();
  args->PushBack(client_id_, allocator);
  args->PushBack(rapidjson::Value(input_action_, allocator).Move(), allocator);

  channel_->InvokeMethod(kPerformActionMethod, std::move(args));
}

void
maliit_im_invoke_action(MaliitServer *obj G_GNUC_UNUSED,
                              const char *action,
                              const char *sequence G_GNUC_UNUSED,
                              gpointer user_data)
{
  ELINUX_LOG(DEBUG) << "maliit_im_invoke_action: " << action;
}

// Callback functions for dbus obj
gboolean TextInputPlugin::MaliitHandleIMInitiatedHide(MaliitContext *obj,
                              GDBusMethodInvocation *invocation,
                              gpointer user_data)
{
  auto self = reinterpret_cast<TextInputPlugin*>(user_data);
  if (!self->active_model_) {
    return FALSE;
  }

  if (self->active_model_->composing()) {
    self->active_model_->EndComposing();
    self->SendStateUpdate(*self->active_model_);
  }

  return FALSE;
}

gboolean TextInputPlugin::MaliitHandleCommitString(MaliitContext *obj,
                              GDBusMethodInvocation *invocation,
                              const gchar *string,
                              int replacement_start G_GNUC_UNUSED,
                              int replacement_length G_GNUC_UNUSED,
                              int cursor_pos G_GNUC_UNUSED,
                              gpointer user_data)
{
  auto self = reinterpret_cast<TextInputPlugin*>(user_data);
  if (!self->active_model_) {
    return FALSE;
  }

  if (self->active_model_->composing()) {
    self->active_model_->UpdateComposingText(string);
    self->active_model_->EndComposing();
  } else {
    self->active_model_->AddText(string);
  }

  self->SendStateUpdate(*self->active_model_);

  return TRUE;
}

gboolean TextInputPlugin::MaliitHandleUpdatePreedit(MaliitContext *obj,
                               GDBusMethodInvocation *invocation,
                               const gchar *string,
                               GVariant *formatListData,
                               gint replaceStart G_GNUC_UNUSED,
                               gint replaceLength G_GNUC_UNUSED,
                               gint cursorPos,
                               gpointer user_data)
{
  auto self = reinterpret_cast<TextInputPlugin*>(user_data);
  if (!self->active_model_) {
    return FALSE;
  }

  if (!self->active_model_->composing()) {
    self->active_model_->BeginComposing();
  }
  self->active_model_->UpdateComposingText(string);

  self->SendStateUpdate(*self->active_model_);

  return TRUE;
}

gboolean TextInputPlugin::MaliitHandleKeyEvent(MaliitContext *obj,
                          GDBusMethodInvocation *invocation,
                          gint type,
                          gint key,
                          gint modifiers,
                          const gchar *text,
                          gboolean auto_repeat G_GNUC_UNUSED,
                          int count G_GNUC_UNUSED,
                          guchar request_type G_GNUC_UNUSED,
                          gpointer user_data)
{
  auto self = reinterpret_cast<TextInputPlugin*>(user_data);
  if (!self->active_model_) {
    return FALSE;
  }

  if (type == Qt::KeyPress) {
    auto it = QtKeyToLinuxEvent.find(key);
    if (it != QtKeyToLinuxEvent.end()) {
      self->OnKeyPressed(it->second, 0);
    }
  }

  return TRUE;
}

gboolean TextInputPlugin::MaliitHandleUpdateInputMethodArea(MaliitContext *obj,
                                          GDBusMethodInvocation *invocation,
                                          gint x,
                                          gint y,
                                          gint width,
                                          gint height,
                                          gpointer user_data)
{
  return TRUE;
}

void TextInputPlugin::MaliitShowInputMethod() {
  GError *error = NULL;

  if (!maliit_server_)
    return;

  if (maliit_server_call_activate_context_sync(maliit_server_,
                                              NULL,
                                              &error)) {
    if (!maliit_server_call_show_input_method_sync(maliit_server_,
                                                    NULL,
                                                    &error)) {
      ELINUX_LOG(ERROR) << "Unable to show input method: " << error->message;
      g_clear_error(&error);
    }
  } else {
    ELINUX_LOG(ERROR) << "Unable to activate context: " << error->message;
    g_clear_error(&error);
  }
}

void TextInputPlugin::MaliitHideInputMethod() {
  GError *error = NULL;

  if (!maliit_server_)
    return;

  if (!maliit_server_call_reset_sync(maliit_server_, NULL, &error)) {
    ELINUX_LOG(ERROR) << "Unable to reset: " << error->message;
    g_clear_error(&error);
  }

  if (!maliit_server_call_hide_input_method_sync(maliit_server_,
                                                  NULL,
                                                  &error)) {
    ELINUX_LOG(ERROR) << "Unable to hide input method: " << error->message;
    g_clear_error(&error);
  }
}

void TextInputPlugin::InitMaliitConnection() {
  glib_ctx_ = g_main_context_new();
  glib_loop_ = g_main_loop_new(glib_ctx_, FALSE);
  g_main_context_push_thread_default(glib_ctx_);

  ELINUX_LOG(INFO) << "Initializing Maliit connection";

  GError *error = NULL;
  maliit_server_ = maliit_get_server_sync(NULL, &error);
  if (maliit_server_) {
      g_object_ref(maliit_server_);
      g_signal_connect(maliit_server_, "invoke-action", G_CALLBACK(maliit_im_invoke_action), this);
  } else {
      ELINUX_LOG(ERROR) << "Unable to connect to Maliit server: " << error->message;
      g_clear_error(&error);
      return;
  }

  maliit_context_ = maliit_get_context_sync(NULL, &error);
  if (maliit_context_) {
      g_object_ref(maliit_context_);
      g_signal_connect(maliit_context_, "handle-im-initiated-hide",
                          G_CALLBACK(MaliitHandleIMInitiatedHide), this);
      g_signal_connect(maliit_context_, "handle-commit-string",
                          G_CALLBACK(MaliitHandleCommitString), this);
      g_signal_connect(maliit_context_, "handle-update-preedit",
                          G_CALLBACK(MaliitHandleUpdatePreedit), this);
      g_signal_connect(maliit_context_, "handle-key-event",
                          G_CALLBACK(MaliitHandleKeyEvent), this);
      g_signal_connect(maliit_context_, "handle-update-input-method-area",
                          G_CALLBACK(MaliitHandleUpdateInputMethodArea), this);

  } else {
      ELINUX_LOG(ERROR) << "Unable to connect to Maliit context: " << error->message;
      g_clear_error(&error);
  }
}

}  // namespace flutter
