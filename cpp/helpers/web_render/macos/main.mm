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

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/status/status_macros.h>
#include <absl/status/statusor.h>
#include <absl/time/time.h>
#include <nlohmann/json.hpp>

#include "sdk/http/render/protocol.h"

using a11::sdk::http::render_protocol::Frame;
using a11::sdk::http::render_protocol::FrameKind;

constexpr int kViewportWidth = 1280;
constexpr int kViewportHeight = 720;

std::string Utf8(NSString* value) {
  if (value == nil) {
    return {};
  }
  const char* bytes = [value UTF8String];
  return bytes == nullptr ? std::string() : std::string(bytes);
}

void SendError(std::string_view code, std::string_view message) {
  (void)a11::sdk::http::render_protocol::WriteControl(
      STDOUT_FILENO,
      nlohmann::json{{"type", "error"},
                     {"code", code},
                     {"message", message}});
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
        bytes.substr(offset,
                     a11::sdk::http::render_protocol::kMaxDataBytes)));
  }
  return a11::sdk::http::render_protocol::WriteControl(
      STDOUT_FILENO, nlohmann::json{{"type", "artifact_end"}, {"role", role}});
}

@interface Renderer : NSObject <WKNavigationDelegate, WKUIDelegate>
@property(nonatomic) WKWebView* webView;
@property(nonatomic) NSWindow* window;
@property(nonatomic) BOOL done;
@property(nonatomic) BOOL responseSent;
@property(nonatomic) BOOL startedSerialization;
@property(nonatomic) NSInteger redirectCount;
@property(nonatomic) NSInteger maxRedirects;
@property(nonatomic) NSUInteger maxBodyBytes;
@property(nonatomic) NSUInteger maxImageBytes;
@property(nonatomic) BOOL includeImage;
@property(nonatomic) NSInteger screenHeights;
@property(nonatomic) NSInteger documentHeight;
@property(nonatomic) NSInteger outputHeight;
@property(nonatomic) NSMutableArray<NSImage*>* tileImages;
@property(nonatomic) NSMutableArray<NSNumber*>* tileSourceTops;
@property(nonatomic) NSMutableArray<NSNumber*>* tileDestinationTops;
@property(nonatomic) NSMutableArray<NSNumber*>* tileHeights;
@end

@implementation Renderer

- (void)failCode:(std::string_view)code message:(NSString*)message {
  if (self.done) {
    return;
  }
  [self.webView stopLoading];
  SendError(code, Utf8(message));
  self.done = YES;
}

- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)action
                    decisionHandler:
                        (void (^)(WKNavigationActionPolicy))decisionHandler {
  NSString* scheme = action.request.URL.scheme.lowercaseString;
  if ([scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"] ||
      [scheme isEqualToString:@"about"]) {
    decisionHandler(WKNavigationActionPolicyAllow);
  } else {
    decisionHandler(WKNavigationActionPolicyCancel);
  }
}

- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationResponse:(WKNavigationResponse*)response
                      decisionHandler:
                          (void (^)(WKNavigationResponsePolicy))decisionHandler {
  if (!self.responseSent && response.isForMainFrame &&
      [response.response isKindOfClass:[NSHTTPURLResponse class]]) {
    NSHTTPURLResponse* http = (NSHTTPURLResponse*)response.response;
    NSMutableArray* fields = [NSMutableArray array];
    for (id key in http.allHeaderFields) {
      [fields addObject:@[ [key description],
                           [http.allHeaderFields[key] description] ]];
    }
    nlohmann::json headers = nlohmann::json::array();
    for (NSArray* field in fields) {
      headers.push_back(nlohmann::json::array(
          {Utf8([field[0] lowercaseString]), Utf8(field[1])}));
    }
    const absl::Status sent =
        a11::sdk::http::render_protocol::WriteControl(
            STDOUT_FILENO,
            nlohmann::json{{"type", "response"},
                           {"final_url", Utf8(http.URL.absoluteString)},
                           {"status_code", http.statusCode},
                           {"headers", std::move(headers)},
                           {"redirect_count", self.redirectCount}});
    if (!sent.ok()) {
      self.done = YES;
      decisionHandler(WKNavigationResponsePolicyCancel);
      return;
    }
    self.responseSent = YES;
  }
  decisionHandler(WKNavigationResponsePolicyAllow);
}

- (void)webView:(WKWebView*)webView
    didReceiveServerRedirectForProvisionalNavigation:
        (WKNavigation*)navigation {
  ++self.redirectCount;
  if (self.redirectCount > self.maxRedirects) {
    [self failCode:"RESOURCE_EXHAUSTED"
            message:@"web-render exceeded options.max_redirects"];
  }
}

- (void)webView:(WKWebView*)webView
    didFailNavigation:(WKNavigation*)navigation
            withError:(NSError*)error {
  [self failCode:"UNAVAILABLE" message:error.localizedDescription];
}

- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
                       withError:(NSError*)error {
  [self failCode:"UNAVAILABLE" message:error.localizedDescription];
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView*)webView {
  [self failCode:"UNAVAILABLE" message:@"WebKit content process terminated"];
}

- (void)webView:(WKWebView*)webView
    runJavaScriptAlertPanelWithMessage:(NSString*)message
                     initiatedByFrame:(WKFrameInfo*)frame
                    completionHandler:(void (^)(void))completionHandler {
  completionHandler();
}

- (void)webView:(WKWebView*)webView
    runJavaScriptConfirmPanelWithMessage:(NSString*)message
                       initiatedByFrame:(WKFrameInfo*)frame
                      completionHandler:
                          (void (^)(BOOL result))completionHandler {
  completionHandler(NO);
}

- (void)webView:(WKWebView*)webView
    runJavaScriptTextInputPanelWithPrompt:(NSString*)prompt
                              defaultText:(NSString*)defaultText
                         initiatedByFrame:(WKFrameInfo*)frame
                        completionHandler:
                            (void (^)(NSString* result))completionHandler {
  completionHandler(nil);
}

- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
               forNavigationAction:(WKNavigationAction*)navigationAction
                    windowFeatures:(WKWindowFeatures*)windowFeatures {
  return nil;
}

- (void)webView:(WKWebView*)webView
    didFinishNavigation:(WKNavigation*)navigation {
  if (self.startedSerialization) {
    return;
  }
  self.startedSerialization = YES;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC),
                 dispatch_get_main_queue(), ^{
                   [self serializeDocument];
                 });
}

