/*
 * Copyright (C) 2021 The Android Open Source Project
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
#include <vector>

#include <android-base/file.h>
#include <android-base/stringprintf.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "apex_preinstalled_data.h"
#include "apexd.h"
#include "apexd_test_utils.h"
#include "apexd_utils.h"

namespace android {
namespace apex {

namespace fs = std::filesystem;

using android::apex::testing::ApexFileEq;
using android::apex::testing::IsOk;
using android::base::GetExecutableDirectory;
using android::base::StringPrintf;
using ::testing::ByRef;
using ::testing::UnorderedElementsAre;

static std::string GetTestDataDir() { return GetExecutableDirectory(); }
static std::string GetTestFile(const std::string& name) {
  return GetTestDataDir() + "/" + name;
}

TEST(ApexdUnitTest, ScanAndGroupApexFiles) {
  TemporaryDir built_in_dir;
  fs::copy(GetTestFile("apex.apexd_test.apex"), built_in_dir.path);
  fs::copy(GetTestFile("com.android.apex.cts.shim.apex"), built_in_dir.path);
  fs::copy(GetTestFile("com.android.apex.compressed.v1.capex"),
           built_in_dir.path);

  TemporaryDir data_dir;
  fs::copy(GetTestFile("com.android.apex.cts.shim.v2.apex"), data_dir.path);

  std::vector<std::string> dirs_to_scan{built_in_dir.path, data_dir.path};
  auto result = ScanAndGroupApexFiles(dirs_to_scan);

  // Verify the contents of result
  auto apexd_test_file = ApexFile::Open(
      StringPrintf("%s/apex.apexd_test.apex", built_in_dir.path));
  auto shim_v1 = ApexFile::Open(
      StringPrintf("%s/com.android.apex.cts.shim.apex", built_in_dir.path));
  auto compressed_apex = ApexFile::Open(StringPrintf(
      "%s/com.android.apex.compressed.v1.capex", built_in_dir.path));
  auto shim_v2 = ApexFile::Open(
      StringPrintf("%s/com.android.apex.cts.shim.v2.apex", data_dir.path));

  ASSERT_EQ(result.size(), 3u);
  ASSERT_THAT(result[apexd_test_file->GetManifest().name()],
              UnorderedElementsAre(ApexFileEq(ByRef(*apexd_test_file))));
  ASSERT_THAT(result[shim_v1->GetManifest().name()],
              UnorderedElementsAre(ApexFileEq(ByRef(*shim_v1)),
                                   ApexFileEq(ByRef(*shim_v2))));
  ASSERT_THAT(result[compressed_apex->GetManifest().name()],
              UnorderedElementsAre(ApexFileEq(ByRef(*compressed_apex))));
}

// Apex that does not have pre-installed version, does not get selected
TEST(ApexdUnitTest, ApexMustHavePreInstalledVersionForSelection) {
  TemporaryDir built_in_dir;
  fs::copy(GetTestFile("apex.apexd_test.apex"), built_in_dir.path);
  fs::copy(GetTestFile("com.android.apex.cts.shim.apex"), built_in_dir.path);
  fs::copy(
      GetTestFile("com.android.apex.test.sharedlibs_generated.v1.libvX.apex"),
      built_in_dir.path);

  // Pre-installed information is not initialized
  ApexPreinstalledData instance;
  std::vector<std::string> dirs_to_scan{built_in_dir.path};
  auto all_apex = ScanAndGroupApexFiles(dirs_to_scan);
  auto result = SelectApexForActivation(std::move(all_apex), instance);
  ASSERT_EQ(result.size(), 0u);

  // Once initialized, pre-installed APEX should get selected
  ASSERT_TRUE(IsOk(instance.Initialize({built_in_dir.path})));
  all_apex = ScanAndGroupApexFiles(dirs_to_scan);
  result = SelectApexForActivation(std::move(all_apex), instance);
  ASSERT_EQ(result.size(), 3u);
  auto apexd_test_file = ApexFile::Open(
      StringPrintf("%s/apex.apexd_test.apex", built_in_dir.path));
  auto shim_v1 = ApexFile::Open(
      StringPrintf("%s/com.android.apex.cts.shim.apex", built_in_dir.path));
  auto shared_lib = ApexFile::Open(StringPrintf(
      "%s/com.android.apex.test.sharedlibs_generated.v1.libvX.apex",
      built_in_dir.path));
  ASSERT_THAT(result, UnorderedElementsAre(ApexFileEq(ByRef(*apexd_test_file)),
                                           ApexFileEq(ByRef(*shim_v1)),
                                           ApexFileEq(ByRef(*shared_lib))));
}

// Higher version gets priority when selecting for activation
TEST(ApexdUnitTest, HigherVersionOfApexIsSelected) {
  TemporaryDir built_in_dir;
  fs::copy(GetTestFile("apex.apexd_test_v2.apex"), built_in_dir.path);
  fs::copy(GetTestFile("com.android.apex.cts.shim.apex"), built_in_dir.path);

  // Initialize pre-installed APEX information
  ApexPreinstalledData instance;
  ASSERT_TRUE(IsOk(instance.Initialize({built_in_dir.path})));

  TemporaryDir data_dir;
  fs::copy(GetTestFile("apex.apexd_test.apex"), data_dir.path);
  fs::copy(GetTestFile("com.android.apex.cts.shim.v2.apex"), data_dir.path);

  std::vector<std::string> dirs_to_scan{built_in_dir.path, data_dir.path};
  auto all_apex = ScanAndGroupApexFiles(dirs_to_scan);
  auto result = SelectApexForActivation(std::move(all_apex), instance);
  ASSERT_EQ(result.size(), 2u);

  auto apexd_test_file_v2 = ApexFile::Open(
      StringPrintf("%s/apex.apexd_test_v2.apex", built_in_dir.path));
  auto shim_v2 = ApexFile::Open(
      StringPrintf("%s/com.android.apex.cts.shim.v2.apex", data_dir.path));
  ASSERT_THAT(result,
              UnorderedElementsAre(ApexFileEq(ByRef(*apexd_test_file_v2)),
                                   ApexFileEq(ByRef(*shim_v2))));
}

// When versions are equal, non-pre-installed version gets priority
TEST(ApexdUnitTest, DataApexGetsPriorityForSameVersions) {
  TemporaryDir built_in_dir;
  fs::copy(GetTestFile("apex.apexd_test.apex"), built_in_dir.path);
  fs::copy(GetTestFile("com.android.apex.cts.shim.apex"), built_in_dir.path);

  // Initialize pre-installed APEX information
  ApexPreinstalledData instance;
  ASSERT_TRUE(IsOk(instance.Initialize({built_in_dir.path})));

  TemporaryDir data_dir;
  fs::copy(GetTestFile("apex.apexd_test.apex"), data_dir.path);
  fs::copy(GetTestFile("com.android.apex.cts.shim.apex"), data_dir.path);

  std::vector<std::string> dirs_to_scan{built_in_dir.path, data_dir.path};
  auto all_apex = ScanAndGroupApexFiles(dirs_to_scan);
  auto result = SelectApexForActivation(std::move(all_apex), instance);
  ASSERT_EQ(result.size(), 2u);

  auto apexd_test_file =
      ApexFile::Open(StringPrintf("%s/apex.apexd_test.apex", data_dir.path));
  auto shim_v1 = ApexFile::Open(
      StringPrintf("%s/com.android.apex.cts.shim.apex", data_dir.path));
  ASSERT_THAT(result, UnorderedElementsAre(ApexFileEq(ByRef(*apexd_test_file)),
                                           ApexFileEq(ByRef(*shim_v1))));
}

// Both versions of shared libs can be selected
TEST(ApexdUnitTest, SharedLibsCanHaveBothVersionSelected) {
  TemporaryDir built_in_dir;
  fs::copy(
      GetTestFile("com.android.apex.test.sharedlibs_generated.v1.libvX.apex"),
      built_in_dir.path);

  // Initialize pre-installed APEX information
  ApexPreinstalledData instance;
  ASSERT_TRUE(IsOk(instance.Initialize({built_in_dir.path})));

  TemporaryDir data_dir;
  fs::copy(
      GetTestFile("com.android.apex.test.sharedlibs_generated.v2.libvY.apex"),
      data_dir.path);

  std::vector<std::string> dirs_to_scan{built_in_dir.path, data_dir.path};
  auto all_apex = ScanAndGroupApexFiles(dirs_to_scan);
  auto result = SelectApexForActivation(std::move(all_apex), instance);
  ASSERT_EQ(result.size(), 2u);

  auto shared_lib_v1 = ApexFile::Open(StringPrintf(
      "%s/com.android.apex.test.sharedlibs_generated.v1.libvX.apex",
      built_in_dir.path));
  auto shared_lib_v2 = ApexFile::Open(StringPrintf(
      "%s/com.android.apex.test.sharedlibs_generated.v2.libvY.apex",
      data_dir.path));
  ASSERT_THAT(result, UnorderedElementsAre(ApexFileEq(ByRef(*shared_lib_v1)),
                                           ApexFileEq(ByRef(*shared_lib_v2))));
}

TEST(ApexdUnitTest, ProcessCompressedApex) {
  TemporaryDir built_in_dir;
  fs::copy(GetTestFile("com.android.apex.compressed.v1.capex"),
           built_in_dir.path);
  auto compressed_apex = ApexFile::Open(StringPrintf(
      "%s/com.android.apex.compressed.v1.capex", built_in_dir.path));

  TemporaryDir decompression_dir, active_apex_dir;
  std::vector<ApexFile> compressed_apex_list;
  compressed_apex_list.emplace_back(std::move(*compressed_apex));
  auto return_value =
      ProcessCompressedApex(std::move(compressed_apex_list),
                            decompression_dir.path, active_apex_dir.path);

  std::string decompressed_file_path = StringPrintf(
      "%s/com.android.apex.compressed@1.apex", decompression_dir.path);
  // Assert output path is not empty
  auto exists = PathExists(decompressed_file_path);
  ASSERT_TRUE(IsOk(exists));
  ASSERT_TRUE(*exists) << decompressed_file_path << " does not exist";

  // Assert that decompressed apex is same as original apex
  const std::string original_apex_file_path =
      GetTestFile("com.android.apex.compressed.v1_original.apex");
  auto comparison_result =
      CompareFiles(original_apex_file_path, decompressed_file_path);
  ASSERT_TRUE(IsOk(comparison_result));
  ASSERT_TRUE(*comparison_result);

  // Assert that the file is hard linked to active_apex_dir
  std::string hardlink_file_path = StringPrintf(
      "%s/com.android.apex.compressed@1.apex", active_apex_dir.path);
  std::error_code ec;
  bool is_hardlink =
      fs::equivalent(decompressed_file_path, hardlink_file_path, ec);
  ASSERT_FALSE(ec) << "Some error occurred while checking for hardlink";
  ASSERT_TRUE(is_hardlink);

  // Assert that return value contains active APEX, not decompressed APEX
  auto active_apex = ApexFile::Open(hardlink_file_path);
  ASSERT_THAT(return_value,
              UnorderedElementsAre(ApexFileEq(ByRef(*active_apex))));
}

TEST(ApexdUnitTest, ProcessCompressedApexRunsVerification) {
  TemporaryDir built_in_dir;
  fs::copy(GetTestFile(
               "com.android.apex.compressed_key_mismatch_with_original.capex"),
           built_in_dir.path);

  auto compressed_apex_mismatch_key = ApexFile::Open(StringPrintf(
      "%s/com.android.apex.compressed_key_mismatch_with_original.capex",
      built_in_dir.path));

  TemporaryDir decompression_dir, active_apex_dir;
  std::vector<ApexFile> compressed_apex_list;
  compressed_apex_list.emplace_back(std::move(*compressed_apex_mismatch_key));
  auto return_value =
      ProcessCompressedApex(std::move(compressed_apex_list),
                            decompression_dir.path, active_apex_dir.path);
  ASSERT_EQ(return_value.size(), 0u);
}

TEST(ApexdUnitTest, DecompressedApexCleanupDeleteIfActiveFileMissing) {
  // Create decompressed apex in decompression_dir
  TemporaryDir decompression_dir;
  fs::copy(GetTestFile("com.android.apex.compressed.v1_original.apex"),
           decompression_dir.path);

  TemporaryDir active_apex_dir;
  RemoveUnlinkedDecompressedApex(decompression_dir.path, active_apex_dir.path);

  // Assert that decompressed apex was deleted
  auto decompressed_file_path =
      StringPrintf("%s/com.android.apex.compressed.v1_original.apex",
                   decompression_dir.path);
  auto file_exists = PathExists(decompressed_file_path);
  ASSERT_TRUE(IsOk(file_exists));
  ASSERT_FALSE(*file_exists)
      << "Unlinked decompressed file did not get deleted";
}

TEST(ApexdUnitTest, DecompressedApexCleanupSameFilenameButNotLinked) {
  // Create decompressed apex in decompression_dir
  TemporaryDir decompression_dir;
  const std::string filename = "com.android.apex.compressed.v1_original.apex";
  fs::copy(GetTestFile(filename), decompression_dir.path);
  auto decompressed_file_path =
      StringPrintf("%s/%s", decompression_dir.path, filename.c_str());

  // Copy the same file to active_apex_dir, instead of hard-linking
  TemporaryDir active_apex_dir;
  fs::copy(GetTestFile(filename), active_apex_dir.path);

  RemoveUnlinkedDecompressedApex(decompression_dir.path, active_apex_dir.path);

  // Assert that decompressed apex was deleted
  auto file_exists = PathExists(decompressed_file_path);
  ASSERT_TRUE(IsOk(file_exists));
  ASSERT_FALSE(*file_exists)
      << "Unlinked decompressed file did not get deleted";
}

TEST(ApexdUnitTest, DecompressedApexCleanupLinkedSurvives) {
  // Create decompressed apex in decompression_dir
  TemporaryDir decompression_dir;
  const std::string filename = "com.android.apex.compressed.v1_original.apex";
  fs::copy(GetTestFile(filename), decompression_dir.path);
  auto decompressed_file_path =
      StringPrintf("%s/%s", decompression_dir.path, filename.c_str());

  // Now hardlink it to active_apex_dir
  TemporaryDir active_apex_dir;
  auto active_file_path =
      StringPrintf("%s/%s", active_apex_dir.path, filename.c_str());
  std::error_code ec;
  fs::create_hard_link(decompressed_file_path, active_file_path, ec);
  ASSERT_FALSE(ec) << "Failed to create hardlink";

  RemoveUnlinkedDecompressedApex(decompression_dir.path, active_apex_dir.path);

  // Assert that decompressed apex was not deleted
  auto file_exists = PathExists(decompressed_file_path);
  ASSERT_TRUE(IsOk(file_exists));
  ASSERT_TRUE(*file_exists) << "Linked decompressed file got deleted";
}

TEST(ApexdUnitTest, DecompressedApexCleanupDeleteIfLinkedToDifferentFilename) {
  // Create decompressed apex in decompression_dir
  TemporaryDir decompression_dir;
  const std::string filename = "com.android.apex.compressed.v1_original.apex";
  fs::copy(GetTestFile(filename), decompression_dir.path);
  auto decompressed_file_path =
      StringPrintf("%s/%s", decompression_dir.path, filename.c_str());

  // Now hardlink it to active_apex_dir but with different filename
  TemporaryDir active_apex_dir;
  auto active_file_path =
      StringPrintf("%s/different.name.apex", active_apex_dir.path);
  std::error_code ec;
  fs::create_hard_link(decompressed_file_path, active_file_path, ec);
  ASSERT_FALSE(ec) << "Failed to create hardlink";

  RemoveUnlinkedDecompressedApex(decompression_dir.path, active_apex_dir.path);

  // Assert that decompressed apex was deleted
  auto file_exists = PathExists(decompressed_file_path);
  ASSERT_TRUE(IsOk(file_exists));
  ASSERT_FALSE(*file_exists)
      << "Unlinked decompressed file did not get deleted";
}

}  // namespace apex
}  // namespace android
