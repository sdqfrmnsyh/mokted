#include "runtime/graphics_launch_policy.h"

#include "runtime/frame_rate_policy.h"
#include "runtime/texture_memory_policy.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

// Keep the Vulkan shader workaround narrowly scoped. TextureManager2 is left
// enabled so the runtime can use its normal texture path and mip selection.
constexpr char kVulkanClientSettingsOverrides[] =
    R"({"FStringGraphicsVulkanShaderMTDenyPattern":"4318:.*"})";

constexpr const char* kIcdDirectories[] = {
    "/usr/share/vulkan/icd.d",
    "/etc/vulkan/icd.d",
    "/usr/local/share/vulkan/icd.d",
};

constexpr char kNativeArchitecture[] = "x86_64";

constexpr const char* kForeignArchitectures[] = {
    "i686", "i586", "i486", "i386",   "x86.",   "aarch64",
    "arm64", "armhf", "armv7", "ppc64", "riscv64", "s390x",
};

struct HostGpus {
  bool intel = false;
  bool nvidia = false;
  bool amd = false;
};

bool ForeignArchitecture(const std::string& name) {
  for (const char* architecture : kForeignArchitectures) {
    if (name.find(architecture) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool SetValue(const char* name, const std::string& value, std::string* error) {
  if (setenv(name, value.c_str(), 1) == 0) {
    return true;
  }
  if (error != nullptr) {
    *error = std::string("cannot publish resolved graphics setting: ") + name;
  }
  return false;
}

bool SetDefault(const char* name, const std::string& value,
                std::string* error) {
  const char* current = std::getenv(name);
  if (current != nullptr && current[0] != '\0') {
    return true;
  }
  return SetValue(name, value, error);
}

bool IsStrictOpenGlName(const std::string& name) {
  return name == "opengl" || name == "gles";
}

bool EnvIsOff(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr &&
         (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0 ||
          std::strcmp(value, "igpu") == 0);
}

bool UnthrottledPresentation(const RuntimeConfig& config) {
  if (config.vsync_mode() == "off" || config.vsync_mode() == "0") {
    return true;
  }
  // For ultra-low-end hardware, auto vsync uses mailbox for lower latency
  // but with frame rate limiting to reduce GPU pressure
  if (config.vsync_mode() == "auto") {
    std::uint64_t host_memory = DetectHostMemoryBytes();
    bool is_ultra_low_end = host_memory < 4ULL * 1024U * 1024U * 1024U;
    if (is_ultra_low_end) {
      // Use mailbox for low latency but with frame pacing
      return config.frame_rate().mode == FrameRateLimitMode::kUnlimited;
    }
  }
  return config.frame_rate().mode == FrameRateLimitMode::kUnlimited;
}

bool ParsePciVendor(const std::string& raw, unsigned int* vendor) {
  if (vendor == nullptr || raw.empty()) {
    return false;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(raw.c_str(), &end, 16);
  if (end == raw.c_str() || parsed > 0xffffUL) {
    return false;
  }
  *vendor = static_cast<unsigned int>(parsed);
  return true;
}

HostGpus DetectHostGpus() {
  HostGpus gpus;
  for (int index = 0; index < 16; ++index) {
    const std::string path = "/sys/class/drm/card" + std::to_string(index) +
                             "/device/vendor";
    std::ifstream input(path);
    std::string raw;
    unsigned int vendor = 0;
    if (!(input >> raw) || !ParsePciVendor(raw, &vendor)) {
      continue;
    }
    if (vendor == 0x8086) {
      gpus.intel = true;
    } else if (vendor == 0x10de) {
      gpus.nvidia = true;
    } else if (vendor == 0x1002) {
      gpus.amd = true;
    }
  }
  return gpus;
}

std::string FindIcdFile(const char* filename_needle) {
  if (filename_needle == nullptr || filename_needle[0] == '\0') {
    return {};
  }
  std::vector<std::filesystem::path> directories;
  for (const char* directory : kIcdDirectories) {
    directories.emplace_back(directory);
  }
  return SelectVulkanIcdManifest(directories, filename_needle);
}

std::string SelectHardwareIcd(const HostGpus& gpus) {
  const bool prefer_discrete = !EnvIsOff("DRI_PRIME") &&
                               !EnvIsOff("__NV_PRIME_RENDER_OFFLOAD");
  if (prefer_discrete && gpus.nvidia) {
    std::string nvidia = FindIcdFile("nvidia_icd");
    if (nvidia.empty()) {
      nvidia = FindIcdFile("nouveau_icd");
    }
    if (!nvidia.empty()) {
      return nvidia;
    }
  }
  if (prefer_discrete && gpus.amd) {
    const std::string amd = FindIcdFile("radeon_icd");
    if (!amd.empty()) {
      return amd;
    }
  }
  if (gpus.intel) {
    std::string intel = FindIcdFile("intel_icd");
    if (intel.empty()) {
      intel = FindIcdFile("intel_hasvk_icd");
    }
    return intel;
  }
  if (gpus.nvidia) {
    std::string nvidia = FindIcdFile("nvidia_icd");
    if (nvidia.empty()) {
      nvidia = FindIcdFile("nouveau_icd");
    }
    return nvidia;
  }
  if (gpus.amd) {
    return FindIcdFile("radeon_icd");
  }
  return {};
}

// Match Mesa ANV: 75% of RAM when the machine has more than 4GiB, else 50%.
// Forcing 50 on an 8GiB UHD 620 iGPU advertises a smaller Vk heap than ANV
// would, which increases BO eviction inside GEM_EXECBUFFER2.
// ULTRA-LOW-END: On systems with < 4GB, use 25% to leave memory for system
const char* AnvSysMemLimitPercent() {
  std::ifstream input("/proc/meminfo");
  std::string key;
  unsigned long kb = 0;
  std::string unit;
  while (input >> key >> kb >> unit) {
    if (key == "MemTotal:") {
      if (kb < 4UL * 1024UL * 1024UL) {
        return "25";
      }
      return kb > 4UL * 1024UL * 1024UL ? "75" : "50";
    }
  }
  return "50";
}

// OPTIMIZED: Apply aggressive OpenGL/Mesa driver optimizations
bool ApplyOpenGlOptimizations(std::string* error) {
  // Mesa (AMD/Intel) optimizations
  if (!SetDefault("MESA_LOADER_DRIVER_OVERRIDE", "", error)) {
    return false;
  }
  // Disable debug/output overhead
  if (!SetDefault("MESA_DEBUG", "silent", error)) {
    return false;
  }
  if (!SetDefault("MESA_VERBOSE", "0", error)) {
    return false;
  }
  if (!SetDefault("MESA_LOG_FILE", "", error)) {
    return false;
  }
  // Performance optimizations
  if (!SetDefault("MESA_GLTHREAD", "true", error)) {
    return false;
  }
  if (!SetDefault("MESA_SHADER_CACHE", "1", error)) {
    return false;
  }
  if (!SetDefault("MESA_SHADER_CACHE_MAX_SIZE", "2G", error)) {
    return false;
  }
  if (!SetDefault("MESA_NO_DITHER", "1", error)) {
    return false;
  }
  if (!SetDefault("MESA_NO_ERROR", "1", error)) {
    return false;
  }
  // Tile-based rendering optimizations
  if (!SetDefault("MESA_TILE_RASTER_ORDER_FIX", "1", error)) {
    return false;
  }
  // Threaded optimizations
  if (!SetDefault("GALLIUM_THREAD", "1", error)) {
    return false;
  }
  if (!SetDefault("GALLIUM_HUD", "", error)) {
    return false;
  }
  // Disable vsync at driver level (we handle it via Vulkan WSI)
  if (!SetDefault("vblank_mode", "0", error)) {
    return false;
  }
  // Buffer storage optimizations
  if (!SetDefault("MESA_EXTENSION_OVERRIDE",
                  "-GL_ARB_sync,+GL_EXT_buffer_storage,"
                  "+GL_ARB_buffer_storage,+GL_ARB_multi_bind,"
                  "+GL_ARB_texture_storage,+GL_ARB_texture_view,"
                  "+GL_EXT_texture_compression_s3tc,+GL_EXT_texture_filter_anisotropic",
                  error)) {
    return false;
  }
  // Force GLSL optimizer
  if (!SetDefault("MESA_GLSL_CACHE_DISABLE", "0", error)) {
    return false;
  }
  if (!SetDefault("MESA_GLSL_OPTIMIZE", "1", error)) {
    return false;
  }
  // Disable sync waits
  if (!SetDefault("MESA_NO_FENCE", "1", error)) {
    return false;
  }
  return true;
}

bool ApplyVulkanIcdPolicy(const HostGpus& gpus, std::string* error) {
  // Drop software/emulation ICDs even when the user already pinned a driver
  // list. Old loaders ignore this variable.
  if (!SetDefault("VK_LOADER_DRIVERS_DISABLE",
                  "lvp_icd:dzn_icd:virtio_icd", error)) {
    return false;
  }
  const char* existing_files = std::getenv("VK_DRIVER_FILES");
  const char* existing_icds = std::getenv("VK_ICD_FILENAMES");
  if ((existing_files != nullptr && existing_files[0] != '\0') ||
      (existing_icds != nullptr && existing_icds[0] != '\0')) {
    return true;
  }
  const std::string icd = SelectHardwareIcd(gpus);
  if (icd.empty()) {
    return true;
  }
  std::fprintf(stderr, "  [runtime] vulkan ICD=%s\n", icd.c_str());
  return SetDefault("VK_DRIVER_FILES", icd, error) &&
         SetDefault("VK_ICD_FILENAMES", icd, error);
}

// OPTIMIZED: Apply aggressive Vulkan driver optimizations
bool ApplyVulkanOptimizations(const HostGpus& gpus, std::string* error) {
  // Detect ultra-low-end system (Stoney Ridge class)
  std::uint64_t host_memory = mocktail::runtime::DetectHostMemoryBytes();
  bool is_ultra_low_end = host_memory < 4ULL * 1024U * 1024U * 1024U;

  // RADV (AMD) optimizations
  if (gpus.amd) {
    if (is_ultra_low_end) {
      // ULTRA-LOW-END: Stoney Ridge (GCN 1.3) - minimal feature set
      if (!SetDefault("RADV_PERFTEST",
                      "aco,nggc,ngg,dcc,fmask,compress,"
                      "no_surface_init,fast_launch,htile,rbplus,"
                      "drirc_shader_binary,drirc_optimize",
                      error)) {
        return false;
      }
      // Disable expensive features on Stoney Ridge
      if (!SetDefault("RADV_DEBUG",
                      "noinfo,nodump,nooutoforder,startup,nowc,noih,noib",
                      error)) {
        return false;
      }
      // Force 32-bit indices, disable mesh/task shaders (not supported on GCN 1.3)
      if (!SetDefault("RADV_FORCE_NGG", "1", error)) {
        return false;
      }
      // Reduce VRAM usage
      if (!SetDefault("RADV_TEX_ANISO_MAX", "2", error)) {
        return false;
      }
    } else {
      // Standard AMD optimizations
      if (!SetDefault("RADV_PERFTEST",
                      "aco,nggc,ngg,mesh_shaders,task_shaders,rt,dcc,display_dcc,"
                      "fmask,compress,es,gs,vs,ps,cs,invariantgeom,"
                      "shader_cycle,no_surface_init,fast_launch,"
                      "tc_compat_htile,htile,rbplus,"
                      "dds,drirc_shader_binary,drirc_optimize,"
                      "list_threaded_submit",
                      error)) {
        return false;
      }
      if (!SetDefault("RADV_DEBUG",
                      "noinfo,nodump,nooutoforder,startup",
                      error)) {
        return false;
      }
      if (!SetDefault("RADV_FORCE_NGG", "1", error)) {
        return false;
      }
    }
  }

  // ANV (Intel) optimizations
  if (gpus.intel) {
    // Already set via ANV_SYS_MEM_LIMIT, add more
    if (!SetDefault("ANV_PRIMITIVE_REPLICATION", "1", error)) {
      return false;
    }
    if (!SetDefault("ANV_ENABLE_GEN12_TESSELLATION", "1", error)) {
      return false;
    }
    if (!SetDefault("ANV_ALWAYS_BINDLESS", "1", error)) {
      return false;
    }
    if (!SetDefault("ANV_USE_DGC", "1", error)) {
      return false;
    }
    // Enable mesh/task shaders on DG2+
    if (!SetDefault("ANV_ENABLE_MESH_SHADERS", "1", error)) {
      return false;
    }
  }

  // NVK (Nouveau/NVIDIA) optimizations
  if (gpus.nvidia) {
    if (!SetDefault("NVK_DEBUG", "noinfo,nodump", error)) {
      return false;
    }
    if (!SetDefault("NVK_PERFTEST", "cs,gs,vs,ps,mesh,task", error)) {
      return false;
    }
  }

  // Universal Vulkan optimizations
  if (!SetDefault("VK_INSTANCE_LAYERS", "VK_LAYER_MESA_overlay", error)) {
    return false;
  }
  // Disable validation layers for performance
  if (!SetDefault("VK_LOADER_LAYERS_DISABLE", "*", error)) {
    return false;
  }
  if (!SetDefault("VK_LAYER_DISABLE", "*", error)) {
    return false;
  }
  // Enable timeline semaphores everywhere
  if (!SetDefault("MESA_VK_WSI_PRESENT_MODE", "mailbox", error)) {
    return false;
  }
  // Threaded submit for all drivers
  if (!SetDefault("MESA_VK_ENABLE_SUBMIT_THREAD", "1", error)) {
    return false;
  }
  // Disable async present fence overhead
  if (!SetDefault("MESA_VK_WSI_DEBUG", "no-incremental", error)) {
    return false;
  }
  return true;
}

}  // namespace

std::string SelectVulkanIcdManifest(
    const std::vector<std::filesystem::path>& directories,
    std::string_view vendor) {
  if (vendor.empty()) {
    return {};
  }
  for (const std::filesystem::path& directory : directories) {
    std::error_code error;
    std::vector<std::string> names;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
      if (!iterator->is_regular_file(error)) {
        continue;
      }
      const std::string name = iterator->path().filename().string();
      if (name.find(".json") == std::string::npos ||
          name.find(vendor) == std::string::npos || ForeignArchitecture(name)) {
        continue;
      }
      names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    std::string generic;
    for (const std::string& name : names) {
      const std::string path = (directory / name).string();
      if (access(path.c_str(), R_OK) != 0) {
        continue;
      }
      if (name.find(kNativeArchitecture) != std::string::npos) {
        return path;
      }
      if (generic.empty()) {
        generic = path;
      }
    }
    if (!generic.empty()) {
      return generic;
    }
  }
  return {};
}

bool ApplyGraphicsLaunchPolicy(const RuntimeConfig& config,
                               std::string* error) {
  if (config.graphics_backend() == GraphicsBackend::kUnknown) {
    if (error != nullptr) {
      *error = "cannot apply an unknown graphics backend";
    }
    return false;
  }

  const bool direct_vulkan =
      config.graphics_backend() == GraphicsBackend::kVulkan;
  if (!SetValue("MOCKTAIL_GRAPHICS_BACKEND",
                config.graphics_backend_name(), error) ||
      !SetValue("MOCKTAIL_PRELOAD_VULKAN_SHIM",
                direct_vulkan ? "1" : "0", error) ||
      !SetDefault("MOCKTAIL_REQUIRE_REAL_GRAPHICS", "1", error)) {
    return false;
  }

  if (IsStrictOpenGlName(config.graphics_backend_name()) &&
      (!SetValue("MOCKTAIL_DISABLE_AUTO_ANGLE_FALLBACK", "1", error) ||
       !SetValue("MOCKTAIL_SOFTWARE_WINDOW_FALLBACK", "0", error) ||
       !SetDefault("MESA_GL_VERSION_OVERRIDE", "3.2", error) ||
       !SetDefault("MESA_GLSL_VERSION_OVERRIDE", "150", error) ||
       !SetDefault("MESA_EXTENSION_OVERRIDE", "-GL_ARB_sync,+GL_EXT_buffer_storage", error) ||
       !SetDefault("vblank_mode", "0", error))) {
    return false;
  }

  // OPTIMIZED: Apply OpenGL/Mesa driver optimizations
  if (IsStrictOpenGlName(config.graphics_backend_name())) {
    if (!ApplyOpenGlOptimizations(error)) {
      return false;
    }
  }

  if (direct_vulkan) {
    const char* wsi_mode =
        UnthrottledPresentation(config) ? "immediate" : "mailbox";
    const HostGpus gpus = DetectHostGpus();
    if (!SetDefault("MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON",
                    kVulkanClientSettingsOverrides, error) ||
        !SetDefault("ANV_SYS_MEM_LIMIT", AnvSysMemLimitPercent(), error) ||
        !SetDefault("MESA_VK_WSI_PRESENT_MODE", wsi_mode, error) ||
        // Move GEM_EXECBUFFER2 off the application thread onto Mesa's submit
        // worker so the render thread is not stuck in i915 ioctl.
        !SetDefault("MESA_VK_ENABLE_SUBMIT_THREAD", "1", error) ||
        !ApplyVulkanIcdPolicy(gpus, error) ||
        !ApplyVulkanOptimizations(gpus, error) ||
        !SetDefault("MOCKTAIL_FORCE_1X1_TEXTURES", "1", error)) {
      return false;
    }
  }
  return true;
}

}  // namespace runtime
}  // namespace mocktail
