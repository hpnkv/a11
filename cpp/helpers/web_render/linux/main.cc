// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/strings/str_cat.h>
#include <absl/time/time.h>
#include <cmath>
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <zlib.h>

#include "sdk/http/render/protocol.h"

namespace {

using a11::sdk::http::render_protocol::Frame;

constexpr int kViewportWidth = 1280;
constexpr int kViewportHeight = 720;

void SendError(std::string_view code, std::string_view message) {
  (void)a11::sdk::http::render_protocol::WriteControl(
      STDOUT_FILENO,
      nlohmann::json{{"type", "error"}, {"code", code}, {"message", message}});
}

absl::Status SendArtifact(std::string_view role, std::string_view mimetype,
                          std::string_view bytes,
                          const nlohmann::json& metadata = {}) {
  nlohmann::json begin{{"type", "artifact_begin"},
                       {"role", role},
                       {"mimetype", mimetype},
                       {"size", bytes.size()}};
  if (metadata.is_object()) {
    for (const auto& [key, value] : metadata.items()) {
      begin[key] = value;
    }
  }
  ABSL_RETURN_IF_ERROR(
      a11::sdk::http::render_protocol::WriteControl(STDOUT_FILENO, begin));
  for (size_t offset = 0; offset < bytes.size();
       offset += a11::sdk::http::render_protocol::kMaxDataBytes) {
    ABSL_RETURN_IF_ERROR(a11::sdk::http::render_protocol::WriteData(
        STDOUT_FILENO,
        bytes.substr(offset, a11::sdk::http::render_protocol::kMaxDataBytes)));
  }
  return a11::sdk::http::render_protocol::WriteControl(
      STDOUT_FILENO, nlohmann::json{{"type", "artifact_end"}, {"role", role}});
}

void AppendBigEndian(std::string& output, std::uint32_t value) {
  output.push_back(static_cast<char>((value >> 24) & 0xff));
  output.push_back(static_cast<char>((value >> 16) & 0xff));
  output.push_back(static_cast<char>((value >> 8) & 0xff));
  output.push_back(static_cast<char>(value & 0xff));
}

void AppendPngChunk(std::string& output, std::string_view type,
                    std::string_view contents) {
  AppendBigEndian(output, static_cast<std::uint32_t>(contents.size()));
  const size_t crc_start = output.size();
  output.append(type);
  output.append(contents);
  const uLong crc =
      crc32(0, reinterpret_cast<const Bytef*>(output.data() + crc_start),
            static_cast<uInt>(type.size() + contents.size()));
  AppendBigEndian(output, static_cast<std::uint32_t>(crc));
}

absl::StatusOr<std::string> EncodePng(const std::vector<std::uint8_t>& rgba,
                                      int width, int height) {
  if (width <= 0 || height <= 0 ||
      rgba.size() != static_cast<size_t>(width) * height * 4) {
    return absl::InvalidArgumentError("Invalid snapshot pixel buffer");
  }
  const size_t row_size = static_cast<size_t>(width) * 4;
  std::vector<std::uint8_t> filtered(static_cast<size_t>(height) *
                                     (row_size + 1));
  for (int y = 0; y < height; ++y) {
    const size_t destination = static_cast<size_t>(y) * (row_size + 1);
    filtered[destination] = 0;
    std::memcpy(filtered.data() + destination + 1,
                rgba.data() + static_cast<size_t>(y) * row_size, row_size);
  }

  uLongf compressed_size = compressBound(filtered.size());
  std::string compressed(compressed_size, '\0');
  const int compressed_status =
      compress2(reinterpret_cast<Bytef*>(compressed.data()), &compressed_size,
                filtered.data(), filtered.size(), Z_BEST_SPEED);
  if (compressed_status != Z_OK) {
    return absl::InternalError("Cannot encode the rendered PNG");
  }
  compressed.resize(compressed_size);

  std::string png("\x89PNG\r\n\x1a\n", 8);
  std::string header;
  AppendBigEndian(header, static_cast<std::uint32_t>(width));
  AppendBigEndian(header, static_cast<std::uint32_t>(height));
  header.append("\x08\x06\x00\x00\x00", 5);
  AppendPngChunk(png, "IHDR", header);
  AppendPngChunk(png, "IDAT", compressed);
  AppendPngChunk(png, "IEND", {});
  return png;
}

struct Renderer {
  GMainLoop* loop = nullptr;
  WebKitWebView* web_view = nullptr;
  bool done = false;
  bool response_sent = false;
  bool serialization_started = false;
  int redirect_count = 0;
  int max_redirects = 5;
  size_t max_body_bytes = 8 * 1024 * 1024;
  size_t max_image_bytes = 8 * 1024 * 1024;
  bool include_image = false;
  int screen_heights = 1;
  int output_height = kViewportHeight;
  int destination_top = 0;
  int actual_top = 0;
  int tile_count = 0;
  guint frame_callback = 0;
  std::vector<std::uint8_t> pixels;
};

void Finish(Renderer* renderer) {
  if (renderer->done) {
    return;
  }
  renderer->done = true;
  g_main_loop_quit(renderer->loop);
}

void Fail(Renderer* renderer, std::string_view code, std::string_view message) {
  if (renderer->done) {
    return;
  }
  webkit_web_view_stop_loading(renderer->web_view);
  SendError(code, message);
  Finish(renderer);
}

void Complete(Renderer* renderer) {
  const absl::Status sent = a11::sdk::http::render_protocol::WriteControl(
      STDOUT_FILENO, nlohmann::json{{"type", "complete"}});
  if (!sent.ok()) {
    Finish(renderer);
    return;
  }
  Finish(renderer);
}

void HeaderField(const char* name, const char* value, gpointer user_data) {
  auto* headers = static_cast<nlohmann::json*>(user_data);
  gchar* lower_name = g_ascii_strdown(name, -1);
  headers->push_back(nlohmann::json::array({lower_name, value}));
  g_free(lower_name);
}

gboolean DecidePolicy(WebKitWebView*, WebKitPolicyDecision* decision,
                      WebKitPolicyDecisionType type, gpointer user_data) {
  auto* renderer = static_cast<Renderer*>(user_data);
  if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
    auto* navigation = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
    WebKitNavigationAction* action =
        webkit_navigation_policy_decision_get_navigation_action(navigation);
    WebKitURIRequest* request = webkit_navigation_action_get_request(action);
    const char* uri = webkit_uri_request_get_uri(request);
    gchar* scheme = uri == nullptr ? nullptr : g_uri_parse_scheme(uri);
    const bool allowed =
        scheme != nullptr && (g_ascii_strcasecmp(scheme, "http") == 0 ||
                              g_ascii_strcasecmp(scheme, "https") == 0 ||
                              g_ascii_strcasecmp(scheme, "about") == 0);
    g_free(scheme);
    if (!allowed) {
      webkit_policy_decision_ignore(decision);
      return TRUE;
    }
  }
  if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE &&
      !renderer->response_sent) {
    auto* response_decision = WEBKIT_RESPONSE_POLICY_DECISION(decision);
    WebKitURIResponse* response =
        webkit_response_policy_decision_get_response(response_decision);
    nlohmann::json headers = nlohmann::json::array();
    SoupMessageHeaders* fields = webkit_uri_response_get_http_headers(response);
    if (fields != nullptr) {
      soup_message_headers_foreach(fields, HeaderField, &headers);
    }
    const char* uri = webkit_uri_response_get_uri(response);
    const absl::Status sent = a11::sdk::http::render_protocol::WriteControl(
        STDOUT_FILENO,
        nlohmann::json{
            {"type", "response"},
            {"final_url", uri == nullptr ? "" : uri},
            {"status_code", webkit_uri_response_get_status_code(response)},
            {"headers", std::move(headers)},
            {"redirect_count", renderer->redirect_count}});
    if (!sent.ok()) {
      Finish(renderer);
      webkit_policy_decision_ignore(decision);
      return TRUE;
    }
    renderer->response_sent = true;
  }
  webkit_policy_decision_use(decision);
  return TRUE;
}

