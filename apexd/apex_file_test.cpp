/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/scopeguard.h>
#include <android-base/strings.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <libavb/libavb.h>
#include <ziparchive/zip_archive.h>

#include "apex_file.h"

using android::base::GetExecutableDirectory;
using android::base::Result;

static const std::string kTestDataDir = GetExecutableDirectory() + "/";

namespace android {
namespace apex {
namespace {

struct ApexFileTestParam {
  const char* type;
  const char* prefix;
};

constexpr const ApexFileTestParam kParameters[] = {
    {"ext4", "apex.apexd_test"}, {"f2fs", "apex.apexd_test_f2fs"}};

class ApexFileTest : public ::testing::TestWithParam<ApexFileTestParam> {};

INSTANTIATE_TEST_SUITE_P(Apex, ApexFileTest, testing::ValuesIn(kParameters));

TEST_P(ApexFileTest, GetOffsetOfSimplePackage) {
  const std::string file_path = kTestDataDir + GetParam().prefix + ".apex";
  Result<ApexFile> apex_file = ApexFile::Open(file_path);
  ASSERT_TRUE(apex_file.ok());

  int32_t zip_image_offset;
  size_t zip_image_size;
  {
    ZipArchiveHandle handle;
    int32_t rc = OpenArchive(file_path.c_str(), &handle);
    ASSERT_EQ(0, rc);
    auto close_guard =
        android::base::make_scope_guard([&handle]() { CloseArchive(handle); });

    ZipEntry entry;
    rc = FindEntry(handle, "apex_payload.img", &entry);
    ASSERT_EQ(0, rc);

    zip_image_offset = entry.offset;
    EXPECT_EQ(zip_image_offset % 4096, 0);
    zip_image_size = entry.uncompressed_length;
    EXPECT_EQ(zip_image_size, entry.compressed_length);
  }

  EXPECT_EQ(zip_image_offset, apex_file->GetImageOffset());
  EXPECT_EQ(zip_image_size, apex_file->GetImageSize());
}

TEST(ApexFileTest, GetOffsetMissingFile) {
  const std::string file_path = kTestDataDir + "missing.apex";
  Result<ApexFile> apex_file = ApexFile::Open(file_path);
  ASSERT_FALSE(apex_file.ok());
  ASSERT_THAT(apex_file.error().message(),
              testing::HasSubstr("Failed to open package"));
}

TEST_P(ApexFileTest, GetApexManifest) {
  const std::string file_path = kTestDataDir + GetParam().prefix + ".apex";
  Result<ApexFile> apex_file = ApexFile::Open(file_path);
  ASSERT_RESULT_OK(apex_file);
  EXPECT_EQ("com.android.apex.test_package", apex_file->GetManifest().name());
  EXPECT_EQ(1u, apex_file->GetManifest().version());
}

TEST_P(ApexFileTest, VerifyApexVerity) {
  const std::string file_path = kTestDataDir + GetParam().prefix + ".apex";
  Result<ApexFile> apex_file = ApexFile::Open(file_path);
  ASSERT_RESULT_OK(apex_file);

  auto verity_or =
      apex_file->VerifyApexVerity(apex_file->GetBundledPublicKey());
  ASSERT_RESULT_OK(verity_or);

  const ApexVerityData& data = *verity_or;
  EXPECT_NE(nullptr, data.desc.get());
  EXPECT_EQ(std::string("368a22e64858647bc45498e92f749f85482ac468"
                        "50ca7ec8071f49dfa47a243c"),
            data.salt);

  const std::string digest_path =
      kTestDataDir + GetParam().prefix + "_digest.txt";
  std::string root_digest;
  ASSERT_TRUE(android::base::ReadFileToString(digest_path, &root_digest))
      << "Failed to read " << digest_path;
  root_digest = android::base::Trim(root_digest);

  EXPECT_EQ(std::string(root_digest), data.root_digest);
}

TEST_P(ApexFileTest, VerifyApexVerityWrongKey) {
  const std::string file_path = kTestDataDir + GetParam().prefix + ".apex";
  Result<ApexFile> apex_file = ApexFile::Open(file_path);
  ASSERT_RESULT_OK(apex_file);

  auto verity_or = apex_file->VerifyApexVerity("wrong-key");
  ASSERT_FALSE(verity_or.ok());
}

TEST_P(ApexFileTest, GetBundledPublicKey) {
  const std::string file_path = kTestDataDir + GetParam().prefix + ".apex";
  Result<ApexFile> apex_file = ApexFile::Open(file_path);
  ASSERT_RESULT_OK(apex_file);

  const std::string key_path =
      kTestDataDir + "apexd_testdata/com.android.apex.test_package.avbpubkey";
  std::string key_content;
  ASSERT_TRUE(android::base::ReadFileToString(key_path, &key_content))
      << "Failed to read " << key_path;

  EXPECT_EQ(key_content, apex_file->GetBundledPublicKey());
}

TEST(ApexFileTest, CorrutedApexB146895998) {
  const std::string apex_path = kTestDataDir + "corrupted_b146895998.apex";
  Result<ApexFile> apex = ApexFile::Open(apex_path);
  ASSERT_RESULT_OK(apex);
  ASSERT_FALSE(apex->VerifyApexVerity("ignored").ok());
}

TEST_P(ApexFileTest, RetrieveFsType) {
  const std::string file_path = kTestDataDir + GetParam().prefix + ".apex";
  Result<ApexFile> apex_file = ApexFile::Open(file_path);
  ASSERT_TRUE(apex_file.ok());

  EXPECT_EQ(std::string(GetParam().type), apex_file->GetFsType());
}

TEST(ApexFileTest, OpenInvalidFilesystem) {
  const std::string file_path =
      kTestDataDir + "apex.apexd_test_corrupt_superblock_apex.apex";
  Result<ApexFile> apex_file = ApexFile::Open(file_path);
  ASSERT_FALSE(apex_file.ok());
  ASSERT_THAT(apex_file.error().message(),
              testing::HasSubstr("Failed to retrieve filesystem type"));
}

}  // namespace
}  // namespace apex
}  // namespace android
