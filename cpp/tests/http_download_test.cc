// Copyright 2026 The A11 Authors.

#include "a11/net/http/download.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/strings/str_cat.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>

#include "a11/concurrency/future.h"
#include "a11/net/http2.h"

namespace a11::net {
namespace {

/// SHA-1 of kBody, so the fixture pins the digest rather than deriving it with
/// the code under test.
constexpr std::string_view kBody = "a11-model-bytes";
constexpr std::string_view kBodySha1 =
    "dbb082af15d0838550ea52fe766c49abd45ac828";

absl::Time Soon() {
  return absl::Now() + absl::Seconds(10);
}

class DownloadTestServer {
 public:
  DownloadTestServer() {
    server_ = Http2Server::Create(
        "127.0.0.1", 0,
        [this](HttpRequest request,
               std::shared_ptr<Http2ResponseWriter> response) -> a11::Task {
          ++served_;
          absl::Status status;
          if (request.path == "/model.bin") {
            status = response->SendResponse(
                200, {{"content-length", absl::StrCat(kBody.size())}},
                std::string(kBody));
          } else if (request.path == "/gone") {
            status = response->SendResponse(404, {}, "no such model");
          } else {
            status = response->SendResponse(400, {}, "unknown");
          }
          return status.ok() ? a11::ReadyTask() : a11::FailedTask(status);
        });
  }

  [[nodiscard]] bool ok() const { return server_.ok(); }
  [[nodiscard]] int served() const { return served_; }
  [[nodiscard]] std::string Url(std::string_view path) const {
    return absl::StrCat("http://127.0.0.1:", (*server_)->port(), path);
  }

 private:
  absl::StatusOr<std::shared_ptr<Http2Server>> server_;
  int served_ = 0;
};

/// A directory of its own per test, so a cache hit is never another test's file.
class HttpDownloadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            absl::StrCat("a11-download-test-",
                         ::testing::UnitTest::GetInstance()
                             ->current_test_info()
                             ->name());
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  /// @return The files in the destination's directory, so a test can assert no
  ///         temporary was left behind.
  std::vector<std::string> SiblingsOf(const std::filesystem::path& path) {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry :
         std::filesystem::directory_iterator(path.parent_path(), error)) {
      names.push_back(entry.path().filename().string());
    }
    return names;
  }

  std::filesystem::path root_;
};

