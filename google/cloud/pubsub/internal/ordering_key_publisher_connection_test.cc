// Copyright 2020 Google LLC
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

#include "google/cloud/pubsub/internal/ordering_key_publisher_connection.h"
#include "google/cloud/pubsub/testing/mock_message_batcher.h"
#include "google/cloud/testing_util/assert_ok.h"
#include <google/protobuf/text_format.h>
#include <gmock/gmock.h>

namespace google {
namespace cloud {
namespace pubsub_internal {
inline namespace GOOGLE_CLOUD_CPP_PUBSUB_NS {
namespace {

TEST(OrderingKeyPublisherConnectionTest, Publish) {
  struct TestStep {
    std::string ordering_key;
    std::string data;
  } steps[] = {
      {"k0", "data0"}, {"k1", "data1"}, {"k0", "data2"},
      {"k0", "data3"}, {"k0", "data4"},
  };

  std::vector<pubsub::Message> received;
  auto factory = [&](std::string const& ordering_key) {
    auto mock = std::make_shared<pubsub_testing::MockMessageBatcher>();
    EXPECT_CALL(*mock, Publish)
        .WillRepeatedly([ordering_key](pubsub::Message const& m) {
          EXPECT_EQ(ordering_key, m.ordering_key());
          auto ack_id = m.ordering_key() + "#" + std::string(m.data());
          return make_ready_future(make_status_or(ack_id));
        });
    EXPECT_CALL(*mock, Flush).Times(2);
    return mock;
  };

  auto publisher = OrderingKeyPublisherConnection::Create(factory);

  std::vector<future<void>> results;
  for (auto const& step : steps) {
    std::string ack_id = step.ordering_key + "#" + step.data;
    results.push_back(publisher
                          ->Publish({pubsub::MessageBuilder{}
                                         .SetData(step.data)
                                         .SetOrderingKey(step.ordering_key)
                                         .Build()})
                          .then([ack_id](future<StatusOr<std::string>> f) {
                            auto r = f.get();
                            ASSERT_STATUS_OK(r);
                            EXPECT_EQ(ack_id, *r);
                          }));
  }
  for (auto& r : results) r.get();

  publisher->Flush();
  publisher->Flush();
}

}  // namespace
}  // namespace GOOGLE_CLOUD_CPP_PUBSUB_NS
}  // namespace pubsub_internal
}  // namespace cloud
}  // namespace google
