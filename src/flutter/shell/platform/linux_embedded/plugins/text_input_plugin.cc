// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux_embedded/plugins/text_input_plugin.h"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <codecvt>
#include <cstdio>
#include <iostream>
#include <locale>
#include <map>
#include <stdexcept>

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
  // Labels the keyboard's action key.
  enum EnterKeyType {
    EnterKeyDefault = 0,
    EnterKeyReturn = 1,
    EnterKeyDone = 2,
    EnterKeyGo = 3,
    EnterKeySend = 4,
    EnterKeySearch = 5,
    EnterKeyNext = 6,
    EnterKeyPrevious = 7
  };
}

namespace Maliit {
  // from maliit/namespace.h
  enum EventRequestType {
    EventRequestBoth = 0,
    EventRequestSignalOnly = 1,
    EventRequestEventOnly = 2
  };
  enum TextContentType {
    FreeTextContentType = 0,
    NumberContentType = 1,
    PhoneNumberContentType = 2,
    EmailContentType = 3,
    UrlContentType = 4,
    CustomContentType = 5
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
constexpr char kTextInputObscureText[] = "obscureText";
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

// MInputContext::hideInputPanel() defers the hide by this much and cancels it
// if a show arrives in the meantime.
constexpr guint kHideDelayMs = 100;

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

std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> Utf16Converter() {
  return std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>();
}

// Completes a fire-and-forget server call. Every generated
// maliit_server_call_*_finish() is a thin wrapper around
// g_dbus_proxy_call_finish(), so one callback completes them all.
// |user_data| is the method name, for the error message.
void MaliitCallFinished(GObject* source, GAsyncResult* res, gpointer user_data) {
  GError* error = nullptr;
  GVariant* reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);
  if (reply) {
    g_variant_unref(reply);
    return;
  }
  ELINUX_LOG(ERROR) << "Maliit " << static_cast<const char*>(user_data)
                    << " failed: " << error->message;
  g_error_free(error);
}
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
    surrounding_dirty_ = true;
  }
}

void TextInputPlugin::DispatchEvent() {
  // Drain the context rather than iterating once: incoming method calls and
  // the replies to our own async calls share these iterations, so a single one
  // per frame would let a burst of commits fall behind.
  for (int i = 0; i < 16 && g_main_context_iteration(glib_ctx_, FALSE); i++) {
  }

  FlushSurrounding();
}

