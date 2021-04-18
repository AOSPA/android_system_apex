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

#include "apexd.h"
#include "apexd_private.h"

#include "apex_constants.h"
#include "apex_database.h"
#include "apex_file.h"
#include "apex_manifest.h"
#include "apex_shim.h"
#include "apexd_checkpoint.h"
#include "apexd_lifecycle.h"
#include "apexd_loop.h"
#include "apexd_prepostinstall.h"
#include "apexd_rollback_utils.h"
#include "apexd_session.h"
#include "apexd_utils.h"
#include "apexd_verity.h"
#include "com_android_apex.h"
#include "string_log.h"

#include <ApexProperties.sysprop.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/macros.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/scopeguard.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <google/protobuf/util/message_differencer.h>
#include <libavb/libavb.h>
#include <libdm/dm.h>
#include <libdm/dm_table.h>
#include <libdm/dm_target.h>
#include <selinux/android.h>

#include <dirent.h>
#include <fcntl.h>
#include <linux/loop.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using android::base::ErrnoError;
using android::base::Error;
using android::base::GetProperty;
using android::base::Join;
using android::base::ParseUint;
using android::base::ReadFully;
using android::base::RemoveFileIfExists;
using android::base::Result;
using android::base::SetProperty;
using android::base::StringPrintf;
using android::base::unique_fd;
using android::dm::DeviceMapper;
using android::dm::DmDeviceState;
using android::dm::DmTable;
using android::dm::DmTargetVerity;
using ::apex::proto::ApexManifest;
using apex::proto::SessionState;
using google::protobuf::util::MessageDifferencer;

namespace android {
namespace apex {

using MountedApexData = MountedApexDatabase::MountedApexData;

namespace {

static constexpr const char* kBuildFingerprintSysprop = "ro.build.fingerprint";

// This should be in UAPI, but it's not :-(
static constexpr const char* kDmVerityRestartOnCorruption =
    "restart_on_corruption";

MountedApexDatabase gMountedApexes;

std::optional<ApexdConfig> gConfig;

CheckpointInterface* gVoldService;
bool gSupportsFsCheckpoints = false;
bool gInFsCheckpointMode = false;

static constexpr size_t kLoopDeviceSetupAttempts = 3u;

// Please DO NOT add new modules to this list without contacting mainline-modularization@ first.
static const std::vector<std::string> kBootstrapApexes = ([]() {
  std::vector<std::string> ret = {
      "com.android.i18n",
      "com.android.runtime",
      "com.android.tzdata",
      "com.android.os.statsd",
  };

  auto vendor_vndk_ver = GetProperty("ro.vndk.version", "");
  if (vendor_vndk_ver != "") {
    ret.push_back("com.android.vndk.v" + vendor_vndk_ver);
  }
  auto product_vndk_ver = GetProperty("ro.product.vndk.version", "");
  if (product_vndk_ver != "" && product_vndk_ver != vendor_vndk_ver) {
    ret.push_back("com.android.vndk.v" + product_vndk_ver);
  }
  return ret;
})();

static constexpr const int kNumRetriesWhenCheckpointingEnabled = 1;

bool IsBootstrapApex(const ApexFile& apex) {
  return std::find(kBootstrapApexes.begin(), kBootstrapApexes.end(),
                   apex.GetManifest().name()) != kBootstrapApexes.end();
}

// Pre-allocate loop devices so that we don't have to wait for them
// later when actually activating APEXes.
Result<void> PreAllocateLoopDevices() {
  auto scan = FindApexes(kApexPackageBuiltinDirs);
  if (!scan.ok()) {
    return scan.error();
  }

  auto size = 0;
  for (const auto& path : *scan) {
    auto apexFile = ApexFile::Open(path);
    if (!apexFile.ok()) {
      continue;
    }
    size++;
    // bootstrap Apexes may be activated on separate namespaces.
    if (IsBootstrapApex(*apexFile)) {
      size++;
    }
  }

  // note: do not call PreAllocateLoopDevices() if size == 0.
  // For devices (e.g. ARC) which doesn't support loop-control
  // PreAllocateLoopDevices() can cause problem when it tries
  // to access /dev/loop-control.
  if (size == 0) {
    return {};
  }
  return loop::PreAllocateLoopDevices(size);
}

std::unique_ptr<DmTable> CreateVerityTable(const ApexVerityData& verity_data,
                                           const std::string& block_device,
                                           const std::string& hash_device,
                                           bool restart_on_corruption) {
  AvbHashtreeDescriptor* desc = verity_data.desc.get();
  auto table = std::make_unique<DmTable>();

  uint32_t hash_start_block = 0;
  if (hash_device == block_device) {
    hash_start_block = desc->tree_offset / desc->hash_block_size;
  }

  auto target = std::make_unique<DmTargetVerity>(
      0, desc->image_size / 512, desc->dm_verity_version, block_device,
      hash_device, desc->data_block_size, desc->hash_block_size,
      desc->image_size / desc->data_block_size, hash_start_block,
      verity_data.hash_algorithm, verity_data.root_digest, verity_data.salt);

  target->IgnoreZeroBlocks();
  if (restart_on_corruption) {
    target->SetVerityMode(kDmVerityRestartOnCorruption);
  }
  table->AddTarget(std::move(target));

  table->set_readonly(true);

  return table;
}

// Deletes a dm-verity device with a given name and path
// Synchronizes on the device actually being deleted from userspace.
Result<void> DeleteVerityDevice(const std::string& name) {
  DeviceMapper& dm = DeviceMapper::Instance();
  auto timeout = std::chrono::milliseconds(
      android::sysprop::ApexProperties::dm_delete_timeout().value_or(750));
  if (!dm.DeleteDevice(name, timeout)) {
    return Error() << "Failed to delete dm-device " << name;
  }
  return {};
}

class DmVerityDevice {
 public:
  DmVerityDevice() : cleared_(true) {}
  explicit DmVerityDevice(std::string name)
      : name_(std::move(name)), cleared_(false) {}
  DmVerityDevice(std::string name, std::string dev_path)
      : name_(std::move(name)),
        dev_path_(std::move(dev_path)),
        cleared_(false) {}

  DmVerityDevice(DmVerityDevice&& other) noexcept
      : name_(std::move(other.name_)),
        dev_path_(std::move(other.dev_path_)),
        cleared_(other.cleared_) {
    other.cleared_ = true;
  }

  DmVerityDevice& operator=(DmVerityDevice&& other) noexcept {
    name_ = other.name_;
    dev_path_ = other.dev_path_;
    cleared_ = other.cleared_;
    other.cleared_ = true;
    return *this;
  }

  ~DmVerityDevice() {
    if (!cleared_) {
      Result<void> ret = DeleteVerityDevice(name_);
      if (!ret.ok()) {
        LOG(ERROR) << ret.error();
      }
    }
  }

  const std::string& GetName() const { return name_; }
  const std::string& GetDevPath() const { return dev_path_; }

  void Release() { cleared_ = true; }

