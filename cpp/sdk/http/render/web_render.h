// Copyright 2026 The A11 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef A11_SDK_HTTP_RENDER_WEB_RENDER_H_
#define A11_SDK_HTTP_RENDER_WEB_RENDER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <absl/base/nullability.h>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>

namespace a11::sdk::http {

/** Metadata available before a page finishes rendering. */
struct WebRenderResponse {
  std::string final_url;
  int status_code = 0;
  std::vector<std::pair<std::string, std::string>> headers;
};

/** Receives response metadata while rendering continues. */
class WebRenderResponseObserver {
 public:
  virtual ~WebRenderResponseObserver() = default;
  /** Publishes the final top-frame response. */
  virtual absl::Status OnResponse(const WebRenderResponse& response) = 0;
};

struct WebRenderRequest {
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string user_agent;
  absl::Time deadline = absl::InfiniteFuture();
  int max_redirects = 5;
  size_t max_body_bytes = 8 * 1024 * 1024;
  bool include_image = false;
  int image_screen_heights = 1;
  size_t max_image_bytes = 8 * 1024 * 1024;
  WebRenderResponseObserver* absl_nullable response_observer = nullptr;
};

struct WebRenderResult {
  std::string final_url;
  int status_code = 0;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string html;
  std::string text;
  std::string image_png;
  int image_width = 0;
  int image_height = 0;
  int image_tile_count = 0;
};

/** Renders one page with the platform helper. */
absl::StatusOr<WebRenderResult> RenderWebPage(const WebRenderRequest& request);

}  // namespace a11::sdk::http

#endif  // A11_SDK_HTTP_RENDER_WEB_RENDER_H_