void TextInputPlugin::OnWindowActivated(bool activated) {
  if (activated) {
    // Focus returned and a field is still active: bring the keyboard back.
    if (keyboard_shown_ && active_model_) {
      MaliitShowInputMethod();
    }
    return;
  }

  // The context belongs to this window. Keeping it active after the shell
  // moved focus elsewhere means the commits meant for the other app keep
  // arriving here. |keyboard_shown_| survives so the panel can be restored.
  MaliitReleaseContext();
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

TextInputPlugin::~TextInputPlugin() {
  // Drop the model first: the release then needs neither the channel nor a
  // completion callback into an object that is going away.
  active_model_ = nullptr;
  CancelDeferredHide();
  MaliitReleaseContext();

  g_clear_object(&maliit_context_);
  g_clear_object(&maliit_server_);
  g_main_context_pop_thread_default(glib_ctx_);
  g_main_loop_unref(glib_loop_);
  g_main_context_unref(glib_ctx_);
}

void TextInputPlugin::HandleMethodCall(
    const flutter::MethodCall<rapidjson::Document>& method_call,
    std::unique_ptr<flutter::MethodResult<rapidjson::Document>> result) {
  const std::string& method = method_call.method_name();

  if (method.compare(kShowMethod) == 0) {
    keyboard_shown_ = true;
    delegate_->UpdateVirtualKeyboardStatus(true);
    MaliitShowInputMethod();
  } else if (method.compare(kHideMethod) == 0) {
    keyboard_shown_ = false;
    delegate_->UpdateVirtualKeyboardStatus(false);
    MaliitHideInputMethod();
  } else if (method.compare(kClearClientMethod) == 0) {
    keyboard_shown_ = false;
    show_pending_ = false;
    // Focus left the field. Tell the server, or its copy of the text outlives
    // the field it came from. The reset comes first, while the model can still
    // say whether there was a preedit. The hide is deferred, so moving to the
    // next field does not flap the panel.
    MaliitReset();
    active_model_ = nullptr;
    surrounding_dirty_ = false;
    focus_changed_ = false;
    MaliitReportFocusLost();
    MaliitHideInputMethod();
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
    obscure_text_ = false;
    auto obscure_text_json = client_config.FindMember(kTextInputObscureText);
    if (obscure_text_json != client_config.MemberEnd() &&
        obscure_text_json->value.IsBool()) {
      obscure_text_ = obscure_text_json->value.GetBool();
    }
    active_model_ = std::make_unique<TextInputModel>();
    focus_changed_ = true;
    if (show_pending_) {
      // A show arrived before anything accepted input; issue it now.
      MaliitShowInputMethod();
    } else if (im_active_) {
      surrounding_dirty_ = true;
    }
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
    // Flutter is the source of truth for the field; report it to Maliit.
    surrounding_dirty_ = true;
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
gboolean TextInputPlugin::MaliitHandleActivationLostEvent(
    MaliitContext* obj,
    GDBusMethodInvocation* invocation,
    gpointer user_data)
{
  maliit_context_complete_activation_lost_event(obj, invocation);

  auto self = reinterpret_cast<TextInputPlugin*>(user_data);
  // Re-activation goes through MaliitShowInputMethod(), which sends
  // activateContext() and the state again. |keyboard_shown_| is left alone so
  // the panel still comes back with the window.
  self->im_active_ = false;
  self->focus_reported_ = false;
  if (self->delegate_) {
    // The panel is gone; give the view its height back.
    self->delegate_->UpdateVirtualKeyboardArea(0, 0, 0, 0);
  }

  return TRUE;
}

gboolean TextInputPlugin::MaliitHandleIMInitiatedHide(MaliitContext *obj,
                              GDBusMethodInvocation *invocation,
                              gpointer user_data)
{
  maliit_context_complete_im_initiated_hide(obj, invocation);

  auto self = reinterpret_cast<TextInputPlugin*>(user_data);

  // The user dismissed the keyboard. MInputContext drops focus from the input
  // item here; Flutter's equivalent (TextInputClient.onConnectionClosed) also
  // fires onSubmitted, which in a chat app would send the message. So the
  // field keeps focus - tapping it asks for the panel again - and only the
  // composition is finished.
  self->keyboard_shown_ = false;
  if (self->delegate_) {
    self->delegate_->UpdateVirtualKeyboardStatus(false);
  }

  if (self->active_model_ && self->active_model_->composing()) {
    self->active_model_->EndComposing();
    self->SendStateUpdate(*self->active_model_);
    self->surrounding_dirty_ = true;
  }

  return TRUE;
}

gboolean TextInputPlugin::MaliitHandleCommitString(MaliitContext *obj,
                              GDBusMethodInvocation *invocation,
                              const gchar *string,
                              int replacement_start,
                              int replacement_length,
                              int cursor_pos,
                              gpointer user_data)
{
  maliit_context_complete_commit_string(obj, invocation);

  auto self = reinterpret_cast<TextInputPlugin*>(user_data);
  // A commit for the preedit we just dropped may already have been in flight
  // when we reset; letting it land would re-insert the text.
  if (!self->active_model_ || self->pending_resets_ > 0) {
    return TRUE;
  }

  self->ApplyReplacement(replacement_start, replacement_length);

  size_t base = self->active_model_->selection().start();
  self->active_model_->AddText(string ? string : "");
  if (cursor_pos >= 0) {
    // Counted from the insertion point; a negative value means "after the
    // inserted text", which is where AddText already left the cursor.
    size_t end = self->active_model_->text_range().end();
    self->active_model_->SetSelection(
        TextRange(std::min(base + static_cast<size_t>(cursor_pos), end)));
  }

  self->SendStateUpdate(*self->active_model_);
  self->surrounding_dirty_ = true;

  return TRUE;
}

gboolean TextInputPlugin::MaliitHandleUpdatePreedit(MaliitContext *obj,
                               GDBusMethodInvocation *invocation,
                               const gchar *string,
                               GVariant *formatListData,
                               gint replaceStart,
                               gint replaceLength,
                               gint cursorPos,
                               gpointer user_data)
{
  maliit_context_complete_update_preedit(obj, invocation);

  auto self = reinterpret_cast<TextInputPlugin*>(user_data);
  if (!self->active_model_ || self->pending_resets_ > 0) {
    return TRUE;
  }

  // Drops the old preedit and any span the IME is pulling back into it (e.g.
  // backspacing into a finished word).
  self->ApplyReplacement(replaceStart, replaceLength);

  // An empty preedit only removes the composition. Leaving composing mode on
  // with a stale word makes it reappear once the field is emptied.
  if (string && string[0] != '\0') {
    self->active_model_->BeginComposing();
    self->active_model_->UpdateComposingText(string);
    if (cursorPos >= 0) {
      // For a preedit the cursor is an offset inside it.
      TextRange composing = self->active_model_->composing_range();
      self->active_model_->SetComposingRange(
          composing, std::min(static_cast<size_t>(cursorPos),
                              composing.length()));
    }
  }

  self->SendStateUpdate(*self->active_model_);
  self->surrounding_dirty_ = true;

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
                          guchar request_type,
                          gpointer user_data)
{
  maliit_context_complete_key_event(obj, invocation);

  auto self = reinterpret_cast<TextInputPlugin*>(user_data);
  if (!self->active_model_) {
    return TRUE;
  }

  // The keyboard asked for the signal only: the key must not reach the app.
  if (request_type == Maliit::EventRequestSignalOnly) {
    return TRUE;
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
  // libmaliit-glib installs its own completer for this one, but it runs after
  // the emission, which returning TRUE below stops. Complete it here instead.
  maliit_context_complete_update_input_method_area(obj, invocation);

  auto self = reinterpret_cast<TextInputPlugin*>(user_data);

  if (self->delegate_) {
    self->delegate_->UpdateVirtualKeyboardArea(x, y, width, height);
  }

  return TRUE;
}

gboolean TextInputPlugin::MaliitHandleSetGlobalCorrectionEnabled(
    MaliitContext* obj,
    GDBusMethodInvocation* invocation,
    gboolean enabled G_GNUC_UNUSED,
    gpointer user_data G_GNUC_UNUSED)
{
  maliit_context_complete_set_global_correction_enabled(obj, invocation);
  return TRUE;
}

gboolean TextInputPlugin::MaliitHandlePreeditRectangle(
    MaliitContext* obj,
    GDBusMethodInvocation* invocation,
    gpointer user_data G_GNUC_UNUSED)
{
  // The server calls this synchronously and the keyboard stalls until it is
  // answered. Flutter does not tell us where the preedit is; "not valid" is an
  // accepted answer.
  maliit_context_complete_preedit_rectangle(obj, invocation, FALSE, 0, 0, 0, 0);
  return TRUE;
}

gboolean TextInputPlugin::MaliitHandleSetRedirectKeys(
    MaliitContext* obj,
    GDBusMethodInvocation* invocation,
    gboolean enabled G_GNUC_UNUSED,
    gpointer user_data G_GNUC_UNUSED)
{
  maliit_context_complete_set_redirect_keys(obj, invocation);
  return TRUE;
}

gboolean TextInputPlugin::MaliitHandleSetDetectableAutoRepeat(
    MaliitContext* obj,
    GDBusMethodInvocation* invocation,
    gboolean enabled G_GNUC_UNUSED,
    gpointer user_data G_GNUC_UNUSED)
{
  maliit_context_complete_set_detectable_auto_repeat(obj, invocation);
  return TRUE;
}

gboolean TextInputPlugin::MaliitHandleSetSelection(
    MaliitContext* obj,
    GDBusMethodInvocation* invocation,
    gint start,
    gint length,
    gpointer user_data)
{
  maliit_context_complete_set_selection(obj, invocation);

  auto self = reinterpret_cast<TextInputPlugin*>(user_data);
  if (!self->active_model_ || start < 0 || length < 0) {
    return TRUE;
  }

  // Moving the caret ends the composition, as it does in Qt.
  if (self->active_model_->composing()) {
    self->active_model_->EndComposing();
  }

  size_t end = self->active_model_->text_range().end();
  size_t base = std::min(static_cast<size_t>(start), end);
  size_t extent = std::min(base + static_cast<size_t>(length), end);
  if (!self->active_model_->SetSelection(TextRange(base, extent))) {
    return TRUE;
  }

  self->SendStateUpdate(*self->active_model_);
  self->surrounding_dirty_ = true;

  return TRUE;
}

gboolean TextInputPlugin::MaliitHandleSelection(
    MaliitContext* obj,
    GDBusMethodInvocation* invocation,
    gpointer user_data)
{
  auto self = reinterpret_cast<TextInputPlugin*>(user_data);

  std::string selected;
  gboolean valid = FALSE;
  if (self->active_model_) {
    TextRange selection = self->active_model_->selection();
    if (!selection.collapsed()) {
      try {
        auto converter = Utf16Converter();
        std::u16string text =
            converter.from_bytes(self->active_model_->GetText());
        selected = converter.to_bytes(
            text.substr(selection.start(), selection.length()));
        valid = TRUE;
      } catch (const std::range_error& e) {
        ELINUX_LOG(ERROR) << "Cannot convert the selected text: " << e.what();
      }
    }
  }

  // Also called synchronously by the server.
  maliit_context_complete_selection(obj, invocation, valid, selected.c_str());
  return TRUE;
}

gboolean TextInputPlugin::MaliitHandleSetLanguage(
    MaliitContext* obj,
    GDBusMethodInvocation* invocation,
    const gchar* language G_GNUC_UNUSED,
    gpointer user_data G_GNUC_UNUSED)
{
  maliit_context_complete_set_language(obj, invocation);
  return TRUE;
}

gboolean TextInputPlugin::MaliitHandleNotifyExtendedAttributeChanged(
    MaliitContext* obj,
    GDBusMethodInvocation* invocation,
    gint id G_GNUC_UNUSED,
    const gchar* target G_GNUC_UNUSED,
    const gchar* target_item G_GNUC_UNUSED,
    const gchar* attribute G_GNUC_UNUSED,
    GVariant* value G_GNUC_UNUSED,
    gpointer user_data G_GNUC_UNUSED)
{
  maliit_context_complete_notify_extended_attribute_changed(obj, invocation);
  return TRUE;
}

void TextInputPlugin::MaliitResetFinished(GObject* source,
                                          GAsyncResult* res,
                                          gpointer user_data) {
  auto self = reinterpret_cast<TextInputPlugin*>(user_data);

  GError* error = nullptr;
  if (!maliit_server_call_reset_finish(MALIIT_SERVER(source), res, &error)) {
    ELINUX_LOG(ERROR) << "Maliit reset failed: " << error->message;
    g_error_free(error);
  }

  if (self->pending_resets_ > 0) {
    self->pending_resets_--;
  }
}

gboolean TextInputPlugin::MaliitHideTimeout(gpointer user_data) {
  auto self = reinterpret_cast<TextInputPlugin*>(user_data);
  self->hide_timer_ = 0;
  self->MaliitHideNow();
  return G_SOURCE_REMOVE;
}

void TextInputPlugin::ApplyReplacement(int start, int length) {
  // The offsets are counted from the start of the preedit, or from the cursor
  // when there is none, so the preedit goes first. Its text stays: unlike Qt,
  // where the preedit lives outside the widget, here it is what the user sees
  // in the field.
  if (active_model_->composing()) {
    active_model_->UpdateComposingText(std::u16string());
    active_model_->EndComposing();
  }

  if (length <= 0) {
    return;
  }

  // Selecting the span and replacing it with nothing keeps this in UTF-16
  // units, which is what Maliit counts in. A negative start (replace the word
  // before the cursor) is the normal case.
  size_t text_end = active_model_->text_range().end();
  long cursor = static_cast<long>(active_model_->selection().start());
  size_t base = std::min(static_cast<size_t>(std::max(cursor + start, 0L)),
                         text_end);
  size_t end = std::min(base + static_cast<size_t>(length), text_end);
  if (base == end) {
    return;
  }
  active_model_->SetSelection(TextRange(base, end));
  active_model_->AddText(std::u16string());
}

void TextInputPlugin::MaliitShowInputMethod() {
  if (!maliit_server_) {
    return;
  }

  CancelDeferredHide();

  if (!active_model_) {
    // Nothing accepts input yet, so the server has no widget state to show a
    // keyboard for. setClient issues the show instead.
    show_pending_ = true;
    return;
  }
  show_pending_ = false;

  if (!im_active_) {
    im_active_ = true;
    maliit_server_call_activate_context(
        maliit_server_, NULL, MaliitCallFinished,
        const_cast<char*>("activateContext"));
  }

  // The state has to be there before the panel is: the word engine reads it to
  // decide what is being typed.
  MaliitUpdateSurrounding(!focus_reported_ || focus_changed_);
  surrounding_dirty_ = false;
  focus_changed_ = false;

  maliit_server_call_show_input_method(
      maliit_server_, NULL, MaliitCallFinished,
      const_cast<char*>("showInputMethod"));
}

void TextInputPlugin::MaliitHideInputMethod() {
  if (!maliit_server_ || hide_timer_) {
    return;
  }

  GSource* source = g_timeout_source_new(kHideDelayMs);
  g_source_set_callback(source, MaliitHideTimeout, this, nullptr);
  hide_timer_ = g_source_attach(source, glib_ctx_);
  g_source_unref(source);
}

void TextInputPlugin::MaliitHideNow() {
  if (!maliit_server_) {
    return;
  }

  maliit_server_call_hide_input_method(
      maliit_server_, NULL, MaliitCallFinished,
      const_cast<char*>("hideInputMethod"));
}

void TextInputPlugin::CancelDeferredHide() {
  if (!hide_timer_) {
    return;
  }

  GSource* source = g_main_context_find_source_by_id(glib_ctx_, hide_timer_);
  if (source) {
    g_source_destroy(source);
  }
  hide_timer_ = 0;
}

void TextInputPlugin::MaliitReset() {
  if (!maliit_server_) {
    return;
  }

  bool had_preedit = active_model_ && active_model_->composing();
  if (had_preedit) {
    // Keep the text and just finish the composition; dropping it the way
    // MInputContext does would erase the word from the field.
    active_model_->EndComposing();
    SendStateUpdate(*active_model_);
    surrounding_dirty_ = true;
  }

  if (!had_preedit) {
    maliit_server_call_reset(maliit_server_, NULL, MaliitCallFinished,
                             const_cast<char*>("reset"));
    return;
  }

  // The keyboard may already have a commit for that preedit in flight. Ignore
  // what arrives until the reset is acknowledged, or it lands afterwards and
  // types the word again.
  pending_resets_++;
  maliit_server_call_reset(maliit_server_, NULL, MaliitResetFinished, this);
}

void TextInputPlugin::MaliitReleaseContext() {
  if (!maliit_server_) {
    return;
  }

  MaliitReset();
  // A pending flush would report focusState:true again right after this.
  surrounding_dirty_ = false;
  focus_changed_ = false;
  MaliitReportFocusLost();
  CancelDeferredHide();
  MaliitHideNow();
}

void TextInputPlugin::MaliitReportFocusLost() {
  if (!maliit_server_ || !focus_reported_) {
    return;
  }

  // focusState alone is the release signal: the server drops the widget state
  // it was predicting against.
  focus_reported_ = false;
  maliit_server_call_update_widget_information(
      maliit_server_, g_variant_new_parsed("{'focusState': <false>}"), TRUE,
      NULL, MaliitCallFinished,
      const_cast<char*>("updateWidgetInformation"));
}

void TextInputPlugin::FlushSurrounding() {
  if (!surrounding_dirty_) {
    return;
  }
  surrounding_dirty_ = false;

  MaliitUpdateSurrounding(focus_changed_);
  focus_changed_ = false;
}

int TextInputPlugin::MaliitContentType() const {
  if (input_type_ == "TextInputType.number") {
    return Maliit::NumberContentType;
  }
  if (input_type_ == "TextInputType.phone") {
    return Maliit::PhoneNumberContentType;
  }
  if (input_type_ == "TextInputType.emailAddress") {
    return Maliit::EmailContentType;
  }
  if (input_type_ == "TextInputType.url") {
    return Maliit::UrlContentType;
  }
  return Maliit::FreeTextContentType;
}

int TextInputPlugin::MaliitEnterKeyType() const {
  if (input_action_ == "TextInputAction.done") {
    return Qt::EnterKeyDone;
  }
  if (input_action_ == "TextInputAction.go") {
    return Qt::EnterKeyGo;
  }
  if (input_action_ == "TextInputAction.send") {
    return Qt::EnterKeySend;
  }
  if (input_action_ == "TextInputAction.search") {
    return Qt::EnterKeySearch;
  }
  if (input_action_ == "TextInputAction.next") {
    return Qt::EnterKeyNext;
  }
  if (input_action_ == "TextInputAction.previous") {
    return Qt::EnterKeyPrevious;
  }
  return Qt::EnterKeyDefault;
}

void TextInputPlugin::MaliitUpdateSurrounding(bool focus_changed) {
  if (!maliit_server_ || !active_model_) {
    return;
  }

  // Both positions are UTF-16 code unit offsets: the server indexes the text
  // it keeps as a QString.
  int cursor = 0;
  int anchor = 0;
  std::string surrounding;
  try {
    auto converter = Utf16Converter();
    std::u16string text = converter.from_bytes(active_model_->GetText());
    if (active_model_->composing()) {
      // Report the committed text only. If the word engine sees the in-flight
      // preedit it tracks words the field no longer holds and restores them.
      TextRange composing = active_model_->composing_range();
      text.erase(composing.start(), composing.length());
      cursor = anchor = static_cast<int>(composing.start());
    } else {
      TextRange selection = active_model_->selection();
      cursor = static_cast<int>(selection.extent());
      anchor = static_cast<int>(selection.base());
    }
    surrounding = converter.to_bytes(text);
  } catch (const std::range_error& e) {
    ELINUX_LOG(ERROR) << "Cannot convert the field text: " << e.what();
    surrounding.clear();
    cursor = anchor = 0;
  }

  if (!g_utf8_validate(surrounding.c_str(), surrounding.size(), nullptr)) {
    // g_variant_new_string() refuses anything else and returns NULL, which
    // drops the key from the dict: the keyboard then works with no context at
    // all. An empty field is the safer lie.
    ELINUX_LOG(ERROR) << "Surrounding text is not valid UTF-8";
    surrounding.clear();
    cursor = anchor = 0;
  }

  int content_type = MaliitContentType();
  // Prediction, correction and autocapitalization only make sense for prose.
  // Obscured fields also opt out so passwords never reach the word engine.
  const char* assist =
      (content_type == Maliit::FreeTextContentType && !obscure_text_) ? "true"
                                                                     : "false";
  const char* hidden_text = obscure_text_ ? "true" : "false";

  // Bake everything except the text into the parsed template; keep the text as
  // a %s placeholder so g_variant_new_parsed handles quoting/escaping.
  char state_template[1024];
  std::snprintf(state_template, sizeof(state_template),
                "{'surroundingText': <%%s>,"
                " 'cursorPosition': <%d>,"
                " 'anchorPosition': <%d>,"
                " 'hasSelection': <%s>,"
                " 'contentType': <%d>,"
                " 'enterKeyType': <%d>,"
                " 'predictionEnabled': <%s>,"
                " 'correctionEnabled': <%s>,"
                " 'autocapitalizationEnabled': <%s>,"
                " 'hiddenText': <%s>,"
                " 'toolbarId': <0>,"
                " 'focusState': <true>}",
                cursor, anchor, cursor != anchor ? "true" : "false",
                content_type, MaliitEnterKeyType(), assist, assist, assist,
                hidden_text);

  GVariant* state = g_variant_new_parsed(state_template, surrounding.c_str());

  // One line per push. When something goes wrong here it is always because a
  // state stopped being sent, which is invisible in everything but this.
  ELINUX_LOG(DEBUG) << "Maliit state: cursor=" << cursor << " anchor=" << anchor
                    << " focusChanged=" << focus_changed
                    << " text=" << surrounding;

  focus_reported_ = true;
  maliit_server_call_update_widget_information(
      maliit_server_, state, focus_changed, NULL, MaliitCallFinished,
      const_cast<char*>("updateWidgetInformation"));
}

void TextInputPlugin::InitMaliitConnection() {
  glib_ctx_ = g_main_context_new();
  glib_loop_ = g_main_loop_new(glib_ctx_, FALSE);
  g_main_context_push_thread_default(glib_ctx_);

  ELINUX_LOG(INFO) << "Initializing Maliit connection";

  maliit_server_ = nullptr;
  maliit_context_ = nullptr;

  GError *error = NULL;
  maliit_server_ = maliit_get_server_sync(NULL, &error);
  if (maliit_server_) {
      g_object_ref(maliit_server_);
      g_signal_connect(maliit_server_, "invoke-action", G_CALLBACK(maliit_im_invoke_action), this);
      // toolbarId 0 in the widget state refers to this extension; the action
      // key can be overridden through it.
      maliit_server_call_register_attribute_extension(
          maliit_server_, 0, "", NULL, MaliitCallFinished,
          const_cast<char*>("registerAttributeExtension"));
  } else {
      // libmaliit-glib returns NULL without an error when the server address
      // property is missing, which is the normal "no keyboard here" case.
      ELINUX_LOG(ERROR) << "Unable to connect to Maliit server: "
                        << (error ? error->message : "no server address");
      g_clear_error(&error);
      return;
  }

  maliit_context_ = maliit_get_context_sync(NULL, &error);
  if (maliit_context_) {
      g_object_ref(maliit_context_);
      g_signal_connect(maliit_context_, "handle-activation-lost-event",
                          G_CALLBACK(MaliitHandleActivationLostEvent), this);
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
      g_signal_connect(maliit_context_, "handle-set-global-correction-enabled",
                          G_CALLBACK(MaliitHandleSetGlobalCorrectionEnabled), this);
      g_signal_connect(maliit_context_, "handle-preedit-rectangle",
                          G_CALLBACK(MaliitHandlePreeditRectangle), this);
      g_signal_connect(maliit_context_, "handle-set-redirect-keys",
                          G_CALLBACK(MaliitHandleSetRedirectKeys), this);
      g_signal_connect(maliit_context_, "handle-set-detectable-auto-repeat",
                          G_CALLBACK(MaliitHandleSetDetectableAutoRepeat), this);
      g_signal_connect(maliit_context_, "handle-set-selection",
                          G_CALLBACK(MaliitHandleSetSelection), this);
      g_signal_connect(maliit_context_, "handle-selection",
                          G_CALLBACK(MaliitHandleSelection), this);
      g_signal_connect(maliit_context_, "handle-set-language",
                          G_CALLBACK(MaliitHandleSetLanguage), this);
      g_signal_connect(maliit_context_, "handle-notify-extended-attribute-changed",
                          G_CALLBACK(MaliitHandleNotifyExtendedAttributeChanged), this);
      // handle-plugin-settings-loaded is left to libmaliit-glib, which
      // completes it itself.
  } else {
      ELINUX_LOG(ERROR) << "Unable to connect to Maliit context: "
                        << (error ? error->message : "no server address");
      g_clear_error(&error);
  }
}

}  // namespace flutter
