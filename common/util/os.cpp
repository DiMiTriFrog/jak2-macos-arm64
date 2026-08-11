#include "os.h"

#include "common/common_types.h"
#include "common/log/log.h"
#include "common/util/string_util.h"

#ifdef __APPLE__
#include <stdio.h>

#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#ifdef _WIN32
// clang-format off
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <psapi.h>
// clang-format on
size_t get_peak_rss() {
  HANDLE hProcess = GetCurrentProcess();
  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc))) {
    return pmc.PeakWorkingSetSize;
  } else {
    return 0;
  }
}
#else
#include <sys/resource.h>
size_t get_peak_rss() {
  rusage x;
  getrusage(RUSAGE_SELF, &x);
  return x.ru_maxrss * 1024;
}
#endif

#ifdef _WIN32
// windows has a __cpuid
#include <intrin.h>
#elif __x86_64__
// using int to be compatible with msvc's intrinsic
void __cpuidex(int result[4], int eax, int ecx) {
  asm("cpuid\n\t"
      : "=a"(result[0]), "=b"(result[1]), "=c"(result[2]), "=d"(result[3])
      : "0"(eax), "2"(ecx));
}
#else
// ARM64 has no x86 CPUID instruction.  The GOAL ARM64 backend does not use
// x86 AVX feature bits, so retain a zero-filled compatibility implementation
// for code that still references this legacy helper on non-x86 hosts.
void __cpuidex(int result[4], int eax, int ecx) {
  (void)eax;
  (void)ecx;
  for (int i = 0; i < 4; i++) {
    result[i] = 0;
  }
}
#endif

CpuInfo gCpuInfo;

void setup_cpu_info() {
  if (gCpuInfo.initialized) {
    return;
  }

#if defined(__aarch64__)
  // AVX is an x86 capability and must not be used as a gate for the native
  // ARM64 runtime.  Keep the fields false so x86-only optional paths remain
  // disabled, while reporting the actual Apple CPU model when available.
#ifdef __APPLE__
  char model[256] = {};
  size_t model_size = sizeof(model);
  if (sysctlbyname("machdep.cpu.brand_string", model, &model_size, nullptr, 0) == 0) {
    gCpuInfo.brand = model;
    gCpuInfo.model = model;
  }
#endif
  if (gCpuInfo.brand.empty()) {
    gCpuInfo.brand = "ARM64";
    gCpuInfo.model = "ARM64";
  }

  printf("-------- CPU Information --------\n");
  printf(" Brand: %s\n", gCpuInfo.brand.c_str());
  printf(" Model: %s\n", gCpuInfo.model.c_str());
  printf(" AVX  : false (not applicable to ARM64)\n");
  printf(" AVX2 : false (not applicable to ARM64)\n");
  fflush(stdout);
  gCpuInfo.initialized = true;
  return;
#endif

  // as a test, get the brand and model
  for (u32 i = 0x80000002; i <= 0x80000004; i++) {
    int result[4];
    __cpuidex(result, i, 0);
    for (auto reg : result) {
      for (int c = 0; c < 4; c++) {
        gCpuInfo.model.push_back(reg);
        reg >>= 8;
      }
    }
  }

  {
    int result[4];
    __cpuidex(result, 0, 0);
    for (auto r : {1, 3, 2}) {
      for (int c = 0; c < 4; c++) {
        gCpuInfo.brand.push_back(result[r]);
        result[r] >>= 8;
      }
    }
  }

  // check for AVX2
  {
    int result[4];
    __cpuidex(result, 7, 0);
    gCpuInfo.has_avx2 = result[1] & (1 << 5);
  }

  {
    int result[4];
    __cpuidex(result, 1, 0);
    gCpuInfo.has_avx = result[2] & (1 << 28);
  }

  printf("-------- CPU Information --------\n");
  printf(" Brand: %s\n", gCpuInfo.brand.c_str());
  printf(" Model: %s\n", gCpuInfo.model.c_str());
  printf(" AVX  : %s\n", gCpuInfo.has_avx ? "true" : "false");
  printf(" AVX2 : %s\n", gCpuInfo.has_avx2 ? "true" : "false");
  fflush(stdout);

  gCpuInfo.initialized = true;
}

CpuInfo& get_cpu_info() {
  return gCpuInfo;
}

bool is_process_translated() {
#ifndef __APPLE__
  return false;
#else
  int translated = 0;
  size_t translated_size = sizeof(translated);
  if (sysctlbyname("sysctl.proc_translated", &translated, &translated_size, nullptr, 0) != 0) {
    // The key is absent for native processes and on older macOS versions.
    return false;
  }
  return translated != 0;
#endif
}

std::optional<double> get_macos_major_version() {
#ifndef __APPLE__
  return {};
#else
  char buffer[128];
  size_t bufferlen = 128;
  auto ok = sysctlbyname("kern.osproductversion", &buffer, &bufferlen, NULL, 0);
  if (ok != 0) {
    lg::warn("Unable to check for `kern.osproductversion` to determine macOS version");
    return {};
  }
  try {
    std::string macos_major_version = buffer;
    if (str_util::contains(buffer, ".")) {
      macos_major_version = str_util::split_string(macos_major_version, ".")[0];
    }
    return std::stod(macos_major_version);
  } catch (std::exception& e) {
    lg::error("Error occured when attempting to convert sysctl value {} to number", buffer);
    return {};
  }
#endif
}