gboolean LoadFailed(WebKitWebView*, WebKitLoadEvent, const char*, GError* error,
                    gpointer user_data) {
  auto* renderer = static_cast<Renderer*>(user_data);
  Fail(renderer, "UNAVAILABLE",
       error == nullptr ? "WPEWebKit navigation failed" : error->message);
  return TRUE;
}

gboolean PermissionRequest(WebKitWebView*, WebKitPermissionRequest* request,
                           gpointer) {
  webkit_permission_request_deny(request);
  return TRUE;
}

gboolean ScriptDialog(WebKitWebView*, WebKitScriptDialog* dialog, gpointer) {
  webkit_script_dialog_close(dialog);
  return TRUE;
}

WebKitWebView* CreateWebView(WebKitWebView*, WebKitNavigationAction*,
                             gpointer) {
  return nullptr;
}

void WebProcessTerminated(WebKitWebView*, WebKitWebProcessTerminationReason,
                          gpointer user_data) {
  Fail(static_cast<Renderer*>(user_data), "UNAVAILABLE",
       "WPEWebKit content process terminated");
}

void SnapshotReady(GObject* source, GAsyncResult* result, gpointer user_data);

void FrameDisplayed(WebKitWebView* web_view, gpointer user_data) {
  auto* renderer = static_cast<Renderer*>(user_data);
  if (renderer->frame_callback != 0) {
    const guint callback = std::exchange(renderer->frame_callback, 0);
    webkit_web_view_remove_frame_displayed_callback(web_view, callback);
  }
  webkit_web_view_get_snapshot(web_view, WEBKIT_SNAPSHOT_REGION_VISIBLE,
                               WEBKIT_SNAPSHOT_OPTIONS_NONE, nullptr,
                               SnapshotReady, renderer);
}

