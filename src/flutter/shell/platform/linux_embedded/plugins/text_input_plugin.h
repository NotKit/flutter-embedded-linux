// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_PLUGINS_TEXT_INPUT_PLUGIN_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_PLUGINS_TEXT_INPUT_PLUGIN_H_

#include <rapidjson/document.h>

#include <memory>
#include <string>

#include <glib.h>
#include <maliit-glib/maliitbus.h>

#include "flutter/shell/platform/common/client_wrapper/include/flutter/binary_messenger.h"
#include "flutter/shell/platform/common/client_wrapper/include/flutter/method_channel.h"
#include "flutter/shell/platform/common/text_input_model.h"
#include "flutter/shell/platform/linux_embedded/window_binding_handler.h"

namespace flutter {

// Talks to maliit-server over libmaliit-glib. The behaviour to match is the one
// in Maliit's own Qt input context (input-context/minputcontext.cpp), not just
// the D-Bus signatures: the server keeps its own copy of the field's text and
// predicts how it evolves, so every edit has to be pushed back to it.
class TextInputPlugin {
 public:
  TextInputPlugin(BinaryMessenger* messenger, WindowBindingHandler* delegate);
  ~TextInputPlugin();

  void OnKeyPressed(uint32_t keycode, uint32_t code_point);
  void DispatchEvent();

  // Hides the on-screen keyboard when the window loses focus and restores it
  // when focus returns to a still-focused text field.
  void OnWindowActivated(bool activated);

 private:
  // Sends the current state of the given model to the Flutter engine.
  void SendStateUpdate(const TextInputModel& model);

  // Sends an action triggered by the Enter key to the Flutter engine.
  void EnterPressed(TextInputModel* model);

  // Called when a method is called on |channel_|;
  void HandleMethodCall(
      const flutter::MethodCall<rapidjson::Document>& method_call,
      std::unique_ptr<flutter::MethodResult<rapidjson::Document>> result);

  void InitMaliitConnection();

  // Context interface. All of it is handled: a handler that returns TRUE stops
  // the emission, so a method left unconnected - or completed late - hangs the
  // server's call until D-Bus times it out 25s later. Every handler completes
  // the invocation first thing.
  static gboolean MaliitHandleActivationLostEvent(
      MaliitContext* obj,
      GDBusMethodInvocation* invocation,
      gpointer user_data);

  static gboolean MaliitHandleIMInitiatedHide(MaliitContext *obj,
                                              GDBusMethodInvocation *invocation,
                                              gpointer user_data);

  static gboolean MaliitHandleCommitString(MaliitContext *obj,
                                           GDBusMethodInvocation *invocation,
                                           const gchar *string,
                                           int replacement_start,
                                           int replacement_length,
                                           int cursor_pos,
                                           gpointer user_data);

  static gboolean MaliitHandleUpdatePreedit(MaliitContext *obj,
                                            GDBusMethodInvocation *invocation,
                                            const gchar *string,
                                            GVariant *formatListData,
                                            gint replaceStart,
                                            gint replaceLength,
                                            gint cursorPos,
                                            gpointer user_data);

  static gboolean MaliitHandleKeyEvent(MaliitContext *obj,
                                       GDBusMethodInvocation *invocation,
                                       gint type,
                                       gint key,
                                       gint modifiers,
                                       const gchar *text,
                                       gboolean auto_repeat,
                                       int count,
                                       guchar request_type,
                                       gpointer user_data);

  static gboolean MaliitHandleUpdateInputMethodArea(MaliitContext *obj,
                                                    GDBusMethodInvocation *invocation,
                                                    gint x,
                                                    gint y,
                                                    gint width,
                                                    gint height,
                                                    gpointer user_data);

  static gboolean MaliitHandleSetGlobalCorrectionEnabled(
      MaliitContext* obj,
      GDBusMethodInvocation* invocation,
      gboolean enabled,
      gpointer user_data);

  static gboolean MaliitHandlePreeditRectangle(
      MaliitContext* obj,
      GDBusMethodInvocation* invocation,
      gpointer user_data);

  static gboolean MaliitHandleSetRedirectKeys(
      MaliitContext* obj,
      GDBusMethodInvocation* invocation,
      gboolean enabled,
      gpointer user_data);

  static gboolean MaliitHandleSetDetectableAutoRepeat(
      MaliitContext* obj,
      GDBusMethodInvocation* invocation,
      gboolean enabled,
      gpointer user_data);