- (void)serializeDocument {
  if (self.done) {
    return;
  }
  if (!self.responseSent) {
    [self failCode:"UNAVAILABLE"
            message:@"The page completed without an HTTP response"];
    return;
  }
  NSString* script =
      @"JSON.stringify({html: document.documentElement.outerHTML, text: "
       "((document.body && document.body.innerText) || "
       "document.documentElement.innerText || "
       "document.documentElement.textContent || ''), height: "
       "Math.max(document.documentElement.scrollHeight, document.body ? "
       "document.body.scrollHeight : 0, 720)})";
  [self.webView evaluateJavaScript:script
                completionHandler:^(id value, NSError* error) {
                  if (error != nil || ![value isKindOfClass:[NSString class]]) {
                    [self failCode:"INTERNAL"
                            message:error.localizedDescription != nil
                                        ? error.localizedDescription
                                        : @"Cannot serialize rendered document"];
                    return;
                  }
                  const std::string encoded = Utf8((NSString*)value);
                  nlohmann::json document =
                      nlohmann::json::parse(encoded, nullptr, false);
                  if (!document.is_object() ||
                      !document.value("html", nlohmann::json()).is_string() ||
                      !document.value("text", nlohmann::json()).is_string() ||
                      !document.value("height", nlohmann::json()).is_number()) {
                    [self failCode:"INTERNAL"
                            message:@"WebKit returned invalid document data"];
                    return;
                  }
                  const std::string html = document.at("html").get<std::string>();
                  const std::string text = document.at("text").get<std::string>();
                  if (html.size() > self.maxBodyBytes ||
                      text.size() > self.maxBodyBytes) {
                    [self failCode:"RESOURCE_EXHAUSTED"
                            message:@"Rendered page output exceeds "
                                     "max_body_bytes"];
                    return;
                  }
                  self.documentHeight = static_cast<NSInteger>(std::ceil(
                      document.at("height").get<double>()));
                  self.outputHeight = std::max<NSInteger>(
                      kViewportHeight,
                      std::min<NSInteger>(self.documentHeight,
                                          self.screenHeights * kViewportHeight));
                  const absl::Status sent =
                      SendArtifact("html", "text/plain", html);
                  if (!sent.ok()) {
                    self.done = YES;
                    return;
                  }
                  const absl::Status textSent =
                      SendArtifact("text", "text/plain", text);
                  if (!textSent.ok()) {
                    self.done = YES;
                    return;
                  }
                  if (!self.includeImage) {
                    [self complete];
                    return;
                  }
                  [self captureTile:0];
                }];
}

