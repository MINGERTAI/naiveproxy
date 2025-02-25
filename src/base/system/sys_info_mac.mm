// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/system/sys_info.h"

#import <Foundation/Foundation.h>
#include <mach/mach_host.h>
#include <mach/mach_init.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#include "base/check_op.h"
#include "base/mac/mac_util.h"
#include "base/mac/scoped_mach_port.h"
#include "base/notreached.h"
#include "base/numerics/safe_conversions.h"
#include "base/process/process_metrics.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"

namespace base {

namespace {

// Queries sysctlbyname() for the given key and returns the value from the
// system or the empty string on failure.
std::string GetSysctlValue(const char* key_name) {
  char value[256];
  size_t len = std::size(value);
  if (sysctlbyname(key_name, &value, &len, nullptr, 0) == 0) {
    DCHECK_GE(len, 1u);
    DCHECK_EQ('\0', value[len - 1]);
    return std::string(value, len - 1);
  }
  return std::string();
}

}  // namespace

// static
std::string SysInfo::OperatingSystemName() {
  return "Mac OS X";
}

// static
std::string SysInfo::OperatingSystemVersion() {
  int32_t major, minor, bugfix;
  OperatingSystemVersionNumbers(&major, &minor, &bugfix);
  return base::StringPrintf("%d.%d.%d", major, minor, bugfix);
}

// static
void SysInfo::OperatingSystemVersionNumbers(int32_t* major_version,
                                            int32_t* minor_version,
                                            int32_t* bugfix_version) {
  NSOperatingSystemVersion version =
      [[NSProcessInfo processInfo] operatingSystemVersion];
  *major_version = static_cast<int32_t>(version.majorVersion);
  *minor_version = static_cast<int32_t>(version.minorVersion);
  *bugfix_version = static_cast<int32_t>(version.patchVersion);
}

// static
std::string SysInfo::OperatingSystemArchitecture() {
  switch (mac::GetCPUType()) {
    case mac::CPUType::kIntel:
      return "x86_64";
    case mac::CPUType::kTranslatedIntel:
    case mac::CPUType::kArm:
      return "arm64";
  }
}

// static
uint64_t SysInfo::AmountOfPhysicalMemoryImpl() {
  struct host_basic_info hostinfo;
  mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
  base::mac::ScopedMachSendRight host(mach_host_self());
  int result = host_info(host.get(), HOST_BASIC_INFO,
                         reinterpret_cast<host_info_t>(&hostinfo), &count);
  if (result != KERN_SUCCESS) {
    NOTREACHED();
    return 0;
  }
  DCHECK_EQ(HOST_BASIC_INFO_COUNT, count);
  return hostinfo.max_mem;
}

// static
uint64_t SysInfo::AmountOfAvailablePhysicalMemoryImpl() {
  SystemMemoryInfoKB info;
  if (!GetSystemMemoryInfo(&info))
    return 0;
  // We should add inactive file-backed memory also but there is no such
  // information from Mac OS unfortunately.
  return checked_cast<uint64_t>(info.free + info.speculative) * 1024;
}

// static
std::string SysInfo::CPUModelName() {
  return GetSysctlValue("machdep.cpu.brand_string");
}

// static
std::string SysInfo::HardwareModelName() {
  return GetSysctlValue("hw.model");
}

// static
SysInfo::HardwareInfo SysInfo::GetHardwareInfoSync() {
  HardwareInfo info;
  info.manufacturer = "Apple Inc.";
  info.model = HardwareModelName();
  DCHECK(IsStringUTF8(info.manufacturer));
  DCHECK(IsStringUTF8(info.model));
  return info;
}

}  // namespace base
