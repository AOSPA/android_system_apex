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
#include <android-base/properties.h>
#include <android-base/scopeguard.h>
#include <android-base/stringprintf.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "apex_database.h"
#include "apex_file_repository.h"
#include "apexd.h"
#include "apexd_test_utils.h"
#include "apexd_utils.h"

#include "com_android_apex.h"

namespace android {
namespace apex {

namespace fs = std::filesystem;

using MountedApexData = MountedApexDatabase::MountedApexData;
using android::apex::testing::ApexFileEq;
using android::apex::testing::IsOk;
using android::base::GetExecutableDirectory;
using android::base::GetProperty;
using android::base::make_scope_guard;
using android::base::StringPrintf;
using com::android::apex::testing::ApexInfoXmlEq;
using ::testing::ByRef;
using ::testing::IsEmpty;
using ::testing::StartsWith;
using ::testing::UnorderedElementsAre;
using ::testing::UnorderedElementsAreArray;

static std::string GetTestDataDir() { return GetExecutableDirectory(); }
static std::string GetTestFile(const std::string& name) {
  return GetTestDataDir() + "/" + name;
}

// Apex that does not have pre-installed version, does not get selected
TEST(ApexdUnitTest, ApexMustHavePreInstalledVersionForSelection) {
  TemporaryDir built_in_dir;
  fs::copy(GetTestFile("apex.apexd_test.apex"), built_in_dir.path);
  fs::copy(GetTestFile("com.android.apex.cts.shim.apex"), built_in_dir.path);
  fs::copy(
      GetTestFile("com.android.apex.test.sharedlibs_generated.v1.libvX.apex"),
      built_in_dir.path);
  ApexFileRepository instance;
  // Pre-installed data needs to be present so that we can add data apex
  ASSERT_TRUE(IsOk(instance.AddPreInstalledApex({built_in_dir.path})));

  TemporaryDir data_dir;
  fs::copy(GetTestFile("apex.apexd_test.apex"), data_dir.path);
  fs::copy(GetTestFile("com.android.apex.cts.shim.apex"), data_dir.path);
  fs::copy(
      GetTestFile("com.android.apex.test.sharedlibs_generated.v1.libvX.apex"),
      data_dir.path);
  ASSERT_TRUE(IsOk(instance.AddDataApex(data_dir.path)));

  const auto all_apex = instance.AllApexFilesByName();
  // Pass a blank instance so that the data apex files are not considered
  // pre-installed
  const ApexFileRepository instance_blank;
  auto result = SelectApexForActivation(all_apex, instance_blank);
  ASSERT_EQ(result.size(), 0u);
  // When passed proper instance they should get selected
  result = SelectApexForActivation(all_apex, instance);
  ASSERT_EQ(result.size(), 4u);
  auto apexd_test_file =
      ApexFile::Open(StringPrintf("%s/apex.apexd_test.apex", data_dir.path));
  auto shim_v1 = ApexFile::Open(
      StringPrintf("%s/com.android.apex.cts.shim.apex", data_dir.path));
  auto shared_lib_1 = ApexFile::Open(StringPrintf(
      "%s/com.android.apex.test.sharedlibs_generated.v1.libvX.apex",
      built_in_dir.path));
  auto shared_lib_2 = ApexFile::Open(StringPrintf(
      "%s/com.android.apex.test.sharedlibs_generated.v1.libvX.apex",
      data_dir.path));
  ASSERT_THAT(result, UnorderedElementsAre(ApexFileEq(ByRef(*apexd_test_file)),
                                           ApexFileEq(ByRef(*shim_v1)),
                                           ApexFileEq(ByRef(*shared_lib_1)),
                                           ApexFileEq(ByRef(*shared_lib_2))));
}

// Higher version gets priority when selecting for activation
TEST(ApexdUnitTest, HigherVersionOfApexIsSelected) {
  TemporaryDir built_in_dir;
  fs::copy(GetTestFile("apex.apexd_test_v2.apex"), built_in_dir.path);
  fs::copy(GetTestFile("com.android.apex.cts.shim.apex"), built_in_dir.path);
  ApexFileRepository instance;
  ASSERT_TRUE(IsOk(instance.AddPreInstalledApex({built_in_dir.path})));

  TemporaryDir data_dir;
  fs::copy(GetTestFile("apex.apexd_test.apex"), data_dir.path);
  fs::copy(GetTestFile("com.android.apex.cts.shim.v2.apex"), data_dir.path);
  ASSERT_TRUE(IsOk(instance.AddDataApex(data_dir.path)));

  auto all_apex = instance.AllApexFilesByName();
  auto result = SelectApexForActivation(all_apex, instance);
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
  ApexFileRepository instance;
  ASSERT_TRUE(IsOk(instance.AddPreInstalledApex({built_in_dir.path})));

  TemporaryDir data_dir;
  fs::copy(GetTestFile("apex.apexd_test.apex"), data_dir.path);
  fs::copy(GetTestFile("com.android.apex.cts.shim.apex"), data_dir.path);
  // Initialize ApexFile repo
  ASSERT_TRUE(IsOk(instance.AddDataApex(data_dir.path)));

  auto all_apex = instance.AllApexFilesByName();
  auto result = SelectApexForActivation(all_apex, instance);
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
  ApexFileRepository instance;
  ASSERT_TRUE(IsOk(instance.AddPreInstalledApex({built_in_dir.path})));

  TemporaryDir data_dir;
  fs::copy(
      GetTestFile("com.android.apex.test.sharedlibs_generated.v2.libvY.apex"),
      data_dir.path);
  // Initialize data APEX information
  ASSERT_TRUE(IsOk(instance.AddDataApex(data_dir.path)));

  auto all_apex = instance.AllApexFilesByName();
  auto result = SelectApexForActivation(all_apex, instance);
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
  std::vector<std::reference_wrapper<const ApexFile>> compressed_apex_list;
  compressed_apex_list.emplace_back(std::cref(*compressed_apex));
  auto return_value = ProcessCompressedApex(
      compressed_apex_list, decompression_dir.path, active_apex_dir.path);

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
  std::vector<std::reference_wrapper<const ApexFile>> compressed_apex_list;
  compressed_apex_list.emplace_back(std::cref(*compressed_apex_mismatch_key));
  auto return_value = ProcessCompressedApex(
      compressed_apex_list, decompression_dir.path, active_apex_dir.path);
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

namespace {
// Copies the compressed apex to |built_in_dir| and decompresses it to
// |decompressed_dir| and then hard links to |data_dir|
void PrepareCompressedApex(const std::string& name,
                           const std::string& built_in_dir,
                           const std::string& data_dir,
                           const std::string& decompressed_dir) {
  fs::copy(GetTestFile(name), built_in_dir);
  auto compressed_apex =
      ApexFile::Open(StringPrintf("%s/%s", built_in_dir.c_str(), name.c_str()));
  std::vector<std::reference_wrapper<const ApexFile>> compressed_apex_list;
  compressed_apex_list.emplace_back(std::cref(*compressed_apex));
  auto return_value =
      ProcessCompressedApex(compressed_apex_list, decompressed_dir, data_dir);
}
}  // namespace

TEST(ApexdUnitTest, ShouldAllocateSpaceForDecompressionNewApex) {
  TemporaryDir built_in_dir;
  ApexFileRepository instance;
  ASSERT_TRUE(IsOk(instance.AddPreInstalledApex({built_in_dir.path})));

  // A brand new compressed APEX is being introduced: selected
  auto result =
      ShouldAllocateSpaceForDecompression("com.android.brand.new", 1, instance);
  ASSERT_TRUE(IsOk(result));
  ASSERT_TRUE(*result);
}

TEST(ApexdUnitTest, ShouldAllocateSpaceForDecompressionWasNotCompressedBefore) {
  // Prepare fake pre-installed apex
  TemporaryDir built_in_dir;
  fs::copy(GetTestFile("apex.apexd_test.apex"), built_in_dir.path);
  ApexFileRepository instance;
  ASSERT_TRUE(IsOk(instance.AddPreInstalledApex({built_in_dir.path})));

  // An existing pre-installed APEX is now compressed in the OTA: selected
  {
    auto result = ShouldAllocateSpaceForDecompression(
        "com.android.apex.test_package", 1, instance);
    ASSERT_TRUE(IsOk(result));
    ASSERT_TRUE(*result);
  }

  // Even if there is a data apex (lower version)
  // Include data apex within calculation now
  TemporaryDir data_dir;
  fs::copy(GetTestFile("apex.apexd_test_v2.apex"), data_dir.path);
  ASSERT_TRUE(IsOk(instance.AddDataApex(data_dir.path)));
  {
    auto result = ShouldAllocateSpaceForDecompression(
        "com.android.apex.test_package", 3, instance);
    ASSERT_TRUE(IsOk(result));
    ASSERT_TRUE(*result);
  }

  // But not if data apex has equal or higher version
  {
    auto result = ShouldAllocateSpaceForDecompression(
        "com.android.apex.test_package", 2, instance);
    ASSERT_TRUE(IsOk(result));
    ASSERT_FALSE(*result);
  }
}

TEST(ApexdUnitTest, ShouldAllocateSpaceForDecompressionVersionCompare) {
  // Prepare fake pre-installed apex
  TemporaryDir built_in_dir, data_dir, decompression_dir;
  PrepareCompressedApex("com.android.apex.compressed.v1.capex",
                        built_in_dir.path, data_dir.path,
                        decompression_dir.path);
  ApexFileRepository instance(decompression_dir.path);
  ASSERT_TRUE(IsOk(instance.AddPreInstalledApex({built_in_dir.path})));
  ASSERT_TRUE(IsOk(instance.AddDataApex(data_dir.path)));

  {
    // New Compressed apex has higher version than decompressed data apex:
    // selected
    auto result = ShouldAllocateSpaceForDecompression(
        "com.android.apex.compressed", 2, instance);
    ASSERT_TRUE(IsOk(result));
    ASSERT_TRUE(*result)
        << "Higher version test with decompressed data returned false";
  }

  // Compare against decompressed data apex
  {
    // New Compressed apex has same version as decompressed data apex: not
    // selected
    auto result = ShouldAllocateSpaceForDecompression(
        "com.android.apex.compressed", 1, instance);
    ASSERT_TRUE(IsOk(result));
    ASSERT_FALSE(*result)
        << "Same version test with decompressed data returned true";
  }

  {
    // New Compressed apex has lower version than decompressed data apex:
    // selected
    auto result = ShouldAllocateSpaceForDecompression(
        "com.android.apex.compressed", 0, instance);
    ASSERT_TRUE(IsOk(result));
    ASSERT_TRUE(*result)
        << "lower version test with decompressed data returned false";
  }

  // Replace decompressed data apex with a higher version
  ApexFileRepository instance_new(decompression_dir.path);
  ASSERT_TRUE(IsOk(instance_new.AddPreInstalledApex({built_in_dir.path})));
  TemporaryDir data_dir_new;
  fs::copy(GetTestFile("com.android.apex.compressed.v2_original.apex"),
           data_dir_new.path);
  ASSERT_TRUE(IsOk(instance_new.AddDataApex(data_dir_new.path)));

  {
    // New Compressed apex has higher version as data apex: selected
    auto result = ShouldAllocateSpaceForDecompression(
        "com.android.apex.compressed", 3, instance_new);
    ASSERT_TRUE(IsOk(result));
    ASSERT_TRUE(*result) << "Higher version test with new data returned false";
  }

  {
    // New Compressed apex has same version as data apex: not selected
    auto result = ShouldAllocateSpaceForDecompression(
        "com.android.apex.compressed", 2, instance_new);
    ASSERT_TRUE(IsOk(result));
    ASSERT_FALSE(*result) << "Same version test with new data returned true";
  }

  {
    // New Compressed apex has lower version than data apex: not selected
    auto result = ShouldAllocateSpaceForDecompression(
        "com.android.apex.compressed", 1, instance_new);
    ASSERT_TRUE(IsOk(result));
    ASSERT_FALSE(*result) << "lower version test with new data returned true";
  }
}

TEST(ApexdUnitTest, ReserveSpaceForCompressedApexCreatesSingleFile) {
  TemporaryDir dest_dir;
  // Reserving space should create a single file in dest_dir with exact size

  ASSERT_TRUE(IsOk(ReserveSpaceForCompressedApex(100, dest_dir.path)));
  auto files = ReadDir(dest_dir.path, [](auto _) { return true; });
  ASSERT_TRUE(IsOk(files));
  ASSERT_EQ(files->size(), 1u);
  EXPECT_EQ(fs::file_size((*files)[0]), 100u);
}

TEST(ApexdUnitTest, ReserveSpaceForCompressedApexSafeToCallMultipleTimes) {
  TemporaryDir dest_dir;
  // Calling ReserveSpaceForCompressedApex multiple times should still create
  // a single file
  ASSERT_TRUE(IsOk(ReserveSpaceForCompressedApex(100, dest_dir.path)));
  ASSERT_TRUE(IsOk(ReserveSpaceForCompressedApex(100, dest_dir.path)));
  auto files = ReadDir(dest_dir.path, [](auto _) { return true; });
  ASSERT_TRUE(IsOk(files));
  ASSERT_EQ(files->size(), 1u);
  EXPECT_EQ(fs::file_size((*files)[0]), 100u);
}

TEST(ApexdUnitTest, ReserveSpaceForCompressedApexShrinkAndGrow) {
  TemporaryDir dest_dir;

  // Create a 100 byte file
  ASSERT_TRUE(IsOk(ReserveSpaceForCompressedApex(100, dest_dir.path)));

  // Should be able to shrink and grow the reserved space
  ASSERT_TRUE(IsOk(ReserveSpaceForCompressedApex(1000, dest_dir.path)));
  auto files = ReadDir(dest_dir.path, [](auto _) { return true; });
  ASSERT_TRUE(IsOk(files));
  ASSERT_EQ(files->size(), 1u);
  EXPECT_EQ(fs::file_size((*files)[0]), 1000u);

  ASSERT_TRUE(IsOk(ReserveSpaceForCompressedApex(10, dest_dir.path)));
  files = ReadDir(dest_dir.path, [](auto _) { return true; });
  ASSERT_TRUE(IsOk(files));
  ASSERT_EQ(files->size(), 1u);
  EXPECT_EQ(fs::file_size((*files)[0]), 10u);
}

TEST(ApexdUnitTest, ReserveSpaceForCompressedApexDeallocateIfPassedZero) {
  TemporaryDir dest_dir;

  // Create a file first
  ASSERT_TRUE(IsOk(ReserveSpaceForCompressedApex(100, dest_dir.path)));
  auto files = ReadDir(dest_dir.path, [](auto _) { return true; });
  ASSERT_TRUE(IsOk(files));
  ASSERT_EQ(files->size(), 1u);

  // Should delete the reserved file if size passed is 0
  ASSERT_TRUE(IsOk(ReserveSpaceForCompressedApex(0, dest_dir.path)));
  files = ReadDir(dest_dir.path, [](auto _) { return true; });
  ASSERT_TRUE(IsOk(files));
  ASSERT_EQ(files->size(), 0u);
}

TEST(ApexdUnitTest, ReserveSpaceForCompressedApexErrorForNegativeValue) {
  TemporaryDir dest_dir;
  // Should return error if negative value is passed
  ASSERT_FALSE(IsOk(ReserveSpaceForCompressedApex(-1, dest_dir.path)));
}

// A test fixture to use for tests that mount/unmount apexes.
class ApexdMountTest : public ::testing::Test {
 public:
  ApexdMountTest() {
    built_in_dir_ = StringPrintf("%s/pre-installed-apex", td_.path);
    data_dir_ = StringPrintf("%s/data-apex", td_.path);
  }

  const std::string& GetBuiltInDir() { return built_in_dir_; }
  const std::string& GetDataDir() { return data_dir_; }

  std::string AddPreInstalledApex(const std::string& apex_name) {
    fs::copy(GetTestFile(apex_name), built_in_dir_);
    return StringPrintf("%s/%s", built_in_dir_.c_str(), apex_name.c_str());
  }

  std::string AddDataApex(const std::string& apex_name) {
    fs::copy(GetTestFile(apex_name), data_dir_);
    return StringPrintf("%s/%s", data_dir_.c_str(), apex_name.c_str());
  }

  void UnmountOnTearDown(const std::string& apex_file) {
    to_unmount_.push_back(apex_file);
  }

 protected:
  void SetUp() final {
    ApexFileRepository::GetInstance().Reset();
    GetApexDatabaseForTesting().Reset();
    ASSERT_TRUE(IsOk(SetUpApexTestEnvironment()));
    ASSERT_EQ(mkdir(built_in_dir_.c_str(), 0755), 0);
    ASSERT_EQ(mkdir(data_dir_.c_str(), 0755), 0);
  }

  void TearDown() final {
    for (const auto& apex : to_unmount_) {
      if (auto status = DeactivatePackage(apex); !status.ok()) {
        LOG(ERROR) << "Failed to unmount " << apex << " : " << status.error();
      }
    }
  }

 private:
  MountNamespaceRestorer restorer_;
  TemporaryDir td_;
  std::string built_in_dir_;
  std::string data_dir_;
  std::vector<std::string> to_unmount_;
};

TEST_F(ApexdMountTest, ActivatePackage) {
  std::string file_path = AddPreInstalledApex("apex.apexd_test.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex({GetBuiltInDir()});

  ASSERT_TRUE(IsOk(ActivatePackage(file_path)));
  UnmountOnTearDown(file_path);

  auto active_apex = GetActivePackage("com.android.apex.test_package");
  ASSERT_TRUE(IsOk(active_apex));
  ASSERT_EQ(active_apex->GetPath(), file_path);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1"));

  ASSERT_TRUE(IsOk(DeactivatePackage(file_path)));
  ASSERT_FALSE(IsOk(GetActivePackage("com.android.apex.test_package")));

  auto new_apex_mounts = GetApexMounts();
  ASSERT_EQ(new_apex_mounts.size(), 0u);
}

TEST_F(ApexdMountTest, ActivateDeactivateSharedLibsApex) {
  ASSERT_EQ(mkdir("/apex/sharedlibs", 0755), 0);
  ASSERT_EQ(mkdir("/apex/sharedlibs/lib", 0755), 0);
  ASSERT_EQ(mkdir("/apex/sharedlibs/lib64", 0755), 0);
  auto deleter = make_scope_guard([]() {
    std::error_code ec;
    fs::remove_all("/apex/sharedlibs", ec);
    if (ec) {
      LOG(ERROR) << "Failed to delete /apex/sharedlibs : " << ec;
    }
  });

  std::string file_path = AddPreInstalledApex(
      "com.android.apex.test.sharedlibs_generated.v1.libvX.apex");
  ApexFileRepository::GetInstance().AddPreInstalledApex({GetBuiltInDir()});

  ASSERT_TRUE(IsOk(ActivatePackage(file_path)));

  auto active_apex = GetActivePackage("com.android.apex.test.sharedlibs");
  ASSERT_TRUE(IsOk(active_apex));
  ASSERT_EQ(active_apex->GetPath(), file_path);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test.sharedlibs@1"));

  ASSERT_TRUE(IsOk(DeactivatePackage(file_path)));
  ASSERT_FALSE(IsOk(GetActivePackage("com.android.apex.test.sharedlibs")));

  auto new_apex_mounts = GetApexMounts();
  ASSERT_EQ(new_apex_mounts.size(), 0u);
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapOnlyPreInstalledApexes) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");

  ASSERT_EQ(
      OnOtaChrootBootstrap({GetBuiltInDir()}, "/data/local/tmp/does-not-exist"),
      0);
  UnmountOnTearDown(apex_path_1);
  UnmountOnTearDown(apex_path_2);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true);
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package_2",
      /* modulePath= */ apex_path_2, /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1", /* isFactory= */ true,
      /* isActive= */ true);
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2)));
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapFailsToScanPreInstalledApexes) {
  AddPreInstalledApex("apex.apexd_test.apex");
  AddPreInstalledApex("apex.apexd_test_corrupt_superblock_apex.apex");

  ASSERT_EQ(OnOtaChrootBootstrap({GetBuiltInDir()}, "/data/local/whatevs"), 1);
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapDataHasHigherVersion) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  std::string apex_path_3 = AddDataApex("apex.apexd_test_v2.apex");

  ASSERT_EQ(OnOtaChrootBootstrap({GetBuiltInDir()}, GetDataDir()), 0);

  UnmountOnTearDown(apex_path_2);
  UnmountOnTearDown(apex_path_3);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@2",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ false);
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package_2",
      /* modulePath= */ apex_path_2, /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1", /* isFactory= */ true,
      /* isActive= */ true);
  auto apex_info_xml_3 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_3,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 2, /* versionName= */ "2",
      /* isFactory= */ false, /* isActive= */ true);
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2),
                                   ApexInfoXmlEq(apex_info_xml_3)));
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapDataHasSameVersion) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  std::string apex_path_3 = AddDataApex("apex.apexd_test.apex");

  ASSERT_EQ(OnOtaChrootBootstrap({GetBuiltInDir()}, GetDataDir()), 0);

  UnmountOnTearDown(apex_path_2);
  UnmountOnTearDown(apex_path_3);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ false);
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package_2",
      /* modulePath= */ apex_path_2, /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1", /* isFactory= */ true,
      /* isActive= */ true);
  auto apex_info_xml_3 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_3,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ false, /* isActive= */ true);
  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2),
                                   ApexInfoXmlEq(apex_info_xml_3)));
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapSystemHasHigherVersion) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test_v2.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  AddDataApex("apex.apexd_test.apex");

  ASSERT_EQ(OnOtaChrootBootstrap({GetBuiltInDir()}, GetDataDir()), 0);

  UnmountOnTearDown(apex_path_1);
  UnmountOnTearDown(apex_path_2);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@2",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 2, /* versionName= */ "2",
      /* isFactory= */ true, /* isActive= */ true);
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package_2",
      /* modulePath= */ apex_path_2, /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1", /* isFactory= */ true,
      /* isActive= */ true);

  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2)));
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapDataHasSameVersionButDifferentKey) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  AddDataApex("apex.apexd_test_different_key.apex");

  ASSERT_EQ(OnOtaChrootBootstrap({GetBuiltInDir()}, GetDataDir()), 0);

  UnmountOnTearDown(apex_path_1);
  UnmountOnTearDown(apex_path_2);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true);
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package_2",
      /* modulePath= */ apex_path_2, /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1", /* isFactory= */ true,
      /* isActive= */ true);

  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2)));
}