TEST_F(HttpDownloadTest, DownloadsVerifiesAndCreatesParentDirectories) {
  DownloadTestServer server;
  ASSERT_TRUE(server.ok());

  // A nested destination whose directories do not exist yet.
  const std::filesystem::path destination = root_ / "models" / "model.bin";
  DownloadOptions options;
  options.destination = destination;
  options.expected_sha1 = std::string(kBodySha1);

  const auto path = Download(server.Url("/model.bin"), options).Await(Soon());
  ASSERT_TRUE(path.ok()) << path.status();
  EXPECT_EQ(*path, destination);
  ASSERT_TRUE(std::filesystem::exists(destination));

  std::ifstream in(destination, std::ios::binary);
  const std::string written((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
  EXPECT_EQ(written, kBody);
  // Nothing but the finished file: the temporary was renamed, not left.
  EXPECT_EQ(SiblingsOf(destination), (std::vector<std::string>{"model.bin"}));
}

TEST_F(HttpDownloadTest, AVerifiedCacheHitDoesNotReachTheNetwork) {
  DownloadTestServer server;
  ASSERT_TRUE(server.ok());

  const std::filesystem::path destination = root_ / "model.bin";
  DownloadOptions options;
  options.destination = destination;
  options.expected_sha1 = std::string(kBodySha1);

  ASSERT_TRUE(Download(server.Url("/model.bin"), options).Await(Soon()).ok());
  EXPECT_EQ(server.served(), 1);

  // The point of the cache: asking again is free.
  const auto again = Download(server.Url("/model.bin"), options).Await(Soon());
  ASSERT_TRUE(again.ok()) << again.status();
  EXPECT_EQ(server.served(), 1);
}

TEST_F(HttpDownloadTest, AnExistingFileWithTheWrongDigestIsRefetched) {
  DownloadTestServer server;
  ASSERT_TRUE(server.ok());

  const std::filesystem::path destination = root_ / "model.bin";
  std::filesystem::create_directories(root_);
  {
    // What an interrupted download leaves behind: present, but not the model.
    std::ofstream truncated(destination, std::ios::binary);
    truncated << "a11-mod";
  }

  DownloadOptions options;
  options.destination = destination;
  options.expected_sha1 = std::string(kBodySha1);
  const auto path = Download(server.Url("/model.bin"), options).Await(Soon());
  ASSERT_TRUE(path.ok()) << path.status();
  EXPECT_EQ(server.served(), 1);

  const auto digest = FileSha1(destination);
  ASSERT_TRUE(digest.ok()) << digest.status();
  EXPECT_EQ(*digest, kBodySha1);
}

TEST_F(HttpDownloadTest, WithoutADigestAnyExistingFileIsACacheHit) {
  DownloadTestServer server;
  ASSERT_TRUE(server.ok());

  const std::filesystem::path destination = root_ / "model.bin";
  std::filesystem::create_directories(root_);
  {
    std::ofstream existing(destination, std::ios::binary);
    existing << "whatever";
  }

  DownloadOptions options;
  options.destination = destination;  // no expected_sha1
  const auto path = Download(server.Url("/model.bin"), options).Await(Soon());
  ASSERT_TRUE(path.ok()) << path.status();
  EXPECT_EQ(server.served(), 0);
}

TEST_F(HttpDownloadTest, ADigestMismatchFailsAndLeavesNothingBehind) {
  DownloadTestServer server;
  ASSERT_TRUE(server.ok());

  const std::filesystem::path destination = root_ / "model.bin";
  DownloadOptions options;
  options.destination = destination;
  options.expected_sha1 = std::string("00000000000000000000000000000000000000ff");

  const auto path = Download(server.Url("/model.bin"), options).Await(Soon());
  ASSERT_FALSE(path.ok());
  EXPECT_EQ(path.status().code(), absl::StatusCode::kDataLoss);
  // Neither the destination nor a half-written temporary: a poisoned cache is
  // worse than an empty one.
  EXPECT_FALSE(std::filesystem::exists(destination));
  EXPECT_TRUE(SiblingsOf(destination).empty());
}

TEST_F(HttpDownloadTest, AFailedRequestLeavesNoTemporary) {
  DownloadTestServer server;
  ASSERT_TRUE(server.ok());

  const std::filesystem::path destination = root_ / "model.bin";
  DownloadOptions options;
  options.destination = destination;

  const auto path = Download(server.Url("/gone"), options).Await(Soon());
  ASSERT_FALSE(path.ok());
  EXPECT_EQ(path.status().code(), absl::StatusCode::kNotFound);
  EXPECT_FALSE(std::filesystem::exists(destination));
  EXPECT_TRUE(SiblingsOf(destination).empty());
}

TEST_F(HttpDownloadTest, ReportsProgressWithTheContentLengthAsTheTotal) {
  DownloadTestServer server;
  ASSERT_TRUE(server.ok());

  std::vector<std::pair<std::uint64_t, std::uint64_t>> progress;
  DownloadOptions options;
  options.destination = root_ / "model.bin";
  options.on_progress = [&progress](std::uint64_t done, std::uint64_t total) {
    progress.emplace_back(done, total);
  };

  ASSERT_TRUE(Download(server.Url("/model.bin"), options).Await(Soon()).ok());
  ASSERT_FALSE(progress.empty());
  EXPECT_EQ(progress.back(),
            std::make_pair(static_cast<std::uint64_t>(kBody.size()),
                           static_cast<std::uint64_t>(kBody.size())));
}

TEST_F(HttpDownloadTest, RequiresADestination) {
  const auto path =
      Download("http://127.0.0.1:1/model.bin", DownloadOptions{}).Await(Soon());
  ASSERT_FALSE(path.ok());
  EXPECT_EQ(path.status().code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(HttpDownloadTest, FileSha1ReadsAFileAndReportsAMissingOne) {
  std::filesystem::create_directories(root_);
  const std::filesystem::path path = root_ / "bytes";
  {
    std::ofstream out(path, std::ios::binary);
    out << kBody;
  }
  const auto digest = FileSha1(path);
  ASSERT_TRUE(digest.ok()) << digest.status();
  EXPECT_EQ(*digest, kBodySha1);

  EXPECT_EQ(FileSha1(root_ / "absent").status().code(),
            absl::StatusCode::kNotFound);
}

TEST_F(HttpDownloadTest, FileSha1HandlesAnEmptyFileAndOneLargerThanItsBuffer) {
  std::filesystem::create_directories(root_);

  const std::filesystem::path empty = root_ / "empty";
  { std::ofstream out(empty, std::ios::binary); }
  const auto empty_digest = FileSha1(empty);
  ASSERT_TRUE(empty_digest.ok()) << empty_digest.status();
  EXPECT_EQ(*empty_digest, "da39a3ee5e6b4b0d3255bfef95601890afd80709");

  // Larger than the 64 KiB read buffer, so the multi-read path is covered.
  const std::filesystem::path large = root_ / "large";
  {
    std::ofstream out(large, std::ios::binary);
    out << std::string(100000, 'x');
  }
  const auto large_digest = FileSha1(large);
  ASSERT_TRUE(large_digest.ok()) << large_digest.status();
  EXPECT_EQ(*large_digest, "f6ee99edde6199a3e982c46ef72bdd5cb5e41ddf");
}

}  // namespace
}  // namespace a11::net