void CaptureTile(Renderer* renderer);

void ScrollReady(GObject* source, GAsyncResult* result, gpointer user_data) {
  auto* renderer = static_cast<Renderer*>(user_data);
  GError* error = nullptr;
  JSCValue* value = webkit_web_view_evaluate_javascript_finish(
      WEBKIT_WEB_VIEW(source), result, &error);
  if (value == nullptr || error != nullptr || !jsc_value_is_number(value)) {
    Fail(renderer, "INTERNAL",
         error == nullptr ? "Cannot scroll the rendered document"
                          : error->message);
    g_clear_error(&error);
    if (value != nullptr) {
      g_object_unref(value);
    }
    return;
  }
  renderer->actual_top =
      static_cast<int>(std::llround(jsc_value_to_double(value)));
  g_object_unref(value);
  renderer->frame_callback = webkit_web_view_add_frame_displayed_callback(
      renderer->web_view, FrameDisplayed, renderer, nullptr);
}

void EmitImage(Renderer* renderer) {
  absl::StatusOr<std::string> png =
      EncodePng(renderer->pixels, kViewportWidth, renderer->output_height);
  if (!png.ok()) {
    Fail(renderer, "INTERNAL", png.status().message());
    return;
  }
  if (png->size() > renderer->max_image_bytes) {
    Fail(renderer, "RESOURCE_EXHAUSTED",
         "Rendered PNG exceeds max_image_bytes");
    return;
  }
  const absl::Status sent = SendArtifact(
      "snapshot", "image/png", *png,
      nlohmann::json{{"width", kViewportWidth},
                     {"height", renderer->output_height},
                     {"requested_screen_heights", renderer->screen_heights},
                     {"tile_count", renderer->tile_count},
                     {"pixel_scale", 1}});
  if (!sent.ok()) {
    Finish(renderer);
    return;
  }
  Complete(renderer);
}