TEST_F(ApexdMountTest,
       OnOtaChrootBootstrapDataHasHigherVersionButDifferentKey) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  std::string apex_path_3 =
      AddDataApex("apex.apexd_test_different_key_v2.apex");

  {
    auto apex = ApexFile::Open(apex_path_3);
    ASSERT_TRUE(IsOk(apex));
    ASSERT_EQ(static_cast<uint64_t>(apex->GetManifest().version()), 2ULL);
  }

  ASSERT_EQ(OnOtaChrootBootstrap({GetBuiltInDir()}, GetDataDir()), 0);

  UnmountOnTearDown(apex_path_1);
  UnmountOnTearDown(apex_path_2);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true);
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package_2",
      /* modulePath= */ apex_path_2, /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1", /* isFactory= */ true,
      /* isActive= */ true);

  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2)));
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapDataApexWithoutPreInstalledApex) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  AddDataApex("apex.apexd_test_different_app.apex");

  ASSERT_EQ(OnOtaChrootBootstrap({GetBuiltInDir()}, GetDataDir()), 0);

  UnmountOnTearDown(apex_path_1);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true);

  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1)));
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapPreInstalledSharedLibsApex) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 = AddPreInstalledApex(
      "com.android.apex.test.sharedlibs_generated.v1.libvX.apex");
  std::string apex_path_3 = AddDataApex("apex.apexd_test_v2.apex");

  ASSERT_EQ(OnOtaChrootBootstrap({GetBuiltInDir()}, GetDataDir()), 0);

  UnmountOnTearDown(apex_path_2);
  UnmountOnTearDown(apex_path_3);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@2",
                                   "/apex/com.android.apex.test.sharedlibs@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ false);
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test.sharedlibs",
      /* modulePath= */ apex_path_2,
      /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true);
  auto apex_info_xml_3 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_3,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 2, /* versionName= */ "2",
      /* isFactory= */ false, /* isActive= */ true);

  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2),
                                   ApexInfoXmlEq(apex_info_xml_3)));

  ASSERT_EQ(access("/apex/sharedlibs", F_OK), 0);

  // Check /apex/sharedlibs is populated properly.
  std::vector<std::string> sharedlibs;
  for (const auto& p : fs::recursive_directory_iterator("/apex/sharedlibs")) {
    if (fs::is_symlink(p)) {
      auto src = fs::read_symlink(p.path());
      ASSERT_EQ(p.path().filename(), src.filename());
      sharedlibs.push_back(p.path().parent_path().string() + "->" +
                           src.parent_path().string());
    }
  }

  std::vector<std::string> expected = {
      "/apex/sharedlibs/lib/libsharedlibtest.so->"
      "/apex/com.android.apex.test.sharedlibs@1/lib/libsharedlibtest.so",
      "/apex/sharedlibs/lib/libc++.so->"
      "/apex/com.android.apex.test.sharedlibs@1/lib/libc++.so",
  };

  // On 64bit devices we also have lib64.
  if (!GetProperty("ro.product.cpu.abilist64", "").empty()) {
    expected.push_back(
        "/apex/sharedlibs/lib64/libsharedlibtest.so->"
        "/apex/com.android.apex.test.sharedlibs@1/lib64/libsharedlibtest.so");
    expected.push_back(
        "/apex/sharedlibs/lib64/libc++.so->"
        "/apex/com.android.apex.test.sharedlibs@1/lib64/libc++.so");
  }
  ASSERT_THAT(sharedlibs, UnorderedElementsAreArray(expected));
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapSharedLibsApexBothVersions) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 = AddPreInstalledApex(
      "com.android.apex.test.sharedlibs_generated.v1.libvX.apex");
  std::string apex_path_3 = AddDataApex("apex.apexd_test_v2.apex");
  std::string apex_path_4 =
      AddDataApex("com.android.apex.test.sharedlibs_generated.v2.libvY.apex");

  ASSERT_EQ(OnOtaChrootBootstrap({GetBuiltInDir()}, GetDataDir()), 0);

  UnmountOnTearDown(apex_path_2);
  UnmountOnTearDown(apex_path_3);
  UnmountOnTearDown(apex_path_4);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@2",
                                   "/apex/com.android.apex.test.sharedlibs@1",
                                   "/apex/com.android.apex.test.sharedlibs@2"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ false);
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test.sharedlibs",
      /* modulePath= */ apex_path_2,
      /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ false);
  auto apex_info_xml_3 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_3,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 2, /* versionName= */ "2",
      /* isFactory= */ false, /* isActive= */ true);
  auto apex_info_xml_4 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test.sharedlibs",
      /* modulePath= */ apex_path_4,
      /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 2, /* versionName= */ "2",
      /* isFactory= */ false, /* isActive= */ true);

  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2),
                                   ApexInfoXmlEq(apex_info_xml_3),
                                   ApexInfoXmlEq(apex_info_xml_4)));

  ASSERT_EQ(access("/apex/sharedlibs", F_OK), 0);

  // Check /apex/sharedlibs is populated properly.
  // Because we don't want to hardcode full paths (they are pretty long and have
  // a hash in them which might change if new prebuilts are dropped in), the
  // assertion logic is a little bit clunky.
  std::vector<std::string> sharedlibs;
  for (const auto& p : fs::recursive_directory_iterator("/apex/sharedlibs")) {
    if (fs::is_symlink(p)) {
      auto src = fs::read_symlink(p.path());
      ASSERT_EQ(p.path().filename(), src.filename());
      sharedlibs.push_back(p.path().parent_path().string() + "->" +
                           src.parent_path().string());
    }
  }

  std::vector<std::string> expected = {
      "/apex/sharedlibs/lib/libsharedlibtest.so->"
      "/apex/com.android.apex.test.sharedlibs@2/lib/libsharedlibtest.so",
      "/apex/sharedlibs/lib/libsharedlibtest.so->"
      "/apex/com.android.apex.test.sharedlibs@1/lib/libsharedlibtest.so",
      "/apex/sharedlibs/lib/libc++.so->"
      "/apex/com.android.apex.test.sharedlibs@1/lib/libc++.so",
  };
  // On 64bit devices we also have lib64.
  if (!GetProperty("ro.product.cpu.abilist64", "").empty()) {
    expected.push_back(
        "/apex/sharedlibs/lib64/libsharedlibtest.so->"
        "/apex/com.android.apex.test.sharedlibs@2/lib64/libsharedlibtest.so");
    expected.push_back(
        "/apex/sharedlibs/lib64/libsharedlibtest.so->"
        "/apex/com.android.apex.test.sharedlibs@1/lib64/libsharedlibtest.so");
    expected.push_back(
        "/apex/sharedlibs/lib64/libc++.so->"
        "/apex/com.android.apex.test.sharedlibs@1/lib64/libc++.so");
  }

  ASSERT_THAT(sharedlibs, UnorderedElementsAreArray(expected));
}

