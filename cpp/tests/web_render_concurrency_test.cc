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

#include <cstdlib>
#include <memory>

#include <absl/status/status.h>
#include <absl/time/clock.h>
#include <absl/time/time.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include "a11/concurrency/executor.h"
#include "a11/concurrency/future.h"
#include "sdk/http/render/protocol.h"

namespace a11::sdk::http {
namespace {

TEST(WebRenderConcurrencyTest, PipeWaitYieldsTheOnlyWorker) {
  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::pipe(descriptors), 0);

  auto entered = std::make_shared<a11::Promise<a11::Unit>>();
  a11::Task waiting = a11::SubmitTask([read_fd = descriptors[0], entered] {
    EXPECT_TRUE(entered->SetResult(a11::Unit{}).ok());
    const absl::Status status =
        render_protocol::ReadFrame(read_fd,
                                   absl::Now() + absl::Milliseconds(300))
            .status();
    return status;
  });
  ASSERT_TRUE(entered->future().Await(absl::Now() + absl::Seconds(1)).ok());

  a11::Task quick = a11::SubmitTask([] { return absl::OkStatus(); });
  EXPECT_TRUE(quick.Await(absl::Now() + absl::Milliseconds(100)).ok());
  EXPECT_EQ(waiting.Await(absl::Now() + absl::Seconds(1)).status().code(),
            absl::StatusCode::kDeadlineExceeded);

  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

}  // namespace
}  // namespace a11::sdk::http

int main(int argc, char** argv) {
  (void)::setenv("A11_POOL_THREADS", "1", 1);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
