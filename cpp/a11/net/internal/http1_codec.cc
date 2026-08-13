// Copyright 2026 The A11 Authors.

#include "a11/net/internal/http1_codec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/status/status_macros.h>
#include <absl/strings/ascii.h>
#include <absl/strings/escaping.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_split.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace a11::net::internal {
namespace {

constexpr std::string_view kCrlf = "\r\n";

// Splits a header block (without the terminating blank line) into its start
// line and the individual, unfolded header lines.
absl::StatusOr<std::vector<std::string_view>> SplitLines(
    std::string_view head_block) {
  std::vector<std::string_view> lines;
  size_t start = 0;
  while (start <= head_block.size()) {
    const size_t newline = head_block.find('\n', start);
    std::string_view line;
    if (newline == std::string_view::npos) {
      line = head_block.substr(start);
      start = head_block.size() + 1;
    } else {
      line = head_block.substr(start, newline - start);
      start = newline + 1;
    }
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.empty()) {
      continue;  // Skip the trailing blank line, if any.
    }
    lines.push_back(line);
  }
  if (lines.empty()) {
    return absl::InvalidArgumentError("Empty HTTP/1.1 message head");
  }
  return lines;
}

absl::Status ParseHeaderLines(const std::vector<std::string_view>& lines,
                              size_t first, HttpHeaders* headers) {
  for (size_t index = first; index < lines.size(); ++index) {
    const std::string_view line = lines[index];
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
      return absl::InvalidArgumentError(
          absl::StrCat("Malformed HTTP/1.1 header line: ", line));
    }
    std::string name(line.substr(0, colon));
    // Field names must not contain whitespace (obs-fold / smuggling guard).
    if (name.find_first_of(" \t") != std::string::npos) {
      return absl::InvalidArgumentError(
          absl::StrCat("Invalid whitespace in HTTP/1.1 header name: ", name));
    }
    absl::AsciiStrToLower(&name);
    std::string_view value = line.substr(colon + 1);
    value = absl::StripAsciiWhitespace(value);
    headers->emplace_back(std::move(name), std::string(value));
  }
  return absl::OkStatus();
}

