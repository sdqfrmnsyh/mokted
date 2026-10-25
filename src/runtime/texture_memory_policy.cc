#include "runtime/texture_memory_policy.h"

#define JSON_NOEXCEPTION 1
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::uint64_t kMebibyte = 1024U * 1024U;
// For ultra-low-end (Stoney Ridge class): detect < 4GB total RAM
constexpr std::uint64_t kUltraLowMemoryHostBytes = 4U * 1024U * kMebibyte;
constexpr std::uint64_t kLowMemoryHostBytes = 8U * 1024U * kMebibyte;
// MAXIMUM AGGRESSIVE: 16 MB for Stoney Ridge / integrated graphics with < 4GB RAM
constexpr std::uint64_t kUltraLowMemoryBudgetBytes = 16U * kMebibyte;
constexpr std::uint64_t kLowMemoryBudgetBytes = 32U * kMebibyte;
// DEEP OPTIMIZATION: Ultra-low minimum for integrated graphics with 2GB VRAM limit
constexpr std::uint64_t kMinimumBudgetBytes = 16U * kMebibyte;
// Cap at 256MB for integrated graphics - they can't address more efficiently
constexpr std::uint64_t kMaximumBudgetBytes = 256U * kMebibyte;
constexpr std::string_view kVideoMemoryOverride =
    "FIntRenderForceVideoMemorySize";

}  // namespace

std::uint64_t DetectHostMemoryBytes() {
  std::ifstream input("/proc/meminfo");
  std::string key;
  while (input >> key) {
    if (key != "MemTotal:") {
      input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
      continue;
    }
    std::uint64_t kibibytes = 0;
    if (!(input >> kibibytes)) {
      break;
    }
    if (kibibytes > std::numeric_limits<std::uint64_t>::max() / 1024U) {
      break;
    }
    return kibibytes * 1024U;
  }
  return 0;
}

std::uint64_t CalculateTextureMemoryBudgetBytes(
    std::uint64_t host_memory_bytes) {
  // ULTRA-LOW-END: Stoney Ridge / integrated graphics with < 4GB RAM
  if (host_memory_bytes < kUltraLowMemoryHostBytes) {
    return kUltraLowMemoryBudgetBytes;
  }
  if (host_memory_bytes < kLowMemoryHostBytes) {
    return kLowMemoryBudgetBytes;
  }
  // MAXIMUM AGGRESSIVE: Use 1/32 of host memory for integrated graphics
  // They have shared system memory - allocating too much starves the CPU
  return std::clamp(host_memory_bytes / 32U, kMinimumBudgetBytes,
                    kMaximumBudgetBytes);
}

bool MergeTextureMemoryClientSettingsOverrides(std::uint64_t budget_bytes,
                                               std::string_view base_json,
                                               std::string* merged_json,
                                               std::string* error) {
  if (merged_json == nullptr) {
    if (error != nullptr) {
      *error = "texture memory client-settings output is required";
    }
    return false;
  }
  nlohmann::json overrides = nlohmann::json::parse(
      base_json.empty() ? "{}" : base_json, nullptr, false, true);
  if (overrides.is_discarded() || !overrides.is_object()) {
    if (error != nullptr) {
      *error = "texture memory client-settings input must be a JSON object";
    }
    return false;
  }
  const std::string key(kVideoMemoryOverride);
  if (budget_bytes != 0 && !overrides.contains(key)) {
    // The override is a Roblox FInt, so it cannot carry more than INT32_MAX.
    const std::uint64_t clamped = std::min<std::uint64_t>(
        budget_bytes,
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()));
    overrides[key] = std::to_string(clamped);
  }
  *merged_json = overrides.dump();
  return true;
}

}  // namespace runtime
}  // namespace mocktail