  static gboolean MaliitHandleSetSelection(MaliitContext* obj,
                                           GDBusMethodInvocation* invocation,
                                           gint start,
                                           gint length,
                                           gpointer user_data);

  static gboolean MaliitHandleSelection(MaliitContext* obj,
                                        GDBusMethodInvocation* invocation,
                                        gpointer user_data);

  static gboolean MaliitHandleSetLanguage(MaliitContext* obj,
                                          GDBusMethodInvocation* invocation,
                                          const gchar* language,
                                          gpointer user_data);

  static gboolean MaliitHandleNotifyExtendedAttributeChanged(
      MaliitContext* obj,
      GDBusMethodInvocation* invocation,
      gint id,
      const gchar* target,
      const gchar* target_item,
      const gchar* attribute,
      GVariant* value,
      gpointer user_data);

  // Completion of maliit_server_call_reset(), which keeps |pending_resets_|.
  static void MaliitResetFinished(GObject* source,
                                  GAsyncResult* res,
                                  gpointer user_data);

  // Fires 100ms after MaliitHideInputMethod().
  static gboolean MaliitHideTimeout(gpointer user_data);

  void MaliitShowInputMethod();

  // Hides after a short delay, so that moving focus between two fields does
  // not flap the panel down and up.
  void MaliitHideInputMethod();
  void MaliitHideNow();
  void CancelDeferredHide();

  // Ends the composition and asks the server to reset. Commits that were
  // already in flight are ignored until the reset completes.
  void MaliitReset();

  // reset + focusState:false + hide. This is the only way to tell the server
  // that nothing accepts input any more.
  void MaliitReleaseContext();

  // Reports the surrounding (committed) text and cursor to the Maliit server.
  void MaliitUpdateSurrounding(bool focus_changed);

  // Tells the server that nothing accepts input any more.
  void MaliitReportFocusLost();

  // Pushes the state if an edit marked it stale.
  void FlushSurrounding();

  // Removes |length| UTF-16 units starting |start| units from the cursor and
  // leaves the cursor at the insertion point. Ends composing first, so the
  // offsets are relative to the start of the preedit, as Qt defines them.
  void ApplyReplacement(int start, int length);

  // Maps the current input type to a Maliit::TextContentType value.
  int MaliitContentType() const;

  // Maps the current input action to a Qt::EnterKeyType value; this is what
  // labels the keyboard's action key.
  int MaliitEnterKeyType() const;

  // The MethodChannel used for communication with the Flutter engine.
  std::unique_ptr<flutter::MethodChannel<rapidjson::Document>> channel_;

  // The active client id.
  int client_id_ = 0;

  // The active model. nullptr if not set.
  std::unique_ptr<TextInputModel> active_model_;

  // Keyboard type of the client. See available options:
  // https://docs.flutter.io/flutter/services/TextInputType-class.html
  std::string input_type_;

  // An action requested by the user on the input client. See available options:
  // https://docs.flutter.io/flutter/services/TextInputAction-class.html
  std::string input_action_;

  // Whether the client hides what is typed (a password field).
  bool obscure_text_ = false;

  // The delegate for virtual keyboard updates.
  WindowBindingHandler* delegate_;

  // Set after an edit; the state is flushed to the server in DispatchEvent(),
  // so that a burst of edits costs one D-Bus call.
  bool surrounding_dirty_ = false;

  // Set when the next state push moves focus in or out.
  bool focus_changed_ = false;

  // Whether Flutter wants the on-screen keyboard shown. Tracks show/hide
  // requests so focus changes can hide and restore the keyboard.
  bool keyboard_shown_ = false;

  // A show request that arrived before any field was focused. The server has
  // no widget state to show a keyboard for yet, so the show is issued from
  // setClient instead.
  bool show_pending_ = false;

  // Whether activateContext() has been sent. It is sent once, when something
  // first accepts input, not on every focus change.
  bool im_active_ = false;

  // Whether the server was last told focusState:true.
  bool focus_reported_ = false;

  // Number of reset() calls still in flight. Commits and preedits that arrive
  // while one is pending belong to the text that was just dropped.
  int pending_resets_ = 0;

  // GLib source id of the deferred hide, 0 when none is armed.
  guint hide_timer_ = 0;

  GMainContext *glib_ctx_ = nullptr;
  GMainLoop *glib_loop_ = nullptr;
  MaliitServer *maliit_server_ = nullptr;
  MaliitContext *maliit_context_ = nullptr;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_PLUGINS_TEXT_INPUT_PLUGIN_H_