// Case-insensitively returns whether the comma-separated header @p name
// contains the token @p token (e.g. Connection: keep-alive, Upgrade).
bool HeaderContainsToken(const HttpHeaders& headers, std::string_view name,
                         std::string_view token) {
  const std::optional<std::string> value = GetHttpHeader(headers, name);
  if (!value.has_value()) {
    return false;
  }
  for (std::string_view piece : absl::StrSplit(*value, ',')) {
    if (absl::EqualsIgnoreCase(absl::StripAsciiWhitespace(piece), token)) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::optional<std::size_t> FindHeaderBlockEnd(std::string_view data) {
  const size_t crlf = data.find("\r\n\r\n");
  const size_t lf = data.find("\n\n");
  size_t best = std::string_view::npos;
  size_t width = 0;
  if (crlf != std::string_view::npos) {
    best = crlf;
    width = 4;
  }
  if (lf != std::string_view::npos && lf < best) {
    best = lf;
    width = 2;
  }
  if (best == std::string_view::npos) {
    return std::nullopt;
  }
  return best + width;
}

absl::StatusOr<Http1RequestHead> ParseRequestHead(
    std::string_view head_block) {
  ABSL_ASSIGN_OR_RETURN(std::vector<std::string_view> lines,
                        SplitLines(head_block));
  const std::vector<std::string_view> parts =
      absl::StrSplit(lines[0], absl::MaxSplits(' ', 2));
  if (parts.size() != 3 || parts[0].empty() || parts[1].empty()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Malformed HTTP/1.1 request line: ", lines[0]));
  }
  if (parts[2] != "HTTP/1.1" && parts[2] != "HTTP/1.0") {
    return absl::InvalidArgumentError(
        absl::StrCat("Unsupported HTTP version: ", parts[2]));
  }
  Http1RequestHead head;
  head.method = std::string(parts[0]);
  absl::AsciiStrToUpper(&head.method);
  head.target = std::string(parts[1]);
  head.version = std::string(parts[2]);
  ABSL_RETURN_IF_ERROR(ParseHeaderLines(lines, 1, &head.headers));
  return head;
}

absl::StatusOr<Http1ResponseHead> ParseResponseHead(
    std::string_view head_block) {
  ABSL_ASSIGN_OR_RETURN(std::vector<std::string_view> lines,
                        SplitLines(head_block));
  const std::vector<std::string_view> parts =
      absl::StrSplit(lines[0], absl::MaxSplits(' ', 2));
  if (parts.size() < 2) {
    return absl::InvalidArgumentError(
        absl::StrCat("Malformed HTTP/1.1 status line: ", lines[0]));
  }
  if (parts[0] != "HTTP/1.1" && parts[0] != "HTTP/1.0") {
    return absl::InvalidArgumentError(
        absl::StrCat("Unsupported HTTP version: ", parts[0]));
  }
  int status = 0;
  if (!absl::SimpleAtoi(parts[1], &status) || status < 100 || status > 599) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid HTTP/1.1 status code: ", parts[1]));
  }
  Http1ResponseHead head;
  head.version = std::string(parts[0]);
  head.status = status;
  head.reason = parts.size() == 3 ? std::string(parts[2]) : std::string();
  ABSL_RETURN_IF_ERROR(ParseHeaderLines(lines, 1, &head.headers));
  return head;
}

namespace {

absl::StatusOr<BodyPlan> PlanBody(const HttpHeaders& headers,
                                  bool allow_until_close) {
  const bool chunked =
      HeaderContainsToken(headers, "transfer-encoding", "chunked");
  const std::optional<std::string> length_header =
      GetHttpHeader(headers, "content-length");
  if (chunked) {
    return BodyPlan{.framing = BodyFraming::kChunked};
  }
  if (length_header.has_value()) {
    std::uint64_t length = 0;
    if (!absl::SimpleAtoi(*length_header, &length)) {
      return absl::InvalidArgumentError(
          absl::StrCat("Invalid Content-Length: ", *length_header));
    }
    return BodyPlan{.framing = BodyFraming::kContentLength,
                    .content_length = static_cast<std::size_t>(length)};
  }
  if (allow_until_close) {
    return BodyPlan{.framing = BodyFraming::kUntilClose};
  }
  return BodyPlan{.framing = BodyFraming::kNone};
}

}  // namespace

absl::StatusOr<BodyPlan> PlanRequestBody(const HttpHeaders& headers) {
  // A request without framing headers has no body (never reads until close).
  return PlanBody(headers, /*allow_until_close=*/false);
}

absl::StatusOr<BodyPlan> PlanResponseBody(std::string_view request_method,
                                          int status,
                                          const HttpHeaders& headers) {
  std::string method(request_method);
  absl::AsciiStrToUpper(&method);
  if (method == "HEAD" || (status >= 100 && status < 200) || status == 204 ||
      status == 304) {
    return BodyPlan{.framing = BodyFraming::kNone};
  }
  return PlanBody(headers, /*allow_until_close=*/true);
}

absl::Status ChunkedDecoder::Feed(std::string_view data, std::string* out,
                                  bool* complete) {
  size_t offset = 0;
  while (offset < data.size() && state_ != State::kComplete) {
    switch (state_) {
      case State::kSize:
      case State::kTrailer: {
        const size_t newline = data.find('\n', offset);
        const bool have_line = newline != std::string_view::npos;
        const std::string_view slice = data.substr(
            offset, (have_line ? newline + 1 : data.size()) - offset);
        pending_.append(slice);
        offset += slice.size();
        if (!have_line) {
          return absl::OkStatus();  // Wait for the rest of the line.
        }
        std::string_view line = pending_;
        if (!line.empty() && line.back() == '\n') {
          line.remove_suffix(1);
        }
        if (!line.empty() && line.back() == '\r') {
          line.remove_suffix(1);
        }
        if (state_ == State::kSize) {
          // Strip any chunk extensions after ';'.
          const size_t semicolon = line.find(';');
          if (semicolon != std::string_view::npos) {
            line = line.substr(0, semicolon);
          }
          line = absl::StripAsciiWhitespace(line);
          std::uint64_t size = 0;
          if (line.empty() ||
              !absl::SimpleHexAtoi(std::string(line), &size)) {
            return absl::InvalidArgumentError(
                absl::StrCat("Invalid chunk size: ", line));
          }
          pending_.clear();
          if (size == 0) {
            state_ = State::kTrailer;  // Zero chunk: read optional trailers.
          } else {
            remaining_ = size;
            state_ = State::kData;
          }
        } else {  // kTrailer
          const bool blank = line.empty();
          if (blank) {
            state_ = State::kComplete;
          } else if (const size_t colon = line.find(':');
                     colon != std::string_view::npos) {
            // A malformed trailer line is dropped rather than failing the body:
            // everything the response was actually for has already been
            // delivered by this point.
            std::string name(line.substr(0, colon));
            absl::AsciiStrToLower(&name);
            trailers_.emplace_back(
                std::move(name),
                std::string(absl::StripAsciiWhitespace(line.substr(colon + 1))));
          }
          pending_.clear();
        }
        break;
      }
      case State::kData: {
        const size_t take = std::min<size_t>(remaining_, data.size() - offset);
        out->append(data.substr(offset, take));
        offset += take;
        remaining_ -= take;
        if (remaining_ == 0) {
          state_ = State::kDataCrlf;
        }
        break;
      }
      case State::kDataCrlf: {
        // Consume the CRLF that terminates a chunk's data.
        const char character = data[offset];
        pending_.push_back(character);
        ++offset;
        if (character == '\n') {
          pending_.clear();
          state_ = State::kSize;
        } else if (pending_.size() > 2) {
          return absl::InvalidArgumentError(
              "Missing CRLF after chunk data");
        }
        break;
      }
      case State::kComplete:
        break;
    }
  }
  if (complete != nullptr) {
    *complete = state_ == State::kComplete;
  }
  return absl::OkStatus();
}

std::string EncodeChunk(std::string_view data) {
  if (data.empty()) {
    return std::string();
  }
  return absl::StrCat(absl::Hex(data.size()), kCrlf, data, kCrlf);
}

std::string EncodeLastChunk(const HttpHeaders& trailers) {
  if (trailers.empty()) {
    return "0\r\n\r\n";
  }
  std::string out = "0\r\n";
  AppendHeaderBlock(trailers, &out);
  absl::StrAppend(&out, kCrlf);
  return out;
}

void AppendHeaderBlock(const HttpHeaders& headers, std::string* out) {
  for (const auto& [name, value] : headers) {
    absl::StrAppend(out, name, ": ", value, kCrlf);
  }
}

std::string SerializeRequest(std::string_view method, std::string_view target,
                             const HttpHeaders& headers) {
  std::string out = absl::StrCat(method, " ", target, " HTTP/1.1", kCrlf);
  AppendHeaderBlock(headers, &out);
  absl::StrAppend(&out, kCrlf);
  return out;
}

std::string SerializeResponse(int status, const HttpHeaders& headers,
                              std::string_view reason) {
  const std::string_view phrase =
      reason.empty() ? DefaultReasonPhrase(status) : reason;
  std::string out =
      absl::StrCat("HTTP/1.1 ", status, " ", phrase, kCrlf);
  AppendHeaderBlock(headers, &out);
  absl::StrAppend(&out, kCrlf);
  return out;
}

std::string ComputeWebSocketAccept(std::string_view key) {
  std::string input = absl::StrCat(key, kWebSocketGuid);
  std::array<unsigned char, SHA_DIGEST_LENGTH> digest{};
  SHA1(reinterpret_cast<const unsigned char*>(input.data()), input.size(),
       digest.data());
  return absl::Base64Escape(
      std::string_view(reinterpret_cast<const char*>(digest.data()),
                       digest.size()));
}

std::string GenerateWebSocketKey() {
  std::array<unsigned char, 16> nonce{};
  RAND_bytes(nonce.data(), static_cast<int>(nonce.size()));
  return absl::Base64Escape(std::string_view(
      reinterpret_cast<const char*>(nonce.data()), nonce.size()));
}

std::string_view DefaultReasonPhrase(int status) {
  switch (status) {
    case 101:
      return "Switching Protocols";
    case 200:
      return "OK";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 426:
      return "Upgrade Required";
    case 500:
      return "Internal Server Error";
    default:
      return "";
  }
}

}  // namespace a11::net::internal
