/*
 * Copyright (C) 2019 The Android Open Source Project
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

#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include "apex_file.h"

#include <android-base/result.h>

namespace android {
namespace apex {

// This class encapsulates pre-installed data for all the apexes on device.
// This data can be used to verify validity of an apex before trying to mount
// it.
//
// It's expected to have a single instance of this class in a process that
// mounts apexes (e.g. apexd, otapreopt_chroot).
class ApexPreinstalledData final {
 public:
  // c-tor and d-tor are exposed for testing.
  ApexPreinstalledData(){};

  ~ApexPreinstalledData() { data_.clear(); };

  // Returns a singletone instance of this class.
  static ApexPreinstalledData& GetInstance();

  // Initializes instance by collecting pre-installed data from the given
  // |dirs|.
  // Note: this call is **not thread safe** and is expected to be performed in a
  // single thread during initialization of apexd. After initialization is
  // finished, all queries to the instance are thread safe.
  android::base::Result<void> Initialize(const std::vector<std::string>& dirs);

  // Returns trusted public key for an apex with the given |name|.
  android::base::Result<const std::string> GetPublicKey(
      const std::string& name) const;

  // Returns path to the pre-installed version of an apex with the given |name|.
  android::base::Result<const std::string> GetPreinstalledPath(
      const std::string& name) const;

  // Checks whether there is a pre-installed version of an apex with the given
  // |name|.
  bool HasPreInstalledVersion(const std::string& name) const;

  // Checks if given |apex| is pre-installed.
  bool IsPreInstalledApex(const ApexFile& apex) const;

 private:
  // Non-copyable && non-moveable.
  ApexPreinstalledData(const ApexPreinstalledData&) = delete;
  ApexPreinstalledData& operator=(const ApexPreinstalledData&) = delete;
  ApexPreinstalledData& operator=(ApexPreinstalledData&&) = delete;
  ApexPreinstalledData(ApexPreinstalledData&&) = delete;

  // Scans apexes in the given directory and adds collected data into |data_|.
  android::base::Result<void> ScanDir(const std::string& dir);

  // Internal struct to hold pre-installed data for the given apex.
  struct ApexData {
    // Public key of this apex.
    std::string public_key;
    // Path to the pre-installed version of this apex.
    std::string path;
  };

  std::unordered_map<std::string, ApexData> data_;
};

}  // namespace apex
}  // namespace android