void SnapshotReady(GObject* source, GAsyncResult* result, gpointer user_data) {
  auto* renderer = static_cast<Renderer*>(user_data);
  GError* error = nullptr;
  WebKitImage* image = webkit_web_view_get_snapshot_finish(
      WEBKIT_WEB_VIEW(source), result, &error);
  if (image == nullptr || error != nullptr) {
    Fail(renderer, "INTERNAL",
         error == nullptr ? "Cannot capture the WPEWebKit snapshot"
                          : error->message);
    g_clear_error(&error);
    if (image != nullptr) {
      g_object_unref(image);
    }
    return;
  }
  const int width = webkit_image_get_width(image);
  const int image_height = webkit_image_get_height(image);
  const guint stride = webkit_image_get_stride(image);
  gsize bytes_size = 0;
  const auto* bytes = static_cast<const std::uint8_t*>(
      g_bytes_get_data(webkit_image_as_bytes(image), &bytes_size));
  if (width < kViewportWidth || image_height < kViewportHeight ||
      stride < static_cast<guint>(width * 4) || bytes == nullptr ||
      bytes_size < static_cast<size_t>(stride) * image_height) {
    g_object_unref(image);
    Fail(renderer, "INTERNAL", "WPEWebKit returned invalid snapshot pixels");
    return;
  }

  const int height = std::min(
      kViewportHeight, renderer->output_height - renderer->destination_top);
  const int source_top =
      std::clamp(renderer->destination_top - renderer->actual_top, 0,
                 kViewportHeight - height);
  for (int y = 0; y < height; ++y) {
    const auto* source_row =
        bytes + static_cast<size_t>(source_top + y) * stride;
    auto* destination_row =
        renderer->pixels.data() +
        static_cast<size_t>(renderer->destination_top + y) * kViewportWidth * 4;
    for (int x = 0; x < kViewportWidth; ++x) {
      const std::uint8_t blue = source_row[x * 4];
      const std::uint8_t green = source_row[x * 4 + 1];
      const std::uint8_t red = source_row[x * 4 + 2];
      const std::uint8_t alpha = source_row[x * 4 + 3];
      destination_row[x * 4] = red;
      destination_row[x * 4 + 1] = green;
      destination_row[x * 4 + 2] = blue;
      destination_row[x * 4 + 3] = alpha;
    }
  }
  g_object_unref(image);
  ++renderer->tile_count;
  renderer->destination_top += kViewportHeight;
  CaptureTile(renderer);
}

void CaptureTile(Renderer* renderer) {
  if (renderer->destination_top >= renderer->output_height) {
    EmitImage(renderer);
    return;
  }
  const std::string script = absl::StrCat(
      "window.scrollTo(0, ", renderer->destination_top, "); window.scrollY;");
  webkit_web_view_evaluate_javascript(renderer->web_view, script.c_str(),
                                      script.size(), nullptr, nullptr, nullptr,
                                      ScrollReady, renderer);
}

void DocumentReady(GObject* source, GAsyncResult* result, gpointer user_data) {
  auto* renderer = static_cast<Renderer*>(user_data);
  GError* error = nullptr;
  JSCValue* value = webkit_web_view_evaluate_javascript_finish(
      WEBKIT_WEB_VIEW(source), result, &error);
  if (value == nullptr || error != nullptr || !jsc_value_is_string(value)) {
    Fail(renderer, "INTERNAL",
         error == nullptr ? "Cannot serialize the rendered document"
                          : error->message);
    g_clear_error(&error);
    if (value != nullptr) {
      g_object_unref(value);
    }
    return;
  }
  gchar* encoded = jsc_value_to_string(value);
  g_object_unref(value);
  nlohmann::json document =
      nlohmann::json::parse(encoded == nullptr ? "" : encoded, nullptr, false);
  g_free(encoded);
  if (!document.is_object() ||
      !document.value("html", nlohmann::json()).is_string() ||
      !document.value("text", nlohmann::json()).is_string() ||
      !document.value("height", nlohmann::json()).is_number()) {
    Fail(renderer, "INTERNAL", "WPEWebKit returned invalid document data");
    return;
  }
  const std::string html = document.at("html").get<std::string>();
  const std::string text = document.at("text").get<std::string>();
  if (html.size() > renderer->max_body_bytes ||
      text.size() > renderer->max_body_bytes) {
    Fail(renderer, "RESOURCE_EXHAUSTED",
         "Rendered page output exceeds max_body_bytes");
    return;
  }
  const absl::Status sent = SendArtifact("html", "text/plain", html);
  if (!sent.ok()) {
    Finish(renderer);
    return;
  }
  const absl::Status text_sent = SendArtifact("text", "text/plain", text);
  if (!text_sent.ok()) {
    Finish(renderer);
    return;
  }
  if (!renderer->include_image) {
    Complete(renderer);
    return;
  }
  const double document_height = document.at("height").get<double>();
  renderer->output_height = std::max(
      kViewportHeight, std::min(static_cast<int>(std::ceil(document_height)),
                                renderer->screen_heights * kViewportHeight));
  renderer->pixels.assign(
      static_cast<size_t>(kViewportWidth) * renderer->output_height * 4, 255);
  CaptureTile(renderer);
}

