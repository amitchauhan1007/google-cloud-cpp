// Copyright 2018 Google LLC
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

#include "google/cloud/storage/client.h"
#include "google/cloud/storage/internal/bucket_access_control_parser.h"
#include "google/cloud/storage/oauth2/google_credentials.h"
#include "google/cloud/storage/retry_policy.h"
#include "google/cloud/storage/testing/canonical_errors.h"
#include "google/cloud/storage/testing/mock_client.h"
#include "google/cloud/storage/testing/retry_tests.h"
#include "google/cloud/testing_util/assert_ok.h"
#include <gmock/gmock.h>

namespace google {
namespace cloud {
namespace storage {
inline namespace STORAGE_CLIENT_NS {
namespace {

using ::google::cloud::storage::testing::canonical_errors::TransientError;
using ::testing::_;
using ::testing::Return;
using ::testing::ReturnRef;
using ms = std::chrono::milliseconds;

/**
 * Test the BucketAccessControls-related functions in storage::Client.
 */
class BucketAccessControlsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    mock_ = std::make_shared<testing::MockClient>();
    EXPECT_CALL(*mock_, client_options())
        .WillRepeatedly(ReturnRef(client_options_));
    client_.reset(new Client{
        std::shared_ptr<internal::RawClient>(mock_),
        ExponentialBackoffPolicy(std::chrono::milliseconds(1),
                                 std::chrono::milliseconds(1), 2.0)});
  }
  void TearDown() override {
    client_.reset();
    mock_.reset();
  }

  std::shared_ptr<testing::MockClient> mock_;
  std::unique_ptr<Client> client_;
  ClientOptions client_options_ =
      ClientOptions(oauth2::CreateAnonymousCredentials());
};

/// @test Verify that we parse JSON objects into BucketAccessControl objects.
TEST_F(BucketAccessControlsTest, Parse) {
  std::string text = R"""({
      "bucket": "foo-bar",
      "domain": "example.com",
      "email": "foobar@example.com",
      "entity": "user-foobar",
      "entityId": "user-foobar-id-123",
      "etag": "XYZ=",
      "id": "bucket-foo-bar-acl-234",
      "kind": "storage#bucketAccessControl",
      "projectTeam": {
        "projectNumber": "3456789",
        "team": "a-team"
      },
      "role": "OWNER"
})""";
  auto actual = internal::BucketAccessControlParser::FromString(text).value();

  EXPECT_EQ("foo-bar", actual.bucket());
  EXPECT_EQ("example.com", actual.domain());
  EXPECT_EQ("foobar@example.com", actual.email());
  EXPECT_EQ("user-foobar", actual.entity());
  EXPECT_EQ("user-foobar-id-123", actual.entity_id());
  EXPECT_EQ("XYZ=", actual.etag());
  EXPECT_EQ("bucket-foo-bar-acl-234", actual.id());
  EXPECT_EQ("storage#bucketAccessControl", actual.kind());
  EXPECT_EQ("3456789", actual.project_team().project_number);
  EXPECT_EQ("a-team", actual.project_team().team);
  EXPECT_EQ("OWNER", actual.role());
}

/// @test Verify that we parse JSON objects into BucketAccessControl objects.
TEST_F(BucketAccessControlsTest, ParseFailure) {
  auto actual = internal::BucketAccessControlParser::FromString("{123");
  EXPECT_FALSE(actual.ok());
}

TEST_F(BucketAccessControlsTest, ListBucketAcl) {
  std::vector<BucketAccessControl> expected{
      internal::BucketAccessControlParser::FromString(R"""({
          "bucket": "test-bucket",
          "entity": "user-test-user-1",
          "role": "OWNER"
      })""")
          .value(),
      internal::BucketAccessControlParser::FromString(R"""({
          "bucket": "test-bucket",
          "entity": "user-test-user-2",
          "role": "READER"
      })""")
          .value(),
  };

  EXPECT_CALL(*mock_, ListBucketAcl(_))
      .WillOnce(
          Return(StatusOr<internal::ListBucketAclResponse>(TransientError())))
      .WillOnce([&expected](internal::ListBucketAclRequest const& r) {
        EXPECT_EQ("test-bucket", r.bucket_name());

        return make_status_or(internal::ListBucketAclResponse{expected});
      });

  StatusOr<std::vector<BucketAccessControl>> actual =
      client_->ListBucketAcl("test-bucket");
  ASSERT_STATUS_OK(actual);
  EXPECT_EQ(expected, *actual);
}

TEST_F(BucketAccessControlsTest, ListBucketAclTooManyFailures) {
  testing::TooManyFailuresStatusTest<internal::ListBucketAclResponse>(
      mock_, EXPECT_CALL(*mock_, ListBucketAcl(_)),
      [](Client& client) {
        return client.ListBucketAcl("test-bucket-name").status();
      },
      "ListBucketAcl");
}

TEST_F(BucketAccessControlsTest, ListBucketAclPermanentFailure) {
  testing::PermanentFailureStatusTest<internal::ListBucketAclResponse>(
      *client_, EXPECT_CALL(*mock_, ListBucketAcl(_)),
      [](Client& client) {
        return client.ListBucketAcl("test-bucket-name").status();
      },
      "ListBucketAcl");
}

TEST_F(BucketAccessControlsTest, CreateBucketAcl) {
  auto expected = internal::BucketAccessControlParser::FromString(R"""({
          "bucket": "test-bucket",
          "entity": "user-test-user-1",
          "role": "READER"
      })""")
                      .value();

  EXPECT_CALL(*mock_, CreateBucketAcl(_))
      .WillOnce(Return(StatusOr<BucketAccessControl>(TransientError())))
      .WillOnce([&expected](internal::CreateBucketAclRequest const& r) {
        EXPECT_EQ("test-bucket", r.bucket_name());
        EXPECT_EQ("user-test-user-1", r.entity());
        EXPECT_EQ("READER", r.role());

        return make_status_or(expected);
      });
  StatusOr<BucketAccessControl> actual = client_->CreateBucketAcl(
      "test-bucket", "user-test-user-1", BucketAccessControl::ROLE_READER());
  ASSERT_STATUS_OK(actual);

  // Compare just a few fields because the values for most of the fields are
  // hard to predict when testing against the production environment.
  EXPECT_EQ(expected.bucket(), actual->bucket());
  EXPECT_EQ(expected.entity(), actual->entity());
  EXPECT_EQ(expected.role(), actual->role());
}

TEST_F(BucketAccessControlsTest, CreateBucketAclTooManyFailures) {
  testing::TooManyFailuresStatusTest<BucketAccessControl>(
      mock_, EXPECT_CALL(*mock_, CreateBucketAcl(_)),
      [](Client& client) {
        return client
            .CreateBucketAcl("test-bucket-name", "user-test-user-1", "READER")
            .status();
      },
      [](Client& client) {
        return client
            .CreateBucketAcl("test-bucket-name", "user-test-user-1", "READER",
                             IfMatchEtag("ABC="))
            .status();
      },
      "CreateBucketAcl");
}

TEST_F(BucketAccessControlsTest, CreateBucketAclPermanentFailure) {
  testing::PermanentFailureStatusTest<BucketAccessControl>(
      *client_, EXPECT_CALL(*mock_, CreateBucketAcl(_)),
      [](Client& client) {
        return client
            .CreateBucketAcl("test-bucket-name", "user-test-user", "READER")
            .status();
      },
      "CreateBucketAcl");
}

TEST_F(BucketAccessControlsTest, DeleteBucketAcl) {
  EXPECT_CALL(*mock_, DeleteBucketAcl(_))
      .WillOnce(Return(StatusOr<internal::EmptyResponse>(TransientError())))
      .WillOnce([](internal::DeleteBucketAclRequest const& r) {
        EXPECT_EQ("test-bucket", r.bucket_name());
        EXPECT_EQ("user-test-user-1", r.entity());

        return make_status_or(internal::EmptyResponse{});
      });
  auto status = client_->DeleteBucketAcl("test-bucket", "user-test-user-1");
  ASSERT_STATUS_OK(status);
}

TEST_F(BucketAccessControlsTest, DeleteBucketAclTooManyFailures) {
  testing::TooManyFailuresStatusTest<internal::EmptyResponse>(
      mock_, EXPECT_CALL(*mock_, DeleteBucketAcl(_)),
      [](Client& client) {
        return client.DeleteBucketAcl("test-bucket-name", "user-test-user-1");
      },
      [](Client& client) {
        return client.DeleteBucketAcl("test-bucket-name", "user-test-user-1",
                                      IfMatchEtag("ABC="));
      },
      "DeleteBucketAcl");
}

TEST_F(BucketAccessControlsTest, DeleteBucketAclPermanentFailure) {
  testing::PermanentFailureStatusTest<internal::EmptyResponse>(
      *client_, EXPECT_CALL(*mock_, DeleteBucketAcl(_)),
      [](Client& client) {
        return client.DeleteBucketAcl("test-bucket-name", "user-test-user");
      },
      "DeleteBucketAcl");
}

TEST_F(BucketAccessControlsTest, GetBucketAcl) {
  BucketAccessControl expected =
      internal::BucketAccessControlParser::FromString(R"""({
          "bucket": "test-bucket",
          "entity": "user-test-user-1",
          "role": "OWNER"
      })""")
          .value();

  EXPECT_CALL(*mock_, GetBucketAcl(_))
      .WillOnce(Return(StatusOr<BucketAccessControl>(TransientError())))
      .WillOnce([&expected](internal::GetBucketAclRequest const& r) {
        EXPECT_EQ("test-bucket", r.bucket_name());
        EXPECT_EQ("user-test-user-1", r.entity());

        return make_status_or(expected);
      });
  StatusOr<BucketAccessControl> actual =
      client_->GetBucketAcl("test-bucket", "user-test-user-1");
  ASSERT_STATUS_OK(actual);

  EXPECT_EQ(expected, *actual);
}

TEST_F(BucketAccessControlsTest, GetBucketAclTooManyFailures) {
  testing::TooManyFailuresStatusTest<BucketAccessControl>(
      mock_, EXPECT_CALL(*mock_, GetBucketAcl(_)),
      [](Client& client) {
        return client.GetBucketAcl("test-bucket-name", "user-test-user-1")
            .status();
      },
      "GetBucketAcl");
}

TEST_F(BucketAccessControlsTest, GetBucketAclPermanentFailure) {
  testing::PermanentFailureStatusTest<BucketAccessControl>(
      *client_, EXPECT_CALL(*mock_, GetBucketAcl(_)),
      [](Client& client) {
        return client.GetBucketAcl("test-bucket-name", "user-test-user-1")
            .status();
      },
      "GetBucketAcl");
}

TEST_F(BucketAccessControlsTest, UpdateBucketAcl) {
  BucketAccessControl expected =
      internal::BucketAccessControlParser::FromString(R"""({
          "bucket": "test-bucket",
          "entity": "user-test-user-1",
          "role": "OWNER"
      })""")
          .value();

  EXPECT_CALL(*mock_, UpdateBucketAcl(_))
      .WillOnce(Return(StatusOr<BucketAccessControl>(TransientError())))
      .WillOnce([&expected](internal::UpdateBucketAclRequest const& r) {
        EXPECT_EQ("test-bucket", r.bucket_name());
        EXPECT_EQ("user-test-user-1", r.entity());
        EXPECT_EQ("OWNER", r.role());

        return make_status_or(expected);
      });
  StatusOr<BucketAccessControl> actual = client_->UpdateBucketAcl(
      "test-bucket",
      BucketAccessControl().set_entity("user-test-user-1").set_role("OWNER"));
  ASSERT_STATUS_OK(actual);

  EXPECT_EQ(expected, *actual);
}

TEST_F(BucketAccessControlsTest, UpdateBucketAclTooManyFailures) {
  testing::TooManyFailuresStatusTest<BucketAccessControl>(
      mock_, EXPECT_CALL(*mock_, UpdateBucketAcl(_)),
      [](Client& client) {
        return client
            .UpdateBucketAcl("test-bucket", BucketAccessControl()
                                                .set_entity("user-test-user-1")
                                                .set_role("OWNER"))
            .status();
      },
      [](Client& client) {
        return client
            .UpdateBucketAcl("test-bucket",
                             BucketAccessControl()
                                 .set_entity("user-test-user-1")
                                 .set_role("OWNER"),
                             IfMatchEtag("ABC="))
            .status();
      },
      "UpdateBucketAcl");
}

TEST_F(BucketAccessControlsTest, UpdateBucketAclPermanentFailure) {
  testing::PermanentFailureStatusTest<BucketAccessControl>(
      *client_, EXPECT_CALL(*mock_, UpdateBucketAcl(_)),
      [](Client& client) {
        return client
            .UpdateBucketAcl("test-bucket", BucketAccessControl()
                                                .set_entity("user-test-user-1")
                                                .set_role("OWNER"))
            .status();
      },
      "UpdateBucketAcl");
}

TEST_F(BucketAccessControlsTest, PatchBucketAcl) {
  BucketAccessControl result =
      internal::BucketAccessControlParser::FromString(R"""({
          "bucket": "test-bucket",
          "entity": "user-test-user-1",
          "role": "OWNER"
      })""")
          .value();

  EXPECT_CALL(*mock_, PatchBucketAcl(_))
      .WillOnce(Return(StatusOr<BucketAccessControl>(TransientError())))
      .WillOnce([&result](internal::PatchBucketAclRequest const& r) {
        EXPECT_EQ("test-bucket", r.bucket_name());
        EXPECT_EQ("user-test-user-1", r.entity());
        nlohmann::json expected{{"role", "OWNER"}};
        auto payload = nlohmann::json::parse(r.payload());
        EXPECT_EQ(expected, payload);

        return make_status_or(result);
      });
  StatusOr<BucketAccessControl> actual = client_->PatchBucketAcl(
      "test-bucket", "user-test-user-1",
      BucketAccessControlPatchBuilder().set_role("OWNER"));
  ASSERT_STATUS_OK(actual);

  EXPECT_EQ(result, *actual);
}

TEST_F(BucketAccessControlsTest, PatchBucketAclTooManyFailures) {
  testing::TooManyFailuresStatusTest<BucketAccessControl>(
      mock_, EXPECT_CALL(*mock_, PatchBucketAcl(_)),
      [](Client& client) {
        return client
            .PatchBucketAcl("test-bucket", "user-test-user-1",
                            BucketAccessControlPatchBuilder())
            .status();
      },
      [](Client& client) {
        return client
            .PatchBucketAcl("test-bucket", "user-test-user-1",
                            BucketAccessControlPatchBuilder(),
                            IfMatchEtag("ABC="))
            .status();
      },
      "PatchBucketAcl");
}

TEST_F(BucketAccessControlsTest, PatchBucketAclPermanentFailure) {
  testing::PermanentFailureStatusTest<BucketAccessControl>(
      *client_, EXPECT_CALL(*mock_, PatchBucketAcl(_)),
      [](Client& client) {
        return client
            .PatchBucketAcl("test-bucket", "user-test-user-1",
                            BucketAccessControlPatchBuilder())
            .status();
      },
      "PatchBucketAcl");
}

}  // namespace
}  // namespace STORAGE_CLIENT_NS
}  // namespace storage
}  // namespace cloud
}  // namespace google