static std::string GetSelinuxContext(const std::string& file) {
  char* ctx;
  if (getfilecon(file.c_str(), &ctx) < 0) {
    PLOG(ERROR) << "Failed to getfilecon " << file;
    return "";
  }
  std::string result(ctx);
  freecon(ctx);
  return result;
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapSelinuxLabelsAreCorrect) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 = AddPreInstalledApex(
      "com.android.apex.test.sharedlibs_generated.v1.libvX.apex");
  std::string apex_path_3 = AddDataApex("apex.apexd_test_v2.apex");

  ASSERT_EQ(OnOtaChrootBootstrap({GetBuiltInDir()}, GetDataDir()), 0);

  EXPECT_EQ(GetSelinuxContext("/apex/apex-info-list.xml"),
            "u:object_r:apex_info_file:s0");

  EXPECT_EQ(GetSelinuxContext("/apex/sharedlibs"),
            "u:object_r:apex_mnt_dir:s0");

  EXPECT_EQ(GetSelinuxContext("/apex/com.android.apex.test_package"),
            "u:object_r:system_file:s0");
  EXPECT_EQ(GetSelinuxContext("/apex/com.android.apex.test_package@2"),
            "u:object_r:system_file:s0");
}

TEST_F(ApexdMountTest, OnOtaChrootBootstrapDmDevicesHaveCorrectName) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  std::string apex_path_3 = AddDataApex("apex.apexd_test_v2.apex");

  ASSERT_EQ(OnOtaChrootBootstrap({GetBuiltInDir()}, GetDataDir()), 0);
  UnmountOnTearDown(apex_path_2);
  UnmountOnTearDown(apex_path_3);

  MountedApexDatabase& db = GetApexDatabaseForTesting();
  // com.android.apex.test_package_2 should be mounted directly on top of loop
  // device.
  db.ForallMountedApexes("com.android.apex.test_package_2",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_THAT(data.device_name, IsEmpty());
                           ASSERT_THAT(data.loop_name, StartsWith("/dev"));
                         });
  // com.android.apex.test_package should be mounted on top of dm-verity device.
  db.ForallMountedApexes("com.android.apex.test_package",
                         [&](const MountedApexData& data, bool latest) {
                           ASSERT_TRUE(latest);
                           ASSERT_EQ(data.device_name,
                                     "com.android.apex.test_package@2.chroot");
                           ASSERT_THAT(data.loop_name, StartsWith("/dev"));
                         });
}