 private:
  std::string name_;
  std::string dev_path_;
  bool cleared_;
};

Result<DmVerityDevice> CreateVerityDevice(const std::string& name,
                                          const DmTable& table) {
  DeviceMapper& dm = DeviceMapper::Instance();

  if (dm.GetState(name) != DmDeviceState::INVALID) {
    // Delete dangling dm-device. This can happen if apexd fails to delete it
    // while unmounting an apex.
    LOG(WARNING) << "Deleting existing dm device " << name;
    auto result = DeleteVerityDevice(name);
    if (!result.ok()) {
      return result.error();
    }
  }

  auto timeout = std::chrono::milliseconds(
      android::sysprop::ApexProperties::dm_create_timeout().value_or(1000));
  std::string dev_path;
  if (!dm.CreateDevice(name, table, &dev_path, timeout)) {
    return Errorf("Couldn't create verity device.");
  }
  return DmVerityDevice(name, dev_path);
}

/**
 * When we create hardlink for a new apex package in kActiveApexPackagesDataDir,
 * there might be an older version of the same package already present in there.
 * Since a new version of the same package is being installed on this boot, the
 * old one needs to deleted so that we don't end up activating same package
 * twice.
 *
 * @param affected_packages package names of the news apex that are being
 * installed in this boot
 * @param files_to_keep path to the new apex packages in
 * kActiveApexPackagesDataDir
 */
Result<void> RemovePreviouslyActiveApexFiles(
    const std::unordered_set<std::string>& affected_packages,
    const std::unordered_set<std::string>& files_to_keep) {
  auto all_active_apex_files =
      FindFilesBySuffix(kActiveApexPackagesDataDir, {kApexPackageSuffix});

  if (!all_active_apex_files.ok()) {
    return all_active_apex_files.error();
  }

  for (const std::string& path : *all_active_apex_files) {
    Result<ApexFile> apex_file = ApexFile::Open(path);
    if (!apex_file.ok()) {
      return apex_file.error();
    }

    const std::string& package_name = apex_file->GetManifest().name();
    if (affected_packages.find(package_name) == affected_packages.end()) {
      // This apex belongs to a package that wasn't part of this stage sessions,
      // hence it should be kept.
      continue;
    }

    if (files_to_keep.find(apex_file->GetPath()) != files_to_keep.end()) {
      // This is a path that was staged and should be kept.
      continue;
    }

    LOG(DEBUG) << "Deleting previously active apex " << apex_file->GetPath();
    if (unlink(apex_file->GetPath().c_str()) != 0) {
      return ErrnoError() << "Failed to unlink " << apex_file->GetPath();
    }
  }

  return {};
}

// Reads the entire device to verify the image is authenticatic
Result<void> ReadVerityDevice(const std::string& verity_device,
                              uint64_t device_size) {
  static constexpr int kBlockSize = 4096;
  static constexpr size_t kBufSize = 1024 * kBlockSize;
  std::vector<uint8_t> buffer(kBufSize);

  unique_fd fd(
      TEMP_FAILURE_RETRY(open(verity_device.c_str(), O_RDONLY | O_CLOEXEC)));
  if (fd.get() == -1) {
    return ErrnoError() << "Can't open " << verity_device;
  }

  size_t bytes_left = device_size;
  while (bytes_left > 0) {
    size_t to_read = std::min(bytes_left, kBufSize);
    if (!android::base::ReadFully(fd.get(), buffer.data(), to_read)) {
      return ErrnoError() << "Can't verify " << verity_device << "; corrupted?";
    }
    bytes_left -= to_read;
  }

  return {};
}

Result<void> VerifyMountedImage(const ApexFile& apex,
                                const std::string& mount_point) {
  // Verify that apex_manifest.pb inside mounted image matches the one in the
  // outer .apex container.
  Result<ApexManifest> verified_manifest =
      ReadManifest(mount_point + "/" + kManifestFilenamePb);
  if (!verified_manifest.ok()) {
    return verified_manifest.error();
  }
  if (!MessageDifferencer::Equals(*verified_manifest, apex.GetManifest())) {
    return Errorf(
        "Manifest inside filesystem does not match manifest outside it");
  }
  if (shim::IsShimApex(apex)) {
    return shim::ValidateShimApex(mount_point, apex);
  }
  return {};
}

Result<MountedApexData> MountPackageImpl(const ApexFile& apex,
                                         const std::string& mount_point,
                                         const std::string& device_name,
                                         const std::string& hashtree_file,
                                         bool verify_image,
                                         bool temp_mount = false) {
  if (apex.IsCompressed()) {
    return Error() << "Cannot directly mount compressed APEX "
                   << apex.GetPath();
  }

  LOG(VERBOSE) << "Creating mount point: " << mount_point;
  // Note: the mount point could exist in case when the APEX was activated
  // during the bootstrap phase (e.g., the runtime or tzdata APEX).
  // Although we have separate mount namespaces to separate the early activated
  // APEXes from the normally activate APEXes, the mount points themselves
  // are shared across the two mount namespaces because /apex (a tmpfs) itself
  // mounted at / which is (and has to be) a shared mount. Therefore, if apexd
  // finds an empty directory under /apex, it's not a problem and apexd can use
  // it.
  auto exists = PathExists(mount_point);
  if (!exists.ok()) {
    return exists.error();
  }
  if (!*exists && mkdir(mount_point.c_str(), kMkdirMode) != 0) {
    return ErrnoError() << "Could not create mount point " << mount_point;
  }
  auto deleter = [&mount_point]() {
    if (rmdir(mount_point.c_str()) != 0) {
      PLOG(WARNING) << "Could not rmdir " << mount_point;
    }
  };
  auto scope_guard = android::base::make_scope_guard(deleter);
  if (!IsEmptyDirectory(mount_point)) {
    return ErrnoError() << mount_point << " is not empty";
  }

  const std::string& full_path = apex.GetPath();

  if (!apex.GetImageOffset() || !apex.GetImageSize()) {
    return Error() << "Cannot create mount point without image offset and size";
  }
  loop::LoopbackDeviceUniqueFd loopback_device;
  for (size_t attempts = 1;; ++attempts) {
    Result<loop::LoopbackDeviceUniqueFd> ret = loop::CreateLoopDevice(
        full_path, apex.GetImageOffset().value(), apex.GetImageSize().value());
    if (ret.ok()) {
      loopback_device = std::move(*ret);
      break;
    }
    if (attempts >= kLoopDeviceSetupAttempts) {
      return Error() << "Could not create loop device for " << full_path << ": "
                     << ret.error();
    }
  }
  LOG(VERBOSE) << "Loopback device created: " << loopback_device.name;

  auto& instance = ApexFileRepository::GetInstance();

  auto public_key = instance.GetPublicKey(apex.GetManifest().name());
  if (!public_key.ok()) {
    return public_key.error();
  }

  auto verity_data = apex.VerifyApexVerity(*public_key);
  if (!verity_data.ok()) {
    return Error() << "Failed to verify Apex Verity data for " << full_path
                   << ": " << verity_data.error();
  }
  std::string block_device = loopback_device.name;
  MountedApexData apex_data(loopback_device.name, apex.GetPath(), mount_point,
                            /* device_name = */ "",
                            /* hashtree_loop_name = */ "",
                            /* is_temp_mount */ temp_mount);

  // for APEXes in immutable partitions, we don't need to mount them on
  // dm-verity because they are already in the dm-verity protected partition;
  // system. However, note that we don't skip verification to ensure that APEXes
  // are correctly signed.
  const bool mount_on_verity = !instance.IsPreInstalledApex(apex);
  DmVerityDevice verity_dev;
  loop::LoopbackDeviceUniqueFd loop_for_hash;
  if (mount_on_verity) {
    std::string hash_device = loopback_device.name;
    if (verity_data->desc->tree_size == 0) {
      if (auto st = PrepareHashTree(apex, *verity_data, hashtree_file);
          !st.ok()) {
        return st.error();
      }
      auto create_loop_status = loop::CreateLoopDevice(hashtree_file, 0, 0);
      if (!create_loop_status.ok()) {
        return create_loop_status.error();
      }
      loop_for_hash = std::move(*create_loop_status);
      hash_device = loop_for_hash.name;
      apex_data.hashtree_loop_name = hash_device;
    }
    auto verity_table =
        CreateVerityTable(*verity_data, loopback_device.name, hash_device,
                          /* restart_on_corruption = */ !verify_image);
    Result<DmVerityDevice> verity_dev_res =
        CreateVerityDevice(device_name, *verity_table);
    if (!verity_dev_res.ok()) {
      return Error() << "Failed to create Apex Verity device " << full_path
                     << ": " << verity_dev_res.error();
    }
    verity_dev = std::move(*verity_dev_res);
    apex_data.device_name = device_name;
    block_device = verity_dev.GetDevPath();

    Result<void> read_ahead_status =
        loop::ConfigureReadAhead(verity_dev.GetDevPath());
    if (!read_ahead_status.ok()) {
      return read_ahead_status.error();
    }
  }
  // TODO(b/158467418): consider moving this inside RunVerifyFnInsideTempMount.
  if (mount_on_verity && verify_image) {
    Result<void> verity_status =
        ReadVerityDevice(block_device, (*verity_data).desc->image_size);
    if (!verity_status.ok()) {
      return verity_status.error();
    }
  }

  uint32_t mount_flags = MS_NOATIME | MS_NODEV | MS_DIRSYNC | MS_RDONLY;
  if (apex.GetManifest().nocode()) {
    mount_flags |= MS_NOEXEC;
  }

  if (!apex.GetFsType()) {
    return Error() << "Cannot mount package without FsType";
  }
  if (mount(block_device.c_str(), mount_point.c_str(),
            apex.GetFsType().value().c_str(), mount_flags, nullptr) == 0) {
    LOG(INFO) << "Successfully mounted package " << full_path << " on "
              << mount_point;
    auto status = VerifyMountedImage(apex, mount_point);
    if (!status.ok()) {
      if (umount2(mount_point.c_str(), UMOUNT_NOFOLLOW) != 0) {
        PLOG(ERROR) << "Failed to umount " << mount_point;
      }
      return Error() << "Failed to verify " << full_path << ": "
                     << status.error();
    }
    // Time to accept the temporaries as good.
    verity_dev.Release();
    loopback_device.CloseGood();
    loop_for_hash.CloseGood();

    scope_guard.Disable();  // Accept the mount.
    return apex_data;
  } else {
    return ErrnoError() << "Mounting failed for package " << full_path;
  }
}

std::string GetHashTreeFileName(const ApexFile& apex, bool is_new) {
  std::string ret =
      std::string(kApexHashTreeDir) + "/" + GetPackageId(apex.GetManifest());
  return is_new ? ret + ".new" : ret;
}

Result<MountedApexData> VerifyAndTempMountPackage(
    const ApexFile& apex, const std::string& mount_point) {
  const std::string& package_id = GetPackageId(apex.GetManifest());
  LOG(DEBUG) << "Temp mounting " << package_id << " to " << mount_point;
  const std::string& temp_device_name = package_id + ".tmp";
  std::string hashtree_file = GetHashTreeFileName(apex, /* is_new = */ true);
  if (access(hashtree_file.c_str(), F_OK) == 0) {
    LOG(DEBUG) << hashtree_file << " already exists. Deleting it";
    if (TEMP_FAILURE_RETRY(unlink(hashtree_file.c_str())) != 0) {
      return ErrnoError() << "Failed to unlink " << hashtree_file;
    }
  }
  auto ret =
      MountPackageImpl(apex, mount_point, temp_device_name, hashtree_file,
                       /* verify_image = */ true, /* temp_mount = */ true);
  if (!ret.ok()) {
    LOG(DEBUG) << "Cleaning up " << hashtree_file;
    if (TEMP_FAILURE_RETRY(unlink(hashtree_file.c_str())) != 0) {
      PLOG(ERROR) << "Failed to unlink " << hashtree_file;
    }
  } else {
    gMountedApexes.AddMountedApex(apex.GetManifest().name(), false, *ret);
  }
  return ret;
}

Result<void> Unmount(const MountedApexData& data) {
  LOG(DEBUG) << "Unmounting " << data.full_path << " from mount point "
             << data.mount_point;
  // Lazily try to umount whatever is mounted.
  if (umount2(data.mount_point.c_str(), UMOUNT_NOFOLLOW) != 0 &&
      errno != EINVAL && errno != ENOENT) {
    return ErrnoError() << "Failed to unmount directory " << data.mount_point;
  }
  // Attempt to delete the folder. If the folder is retained, other
  // data may be incorrect.
  if (rmdir(data.mount_point.c_str()) != 0) {
    PLOG(ERROR) << "Failed to rmdir directory " << data.mount_point;
  }

  // Try to free up the device-mapper device.
  if (!data.device_name.empty()) {
    const auto& result = DeleteVerityDevice(data.device_name);
    if (!result.ok()) {
      return result;
    }
  }

  // Try to free up the loop device.
  auto log_fn = [](const std::string& path, const std::string& /*id*/) {
    LOG(VERBOSE) << "Freeing loop device " << path << " for unmount.";
  };
  if (!data.loop_name.empty()) {
    loop::DestroyLoopDevice(data.loop_name, log_fn);
  }
  if (!data.hashtree_loop_name.empty()) {
    loop::DestroyLoopDevice(data.hashtree_loop_name, log_fn);
  }

  return {};
}

template <typename VerifyFn>
Result<void> RunVerifyFnInsideTempMount(const ApexFile& apex,
                                        const VerifyFn& verify_fn,
                                        bool unmount_during_cleanup) {
  // Temp mount image of this apex to validate it was properly signed;
  // this will also read the entire block device through dm-verity, so
  // we can be sure there is no corruption.
  const std::string& temp_mount_point =
      apexd_private::GetPackageTempMountPoint(apex.GetManifest());

  Result<MountedApexData> mount_status =
      VerifyAndTempMountPackage(apex, temp_mount_point);
  if (!mount_status.ok()) {
    LOG(ERROR) << "Failed to temp mount to " << temp_mount_point << " : "
               << mount_status.error();
    return mount_status.error();
  }
  auto cleaner = [&]() {
    if (unmount_during_cleanup) {
      LOG(DEBUG) << "Unmounting " << temp_mount_point;
      Result<void> result = Unmount(*mount_status);
      if (!result.ok()) {
        LOG(WARNING) << "Failed to unmount " << temp_mount_point << " : "
                     << result.error();
      }
      gMountedApexes.RemoveMountedApex(apex.GetManifest().name(),
                                       apex.GetPath(), true);
    }
  };
  auto scope_guard = android::base::make_scope_guard(cleaner);
  return verify_fn(temp_mount_point);
}

template <typename HookFn, typename HookCall>
Result<void> PrePostinstallPackages(const std::vector<ApexFile>& apexes,
                                    HookFn fn, HookCall call) {
  auto scope_guard = android::base::make_scope_guard([&]() {
    for (const ApexFile& apex_file : apexes) {
      apexd_private::UnmountTempMount(apex_file);
    }
  });
  if (apexes.empty()) {
    return Errorf("Empty set of inputs");
  }

  // 1) Check whether the APEXes have hooks.
  bool has_hooks = false;
  for (const ApexFile& apex_file : apexes) {
    if (!(apex_file.GetManifest().*fn)().empty()) {
      has_hooks = true;
      break;
    }
  }

  // 2) If we found hooks, temp mount if required, and run the pre/post-install.
  if (has_hooks) {
    std::vector<std::string> mount_points;
    for (const ApexFile& apex : apexes) {
      // Retrieve the mount data if the apex is already temp mounted, temp
      // mount it otherwise.
      std::string mount_point =
          apexd_private::GetPackageTempMountPoint(apex.GetManifest());
      Result<MountedApexData> mount_data =
          apexd_private::GetTempMountedApexData(apex.GetManifest().name());
      if (!mount_data.ok()) {
        mount_data = VerifyAndTempMountPackage(apex, mount_point);
        if (!mount_data.ok()) {
          return mount_data.error();
        }
      }
      mount_points.push_back(mount_point);
    }

    Result<void> install_status = (*call)(apexes, mount_points);
    if (!install_status.ok()) {
      return install_status;
    }
  }

  return {};
}

Result<void> PreinstallPackages(const std::vector<ApexFile>& apexes) {
  return PrePostinstallPackages(apexes, &ApexManifest::preinstallhook,
                                &StagePreInstall);
}

Result<void> PostinstallPackages(const std::vector<ApexFile>& apexes) {
  return PrePostinstallPackages(apexes, &ApexManifest::postinstallhook,
                                &StagePostInstall);
}

// Converts a list of apex file paths into a list of ApexFile objects
//
// Returns error when trying to open empty set of inputs.
Result<std::vector<ApexFile>> OpenApexFiles(
    const std::vector<std::string>& paths) {
  if (paths.empty()) {
    return Errorf("Empty set of inputs");
  }
  std::vector<ApexFile> ret;
  for (const std::string& path : paths) {
    Result<ApexFile> apex_file = ApexFile::Open(path);
    if (!apex_file.ok()) {
      return apex_file.error();
    }
    ret.emplace_back(std::move(*apex_file));
  }
  return ret;
}

Result<void> ValidateStagingShimApex(const ApexFile& to) {
  using android::base::StringPrintf;
  auto system_shim = ApexFile::Open(
      StringPrintf("%s/%s", kApexPackageSystemDir, shim::kSystemShimApexName));
  if (!system_shim.ok()) {
    return system_shim.error();
  }
  auto verify_fn = [&](const std::string& system_apex_path) {
    return shim::ValidateUpdate(system_apex_path, to.GetPath());
  };
  return RunVerifyFnInsideTempMount(*system_shim, verify_fn, true);
}

// A version of apex verification that happens during boot.
// This function should only verification checks that are necessary to run on
// each boot. Try to avoid putting expensive checks inside this function.
Result<void> VerifyPackageBoot(const ApexFile& apex_file) {
  // TODO(ioffe): why do we need this here?
  auto& instance = ApexFileRepository::GetInstance();
  auto public_key = instance.GetPublicKey(apex_file.GetManifest().name());
  if (!public_key.ok()) {
    return public_key.error();
  }
  Result<ApexVerityData> verity_or = apex_file.VerifyApexVerity(*public_key);
  if (!verity_or.ok()) {
    return verity_or.error();
  }

  if (shim::IsShimApex(apex_file)) {
    // Validating shim is not a very cheap operation, but it's fine to perform
    // it here since it only runs during CTS tests and will never be triggered
    // during normal flow.
    const auto& result = ValidateStagingShimApex(apex_file);
    if (!result.ok()) {
      return result;
    }
  }
  return {};
}

// A version of apex verification that happens on SubmitStagedSession.
// This function contains checks that might be expensive to perform, e.g. temp
// mounting a package and reading entire dm-verity device, and shouldn't be run
// during boot.
Result<void> VerifyPackageInstall(const ApexFile& apex_file) {
  const auto& verify_package_boot_status = VerifyPackageBoot(apex_file);
  if (!verify_package_boot_status.ok()) {
    return verify_package_boot_status;
  }

  constexpr const auto kSuccessFn = [](const std::string& /*mount_point*/) {
    return Result<void>{};
  };
  return RunVerifyFnInsideTempMount(apex_file, kSuccessFn, false);
}

template <typename VerifyApexFn>
Result<std::vector<ApexFile>> VerifyPackages(
    const std::vector<std::string>& paths, const VerifyApexFn& verify_apex_fn) {
  Result<std::vector<ApexFile>> apex_files = OpenApexFiles(paths);
  if (!apex_files.ok()) {
    return apex_files.error();
  }

  LOG(DEBUG) << "VerifyPackages() for " << Join(paths, ',');

  for (const ApexFile& apex_file : *apex_files) {
    Result<void> result = verify_apex_fn(apex_file);
    if (!result.ok()) {
      return result.error();
    }
  }
  return std::move(*apex_files);
}

Result<ApexFile> VerifySessionDir(const int session_id) {
  std::string session_dir_path = std::string(kStagedSessionsDir) + "/session_" +
                                 std::to_string(session_id);
  LOG(INFO) << "Scanning " << session_dir_path
            << " looking for packages to be validated";
  Result<std::vector<std::string>> scan =
      FindFilesBySuffix(session_dir_path, {kApexPackageSuffix});
  if (!scan.ok()) {
    LOG(WARNING) << scan.error();
    return scan.error();
  }

  if (scan->size() > 1) {
    return Errorf(
        "More than one APEX package found in the same session directory.");
  }

  auto verified = VerifyPackages(*scan, VerifyPackageInstall);
  if (!verified.ok()) {
    return verified.error();
  }
  return std::move((*verified)[0]);
}

Result<void> DeleteBackup() {
  auto exists = PathExists(std::string(kApexBackupDir));
  if (!exists.ok()) {
    return Error() << "Can't clean " << kApexBackupDir << " : "
                   << exists.error();
  }
  if (!*exists) {
    LOG(DEBUG) << kApexBackupDir << " does not exist. Nothing to clean";
    return {};
  }
  return DeleteDirContent(std::string(kApexBackupDir));
}

Result<void> BackupActivePackages() {
  LOG(DEBUG) << "Initializing  backup of " << kActiveApexPackagesDataDir;

  // Previous restore might've delete backups folder.
  auto create_status = CreateDirIfNeeded(kApexBackupDir, 0700);
  if (!create_status.ok()) {
    return Error() << "Backup failed : " << create_status.error();
  }

  auto apex_active_exists = PathExists(std::string(kActiveApexPackagesDataDir));
  if (!apex_active_exists.ok()) {
    return Error() << "Backup failed : " << apex_active_exists.error();
  }
  if (!*apex_active_exists) {
    LOG(DEBUG) << kActiveApexPackagesDataDir
               << " does not exist. Nothing to backup";
    return {};
  }

  auto active_packages =
      FindFilesBySuffix(kActiveApexPackagesDataDir, {kApexPackageSuffix});
  if (!active_packages.ok()) {
    return Error() << "Backup failed : " << active_packages.error();
  }

  auto cleanup_status = DeleteBackup();
  if (!cleanup_status.ok()) {
    return Error() << "Backup failed : " << cleanup_status.error();
  }

  auto backup_path_fn = [](const ApexFile& apex_file) {
    return StringPrintf("%s/%s%s", kApexBackupDir,
                        GetPackageId(apex_file.GetManifest()).c_str(),
                        kApexPackageSuffix);
  };

  auto deleter = []() {
    auto result = DeleteDirContent(std::string(kApexBackupDir));
    if (!result.ok()) {
      LOG(ERROR) << "Failed to cleanup " << kApexBackupDir << " : "
                 << result.error();
    }
  };
  auto scope_guard = android::base::make_scope_guard(deleter);

  for (const std::string& path : *active_packages) {
    Result<ApexFile> apex_file = ApexFile::Open(path);
    if (!apex_file.ok()) {
      return Error() << "Backup failed : " << apex_file.error();
    }
    const auto& dest_path = backup_path_fn(*apex_file);
    if (link(apex_file->GetPath().c_str(), dest_path.c_str()) != 0) {
      return ErrnoError() << "Failed to backup " << apex_file->GetPath();
    }
  }

  scope_guard.Disable();  // Accept the backup.
  return {};
}

Result<void> RestoreActivePackages() {
  LOG(DEBUG) << "Initializing  restore of " << kActiveApexPackagesDataDir;

  auto backup_exists = PathExists(std::string(kApexBackupDir));
  if (!backup_exists.ok()) {
    return backup_exists.error();
  }
  if (!*backup_exists) {
    return Error() << kApexBackupDir << " does not exist";
  }

  struct stat stat_data;
  if (stat(kActiveApexPackagesDataDir, &stat_data) != 0) {
    return ErrnoError() << "Failed to access " << kActiveApexPackagesDataDir;
  }

  LOG(DEBUG) << "Deleting existing packages in " << kActiveApexPackagesDataDir;
  auto delete_status =
      DeleteDirContent(std::string(kActiveApexPackagesDataDir));
  if (!delete_status.ok()) {
    return delete_status;
  }

  LOG(DEBUG) << "Renaming " << kApexBackupDir << " to "
             << kActiveApexPackagesDataDir;
  if (rename(kApexBackupDir, kActiveApexPackagesDataDir) != 0) {
    return ErrnoError() << "Failed to rename " << kApexBackupDir << " to "
                        << kActiveApexPackagesDataDir;
  }

  LOG(DEBUG) << "Restoring original permissions for "
             << kActiveApexPackagesDataDir;
  if (chmod(kActiveApexPackagesDataDir, stat_data.st_mode & ALLPERMS) != 0) {
    return ErrnoError() << "Failed to restore original permissions for "
                        << kActiveApexPackagesDataDir;
  }

  return {};
}

Result<void> UnmountPackage(const ApexFile& apex, bool allow_latest) {
  LOG(INFO) << "Unmounting " << GetPackageId(apex.GetManifest());

  const ApexManifest& manifest = apex.GetManifest();

  std::optional<MountedApexData> data;
  bool latest = false;

  auto fn = [&](const MountedApexData& d, bool l) {
    if (d.full_path == apex.GetPath()) {
      data.emplace(d);
      latest = l;
    }
  };
  gMountedApexes.ForallMountedApexes(manifest.name(), fn);

  if (!data) {
    return Error() << "Did not find " << apex.GetPath();
  }

  // Concept of latest sharedlibs apex is somewhat blurred. Since this is only
  // used in testing, it is ok to always allow unmounting sharedlibs apex.
  if (latest && !manifest.providesharedapexlibs()) {
    if (!allow_latest) {
      return Error() << "Package " << apex.GetPath() << " is active";
    }
    std::string mount_point = apexd_private::GetActiveMountPoint(manifest);
    LOG(INFO) << "Unmounting and deleting " << mount_point;
    if (umount2(mount_point.c_str(), UMOUNT_NOFOLLOW) != 0) {
      return ErrnoError() << "Failed to unmount " << mount_point;
    }
    if (rmdir(mount_point.c_str()) != 0) {
      PLOG(ERROR) << "Could not rmdir " << mount_point;
      // Continue here.
    }
  }

  // Clean up gMountedApexes now, even though we're not fully done.
  gMountedApexes.RemoveMountedApex(manifest.name(), apex.GetPath());
  return Unmount(*data);
}

}  // namespace

void SetConfig(const ApexdConfig& config) { gConfig = config; }

Result<void> MountPackage(const ApexFile& apex, const std::string& mount_point,
                          const std::string& device_name) {
  auto ret = MountPackageImpl(apex, mount_point, device_name,
                              GetHashTreeFileName(apex, /* is_new= */ false),
                              /* verify_image = */ false);
  if (!ret.ok()) {
    return ret.error();
  }

  gMountedApexes.AddMountedApex(apex.GetManifest().name(), false, *ret);
  return {};
}

namespace apexd_private {

Result<void> UnmountTempMount(const ApexFile& apex) {
  const ApexManifest& manifest = apex.GetManifest();
  LOG(VERBOSE) << "Unmounting all temp mounts for package " << manifest.name();

  bool finished_unmounting = false;
  // If multiple temp mounts exist, ensure that all are unmounted.
  while (!finished_unmounting) {
    Result<MountedApexData> data =
        apexd_private::GetTempMountedApexData(manifest.name());
    if (!data.ok()) {
      finished_unmounting = true;
    } else {
      gMountedApexes.RemoveMountedApex(manifest.name(), data->full_path, true);
      Unmount(*data);
    }
  }
  return {};
}

Result<MountedApexData> GetTempMountedApexData(const std::string& package) {
  bool found = false;
  Result<MountedApexData> mount_data;
  gMountedApexes.ForallMountedApexes(
      package,
      [&](const MountedApexData& data, [[maybe_unused]] bool latest) {
        if (!found) {
          mount_data = data;
          found = true;
        }
      },
      true);
  if (found) {
    return mount_data;
  }
  return Error() << "No temp mount data found for " << package;
}

bool IsMounted(const std::string& full_path) {
  bool found_mounted = false;
  gMountedApexes.ForallMountedApexes([&](const std::string&,
                                         const MountedApexData& data,
                                         [[maybe_unused]] bool latest) {
    if (full_path == data.full_path) {
      found_mounted = true;
    }
  });
  return found_mounted;
}

std::string GetPackageMountPoint(const ApexManifest& manifest) {
  return StringPrintf("%s/%s", kApexRoot, GetPackageId(manifest).c_str());
}

std::string GetPackageTempMountPoint(const ApexManifest& manifest) {
  return StringPrintf("%s.tmp", GetPackageMountPoint(manifest).c_str());
}

std::string GetActiveMountPoint(const ApexManifest& manifest) {
  return StringPrintf("%s/%s", kApexRoot, manifest.name().c_str());
}

}  // namespace apexd_private

Result<void> ResumeRevertIfNeeded() {
  auto sessions =
      ApexSession::GetSessionsInState(SessionState::REVERT_IN_PROGRESS);
  if (sessions.empty()) {
    return {};
  }
  return RevertActiveSessions("");
}

Result<void> ActivateSharedLibsPackage(const std::string& mount_point) {
  for (const auto& lib_path : {"lib", "lib64"}) {
    std::string apex_lib_path = mount_point + "/" + lib_path;
    auto lib_dir = PathExists(apex_lib_path);
    if (!lib_dir.ok() || !*lib_dir) {
      continue;
    }

    auto iter = std::filesystem::directory_iterator(apex_lib_path);
    std::error_code ec;

    while (iter != std::filesystem::end(iter)) {
      const auto& lib_entry = *iter;
      if (!lib_entry.is_directory()) {
        iter = iter.increment(ec);
        if (ec) {
          return Error() << "Failed to scan " << apex_lib_path << " : "
                         << ec.message();
        }
        continue;
      }

      const auto library_name = lib_entry.path().filename();
      const std::string library_symlink_dir =
          StringPrintf("%s/%s/%s/%s", kApexRoot, kApexSharedLibsSubDir,
                       lib_path, library_name.c_str());

      auto symlink_dir = PathExists(library_symlink_dir);
      if (!symlink_dir.ok() || !*symlink_dir) {
        std::filesystem::create_directory(library_symlink_dir, ec);
        if (ec) {
          return Error() << "Failed to create directory " << library_symlink_dir
                         << ": " << ec.message();
        }
      }

      auto inner_iter =
          std::filesystem::directory_iterator(lib_entry.path().string());

      while (inner_iter != std::filesystem::end(inner_iter)) {
        const auto& lib_items = *inner_iter;
        const auto hash_value = lib_items.path().filename();
        const std::string library_symlink_hash = StringPrintf(
            "%s/%s", library_symlink_dir.c_str(), hash_value.c_str());

        auto hash_dir = PathExists(library_symlink_hash);
        if (hash_dir.ok() && *hash_dir) {
          // Compare file size for two library files with same name and hash
          // value
          auto existing_file_path =
              library_symlink_hash + "/" + library_name.string();
          auto existing_file_size = GetFileSize(existing_file_path);
          if (!existing_file_size.ok()) {
            return existing_file_size.error();
          }

          auto new_file_path =
              lib_items.path().string() + "/" + library_name.string();
          auto new_file_size = GetFileSize(new_file_path);
          if (!new_file_size.ok()) {
            return new_file_size.error();
          }

          if (*existing_file_size != *new_file_size) {
            return Error() << "There are two libraries with same hash and "
                              "different file size : "
                           << existing_file_path << " and " << new_file_path;
          }

          inner_iter = inner_iter.increment(ec);
          if (ec) {
            return Error() << "Failed to scan " << lib_entry.path().string()
                           << " : " << ec.message();
          }
          continue;
        }
        std::filesystem::create_directory_symlink(lib_items.path(),
                                                  library_symlink_hash, ec);
        if (ec) {
          return Error() << "Failed to create symlink from " << lib_items.path()
                         << " to " << library_symlink_hash << ec.message();
        }

        inner_iter = inner_iter.increment(ec);
        if (ec) {
          return Error() << "Failed to scan " << lib_entry.path().string()
                         << " : " << ec.message();
        }
      }

      iter = iter.increment(ec);
      if (ec) {
        return Error() << "Failed to scan " << apex_lib_path << " : "
                       << ec.message();
      }
    }
  }

  return {};
}

bool IsValidPackageName(const std::string& package_name) {
  return kBannedApexName.count(package_name) == 0;
}

Result<void> ActivatePackageImpl(const ApexFile& apex_file,
                                 const std::string& device_name) {
  const ApexManifest& manifest = apex_file.GetManifest();

  if (!IsValidPackageName(manifest.name())) {
    return Errorf("Package name {} is not allowed.", manifest.name());
  }

  // See whether we think it's active, and do not allow to activate the same
  // version. Also detect whether this is the highest version.
  // We roll this into a single check.
  bool is_newest_version = true;
  bool version_found_mounted = false;
  {
    uint64_t new_version = manifest.version();
    bool version_found_active = false;
    gMountedApexes.ForallMountedApexes(
        manifest.name(), [&](const MountedApexData& data, bool latest) {
          Result<ApexFile> other_apex = ApexFile::Open(data.full_path);
          if (!other_apex.ok()) {
            return;
          }
          if (static_cast<uint64_t>(other_apex->GetManifest().version()) ==
              new_version) {
            version_found_mounted = true;
            version_found_active = latest;
          }
          if (static_cast<uint64_t>(other_apex->GetManifest().version()) >
              new_version) {
            is_newest_version = false;
          }
        });
    // If the package provides shared libraries to other APEXs, we need to
    // activate all versions available (i.e. preloaded on /system/apex and
    // available on /data/apex/active). The reason is that there might be some
    // APEXs loaded from /system/apex that reference the libraries contained on
    // the preloaded version of the apex providing shared libraries.
    if (version_found_active && !manifest.providesharedapexlibs()) {
      LOG(DEBUG) << "Package " << manifest.name() << " with version "
                 << manifest.version() << " already active";
      return {};
    }
  }

  const std::string& mount_point =
      apexd_private::GetPackageMountPoint(manifest);

  if (!version_found_mounted) {
    auto mount_status = MountPackage(apex_file, mount_point, device_name);
    if (!mount_status.ok()) {
      return mount_status;
    }
  }

  // For packages providing shared libraries, avoid creating a bindmount since
  // there is no use for the /apex/<package_name> directory. However, mark the
  // highest version as latest so that the latest version of the package can be
  // properly reported to PackageManager.
  if (manifest.providesharedapexlibs()) {
    if (is_newest_version) {
      gMountedApexes.SetLatest(manifest.name(), apex_file.GetPath());
    }
  } else {
    bool mounted_latest = false;
    // Bind mount the latest version to /apex/<package_name>, unless the
    // package provides shared libraries to other APEXs.
    if (is_newest_version) {
      const Result<void>& update_st = apexd_private::BindMount(
          apexd_private::GetActiveMountPoint(manifest), mount_point);
      mounted_latest = update_st.has_value();
      if (!update_st.ok()) {
        return Error() << "Failed to update package " << manifest.name()
                       << " to version " << manifest.version() << " : "
                       << update_st.error();
      }
    }
    if (mounted_latest) {
      gMountedApexes.SetLatest(manifest.name(), apex_file.GetPath());
    }
  }

  if (manifest.providesharedapexlibs()) {
    const auto& handle_shared_libs_apex =
        ActivateSharedLibsPackage(mount_point);
    if (!handle_shared_libs_apex.ok()) {
      return handle_shared_libs_apex;
    }
  }

  LOG(DEBUG) << "Successfully activated " << apex_file.GetPath()
             << " package_name: " << manifest.name()
             << " version: " << manifest.version();
  return {};
}

Result<void> ActivatePackage(const std::string& full_path) {
  LOG(INFO) << "Trying to activate " << full_path;

  Result<ApexFile> apex_file = ApexFile::Open(full_path);
  if (!apex_file.ok()) {
    return apex_file.error();
  }
  return ActivatePackageImpl(*apex_file,
                             GetPackageId(apex_file->GetManifest()));
}

Result<void> DeactivatePackage(const std::string& full_path) {
  LOG(INFO) << "Trying to deactivate " << full_path;

  Result<ApexFile> apex_file = ApexFile::Open(full_path);
  if (!apex_file.ok()) {
    return apex_file.error();
  }

  return UnmountPackage(*apex_file, /* allow_latest= */ true);
}

std::vector<ApexFile> GetActivePackages() {
  std::vector<ApexFile> ret;
  gMountedApexes.ForallMountedApexes(
      [&](const std::string&, const MountedApexData& data, bool latest) {
        if (!latest) {
          return;
        }

        Result<ApexFile> apex_file = ApexFile::Open(data.full_path);
        if (!apex_file.ok()) {
          return;
        }
        ret.emplace_back(std::move(*apex_file));
      });

  return ret;
}

Result<void> EmitApexInfoList(bool is_bootstrap) {
  // on a non-updatable device, we don't have APEX database to emit
  if (!android::sysprop::ApexProperties::updatable().value_or(false)) {
    return {};
  }

  // Apexd runs both in "bootstrap" and "default" mount namespace.
  // To expose /apex/apex-info-list.xml separately in each mount namespaces,
  // we write /apex/.<namespace>-apex-info-list .xml file first and then
  // bind mount it to the canonical file (/apex/apex-info-list.xml).
  const std::string file_name =
      fmt::format("{}/.{}-{}", kApexRoot,
                  is_bootstrap ? "bootstrap" : "default", kApexInfoList);

  unique_fd fd(TEMP_FAILURE_RETRY(
      open(file_name.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)));
  if (fd.get() == -1) {
    return ErrnoErrorf("Can't open {}", file_name);
  }

  const std::vector<ApexFile> active(GetActivePackages());

  std::vector<ApexFile> inactive;
  // we skip for non-activated built-in apexes in bootstrap mode
  // in order to avoid boottime increase
  if (!is_bootstrap) {
    inactive = GetFactoryPackages();
    auto new_end = std::remove_if(
        inactive.begin(), inactive.end(), [&active](const ApexFile& apex) {
          return std::any_of(active.begin(), active.end(),
                             [&apex](const ApexFile& active_apex) {
                               return apex.GetPath() == active_apex.GetPath();
                             });
        });
    inactive.erase(new_end, inactive.end());
  }

  std::stringstream xml;
  CollectApexInfoList(xml, active, inactive);

  if (!android::base::WriteStringToFd(xml.str(), fd)) {
    return ErrnoErrorf("Can't write to {}", file_name);
  }

  fd.reset();

  const std::string mount_point =
      fmt::format("{}/{}", kApexRoot, kApexInfoList);
  if (access(mount_point.c_str(), F_OK) != 0) {
    close(open(mount_point.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
               0644));
  }
  if (mount(file_name.c_str(), mount_point.c_str(), nullptr, MS_BIND,
            nullptr) == -1) {
    return ErrnoErrorf("Can't bind mount {} to {}", file_name, mount_point);
  }
  return RestoreconPath(file_name);
}

namespace {
std::unordered_map<std::string, uint64_t> GetActivePackagesMap() {
  std::vector<ApexFile> active_packages = GetActivePackages();
  std::unordered_map<std::string, uint64_t> ret;
  for (const auto& package : active_packages) {
    const ApexManifest& manifest = package.GetManifest();
    ret.insert({manifest.name(), manifest.version()});
  }
  return ret;
}

}  // namespace

std::vector<ApexFile> GetFactoryPackages() {
  std::vector<ApexFile> ret;

  // Decompressed APEX is considered factory package
  std::vector<std::string> decompressed_pkg_names;
  auto active_pkgs = GetActivePackages();
  for (ApexFile& apex : active_pkgs) {
    if (ApexFileRepository::GetInstance().IsDecompressedApex(apex)) {
      decompressed_pkg_names.push_back(apex.GetManifest().name());
      ret.emplace_back(std::move(apex));
    }
  }

  for (const auto& dir : kApexPackageBuiltinDirs) {
    auto all_apex_files = FindFilesBySuffix(
        dir, {kApexPackageSuffix, kCompressedApexPackageSuffix});
    if (!all_apex_files.ok()) {
      LOG(ERROR) << all_apex_files.error();
      continue;
    }

    for (const std::string& path : *all_apex_files) {
      Result<ApexFile> apex_file = ApexFile::Open(path);
      if (!apex_file.ok()) {
        LOG(ERROR) << apex_file.error();
        continue;
      }
      // Ignore compressed APEX if it has been decompressed already
      if (apex_file->IsCompressed() &&
          std::find(decompressed_pkg_names.begin(),
                    decompressed_pkg_names.end(),
                    apex_file->GetManifest().name()) !=
              decompressed_pkg_names.end()) {
        continue;
      }

      ret.emplace_back(std::move(*apex_file));
    }
  }
  return ret;
}

Result<ApexFile> GetActivePackage(const std::string& packageName) {
  std::vector<ApexFile> packages = GetActivePackages();
  for (ApexFile& apex : packages) {
    if (apex.GetManifest().name() == packageName) {
      return std::move(apex);
    }
  }

  return ErrnoError() << "Cannot find matching package for: " << packageName;
}

/**
 * Abort individual staged session.
 *
 * Returns without error only if session was successfully aborted.
 **/
Result<void> AbortStagedSession(int session_id) {
  auto session = ApexSession::GetSession(session_id);
  if (!session.ok()) {
    return Error() << "No session found with id " << session_id;
  }
  switch (session->GetState()) {
    case SessionState::VERIFIED:
      [[clang::fallthrough]];
    case SessionState::STAGED:
      return session->DeleteSession();
    default:
      return Error() << "Session " << *session << " can't be aborted";
  }
}

namespace {

// TODO(b/179497746): Avoid scanning apex directly here
Result<std::vector<ApexFile>> ScanApexFiles(const char* apex_package_dir,
                                            bool include_compressed = false) {
  LOG(INFO) << "Scanning " << apex_package_dir << " looking for APEX packages.";
  if (access(apex_package_dir, F_OK) != 0 && errno == ENOENT) {
    LOG(INFO) << "... does not exist. Skipping";
    return {};
  }
  std::vector<std::string> suffix_list = {kApexPackageSuffix};
  if (include_compressed) {
    suffix_list.push_back(kCompressedApexPackageSuffix);
  }
  Result<std::vector<std::string>> scan =
      FindFilesBySuffix(apex_package_dir, suffix_list);
  if (!scan.ok()) {
    return Error() << "Failed to scan " << apex_package_dir << " : "
                   << scan.error();
  }
  std::vector<ApexFile> ret;
  for (const auto& name : *scan) {
    LOG(INFO) << "Found " << name;
    Result<ApexFile> apex_file = ApexFile::Open(name);
    if (!apex_file.ok()) {
      LOG(ERROR) << "Failed to scan " << name << " : " << apex_file.error();
    } else {
      ret.emplace_back(std::move(*apex_file));
    }
  }
  return ret;
}

std::vector<Result<void>> ActivateApexWorker(
    bool is_ota_chroot, std::queue<const ApexFile*>& apex_queue,
    std::mutex& mutex) {
  std::vector<Result<void>> ret;

  while (true) {
    const ApexFile* apex;
    {
      std::lock_guard lock(mutex);
      if (apex_queue.empty()) break;
      apex = apex_queue.front();
      apex_queue.pop();
    }

    std::string device_name = GetPackageId(apex->GetManifest());
    if (is_ota_chroot) {
      device_name += ".chroot";
    }
    if (auto res = ActivatePackageImpl(*apex, device_name); !res.ok()) {
      ret.push_back(Error() << "Failed to activate " << apex->GetPath() << " : "
                            << res.error());
    } else {
      ret.push_back({});
    }
  }

  return ret;
}

Result<void> ActivateApexPackages(
    const std::vector<std::reference_wrapper<const ApexFile>>& apexes,
    bool is_ota_chroot) {
  std::queue<const ApexFile*> apex_queue;
  std::mutex apex_queue_mutex;

  for (const ApexFile& apex : apexes) {
    apex_queue.emplace(&apex);
  }

  // Creates threads as many as half number of cores for the performance.
  size_t worker_num = std::max(get_nprocs_conf() >> 1, 1);
  worker_num = std::min(apex_queue.size(), worker_num);

  // On -eng builds there might be two different pre-installed art apexes.
  // Attempting to activate them in parallel will result in UB (e.g.
  // apexd-bootstrap might crash). In order to avoid this, for the time being on
  // -eng builds activate apexes sequentially.
  // TODO(b/176497601): remove this.
  if (GetProperty("ro.build.type", "") == "eng") {
    worker_num = 1;
  }

  std::vector<std::future<std::vector<Result<void>>>> futures;
  futures.reserve(worker_num);
  for (size_t i = 0; i < worker_num; i++) {
    futures.push_back(std::async(std::launch::async, ActivateApexWorker,
                                 std::ref(is_ota_chroot), std::ref(apex_queue),
                                 std::ref(apex_queue_mutex)));
  }

  size_t activated_cnt = 0;
  size_t failed_cnt = 0;
  for (size_t i = 0; i < futures.size(); i++) {
    for (const auto& res : futures[i].get()) {
      if (res.ok()) {
        ++activated_cnt;
      } else {
        ++failed_cnt;
        LOG(ERROR) << res.error();
      }
    }
  }

  if (failed_cnt > 0) {
    return Error() << "Failed to activate " << failed_cnt << " APEX packages";
  }
  LOG(INFO) << "Activated " << activated_cnt << " packages.";
  return {};
}

// A fallback function in case some of the apexes failed to activate. For all
// such apexes that were coming from /data partition we will attempt to activate
// their corresponding pre-installed copies.
Result<void> ActivateMissingApexes(
    const std::vector<std::reference_wrapper<const ApexFile>>& apexes,
    bool is_ota_chroot) {
  LOG(INFO) << "Trying to activate pre-installed versions of missing apexes";
  const auto& file_repository = ApexFileRepository::GetInstance();
  const auto& activated_apexes = GetActivePackagesMap();
  std::vector<std::reference_wrapper<const ApexFile>> fallback_apexes;
  for (const auto& apex_ref : apexes) {
    const auto& apex = apex_ref.get();
    if (apex.GetManifest().providesharedapexlibs()) {
      // We must mount both versions of sharedlibs apex anyway. Not much we can
      // do here.
      continue;
    }
    if (file_repository.IsPreInstalledApex(apex)) {
      // We tried to activate pre-installed apex in the first place. No need to
      // try again.
      continue;
    }
    const std::string& name = apex.GetManifest().name();
    if (activated_apexes.find(name) == activated_apexes.end()) {
      fallback_apexes.push_back(file_repository.GetPreInstalledApex(name));
    }
  }
  return ActivateApexPackages(fallback_apexes, is_ota_chroot);
}

}  // namespace

// TODO(b/179497746): Avoid scanning APEX here
Result<void> ScanPackagesDirAndActivate(const char* apex_package_dir) {
  auto apexes = ScanApexFiles(apex_package_dir);
  if (!apexes.ok()) {
    return apexes.error();
  }
  std::vector<std::reference_wrapper<const ApexFile>> apexes_ref;
  std::transform(apexes->begin(), apexes->end(), std::back_inserter(apexes_ref),
                 [](const auto& x) { return std::cref(x); });
  return ActivateApexPackages(apexes_ref, /* is_ota_chroot= */ false);
}

/**
 * Snapshots data from base_dir/apexdata/<apex name> to
 * base_dir/apexrollback/<rollback id>/<apex name>.
 */
Result<void> SnapshotDataDirectory(const std::string& base_dir,
                                   const int rollback_id,
                                   const std::string& apex_name,
                                   bool pre_restore = false) {
  auto rollback_path =
      StringPrintf("%s/%s/%d%s", base_dir.c_str(), kApexSnapshotSubDir,
                   rollback_id, pre_restore ? kPreRestoreSuffix : "");
  const Result<void> result = CreateDirIfNeeded(rollback_path, 0700);
  if (!result.ok()) {
    return Error() << "Failed to create snapshot directory for rollback "
                   << rollback_id << " : " << result.error();
  }
  auto from_path = StringPrintf("%s/%s/%s", base_dir.c_str(), kApexDataSubDir,
                                apex_name.c_str());
  auto to_path =
      StringPrintf("%s/%s", rollback_path.c_str(), apex_name.c_str());

  return ReplaceFiles(from_path, to_path);
}

/**
 * Restores snapshot from base_dir/apexrollback/<rollback id>/<apex name>
 * to base_dir/apexdata/<apex name>.
 * Note the snapshot will be deleted after restoration succeeded.
 */
Result<void> RestoreDataDirectory(const std::string& base_dir,
                                  const int rollback_id,
                                  const std::string& apex_name,
                                  bool pre_restore = false) {
  auto from_path = StringPrintf(
      "%s/%s/%d%s/%s", base_dir.c_str(), kApexSnapshotSubDir, rollback_id,
      pre_restore ? kPreRestoreSuffix : "", apex_name.c_str());
  auto to_path = StringPrintf("%s/%s/%s", base_dir.c_str(), kApexDataSubDir,
                              apex_name.c_str());
  Result<void> result = ReplaceFiles(from_path, to_path);
  if (!result.ok()) {
    return result;
  }
  result = RestoreconPath(to_path);
  if (!result.ok()) {
    return result;
  }
  result = DeleteDir(from_path);
  if (!result.ok()) {
    LOG(ERROR) << "Failed to delete the snapshot: " << result.error();
  }
  return {};
}

void SnapshotOrRestoreDeIfNeeded(const std::string& base_dir,
                                 const ApexSession& session) {
  if (session.HasRollbackEnabled()) {
    for (const auto& apex_name : session.GetApexNames()) {
      Result<void> result =
          SnapshotDataDirectory(base_dir, session.GetRollbackId(), apex_name);
      if (!result.ok()) {
        LOG(ERROR) << "Snapshot failed for " << apex_name << ": "
                   << result.error();
      }
    }
  } else if (session.IsRollback()) {
    for (const auto& apex_name : session.GetApexNames()) {
      if (!gInFsCheckpointMode) {
        // Snapshot before restore so this rollback can be reverted.
        SnapshotDataDirectory(base_dir, session.GetRollbackId(), apex_name,
                              true /* pre_restore */);
      }
      Result<void> result =
          RestoreDataDirectory(base_dir, session.GetRollbackId(), apex_name);
      if (!result.ok()) {
        LOG(ERROR) << "Restore of data failed for " << apex_name << ": "
                   << result.error();
      }
    }
  }
}

void SnapshotOrRestoreDeSysData() {
  auto sessions = ApexSession::GetSessionsInState(SessionState::ACTIVATED);

  for (const ApexSession& session : sessions) {
    SnapshotOrRestoreDeIfNeeded(kDeSysDataDir, session);
  }
}

int SnapshotOrRestoreDeUserData() {
  auto user_dirs = GetDeUserDirs();

  if (!user_dirs.ok()) {
    LOG(ERROR) << "Error reading dirs " << user_dirs.error();
    return 1;
  }

  auto sessions = ApexSession::GetSessionsInState(SessionState::ACTIVATED);

  for (const ApexSession& session : sessions) {
    for (const auto& user_dir : *user_dirs) {
      SnapshotOrRestoreDeIfNeeded(user_dir, session);
    }
  }

  return 0;
}

Result<void> SnapshotCeData(const int user_id, const int rollback_id,
                            const std::string& apex_name) {
  auto base_dir = StringPrintf("%s/%d", kCeDataDir, user_id);
  return SnapshotDataDirectory(base_dir, rollback_id, apex_name);
}

Result<void> RestoreCeData(const int user_id, const int rollback_id,
                           const std::string& apex_name) {
  auto base_dir = StringPrintf("%s/%d", kCeDataDir, user_id);
  return RestoreDataDirectory(base_dir, rollback_id, apex_name);
}

//  Migrates sessions directory from /data/apex/sessions to
//  /metadata/apex/sessions, if necessary.
Result<void> MigrateSessionsDirIfNeeded() {
  return ApexSession::MigrateToMetadataSessionsDir();
}

Result<void> DestroySnapshots(const std::string& base_dir,
                              const int rollback_id) {
  auto path = StringPrintf("%s/%s/%d", base_dir.c_str(), kApexSnapshotSubDir,
                           rollback_id);
  return DeleteDir(path);
}

Result<void> DestroyDeSnapshots(const int rollback_id) {
  DestroySnapshots(kDeSysDataDir, rollback_id);

  auto user_dirs = GetDeUserDirs();
  if (!user_dirs.ok()) {
    return Error() << "Error reading user dirs " << user_dirs.error();
  }

  for (const auto& user_dir : *user_dirs) {
    DestroySnapshots(user_dir, rollback_id);
  }

  return {};
}

Result<void> DestroyCeSnapshots(const int user_id, const int rollback_id) {
  auto path = StringPrintf("%s/%d/%s/%d", kCeDataDir, user_id,
                           kApexSnapshotSubDir, rollback_id);
  return DeleteDir(path);
}

/**
 * Deletes all credential-encrypted snapshots for the given user, except for
 * those listed in retain_rollback_ids.
 */
Result<void> DestroyCeSnapshotsNotSpecified(
    int user_id, const std::vector<int>& retain_rollback_ids) {
  auto snapshot_root =
      StringPrintf("%s/%d/%s", kCeDataDir, user_id, kApexSnapshotSubDir);
  auto snapshot_dirs = GetSubdirs(snapshot_root);
  if (!snapshot_dirs.ok()) {
    return Error() << "Error reading snapshot dirs " << snapshot_dirs.error();
  }

  for (const auto& snapshot_dir : *snapshot_dirs) {
    uint snapshot_id;
    bool parse_ok = ParseUint(
        std::filesystem::path(snapshot_dir).filename().c_str(), &snapshot_id);
    if (parse_ok &&
        std::find(retain_rollback_ids.begin(), retain_rollback_ids.end(),
                  snapshot_id) == retain_rollback_ids.end()) {
      Result<void> result = DeleteDir(snapshot_dir);
      if (!result.ok()) {
        return Error() << "Destroy CE snapshot failed for " << snapshot_dir
                       << " : " << result.error();
      }
    }
  }
  return {};
}

void RestorePreRestoreSnapshotsIfPresent(const std::string& base_dir,
                                         const ApexSession& session) {
  auto pre_restore_snapshot_path =
      StringPrintf("%s/%s/%d%s", base_dir.c_str(), kApexSnapshotSubDir,
                   session.GetRollbackId(), kPreRestoreSuffix);
  if (PathExists(pre_restore_snapshot_path).ok()) {
    for (const auto& apex_name : session.GetApexNames()) {
      Result<void> result = RestoreDataDirectory(
          base_dir, session.GetRollbackId(), apex_name, true /* pre_restore */);
      if (!result.ok()) {
        LOG(ERROR) << "Restore of pre-restore snapshot failed for " << apex_name
                   << ": " << result.error();
      }
    }
  }
}

void RestoreDePreRestoreSnapshotsIfPresent(const ApexSession& session) {
  RestorePreRestoreSnapshotsIfPresent(kDeSysDataDir, session);

  auto user_dirs = GetDeUserDirs();
  if (!user_dirs.ok()) {
    LOG(ERROR) << "Error reading user dirs to restore pre-restore snapshots"
               << user_dirs.error();
  }

  for (const auto& user_dir : *user_dirs) {
    RestorePreRestoreSnapshotsIfPresent(user_dir, session);
  }
}

void DeleteDePreRestoreSnapshots(const std::string& base_dir,
                                 const ApexSession& session) {
  auto pre_restore_snapshot_path =
      StringPrintf("%s/%s/%d%s", base_dir.c_str(), kApexSnapshotSubDir,
                   session.GetRollbackId(), kPreRestoreSuffix);
  Result<void> result = DeleteDir(pre_restore_snapshot_path);
  if (!result.ok()) {
    LOG(ERROR) << "Deletion of pre-restore snapshot failed: " << result.error();
  }
}

void DeleteDePreRestoreSnapshots(const ApexSession& session) {
  DeleteDePreRestoreSnapshots(kDeSysDataDir, session);

  auto user_dirs = GetDeUserDirs();
  if (!user_dirs.ok()) {
    LOG(ERROR) << "Error reading user dirs to delete pre-restore snapshots"
               << user_dirs.error();
  }

  for (const auto& user_dir : *user_dirs) {
    DeleteDePreRestoreSnapshots(user_dir, session);
  }
}

void OnBootCompleted() {
  ApexdLifecycle::GetInstance().MarkBootCompleted();
  BootCompletedCleanup();
}

// Returns true if any session gets staged
void ScanStagedSessionsDirAndStage() {
  LOG(INFO) << "Scanning " << ApexSession::GetSessionsDir()
            << " looking for sessions to be activated.";

  auto sessions_to_activate =
      ApexSession::GetSessionsInState(SessionState::STAGED);
  if (gSupportsFsCheckpoints) {
    // A session that is in the ACTIVATED state should still be re-activated if
    // fs checkpointing is supported. In this case, a session may be in the
    // ACTIVATED state yet the data/apex/active directory may have been
    // reverted. The session should be reverted in this scenario.
    auto activated_sessions =
        ApexSession::GetSessionsInState(SessionState::ACTIVATED);
    sessions_to_activate.insert(sessions_to_activate.end(),
                                activated_sessions.begin(),
                                activated_sessions.end());
  }

  for (auto& session : sessions_to_activate) {
    auto session_id = session.GetId();

    auto session_failed_fn = [&]() {
      LOG(WARNING) << "Marking session " << session_id << " as failed.";
      auto st = session.UpdateStateAndCommit(SessionState::ACTIVATION_FAILED);
      if (!st.ok()) {
        LOG(WARNING) << "Failed to mark session " << session_id
                     << " as failed : " << st.error();
      }
    };
    auto scope_guard = android::base::make_scope_guard(session_failed_fn);

    std::string build_fingerprint = GetProperty(kBuildFingerprintSysprop, "");
    if (session.GetBuildFingerprint().compare(build_fingerprint) != 0) {
      LOG(ERROR) << "APEX build fingerprint has changed";
      continue;
    }

    std::vector<std::string> dirs_to_scan;
    if (session.GetChildSessionIds().empty()) {
      dirs_to_scan.push_back(std::string(kStagedSessionsDir) + "/session_" +
                             std::to_string(session_id));
    } else {
      for (auto child_session_id : session.GetChildSessionIds()) {
        dirs_to_scan.push_back(std::string(kStagedSessionsDir) + "/session_" +
                               std::to_string(child_session_id));
      }
    }

    std::vector<std::string> apexes;
    bool scan_successful = true;
    for (const auto& dir_to_scan : dirs_to_scan) {
      Result<std::vector<std::string>> scan =
          FindFilesBySuffix(dir_to_scan, {kApexPackageSuffix});
      if (!scan.ok()) {
        LOG(WARNING) << scan.error();
        scan_successful = false;
        break;
      }

      if (scan->size() > 1) {
        LOG(WARNING) << "More than one APEX package found in the same session "
                     << "directory " << dir_to_scan << ", skipping activation.";
        scan_successful = false;
        break;
      }

      if (scan->empty()) {
        LOG(WARNING) << "No APEX packages found while scanning " << dir_to_scan
                     << " session id: " << session_id << ".";
        scan_successful = false;
        break;
      }
      apexes.push_back(std::move((*scan)[0]));
    }

    if (!scan_successful) {
      continue;
    }

    // Run postinstall, if necessary.
    Result<void> postinstall_status = PostinstallPackages(apexes);
    if (!postinstall_status.ok()) {
      LOG(ERROR) << "Postinstall failed for session "
                 << std::to_string(session_id) << ": "
                 << postinstall_status.error();
      continue;
    }

    for (const auto& apex : apexes) {
      // TODO(b/158470836): Avoid opening ApexFile repeatedly.
      Result<ApexFile> apex_file = ApexFile::Open(apex);
      if (!apex_file.ok()) {
        LOG(ERROR) << "Cannot open apex file during staging: " << apex;
        continue;
      }
      session.AddApexName(apex_file->GetManifest().name());
    }

    const Result<void> result = StagePackages(apexes);
    if (!result.ok()) {
      LOG(ERROR) << "Activation failed for packages " << Join(apexes, ',')
                 << ": " << result.error();
      continue;
    }

    // Session was OK, release scopeguard.
    scope_guard.Disable();

    auto st = session.UpdateStateAndCommit(SessionState::ACTIVATED);
    if (!st.ok()) {
      LOG(ERROR) << "Failed to mark " << session
                 << " as activated : " << st.error();
    }
  }
}

Result<void> PreinstallPackages(const std::vector<std::string>& paths) {
  Result<std::vector<ApexFile>> apex_files = OpenApexFiles(paths);
  if (!apex_files.ok()) {
    return apex_files.error();
  }
  LOG(DEBUG) << "PreinstallPackages() for " << Join(paths, ',');
  return PreinstallPackages(*apex_files);
}

Result<void> PostinstallPackages(const std::vector<std::string>& paths) {
  Result<std::vector<ApexFile>> apex_files = OpenApexFiles(paths);
  if (!apex_files.ok()) {
    return apex_files.error();
  }
  LOG(DEBUG) << "PostinstallPackages() for " << Join(paths, ',');
  return PostinstallPackages(*apex_files);
}

namespace {
std::string StageDestPath(const ApexFile& apex_file) {
  return StringPrintf("%s/%s%s", kActiveApexPackagesDataDir,
                      GetPackageId(apex_file.GetManifest()).c_str(),
                      kApexPackageSuffix);
}

}  // namespace

Result<void> StagePackages(const std::vector<std::string>& tmp_paths) {
  if (tmp_paths.empty()) {
    return Errorf("Empty set of inputs");
  }
  LOG(DEBUG) << "StagePackages() for " << Join(tmp_paths, ',');

  // Note: this function is temporary. As such the code is not optimized, e.g.,
  //       it will open ApexFiles multiple times.

  // 1) Verify all packages.
  auto verify_status = VerifyPackages(tmp_paths, VerifyPackageBoot);
  if (!verify_status.ok()) {
    return verify_status.error();
  }

  // Make sure that kActiveApexPackagesDataDir exists.
  auto create_dir_status =
      CreateDirIfNeeded(std::string(kActiveApexPackagesDataDir), 0755);
  if (!create_dir_status.ok()) {
    return create_dir_status.error();
  }

  // 2) Now stage all of them.

  // Ensure the APEX gets removed on failure.
  std::unordered_set<std::string> staged_files;
  std::vector<std::string> changed_hashtree_files;
  auto deleter = [&staged_files, &changed_hashtree_files]() {
    for (const std::string& staged_path : staged_files) {
      if (TEMP_FAILURE_RETRY(unlink(staged_path.c_str())) != 0) {
        PLOG(ERROR) << "Unable to unlink " << staged_path;
      }
    }
    for (const std::string& hashtree_file : changed_hashtree_files) {
      if (TEMP_FAILURE_RETRY(unlink(hashtree_file.c_str())) != 0) {
        PLOG(ERROR) << "Unable to unlink " << hashtree_file;
      }
    }
  };
  auto scope_guard = android::base::make_scope_guard(deleter);

  std::unordered_set<std::string> staged_packages;
  for (const std::string& path : tmp_paths) {
    Result<ApexFile> apex_file = ApexFile::Open(path);
    if (!apex_file.ok()) {
      return apex_file.error();
    }
    // First promote new hashtree file to the one that will be used when
    // mounting apex.
    std::string new_hashtree_file = GetHashTreeFileName(*apex_file,
                                                        /* is_new = */ true);
    std::string old_hashtree_file = GetHashTreeFileName(*apex_file,
                                                        /* is_new = */ false);
    if (access(new_hashtree_file.c_str(), F_OK) == 0) {
      if (TEMP_FAILURE_RETRY(rename(new_hashtree_file.c_str(),
                                    old_hashtree_file.c_str())) != 0) {
        return ErrnoError() << "Failed to move " << new_hashtree_file << " to "
                            << old_hashtree_file;
      }
      changed_hashtree_files.emplace_back(std::move(old_hashtree_file));
    }
    // And only then move apex to /data/apex/active.
    std::string dest_path = StageDestPath(*apex_file);
    if (access(dest_path.c_str(), F_OK) == 0) {
      LOG(DEBUG) << dest_path << " already exists. Deleting";
      if (TEMP_FAILURE_RETRY(unlink(dest_path.c_str())) != 0) {
        return ErrnoError() << "Failed to unlink " << dest_path;
      }
    }

    if (link(apex_file->GetPath().c_str(), dest_path.c_str()) != 0) {
      return ErrnoError() << "Unable to link " << apex_file->GetPath() << " to "
                          << dest_path;
    }
    staged_files.insert(dest_path);
    staged_packages.insert(apex_file->GetManifest().name());

    LOG(DEBUG) << "Success linking " << apex_file->GetPath() << " to "
               << dest_path;
  }

  scope_guard.Disable();  // Accept the state.

  return RemovePreviouslyActiveApexFiles(staged_packages, staged_files);
}

Result<void> UnstagePackages(const std::vector<std::string>& paths) {
  if (paths.empty()) {
    return Errorf("Empty set of inputs");
  }
  LOG(DEBUG) << "UnstagePackages() for " << Join(paths, ',');

  for (const std::string& path : paths) {
    auto apex = ApexFile::Open(path);
    if (!apex.ok()) {
      return apex.error();
    }
    if (ApexFileRepository::GetInstance().IsPreInstalledApex(*apex)) {
      return Error() << "Can't uninstall pre-installed apex " << path;
    }
  }

  for (const std::string& path : paths) {
    if (unlink(path.c_str()) != 0) {
      return ErrnoError() << "Can't unlink " << path;
    }
  }

  return {};
}

/**
 * During apex installation, staged sessions located in /data/apex/sessions
 * mutate the active sessions in /data/apex/active. If some error occurs during
 * installation of apex, we need to revert /data/apex/active to its original
 * state and reboot.
 *
 * Also, we need to put staged sessions in /data/apex/sessions in REVERTED state
 * so that they do not get activated on next reboot.
 */
Result<void> RevertActiveSessions(const std::string& crashing_native_process) {
  // First check whenever there is anything to revert. If there is none, then
  // fail. This prevents apexd from boot looping a device in case a native
  // process is crashing and there are no apex updates.
  auto active_sessions = ApexSession::GetActiveSessions();
  if (active_sessions.empty()) {
    return Error() << "Revert requested, when there are no active sessions.";
  }

  for (auto& session : active_sessions) {
    if (!crashing_native_process.empty()) {
      session.SetCrashingNativeProcess(crashing_native_process);
    }
    auto status =
        session.UpdateStateAndCommit(SessionState::REVERT_IN_PROGRESS);
    if (!status.ok()) {
      return Error() << "Revert of session " << session
                     << " failed : " << status.error();
    }
  }

  if (!gInFsCheckpointMode) {
    auto restore_status = RestoreActivePackages();
    if (!restore_status.ok()) {
      for (auto& session : active_sessions) {
        auto st = session.UpdateStateAndCommit(SessionState::REVERT_FAILED);
        LOG(DEBUG) << "Marking " << session << " as failed to revert";
        if (!st.ok()) {
          LOG(WARNING) << "Failed to mark session " << session
                       << " as failed to revert : " << st.error();
        }
      }
      return restore_status;
    }
  } else {
    LOG(INFO) << "Not restoring active packages in checkpoint mode.";
  }

  for (auto& session : active_sessions) {
    if (!gInFsCheckpointMode && session.IsRollback()) {
      // If snapshots have already been restored, undo that by restoring the
      // pre-restore snapshot.
      RestoreDePreRestoreSnapshotsIfPresent(session);
    }

    auto status = session.UpdateStateAndCommit(SessionState::REVERTED);
    if (!status.ok()) {
      LOG(WARNING) << "Failed to mark session " << session
                   << " as reverted : " << status.error();
    }
  }

  return {};
}

Result<void> RevertActiveSessionsAndReboot(
    const std::string& crashing_native_process) {
  auto status = RevertActiveSessions(crashing_native_process);
  if (!status.ok()) {
    return status;
  }
  LOG(ERROR) << "Successfully reverted. Time to reboot device.";
  if (gInFsCheckpointMode) {
    Result<void> res = gVoldService->AbortChanges(
        "apexd_initiated" /* message */, false /* retry */);
    if (!res.ok()) {
      LOG(ERROR) << res.error();
    }
  }
  Reboot();
  return {};
}

Result<void> CreateSharedLibsApexDir() {
  // Creates /apex/sharedlibs/lib{,64} for SharedLibs APEXes.
  std::string shared_libs_sub_dir =
      StringPrintf("%s/%s", kApexRoot, kApexSharedLibsSubDir);
  auto dir_exists = PathExists(shared_libs_sub_dir);
  if (!dir_exists.ok() || !*dir_exists) {
    std::error_code error_code;
    std::filesystem::create_directory(shared_libs_sub_dir, error_code);
    if (error_code) {
      return Error() << "Failed to create directory " << shared_libs_sub_dir
                     << ": " << error_code.message();
    }
  }
  for (const auto& lib_path : {"lib", "lib64"}) {
    std::string apex_lib_path =
        StringPrintf("%s/%s", shared_libs_sub_dir.c_str(), lib_path);
    auto lib_dir_exists = PathExists(apex_lib_path);
    if (!lib_dir_exists.ok() || !*lib_dir_exists) {
      std::error_code error_code;
      std::filesystem::create_directory(apex_lib_path, error_code);
      if (error_code) {
        return Error() << "Failed to create directory " << apex_lib_path << ": "
                       << error_code.message();
      }
    }
  }

  return {};
}

int OnBootstrap() {
  Result<void> pre_allocate = PreAllocateLoopDevices();
  if (!pre_allocate.ok()) {
    LOG(ERROR) << "Failed to pre-allocate loop devices : "
               << pre_allocate.error();
  }

  ApexFileRepository& instance = ApexFileRepository::GetInstance();
  static const std::vector<std::string> kBootstrapApexDirs{
      kApexPackageSystemDir, kApexPackageSystemExtDir, kApexPackageVendorDir};
  Result<void> status = instance.AddPreInstalledApex(kBootstrapApexDirs);
  if (!status.ok()) {
    LOG(ERROR) << "Failed to collect APEX keys : " << status.error();
    return 1;
  }

  // Create directories for APEX shared libraries.
  auto sharedlibs_apex_dir = CreateSharedLibsApexDir();
  if (!sharedlibs_apex_dir.ok()) {
    LOG(ERROR) << sharedlibs_apex_dir.error();
    return 1;
  }

  // Find all bootstrap apexes
  std::vector<ApexFile> bootstrap_apexes;
  for (const auto& dir : kBootstrapApexDirs) {
    auto scan = ScanApexFiles(dir.c_str());
    if (!scan.ok()) {
      LOG(ERROR) << "Failed to scan APEX files in " << dir << " : "
                 << scan.error();
      return 1;
    }
    std::copy_if(std::make_move_iterator(scan->begin()),
                 std::make_move_iterator(scan->end()),
                 std::back_inserter(bootstrap_apexes), IsBootstrapApex);
  }

  // Now activate bootstrap apexes.
  std::vector<std::reference_wrapper<const ApexFile>> bootstrap_apexes_ref;
  std::transform(bootstrap_apexes.begin(), bootstrap_apexes.end(),
                 std::back_inserter(bootstrap_apexes_ref),
                 [](const auto& x) { return std::cref(x); });
  auto ret = ActivateApexPackages(bootstrap_apexes_ref,
                                  /* is_ota_chroot= */ false);
  if (!ret.ok()) {
    LOG(ERROR) << "Failed to activate bootstrap apex files : " << ret.error();
    return 1;
  }

  OnAllPackagesActivated(/*is_bootstrap=*/true);
  LOG(INFO) << "Bootstrapping done";
  return 0;
}

Result<void> RemountApexFile(const std::string& path) {
  if (auto ret = DeactivatePackage(path); !ret.ok()) {
    return ret;
  }
  return ActivatePackage(path);
}

void InitializeVold(CheckpointInterface* checkpoint_service) {
  if (checkpoint_service != nullptr) {
    gVoldService = checkpoint_service;
    Result<bool> supports_fs_checkpoints =
        gVoldService->SupportsFsCheckpoints();
    if (supports_fs_checkpoints.ok()) {
      gSupportsFsCheckpoints = *supports_fs_checkpoints;
    } else {
      LOG(ERROR) << "Failed to check if filesystem checkpoints are supported: "
                 << supports_fs_checkpoints.error();
    }
    if (gSupportsFsCheckpoints) {
      Result<bool> needs_checkpoint = gVoldService->NeedsCheckpoint();
      if (needs_checkpoint.ok()) {
        gInFsCheckpointMode = *needs_checkpoint;
      } else {
        LOG(ERROR) << "Failed to check if we're in filesystem checkpoint mode: "
                   << needs_checkpoint.error();
      }
    }
  }
}

void Initialize(CheckpointInterface* checkpoint_service) {
  InitializeVold(checkpoint_service);
  ApexFileRepository& instance = ApexFileRepository::GetInstance();
  Result<void> status = instance.AddPreInstalledApex(kApexPackageBuiltinDirs);
  if (!status.ok()) {
    LOG(ERROR) << "Failed to collect pre-installed APEX files : "
               << status.error();
    return;
  }
  gMountedApexes.PopulateFromMounts();
}

// Note: Pre-installed apex are initialized in Initialize(CheckpointInterface*)
// TODO(b/172911822): Consolidate this with Initialize() when
//  ApexFileRepository can act as cache and re-scanning is not expensive
void InitializeDataApex() {
  ApexFileRepository& instance = ApexFileRepository::GetInstance();
  Result<void> status = instance.AddDataApex(kActiveApexPackagesDataDir);
  if (!status.ok()) {
    LOG(ERROR) << "Failed to collect data APEX files : " << status.error();
    return;
  }
}

/**
 * For every package X, there can be at most two APEX, pre-installed vs
 * installed on data. We usually select only one of these APEX for each package
 * based on the following conditions:
 *   - Package X must be pre-installed on one of the built-in directories.
 *   - If there are multiple APEX, we select the one with highest version.
 *   - If there are multiple with same version, we give priority to APEX on
 * /data partition.
 *
 * Typically, only one APEX is activated for each package, but APEX that provide
 * shared libs are exceptions. We have to activate both APEX for them.
 *
 * @param all_apex all the APEX grouped by their package name
 * @return list of ApexFile that needs to be activated
 */
std::vector<std::reference_wrapper<const ApexFile>> SelectApexForActivation(
    const std::unordered_map<
        std::string, std::vector<std::reference_wrapper<const ApexFile>>>&
        all_apex,
    const ApexFileRepository& instance) {
  LOG(INFO) << "Selecting APEX for activation";
  std::vector<std::reference_wrapper<const ApexFile>> activation_list;
  // For every package X, select which APEX to activate
  for (auto& apex_it : all_apex) {
    const std::string& package_name = apex_it.first;
    const std::vector<std::reference_wrapper<const ApexFile>>& apex_files =
        apex_it.second;

    if (apex_files.size() > 2 || apex_files.size() == 0) {
      LOG(FATAL) << "Unexpectedly found more than two versions or none for "
                    "APEX package "
                 << package_name;
      continue;
    }

    // The package must have a pre-installed version before we consider it for
    // activation
    if (!instance.HasPreInstalledVersion(package_name)) {
      LOG(INFO) << "Package " << package_name << " is not pre-installed";
      continue;
    }

    if (apex_files.size() == 1) {
      LOG(DEBUG) << "Selecting the only APEX: " << package_name << " "
                 << apex_files[0].get().GetPath();
      activation_list.emplace_back(apex_files[0]);
      continue;
    }

    // TODO(b/179497746): Now that we are dealing with list of reference, this
    //  selection process can be simplified by sorting the vector.

    // Given an APEX A and the version of the other APEX B, should we activate
    // it?
    auto select_apex = [&instance, &activation_list](
                           const std::reference_wrapper<const ApexFile>& a_ref,
                           const int version_b) mutable {
      const ApexFile& a = a_ref.get();
      // APEX that provides shared library always gets activated
      const bool provides_shared_apex_libs =
          a.GetManifest().providesharedapexlibs();
      // If A has higher version than B, then it should be activated
      const bool higher_version = a.GetManifest().version() > version_b;
      // If A has same version as B, then data version should get activated
      const bool same_version_priority_to_data =
          a.GetManifest().version() == version_b &&
          !instance.IsPreInstalledApex(a);
      // If A has same version as B and they are both pre-installed,
      // then it means one of them is compressed. Choose decompressed copy.
      const bool decompressed = instance.IsDecompressedApex(a);
      if (provides_shared_apex_libs || higher_version ||
          same_version_priority_to_data || decompressed) {
        LOG(DEBUG) << "Selecting between two APEX: " << a.GetManifest().name()
                   << " " << a.GetPath();
        activation_list.emplace_back(a_ref);
      }
    };
    const int version_0 = apex_files[0].get().GetManifest().version();
    const int version_1 = apex_files[1].get().GetManifest().version();
    select_apex(apex_files[0].get(), version_1);
    select_apex(apex_files[1].get(), version_0);
  }
  return activation_list;
}

/**
 * For each compressed APEX, decompress it to kApexDecompressedDir and hard
 * link it to kActiveApexPackagesDataDir. Returns list of decompressed APEX.
 */
std::vector<ApexFile> ProcessCompressedApex(
    const std::vector<std::reference_wrapper<const ApexFile>>& compressed_apex,
    const std::string& decompression_dir, const std::string& active_apex_dir) {
  LOG(INFO) << "Processing compressed APEX";

  // Clean up reserved space before decompressing capex
  if (auto ret = DeleteDirContent(kOtaReservedDir); !ret.ok()) {
    LOG(ERROR) << "Failed to clean up reserved space: " << ret.error();
  }

  std::vector<ApexFile> decompressed_apex_list;
  for (const ApexFile& apex_file : compressed_apex) {
    if (!apex_file.IsCompressed()) {
      continue;
    }

    // Files to clean up if processing fails for any reason
    std::vector<std::string> cleanup;
    auto scope_gaurd = android::base::make_scope_guard([&cleanup] {
      for (const auto& file_path : cleanup) {
        RemoveFileIfExists(file_path);
      }
    });

    // Decompress them to kApexDecompressedDir
    std::string dest_path_decompressed = StringPrintf(
        "%s/%s%s", decompression_dir.c_str(),
        GetPackageId(apex_file.GetManifest()).c_str(), kApexPackageSuffix);
    cleanup.push_back(dest_path_decompressed);
    auto result = apex_file.Decompress(dest_path_decompressed);
    if (!result.ok()) {
      LOG(ERROR) << "Failed to decompress : " << apex_file.GetPath().c_str()
                 << " " << result.error();
      continue;
    }

    // Fix label of decompressed file
    auto restore = RestoreconPath(dest_path_decompressed);
    if (!restore.ok()) {
      LOG(ERROR) << restore.error();
      continue;
    }

    // Hardlink to kActiveApexPackagesDataDir so that they get activated on
    // reboot
    const auto& dest_path_active = StringPrintf(
        "%s/%s%s", active_apex_dir.c_str(),
        GetPackageId(apex_file.GetManifest()).c_str(), kApexPackageSuffix);
    cleanup.push_back(dest_path_active);
    if (link(dest_path_decompressed.c_str(), dest_path_active.c_str()) != 0) {
      LOG(ERROR) << "Failed to link decompressed APEX " << apex_file.GetPath();
      continue;
    }

    // Post decompression verification
    auto hardlinked_apex = ApexFile::Open(dest_path_active);
    if (!hardlinked_apex.ok()) {
      LOG(ERROR) << "Failed to open hard-linked APEX : " << dest_path_active
                 << hardlinked_apex.error();
      continue;
    }
    if (apex_file.GetBundledPublicKey() !=
        hardlinked_apex->GetBundledPublicKey()) {
      LOG(ERROR) << "Public key of compressed APEX is different than original "
                    "APEX for "
                 << hardlinked_apex->GetPath();
      continue;
    }

    // Decompressed APEX has been successfully processed. Accept it.
    scope_gaurd.Disable();
    decompressed_apex_list.emplace_back(std::move(*hardlinked_apex));
  }
  return std::move(decompressed_apex_list);
}

void OnStart() {
  LOG(INFO) << "Marking APEXd as starting";
  if (!SetProperty(gConfig->apex_status_sysprop, kApexStatusStarting)) {
    PLOG(ERROR) << "Failed to set " << gConfig->apex_status_sysprop << " to "
                << kApexStatusStarting;
  }

  // Ask whether we should revert any active sessions; this can happen if
  // we've exceeded the retry count on a device that supports filesystem
  // checkpointing.
  if (gSupportsFsCheckpoints) {
    Result<bool> needs_revert = gVoldService->NeedsRollback();
    if (!needs_revert.ok()) {
      LOG(ERROR) << "Failed to check if we need a revert: "
                 << needs_revert.error();
    } else if (*needs_revert) {
      LOG(INFO) << "Exceeded number of session retries ("
                << kNumRetriesWhenCheckpointingEnabled
                << "). Starting a revert";
      RevertActiveSessions("");
    }
  }

  // Create directories for APEX shared libraries.
  auto sharedlibs_apex_dir = CreateSharedLibsApexDir();
  if (!sharedlibs_apex_dir.ok()) {
    LOG(ERROR) << sharedlibs_apex_dir.error();
  }

  // If there is any new apex to be installed on /data/app-staging, hardlink
  // them to /data/apex/active first.
  ScanStagedSessionsDirAndStage();
  if (auto status = ApexFileRepository::GetInstance().AddDataApex(
          gConfig->active_apex_data_dir);
      !status.ok()) {
    LOG(ERROR) << "Failed to collect data APEX files : " << status.error();
  }

  auto status = ResumeRevertIfNeeded();
  if (!status.ok()) {
    LOG(ERROR) << "Failed to resume revert : " << status.error();
  }

  // Group every ApexFile on device by name
  const auto& instance = ApexFileRepository::GetInstance();
  const auto& all_apex = instance.AllApexFilesByName();
  // There can be multiple APEX packages with package name X. Determine which
  // one to activate.
  auto activation_list = SelectApexForActivation(all_apex, instance);

  // Process compressed APEX, if any
  std::vector<std::reference_wrapper<const ApexFile>> compressed_apex;
  for (auto it = activation_list.begin(); it != activation_list.end();) {
    if (it->get().IsCompressed()) {
      compressed_apex.emplace_back(*it);
      it = activation_list.erase(it);
    } else {
      it++;
    }
  }
  std::vector<ApexFile> decompressed_apex;
  if (!compressed_apex.empty()) {
    decompressed_apex = ProcessCompressedApex(compressed_apex);
    for (const ApexFile& apex_file : decompressed_apex) {
      activation_list.emplace_back(std::cref(apex_file));
    }
  }

  int data_apex_cnt = std::count_if(
      activation_list.begin(), activation_list.end(), [](const auto& a) {
        return !ApexFileRepository::GetInstance().IsPreInstalledApex(a.get());
      });
  if (data_apex_cnt > 0) {
    Result<void> pre_allocate = loop::PreAllocateLoopDevices(data_apex_cnt);
    if (!pre_allocate.ok()) {
      LOG(ERROR) << "Failed to pre-allocate loop devices : "
                 << pre_allocate.error();
    }
  }

  // TODO(b/179248390): activate parallelly if possible
  auto activate_status =
      ActivateApexPackages(activation_list, /* is_ota_chroot= */ false);
  if (!activate_status.ok()) {
    LOG(ERROR) << "Failed to activate packages: " << activate_status.error();
    Result<void> revert_status = RevertActiveSessionsAndReboot("");
    if (!revert_status.ok()) {
      LOG(ERROR) << "Failed to revert : " << revert_status.error();
    }
    auto retry_status = ActivateMissingApexes(activation_list,
                                              /* is_ota_chroot= */ false);
    if (!retry_status.ok()) {
      LOG(ERROR) << retry_status.error();
    }
  }

  // Now that APEXes are mounted, snapshot or restore DE_sys data.
  SnapshotOrRestoreDeSysData();
}

void OnAllPackagesActivated(bool is_bootstrap) {
  auto result = EmitApexInfoList(is_bootstrap);
  if (!result.ok()) {
    LOG(ERROR) << "cannot emit apex info list: " << result.error();
  }

  // Because apexd in bootstrap mode runs in blocking mode
  // we don't have to set as activated.
  if (is_bootstrap) {
    return;
  }

  // Set a system property to let other components know that APEXs are
  // activated, but are not yet ready to be used. init is expected to wait
  // for this status before performing configuration based on activated
  // apexes. Other components that need to use APEXs should wait for the
  // ready state instead.
  LOG(INFO) << "Marking APEXd as activated";
  if (!SetProperty(gConfig->apex_status_sysprop, kApexStatusActivated)) {
    PLOG(ERROR) << "Failed to set " << gConfig->apex_status_sysprop << " to "
                << kApexStatusActivated;
  }
}

void OnAllPackagesReady() {
  // Set a system property to let other components know that APEXs are
  // correctly mounted and ready to be used. Before using any file from APEXs,
  // they can query this system property to ensure that they are okay to
  // access. Or they may have a on-property trigger to delay a task until
  // APEXs become ready.
  LOG(INFO) << "Marking APEXd as ready";
  if (!SetProperty(gConfig->apex_status_sysprop, kApexStatusReady)) {
    PLOG(ERROR) << "Failed to set " << gConfig->apex_status_sysprop << " to "
                << kApexStatusReady;
  }
}

Result<std::vector<ApexFile>> SubmitStagedSession(
    const int session_id, const std::vector<int>& child_session_ids,
    const bool has_rollback_enabled, const bool is_rollback,
    const int rollback_id) {
  if (session_id == 0) {
    return Error() << "Session id was not provided.";
  }

  if (!gSupportsFsCheckpoints) {
    Result<void> backup_status = BackupActivePackages();
    if (!backup_status.ok()) {
      // Do not proceed with staged install without backup
      return backup_status.error();
    }
  }

  std::vector<int> ids_to_scan;
  if (!child_session_ids.empty()) {
    ids_to_scan = child_session_ids;
  } else {
    ids_to_scan = {session_id};
  }

  std::vector<ApexFile> ret;
  for (int id_to_scan : ids_to_scan) {
    auto verified = VerifySessionDir(id_to_scan);
    if (!verified.ok()) {
      return verified.error();
    }
    ret.push_back(std::move(*verified));
  }

  // Run preinstall, if necessary.
  Result<void> preinstall_status = PreinstallPackages(ret);
  if (!preinstall_status.ok()) {
    return preinstall_status.error();
  }

  if (has_rollback_enabled && is_rollback) {
    return Error() << "Cannot set session " << session_id << " as both a"
                   << " rollback and enabled for rollback.";
  }

  auto session = ApexSession::CreateSession(session_id);
  if (!session.ok()) {
    return session.error();
  }
  (*session).SetChildSessionIds(child_session_ids);
  std::string build_fingerprint = GetProperty(kBuildFingerprintSysprop, "");
  (*session).SetBuildFingerprint(build_fingerprint);
  session->SetHasRollbackEnabled(has_rollback_enabled);
  session->SetIsRollback(is_rollback);
  session->SetRollbackId(rollback_id);
  Result<void> commit_status =
      (*session).UpdateStateAndCommit(SessionState::VERIFIED);
  if (!commit_status.ok()) {
    return commit_status.error();
  }

  return ret;
}

Result<void> MarkStagedSessionReady(const int session_id) {
  auto session = ApexSession::GetSession(session_id);
  if (!session.ok()) {
    return session.error();
  }
  // We should only accept sessions in SessionState::VERIFIED or
  // SessionState::STAGED state. In the SessionState::STAGED case, this
  // function is effectively a no-op.
  auto session_state = (*session).GetState();
  if (session_state == SessionState::STAGED) {
    return {};
  }
  if (session_state == SessionState::VERIFIED) {
    return (*session).UpdateStateAndCommit(SessionState::STAGED);
  }
  return Error() << "Invalid state for session " << session_id
                 << ". Cannot mark it as ready.";
}

Result<void> MarkStagedSessionSuccessful(const int session_id) {
  auto session = ApexSession::GetSession(session_id);
  if (!session.ok()) {
    return session.error();
  }
  // Only SessionState::ACTIVATED or SessionState::SUCCESS states are accepted.
  // In the SessionState::SUCCESS state, this function is a no-op.
  if (session->GetState() == SessionState::SUCCESS) {
    return {};
  } else if (session->GetState() == SessionState::ACTIVATED) {
    auto cleanup_status = DeleteBackup();
    if (!cleanup_status.ok()) {
      return Error() << "Failed to mark session " << *session
                     << " as successful : " << cleanup_status.error();
    }
    if (session->IsRollback() && !gInFsCheckpointMode) {
      DeleteDePreRestoreSnapshots(*session);
    }
    return session->UpdateStateAndCommit(SessionState::SUCCESS);
  } else {
    return Error() << "Session " << *session << " can not be marked successful";
  }
}

namespace {

// Removes APEXes on /data that have not been activated
void RemoveInactiveDataApex() {
  auto data_apexes =
      FindFilesBySuffix(kActiveApexPackagesDataDir, {kApexPackageSuffix});
  if (!data_apexes.ok()) {
    LOG(ERROR) << "Failed to scan " << kActiveApexPackagesDataDir << " : "
               << data_apexes.error();
    return;
  }
  for (const auto& path : *data_apexes) {
    if (!apexd_private::IsMounted(path)) {
      LOG(INFO) << "Removing inactive data APEX " << path;
      if (unlink(path.c_str()) != 0) {
        PLOG(ERROR) << "Failed to unlink inactive data APEX " << path;
      }
    }
  }
}

}  // namespace

// Decompressed APEX in |decompression_dir| that are no longer linked to
// |apex_active_dir| should be removed
void RemoveUnlinkedDecompressedApex(
    const std::string& decompression_dir = kApexDecompressedDir,
    const std::string& apex_active_dir = kActiveApexPackagesDataDir) {
  auto decompressed_files = ReadDir(decompression_dir, [](auto _) {
    (void)_;
    return true;
  });
  if (!decompressed_files.ok()) {
    return;
  }

  namespace fs = std::filesystem;
  for (const std::string& decompressed_file : *decompressed_files) {
    // Check if this file is still hard linked to one of the files in
    // apex_active_dir
    const std::string filename = fs::path(decompressed_file).filename();
    const std::string active_copy_path =
        StringPrintf("%s/%s", apex_active_dir.c_str(), filename.c_str());
    std::error_code ec;
    const bool hard_link_exists =
        fs::equivalent(decompressed_file, active_copy_path, ec);
    if (ec || !hard_link_exists) {
      LOG(INFO) << "Cleaning up unused decompressed APEX file: "
                << decompressed_file;
      if (unlink(decompressed_file.c_str()) != 0) {
        PLOG(ERROR) << "Failed to delete unused decompressed APEX file: "
                    << decompressed_file;
      }
    }
  }
}

void BootCompletedCleanup() {
  RemoveInactiveDataApex();
  RemoveUnlinkedDecompressedApex();
  ApexSession::DeleteFinalizedSessions();
}

int UnmountAll() {
  gMountedApexes.PopulateFromMounts();
  int ret = 0;
  gMountedApexes.ForallMountedApexes([&](const std::string& /*package*/,
                                         const MountedApexData& data,
                                         bool latest) {
    LOG(INFO) << "Unmounting " << data.full_path << " mounted on "
              << data.mount_point;
    if (latest) {
      auto pos = data.mount_point.find('@');
      CHECK(pos != std::string::npos);
      std::string bind_mount = data.mount_point.substr(0, pos);
      if (umount2(bind_mount.c_str(), UMOUNT_NOFOLLOW) != 0) {
        PLOG(ERROR) << "Failed to unmount bind-mount " << bind_mount;
        ret = 1;
      }
    }
    if (auto status = Unmount(data); !status.ok()) {
      LOG(ERROR) << "Failed to unmount " << data.mount_point << " : "
                 << status.error();
      ret = 1;
    }
  });
  return ret;
}

Result<void> RemountPackages() {
  std::vector<std::string> apexes;
  gMountedApexes.ForallMountedApexes([&apexes](const std::string& /*package*/,
                                               const MountedApexData& data,
                                               bool latest) {
    if (latest) {
      LOG(DEBUG) << "Found active APEX " << data.full_path;
      apexes.push_back(data.full_path);
    }
  });
  std::vector<std::string> failed;
  for (const std::string& apex : apexes) {
    // Since this is only used during development workflow, we are trying to
    // remount as many apexes as possible instead of failing fast.
    if (auto ret = RemountApexFile(apex); !ret.ok()) {
      LOG(WARNING) << "Failed to remount " << apex << " : " << ret.error();
      failed.emplace_back(apex);
    }
  }
  static constexpr const char* kErrorMessage =
      "Failed to remount following APEX packages, hence previous versions of "
      "them are still active. If APEX you are developing is in this list, it "
      "means that there still are alive processes holding a reference to the "
      "previous version of your APEX.\n";
  if (!failed.empty()) {
    return Error() << kErrorMessage << "Failed (" << failed.size() << ") "
                   << "APEX packages: [" << Join(failed, ',') << "]";
  }
  return {};
}

// Given a single new APEX incoming via OTA, should we allocate space for it?
Result<bool> ShouldAllocateSpaceForDecompression(
    const std::string& new_apex_name, const int64_t new_apex_version,
    const ApexFileRepository& instance) {
  // An apex at most will have two versions on device: pre-installed and data.

  // Check if there is a pre-installed version for the new apex.
  if (!instance.HasPreInstalledVersion(new_apex_name)) {
    // We are introducing a new APEX that doesn't exist at all
    return true;
  }

  // From here on, we can assume there is a pre-installed APEX.
  // Check if there is a data apex
  if (!instance.HasDataVersion(new_apex_name)) {
    // Decompression of compressed APEX is prevented by existing data apex.
    // Since there is none, then new_apex will definitely get decompressed.
    return true;
  }

  // From here on, data apex exists. So we should compare directly against data
  // apex.
  // TODO(b/179497746): have ApexFileRepo provide reference to individual
  // pre-installed apex by name
  const auto data_apex_path = instance.GetDataPath(new_apex_name);
  if (!data_apex_path.ok()) {
    return Error() << "Failed to open data apex";
  }
  const auto data_apex = ApexFile::Open(*data_apex_path);
  // Compare the data apex version with new apex
  const int64_t data_version = data_apex->GetManifest().version();

  // new_apex will compete against the data apex if data apex is from an update.
  if (instance.IsDecompressedApex(*data_apex)) {
    // Since data apex is decompressed, it means device is using the compressed
    // pre-installed version. If new apex has higher version, we are upgrading
    // the pre-install version and if new apex has lower version, we are
    // downgrading it. So the current decompressed apex should be replaced
    // with the new decompressed apex to reflect that.

    return new_apex_version != data_version;
  }

  // If data apex is not decompressed, then we only decompress the new_apex
  // if it has higher version than data apex.
  return new_apex_version > data_version;
}

void CollectApexInfoList(std::ostream& os,
                         const std::vector<ApexFile>& active_apexs,
                         const std::vector<ApexFile>& inactive_apexs) {
  std::vector<com::android::apex::ApexInfo> apex_infos;

  auto convert_to_autogen = [&apex_infos](const ApexFile& apex,
                                          bool is_active) {
    auto& instance = ApexFileRepository::GetInstance();

    auto preinstalled_path =
        instance.GetPreinstalledPath(apex.GetManifest().name());
    std::optional<std::string> preinstalled_module_path;
    if (preinstalled_path.ok()) {
      preinstalled_module_path = *preinstalled_path;
    }
    com::android::apex::ApexInfo apex_info(
        apex.GetManifest().name(), apex.GetPath(), preinstalled_module_path,
        apex.GetManifest().version(), apex.GetManifest().versionname(),
        instance.IsPreInstalledApex(apex), is_active);
    apex_infos.emplace_back(apex_info);
  };
  for (const auto& apex : active_apexs) {
    convert_to_autogen(apex, /* is_active= */ true);
  }
  for (const auto& apex : inactive_apexs) {
    convert_to_autogen(apex, /* is_active= */ false);
  }
  com::android::apex::ApexInfoList apex_info_list(apex_infos);
  com::android::apex::write(os, apex_info_list);
}

// Reserve |size| bytes in |dest_dir| by creating a zero-filled file
Result<void> ReserveSpaceForCompressedApex(int64_t size,
                                           const std::string& dest_dir) {
  LOG(INFO) << "Reserving " << size << " bytes for compressed APEX";

  if (size < 0) {
    return Error() << "Cannot reserve negative byte of space";
  }
  auto file_path = StringPrintf("%s/full.tmp", dest_dir.c_str());
  if (size == 0) {
    RemoveFileIfExists(file_path);
    return {};
  }

  unique_fd dest_fd(
      open(file_path.c_str(), O_WRONLY | O_CLOEXEC | O_CREAT, 0644));
  if (dest_fd.get() == -1) {
    return ErrnoError() << "Failed to open file for reservation "
                        << file_path.c_str();
  }

  // Resize to required size
  std::error_code ec;
  std::filesystem::resize_file(file_path, size, ec);
  if (ec) {
    RemoveFileIfExists(file_path);
    return ErrnoError() << "Failed to resize file " << file_path.c_str()
                        << " : " << ec.message();
  }

  return {};
}

int OnOtaChrootBootstrap(const std::vector<std::string>& built_in_dirs,
                         const std::string& apex_data_dir) {
  auto& instance = ApexFileRepository::GetInstance();
  if (auto status = instance.AddPreInstalledApex(built_in_dirs); !status.ok()) {
    LOG(ERROR) << "Failed to scan pre-installed apexes from "
               << Join(built_in_dirs, ',');
    return 1;
  }
  if (auto status = instance.AddDataApex(apex_data_dir); !status.ok()) {
    LOG(ERROR) << "Failed to scan upgraded apexes from " << apex_data_dir;
    // Failing to scan upgraded apexes is not fatal, since we can still try to
    // run otapreopt using only pre-installed apexes. Worst case, apps will be
    // re-optimized on next boot.
  }

  // Create directories for APEX shared libraries.
  if (auto status = CreateSharedLibsApexDir(); !status.ok()) {
    LOG(ERROR) << "Failed to create /apex/sharedlibs : " << status.ok();
    return 1;
  }

  auto activation_list =
      SelectApexForActivation(instance.AllApexFilesByName(), instance);
  auto activate_status = ActivateApexPackages(activation_list,
                                              /* is_ota_chroot= */ true);

  if (!activate_status.ok()) {
    LOG(ERROR) << "Failed to activate apex packages : "
               << activate_status.error();
    auto retry_status = ActivateMissingApexes(activation_list,
                                              /* is_ota_chroot= */ true);
    if (!retry_status.ok()) {
      LOG(ERROR) << retry_status.error();
    }
  }

  // There are a bunch of places that are producing apex-info.xml file.
  // We should consolidate the logic in one function and make all other places
  // use it.
  auto active_apexes = GetActivePackages();
  // We can't use GetFactoryPackages here, since it scans
  // kApexPackageBuiltinDirs. Once that is fixed, code here can be simplified.
  std::vector<ApexFile> inactive_apexes;
  for (const auto& pre_installed_apex : instance.GetPreInstalledApexFiles()) {
    if (std::none_of(active_apexes.begin(), active_apexes.end(),
                     [&pre_installed_apex](const auto& active_apex) {
                       return pre_installed_apex.get().GetPath() ==
                              active_apex.GetPath();
                     })) {
      // We need to do re-open the file here because CollectApexInfoList
      // accepts std::vector<ApexFile> :(
      auto apex = ApexFile::Open(pre_installed_apex.get().GetPath());
      if (apex.ok()) {
        inactive_apexes.push_back(std::move(*apex));
      } else {
        LOG(ERROR) << "Failed to open " << pre_installed_apex.get().GetPath()
                   << " : " << apex.error();
      }
    }
  }
  std::stringstream xml;
  CollectApexInfoList(xml, GetActivePackages(), inactive_apexes);
  std::string file_name = StringPrintf("%s/%s", kApexRoot, kApexInfoList);
  unique_fd fd(TEMP_FAILURE_RETRY(
      open(file_name.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)));
  if (fd.get() == -1) {
    PLOG(ERROR) << "Can't open " << file_name;
    return 1;
  }

  if (!android::base::WriteStringToFd(xml.str(), fd)) {
    PLOG(ERROR) << "Can't write to " << file_name;
    return 1;
  }

  fd.reset();

  if (auto status = RestoreconPath(file_name); !status.ok()) {
    LOG(ERROR) << "Failed to restorecon " << file_name << " : "
               << status.error();
    return 1;
  }

  return 0;
}

android::apex::MountedApexDatabase& GetApexDatabaseForTesting() {
  return gMountedApexes;
}

}  // namespace apex
}  // namespace android