- (void)captureTile:(NSInteger)destinationTop {
  if (self.done) {
    return;
  }
  if (destinationTop >= self.outputHeight) {
    [self encodeImage];
    return;
  }
  NSString* script = [NSString
      stringWithFormat:@"window.scrollTo(0, %ld); window.scrollY;",
                       static_cast<long>(destinationTop)];
  [self.webView evaluateJavaScript:script
                completionHandler:^(id value, NSError* error) {
                  if (error != nil || ![value isKindOfClass:[NSNumber class]]) {
                    [self failCode:"INTERNAL"
                            message:error.localizedDescription != nil
                                        ? error.localizedDescription
                                        : @"Cannot scroll rendered document"];
                    return;
                  }
                  const NSInteger actualTop =
                      static_cast<NSInteger>(std::llround([value doubleValue]));
                  dispatch_after(
                      dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC),
                      dispatch_get_main_queue(), ^{
                        WKSnapshotConfiguration* config =
                            [[WKSnapshotConfiguration alloc] init];
                        config.rect = CGRectMake(0, 0, kViewportWidth,
                                                  kViewportHeight);
                        config.snapshotWidth = @(kViewportWidth);
                        config.afterScreenUpdates = YES;
                        [self.webView
                            takeSnapshotWithConfiguration:config
                                        completionHandler:^(NSImage* image,
                                                            NSError* snapError) {
                                          if (snapError != nil || image == nil) {
                                            [self
                                                failCode:"INTERNAL"
                                                 message:snapError
                                                                     .localizedDescription !=
                                                                 nil
                                                             ? snapError
                                                                   .localizedDescription
                                                             : @"Cannot capture WebKit snapshot"];
                                            return;
                                          }
                                          const NSInteger height =
                                              std::min<NSInteger>(
                                                  kViewportHeight,
                                                  self.outputHeight -
                                                      destinationTop);
                                          const NSInteger sourceTop =
                                              std::clamp<NSInteger>(
                                                  destinationTop - actualTop, 0,
                                                  kViewportHeight - height);
                                          [self.tileImages addObject:image];
                                          [self.tileSourceTops
                                              addObject:@(sourceTop)];
                                          [self.tileDestinationTops
                                              addObject:@(destinationTop)];
                                          [self.tileHeights addObject:@(height)];
                                          [self captureTile:destinationTop +
                                                            kViewportHeight];
                                        }];
                      });
                }];
}

- (void)encodeImage {
  NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc]
      initWithBitmapDataPlanes:nil
                    pixelsWide:kViewportWidth
                    pixelsHigh:self.outputHeight
                 bitsPerSample:8
               samplesPerPixel:4
                      hasAlpha:YES
                      isPlanar:NO
                colorSpaceName:NSCalibratedRGBColorSpace
                   bytesPerRow:0
                  bitsPerPixel:0];
  if (bitmap == nil) {
    [self failCode:"INTERNAL" message:@"Cannot allocate snapshot bitmap"];
    return;
  }
  NSGraphicsContext* context =
      [NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap];
  [NSGraphicsContext saveGraphicsState];
  [NSGraphicsContext setCurrentContext:context];
  [[NSColor whiteColor] setFill];
  NSRectFill(NSMakeRect(0, 0, kViewportWidth,
                        static_cast<CGFloat>(self.outputHeight)));
  for (NSUInteger index = 0; index < self.tileImages.count; ++index) {
    const NSInteger sourceTop = self.tileSourceTops[index].integerValue;
    const NSInteger destinationTop =
        self.tileDestinationTops[index].integerValue;
    const NSInteger height = self.tileHeights[index].integerValue;
    NSRect source =
        NSMakeRect(0,
                   static_cast<CGFloat>(kViewportHeight - sourceTop - height),
                   kViewportWidth, static_cast<CGFloat>(height));
    NSRect destination =
        NSMakeRect(0,
                   static_cast<CGFloat>(self.outputHeight - destinationTop -
                                        height),
                   kViewportWidth, static_cast<CGFloat>(height));
    [self.tileImages[index] drawInRect:destination
                              fromRect:source
                             operation:NSCompositingOperationCopy
                              fraction:1.0
                        respectFlipped:NO
                                 hints:nil];
  }
  [NSGraphicsContext restoreGraphicsState];
  NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG
                                     properties:@{}];
  if (png == nil || png.length > self.maxImageBytes) {
    [self failCode:"RESOURCE_EXHAUSTED"
            message:@"Rendered PNG exceeds max_image_bytes"];
    return;
  }
  const std::string bytes(static_cast<const char*>(png.bytes), png.length);
  const absl::Status sent = SendArtifact(
      "snapshot", "image/png", bytes,
      nlohmann::json{{"width", kViewportWidth},
                     {"height", self.outputHeight},
                     {"tile_count", self.tileImages.count},
                     {"pixel_scale", 1}});
  if (!sent.ok()) {
    self.done = YES;
    return;
  }
  [self complete];
}

