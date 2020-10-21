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

#include "google/cloud/pubsub/internal/batching_publisher_connection.h"
#include "google/cloud/pubsub/mocks/mock_publisher_connection.h"
#include "google/cloud/pubsub/testing/test_retry_policies.h"
#include "google/cloud/testing_util/assert_ok.h"
#include "google/cloud/testing_util/fake_completion_queue_impl.h"
#include "google/cloud/testing_util/status_matchers.h"
#include <gmock/gmock.h>

namespace google {
namespace cloud {
namespace pubsub_internal {
inline namespace GOOGLE_CLOUD_CPP_PUBSUB_NS {
namespace {

using ::google::cloud::testing_util::StatusIs;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::Return;

std::vector<std::string> DataElements(
    pubsub::PublisherConnection::PublishParams const& p) {
  std::vector<std::string> data;
  std::transform(p.request.messages().begin(), p.request.messages().end(),
                 std::back_inserter(data),
                 [](google::pubsub::v1::PubsubMessage const& m) {
                   return std::string(m.data());
                 });
  return data;
}

TEST(BatchingPublisherConnectionTest, DefaultMakesProgress) {
  auto mock = std::make_shared<pubsub_mocks::MockPublisherConnection>();
  pubsub::Topic const topic("test-project", "test-topic");

  google::cloud::internal::AutomaticallyCreatedBackgroundThreads background;
  EXPECT_CALL(*mock, cq).WillRepeatedly(Return(background.cq()));
  EXPECT_CALL(*mock, Publish)
      .WillOnce([&](pubsub::PublisherConnection::PublishParams const& p) {
        EXPECT_EQ(topic.FullName(), p.request.topic());
        EXPECT_THAT(DataElements(p), ElementsAre("test-data-0", "test-data-1"));
        google::pubsub::v1::PublishResponse response;
        response.add_message_ids("test-message-id-0");
        response.add_message_ids("test-message-id-1");
        return make_ready_future(make_status_or(response));
      });

  auto publisher = BatchingPublisherConnection::Create(
      topic,
      pubsub::PublisherOptions{}
          .set_maximum_batch_message_count(4)
          .set_maximum_hold_time(std::chrono::milliseconds(50)),
      mock);

  // We expect the responses to be satisfied in the context of the completion
  // queue threads. This is an important property, the processing of any
  // responses should be scheduled with any other work.
  auto const main_thread = std::this_thread::get_id();
  std::vector<future<void>> published;
  published.push_back(
      publisher
          ->Publish({pubsub::MessageBuilder{}.SetData("test-data-0").Build()})
          .then([&](future<StatusOr<std::string>> f) {
            auto r = f.get();
            ASSERT_STATUS_OK(r);
            EXPECT_EQ("test-message-id-0", *r);
            EXPECT_NE(main_thread, std::this_thread::get_id());
          }));
  published.push_back(
      publisher
          ->Publish({pubsub::MessageBuilder{}.SetData("test-data-1").Build()})
          .then([&](future<StatusOr<std::string>> f) {
            auto r = f.get();
            ASSERT_STATUS_OK(r);
            EXPECT_EQ("test-message-id-1", *r);
            EXPECT_NE(main_thread, std::this_thread::get_id());
          }));
  publisher->Flush();
  for (auto& p : published) p.get();
}

TEST(BatchingPublisherConnectionTest, BatchByMessageCount) {
  auto mock = std::make_shared<pubsub_mocks::MockPublisherConnection>();
  pubsub::Topic const topic("test-project", "test-topic");

  google::cloud::internal::AutomaticallyCreatedBackgroundThreads background;
  EXPECT_CALL(*mock, cq).WillRepeatedly(Return(background.cq()));
  EXPECT_CALL(*mock, Publish)
      .WillOnce([&](pubsub::PublisherConnection::PublishParams const& p) {
        EXPECT_EQ(topic.FullName(), p.request.topic());
        EXPECT_THAT(DataElements(p), ElementsAre("test-data-0", "test-data-1"));
        google::pubsub::v1::PublishResponse response;
        response.add_message_ids("test-message-id-0");
        response.add_message_ids("test-message-id-1");
        return make_ready_future(make_status_or(response));
      });

  // Use our own completion queue, initially inactive, to avoid race conditions
  // due to the zero-maximum-hold-time timer expiring.
  google::cloud::CompletionQueue cq;
  auto publisher = BatchingPublisherConnection::Create(
      topic, pubsub::PublisherOptions{}.set_maximum_batch_message_count(2),
      mock);
  auto r0 =
      publisher
          ->Publish({pubsub::MessageBuilder{}.SetData("test-data-0").Build()})
          .then([](future<StatusOr<std::string>> f) {
            auto r = f.get();
            ASSERT_STATUS_OK(r);
            EXPECT_EQ("test-message-id-0", *r);
          });
  auto r1 =
      publisher
          ->Publish({pubsub::MessageBuilder{}.SetData("test-data-1").Build()})
          .then([](future<StatusOr<std::string>> f) {
            auto r = f.get();
            ASSERT_STATUS_OK(r);
            EXPECT_EQ("test-message-id-1", *r);
          });

  std::thread t([&cq] { cq.Run(); });

  r0.get();
  r1.get();

  cq.Shutdown();
  t.join();
}

TEST(BatchingPublisherConnectionTest, BatchByMessageSize) {
  auto mock = std::make_shared<pubsub_mocks::MockPublisherConnection>();
  pubsub::Topic const topic("test-project", "test-topic");

  google::cloud::internal::AutomaticallyCreatedBackgroundThreads background;
  EXPECT_CALL(*mock, cq).WillRepeatedly(Return(background.cq()));
  EXPECT_CALL(*mock, Publish)
      .WillOnce([&](pubsub::PublisherConnection::PublishParams const& p) {
        EXPECT_EQ(topic.FullName(), p.request.topic());
        EXPECT_THAT(DataElements(p), ElementsAre("test-data-0", "test-data-1"));
        google::pubsub::v1::PublishResponse response;
        response.add_message_ids("test-message-id-0");
        response.add_message_ids("test-message-id-1");
        return make_ready_future(make_status_or(response));
      });

  // see https://cloud.google.com/pubsub/pricing
  auto constexpr kMessageSizeOverhead = 20;
  auto constexpr kMaxMessageBytes =
      sizeof("test-data-N") + kMessageSizeOverhead + 2;
  // Use our own completion queue, initially inactive, to avoid race conditions
  // due to the zero-maximum-hold-time timer expiring.
  google::cloud::CompletionQueue cq;
  auto publisher = BatchingPublisherConnection::Create(
      topic,
      pubsub::PublisherOptions{}
          .set_maximum_batch_message_count(4)
          .set_maximum_batch_bytes(kMaxMessageBytes),
      mock);
  auto r0 =
      publisher
          ->Publish({pubsub::MessageBuilder{}.SetData("test-data-0").Build()})
          .then([](future<StatusOr<std::string>> f) {
            auto r = f.get();
            ASSERT_STATUS_OK(r);
            EXPECT_EQ("test-message-id-0", *r);
          });
  auto r1 =
      publisher
          ->Publish({pubsub::MessageBuilder{}.SetData("test-data-1").Build()})
          .then([](future<StatusOr<std::string>> f) {
            auto r = f.get();
            ASSERT_STATUS_OK(r);
            EXPECT_EQ("test-message-id-1", *r);
          });

  std::thread t([&cq] { cq.Run(); });

  r0.get();
  r1.get();

  cq.Shutdown();
  t.join();
}

TEST(BatchingPublisherConnectionTest, BatchByMaximumHoldTime) {
  auto mock = std::make_shared<pubsub_mocks::MockPublisherConnection>();
  pubsub::Topic const topic("test-project", "test-topic");

  google::cloud::internal::AutomaticallyCreatedBackgroundThreads background;
  EXPECT_CALL(*mock, cq).WillRepeatedly(Return(background.cq()));
  EXPECT_CALL(*mock, Publish)
      .WillOnce([&](pubsub::PublisherConnection::PublishParams const& p) {
        EXPECT_EQ(topic.FullName(), p.request.topic());
        EXPECT_THAT(DataElements(p), ElementsAre("test-data-0", "test-data-1"));
        google::pubsub::v1::PublishResponse response;
        response.add_message_ids("test-message-id-0");
        response.add_message_ids("test-message-id-1");
        return make_ready_future(make_status_or(response));
      });

  // Use our own completion queue, initially inactive, to avoid race conditions
  // due to the maximum-hold-time timer expiring.
  google::cloud::CompletionQueue cq;
  auto publisher = BatchingPublisherConnection::Create(
      topic,
      pubsub::PublisherOptions{}
          .set_maximum_hold_time(std::chrono::milliseconds(5))
          .set_maximum_batch_message_count(4),
      mock);
  auto r0 =
      publisher
          ->Publish({pubsub::MessageBuilder{}.SetData("test-data-0").Build()})
          .then([](future<StatusOr<std::string>> f) {
            auto r = f.get();
            ASSERT_STATUS_OK(r);
            EXPECT_EQ("test-message-id-0", *r);
          });
  auto r1 =
      publisher
          ->Publish({pubsub::MessageBuilder{}.SetData("test-data-1").Build()})
          .then([](future<StatusOr<std::string>> f) {
            auto r = f.get();
            ASSERT_STATUS_OK(r);
            EXPECT_EQ("test-message-id-1", *r);
          });

  std::thread t([&cq] { cq.Run(); });

  r0.get();
  r1.get();

  cq.Shutdown();
  t.join();
}

TEST(BatchingPublisherConnectionTest, BatchByFlush) {
  auto mock = std::make_shared<pubsub_mocks::MockPublisherConnection>();
  pubsub::Topic const topic("test-project", "test-topic");

  google::cloud::internal::AutomaticallyCreatedBackgroundThreads background;
  EXPECT_CALL(*mock, cq).WillRepeatedly(Return(background.cq()));
  EXPECT_CALL(*mock, Publish)
      .WillOnce([&](pubsub::PublisherConnection::PublishParams const& p) {
        EXPECT_EQ(topic.FullName(), p.request.topic());
        EXPECT_THAT(DataElements(p), ElementsAre("test-data-0", "test-data-1"));
        google::pubsub::v1::PublishResponse response;
        response.add_message_ids("test-message-id-0");
        response.add_message_ids("test-message-id-1");
        return make_ready_future(make_status_or(response));
      })
      .WillRepeatedly([&](pubsub::PublisherConnection::PublishParams const& p) {
        EXPECT_EQ(topic.FullName(), p.request.topic());
        google::pubsub::v1::PublishResponse response;
        for (auto const& m : p.request.messages()) {
          response.add_message_ids("ack-for-" + std::string(m.data()));
        }
        return make_ready_future(make_status_or(response));
      });

  // Use our own completion queue, initially inactive, to avoid race conditions
  // due to the maximum-hold-time timer expiring.
  google::cloud::CompletionQueue cq;
  auto publisher = BatchingPublisherConnection::Create(
      topic,
      pubsub::PublisherOptions{}
          .set_maximum_hold_time(std::chrono::milliseconds(5))
          .set_maximum_batch_message_count(4),
      mock);

  std::vector<future<void>> results;
  for (auto i : {0, 1}) {
    results.push_back(
        publisher
            ->Publish({pubsub::MessageBuilder{}
                           .SetData("test-data-" + std::to_string(i))
                           .Build()})
            .then([i](future<StatusOr<std::string>> f) {
              auto r = f.get();
              ASSERT_STATUS_OK(r);
              EXPECT_EQ("test-message-id-" + std::to_string(i), *r);
            }));
  }

  // Trigger the first `.WillOnce()` expectation.  CQ is not running yet, so the
  // flush cannot be explained by a timer, and the message count is too low.
  publisher->Flush();

  for (auto i : {2, 3, 4}) {
    auto data = std::string{"test-data-"} + std::to_string(i);
    results.push_back(
        publisher->Publish({pubsub::MessageBuilder{}.SetData(data).Build()})
            .then([data](future<StatusOr<std::string>> f) {
              auto r = f.get();
              ASSERT_STATUS_OK(r);
              EXPECT_EQ("ack-for-" + data, *r);
            }));
  }

  std::thread t([&cq] { cq.Run(); });
  for (auto& r : results) r.get();
  cq.Shutdown();
  t.join();
}

TEST(BatchingPublisherConnectionTest, HandleError) {
  auto mock = std::make_shared<pubsub_mocks::MockPublisherConnection>();
  pubsub::Topic const topic("test-project", "test-topic");

  auto const error_status = Status(StatusCode::kPermissionDenied, "uh-oh");
  google::cloud::internal::AutomaticallyCreatedBackgroundThreads background;
  EXPECT_CALL(*mock, cq).WillRepeatedly(Return(background.cq()));
  EXPECT_CALL(*mock, Publish)
      .WillOnce([&](pubsub::PublisherConnection::PublishParams const&) {
        return make_ready_future(
            StatusOr<google::pubsub::v1::PublishResponse>(error_status));
      });

  auto publisher = BatchingPublisherConnection::Create(
      topic, pubsub::PublisherOptions{}.set_maximum_batch_message_count(2),
      mock);
  auto check_status = [&](future<StatusOr<std::string>> f) {
    auto r = f.get();
    EXPECT_THAT(r.status(),
                StatusIs(StatusCode::kPermissionDenied, HasSubstr("uh-oh")));
  };
  auto r0 =
      publisher
          ->Publish({pubsub::MessageBuilder{}.SetData("test-data-0").Build()})
          .then(check_status);
  auto r1 =
      publisher
          ->Publish({pubsub::MessageBuilder{}.SetData("test-data-1").Build()})
          .then(check_status);

  r0.get();
  r1.get();
}

TEST(BatchingPublisherConnectionTest, HandleInvalidResponse) {
  auto mock = std::make_shared<pubsub_mocks::MockPublisherConnection>();
  pubsub::Topic const topic("test-project", "test-topic");

  google::cloud::internal::AutomaticallyCreatedBackgroundThreads background;
  EXPECT_CALL(*mock, cq).WillRepeatedly(Return(background.cq()));
  EXPECT_CALL(*mock, Publish)
      .WillOnce([&](pubsub::PublisherConnection::PublishParams const&) {
        google::pubsub::v1::PublishResponse response;
        return make_ready_future(make_status_or(response));
      });

  auto publisher = BatchingPublisherConnection::Create(
      topic, pubsub::PublisherOptions{}.set_maximum_batch_message_count(2),
      mock);
  auto check_status = [&](future<StatusOr<std::string>> f) {
    auto r = f.get();
    EXPECT_THAT(r.status(), StatusIs(StatusCode::kUnknown,
                                     HasSubstr("mismatched message id count")));
  };
  auto r0 =
      publisher
          ->Publish({pubsub::MessageBuilder{}.SetData("test-data-0").Build()})
          .then(check_status);
  auto r1 =
      publisher
          ->Publish({pubsub::MessageBuilder{}.SetData("test-data-1").Build()})
          .then(check_status);

  r0.get();
  r1.get();
}

}  // namespace
}  // namespace GOOGLE_CLOUD_CPP_PUBSUB_NS
}  // namespace pubsub_internal
}  // namespace cloud
}  // namespace google