TEST_F(ApexdMountTest,
       OnOtaChrootBootstrapFailsToActivatePreInstalledApexKeepsGoing) {
  std::string apex_path_1 =
      AddPreInstalledApex("apex.apexd_test_manifest_mismatch.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");

  ASSERT_EQ(OnOtaChrootBootstrap({GetBuiltInDir()}, GetDataDir()), 1);
  UnmountOnTearDown(apex_path_2);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 137, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ false);
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package_2",
      /* modulePath= */ apex_path_2, /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1", /* isFactory= */ true,
      /* isActive= */ true);

  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2)));
}

TEST_F(ApexdMountTest,
       OnOtaChrootBootstrapFailsToActivateDataApexFallsBackToPreInstalled) {
  std::string apex_path_1 = AddPreInstalledApex("apex.apexd_test.apex");
  std::string apex_path_2 =
      AddPreInstalledApex("apex.apexd_test_different_app.apex");
  std::string apex_path_3 =
      AddDataApex("apex.apexd_test_manifest_mismatch.apex");

  ASSERT_EQ(OnOtaChrootBootstrap({GetBuiltInDir()}, GetDataDir()), 1);
  UnmountOnTearDown(apex_path_1);
  UnmountOnTearDown(apex_path_2);

  auto apex_mounts = GetApexMounts();
  ASSERT_THAT(apex_mounts,
              UnorderedElementsAre("/apex/com.android.apex.test_package",
                                   "/apex/com.android.apex.test_package@1",
                                   "/apex/com.android.apex.test_package_2",
                                   "/apex/com.android.apex.test_package_2@1"));

  ASSERT_EQ(access("/apex/apex-info-list.xml", F_OK), 0);
  auto info_list =
      com::android::apex::readApexInfoList("/apex/apex-info-list.xml");
  ASSERT_TRUE(info_list.has_value());
  auto apex_info_xml_1 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package",
      /* modulePath= */ apex_path_1,
      /* preinstalledModulePath= */ apex_path_1,
      /* versionCode= */ 1, /* versionName= */ "1",
      /* isFactory= */ true, /* isActive= */ true);
  auto apex_info_xml_2 = com::android::apex::ApexInfo(
      /* moduleName= */ "com.android.apex.test_package_2",
      /* modulePath= */ apex_path_2, /* preinstalledModulePath= */ apex_path_2,
      /* versionCode= */ 1, /* versionName= */ "1", /* isFactory= */ true,
      /* isActive= */ true);

  ASSERT_THAT(info_list->getApexInfo(),
              UnorderedElementsAre(ApexInfoXmlEq(apex_info_xml_1),
                                   ApexInfoXmlEq(apex_info_xml_2)));
}

}  // namespace apex
}  // namespace android