- (void)complete {
  if (self.done) {
    return;
  }
  const absl::Status sent =
      a11::sdk::http::render_protocol::WriteControl(
          STDOUT_FILENO, nlohmann::json{{"type", "complete"}});
  self.done = YES;
  if (!sent.ok()) {
    return;
  }
}

@end

absl::StatusOr<nlohmann::json> ReadCommand() {
  ABSL_ASSIGN_OR_RETURN(
      Frame frame, a11::sdk::http::render_protocol::ReadFrame(
                       STDIN_FILENO, absl::InfiniteFuture()));
  return a11::sdk::http::render_protocol::ParseControl(frame);
}

int Run() {
  const absl::Status hello = a11::sdk::http::render_protocol::WriteControl(
      STDOUT_FILENO,
      nlohmann::json{
          {"type", "hello"},
          {"protocol", a11::sdk::http::render_protocol::kVersion},
          {"implementation", "wkwebview"},
          {"engine_version", Utf8([[NSProcessInfo processInfo]
                                      operatingSystemVersionString])},
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
  NSURL* target = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
  if (target == nil ||
      (![[target.scheme lowercaseString] isEqualToString:@"http"] &&
       ![[target.scheme lowercaseString] isEqualToString:@"https"])) {
    SendError("INVALID_ARGUMENT", "web-render requires an HTTP(S) URL");
    return 2;
  }

  [NSApplication sharedApplication];
  Renderer* renderer = [[Renderer alloc] init];
  renderer.maxRedirects = command->value("max_redirects", 5);
  renderer.maxBodyBytes = command->value(
      "max_body_bytes", static_cast<std::uint64_t>(8 * 1024 * 1024));
  renderer.includeImage = command->value("include_image", false);
  renderer.screenHeights = command->value("image_screen_heights", 1);
  renderer.maxImageBytes = command->value(
      "max_image_bytes", static_cast<std::uint64_t>(8 * 1024 * 1024));
  renderer.tileImages = [NSMutableArray array];
  renderer.tileSourceTops = [NSMutableArray array];
  renderer.tileDestinationTops = [NSMutableArray array];
  renderer.tileHeights = [NSMutableArray array];

  WKWebViewConfiguration* configuration =
      [[WKWebViewConfiguration alloc] init];
  configuration.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
  configuration.preferences.javaScriptCanOpenWindowsAutomatically = NO;
  WKWebView* webView = [[WKWebView alloc]
      initWithFrame:NSMakeRect(0, 0, kViewportWidth, kViewportHeight)
      configuration:configuration];
  renderer.webView = webView;
  webView.navigationDelegate = renderer;
  webView.UIDelegate = renderer;
  const std::string user_agent =
      command->value("user_agent", std::string());
  if (!user_agent.empty()) {
    webView.customUserAgent =
        [NSString stringWithUTF8String:user_agent.c_str()];
  }
  NSWindow* window = [[NSWindow alloc]
      initWithContentRect:NSMakeRect(-20000, -20000, kViewportWidth,
                                     kViewportHeight)
                styleMask:NSWindowStyleMaskBorderless
                  backing:NSBackingStoreBuffered
                    defer:NO];
  renderer.window = window;
  window.contentView = webView;
  [window orderFrontRegardless];

  NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:target];
  request.HTTPMethod = @"GET";
  if (command->contains("headers") && command->at("headers").is_array()) {
    for (const nlohmann::json& field : command->at("headers")) {
      if (field.is_array() && field.size() == 2 && field[0].is_string() &&
          field[1].is_string()) {
        [request addValue:[NSString stringWithUTF8String:
                                      field[1].get_ref<const std::string&>().c_str()]
            forHTTPHeaderField:[NSString stringWithUTF8String:
                                         field[0]
                                             .get_ref<const std::string&>()
                                             .c_str()]];
      }
    }
  }
  [webView loadRequest:request];

  const std::int64_t timeout_ms = std::max<std::int64_t>(
      1, command->value("timeout_ms", static_cast<std::int64_t>(60000)));
  const std::int64_t timeout_ns =
      timeout_ms > INT64_MAX / static_cast<std::int64_t>(NSEC_PER_MSEC)
          ? INT64_MAX
          : timeout_ms * static_cast<std::int64_t>(NSEC_PER_MSEC);
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, timeout_ns),
                 dispatch_get_main_queue(), ^{
                   [renderer failCode:"DEADLINE_EXCEEDED"
                               message:@"web-render helper timed out"];
                 });
  while (!renderer.done) {
    @autoreleasepool {
      [[NSRunLoop currentRunLoop]
          runMode:NSDefaultRunLoopMode
       beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
  }
  [window orderOut:nil];
  [window close];
  return 0;
}

int main() {
  @autoreleasepool {
    return Run();
  }
}