gboolean SerializeDocument(gpointer user_data) {
  auto* renderer = static_cast<Renderer*>(user_data);
  if (renderer->done) {
    return G_SOURCE_REMOVE;
  }
  if (!renderer->response_sent) {
    Fail(renderer, "UNAVAILABLE",
         "The page completed without an HTTP response");
    return G_SOURCE_REMOVE;
  }
  constexpr std::string_view script =
      "JSON.stringify({html: document.documentElement.outerHTML, text: "
      "((document.body && document.body.innerText) || "
      "document.documentElement.innerText || "
      "document.documentElement.textContent || ''), height: "
      "Math.max(document.documentElement.scrollHeight, document.body ? "
      "document.body.scrollHeight : 0, 720)})";
  webkit_web_view_evaluate_javascript(renderer->web_view, script.data(),
                                      script.size(), nullptr, nullptr, nullptr,
                                      DocumentReady, renderer);
  return G_SOURCE_REMOVE;
}

void LoadChanged(WebKitWebView*, WebKitLoadEvent event, gpointer user_data) {
  auto* renderer = static_cast<Renderer*>(user_data);
  if (event == WEBKIT_LOAD_REDIRECTED) {
    ++renderer->redirect_count;
    if (renderer->redirect_count > renderer->max_redirects) {
      Fail(renderer, "RESOURCE_EXHAUSTED",
           "web-render exceeded options.max_redirects");
    }
    return;
  }
  if (event == WEBKIT_LOAD_FINISHED && !renderer->serialization_started) {
    renderer->serialization_started = true;
    g_timeout_add(500, SerializeDocument, renderer);
  }
}

gboolean Timeout(gpointer user_data) {
  Fail(static_cast<Renderer*>(user_data), "DEADLINE_EXCEEDED",
       "web-render helper timed out");
  return G_SOURCE_REMOVE;
}

absl::StatusOr<nlohmann::json> ReadCommand() {
  ABSL_ASSIGN_OR_RETURN(Frame frame, a11::sdk::http::render_protocol::ReadFrame(
                                         STDIN_FILENO, absl::InfiniteFuture()));
  return a11::sdk::http::render_protocol::ParseControl(frame);
}

int Run() {
  g_setenv("WPE_DISPLAY", "wpe-display-headless", TRUE);
  const std::string engine_version =
      absl::StrCat(webkit_get_major_version(), ".", webkit_get_minor_version(),
                   ".", webkit_get_micro_version());
  const absl::Status hello = a11::sdk::http::render_protocol::WriteControl(
      STDOUT_FILENO,
      nlohmann::json{
          {"type", "hello"},
          {"protocol", a11::sdk::http::render_protocol::kVersion},
          {"implementation", "wpe-platform-headless"},
          {"engine_version", engine_version},
          {"features", nlohmann::json::array(
                           {"html", "text", "snapshot-png", "artifacts-v1"})}});
  if (!hello.ok()) {
    return 1;
  }
  absl::StatusOr<nlohmann::json> command = ReadCommand();
  if (!command.ok() || !command->is_object() ||
      command->value("type", "") != "render") {
    SendError("INVALID_ARGUMENT", command.ok() ? "invalid render request"
                                               : command.status().message());
    return 2;
  }
  const std::string url = command->value("url", std::string());
  gchar* scheme = g_uri_parse_scheme(url.c_str());
  const bool valid_url =
      scheme != nullptr && (g_ascii_strcasecmp(scheme, "http") == 0 ||
                            g_ascii_strcasecmp(scheme, "https") == 0);
  g_free(scheme);
  if (!valid_url) {
    SendError("INVALID_ARGUMENT", "web-render requires an HTTP(S) URL");
    return 2;
  }

  Renderer renderer;
  renderer.loop = g_main_loop_new(nullptr, FALSE);
  renderer.max_redirects = command->value("max_redirects", 5);
  renderer.max_body_bytes = command->value(
      "max_body_bytes", static_cast<std::uint64_t>(8 * 1024 * 1024));
  renderer.include_image = command->value("include_image", false);
  renderer.screen_heights = command->value("image_screen_heights", 1);
  renderer.max_image_bytes = command->value(
      "max_image_bytes", static_cast<std::uint64_t>(8 * 1024 * 1024));

  GError* display_error = nullptr;
  WPEDisplay* display = wpe_display_get_default();
  if (display == nullptr || !wpe_display_connect(display, &display_error)) {
    SendError("FAILED_PRECONDITION",
              display_error == nullptr
                  ? "WPEPlatform headless display is unavailable"
                  : display_error->message);
    g_clear_error(&display_error);
    g_main_loop_unref(renderer.loop);
    return 3;
  }

  WebKitNetworkSession* session = webkit_network_session_new_ephemeral();
  WebKitSettings* settings = webkit_settings_new();
  const std::string user_agent = command->value("user_agent", std::string());
  if (!user_agent.empty()) {
    webkit_settings_set_user_agent(settings, user_agent.c_str());
  }
  renderer.web_view = WEBKIT_WEB_VIEW(
      g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", session, "settings",
                   settings, "display", display, nullptr));
  g_object_unref(settings);
  g_object_unref(session);
  if (renderer.web_view == nullptr) {
    SendError("FAILED_PRECONDITION",
              "Cannot create a WPEPlatform headless WebView");
    g_main_loop_unref(renderer.loop);
    return 3;
  }
  WebKitColor background{.red = 1, .green = 1, .blue = 1, .alpha = 1};
  webkit_web_view_set_background_color(renderer.web_view, &background);
  WPEView* platform_view = webkit_web_view_get_wpe_view(renderer.web_view);
  if (platform_view == nullptr ||
      !wpe_toplevel_resize(wpe_view_get_toplevel(platform_view), kViewportWidth,
                           kViewportHeight)) {
    SendError("FAILED_PRECONDITION",
              "Cannot size the WPEPlatform headless WebView");
    g_object_unref(renderer.web_view);
    g_main_loop_unref(renderer.loop);
    return 3;
  }

  g_signal_connect(renderer.web_view, "decide-policy", G_CALLBACK(DecidePolicy),
                   &renderer);
  g_signal_connect(renderer.web_view, "load-changed", G_CALLBACK(LoadChanged),
                   &renderer);
  g_signal_connect(renderer.web_view, "load-failed", G_CALLBACK(LoadFailed),
                   &renderer);
  g_signal_connect(renderer.web_view, "permission-request",
                   G_CALLBACK(PermissionRequest), &renderer);
  g_signal_connect(renderer.web_view, "script-dialog", G_CALLBACK(ScriptDialog),
                   &renderer);
  g_signal_connect(renderer.web_view, "create", G_CALLBACK(CreateWebView),
                   &renderer);
  g_signal_connect(renderer.web_view, "web-process-terminated",
                   G_CALLBACK(WebProcessTerminated), &renderer);

  WebKitURIRequest* request = webkit_uri_request_new(url.c_str());
  SoupMessageHeaders* request_headers =
      webkit_uri_request_get_http_headers(request);
  if (command->contains("headers") && command->at("headers").is_array()) {
    for (const nlohmann::json& field : command->at("headers")) {
      if (field.is_array() && field.size() == 2 && field[0].is_string() &&
          field[1].is_string()) {
        soup_message_headers_append(
            request_headers, field[0].get_ref<const std::string&>().c_str(),
            field[1].get_ref<const std::string&>().c_str());
      }
    }
  }
  webkit_web_view_load_request(renderer.web_view, request);
  g_object_unref(request);

  const std::int64_t timeout_ms = std::clamp<std::int64_t>(
      command->value("timeout_ms", static_cast<std::int64_t>(60000)), 1,
      std::numeric_limits<guint>::max());
  g_timeout_add(static_cast<guint>(timeout_ms), Timeout, &renderer);
  g_main_loop_run(renderer.loop);

  if (renderer.frame_callback != 0) {
    webkit_web_view_remove_frame_displayed_callback(renderer.web_view,
                                                    renderer.frame_callback);
  }
  g_object_unref(renderer.web_view);
  g_main_loop_unref(renderer.loop);
  return 0;
}

}  // namespace

int main() {
  return Run();
}
