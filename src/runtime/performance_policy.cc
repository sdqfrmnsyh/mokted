// Modified by vii from komaruworld/mocktail. See README "About this fork".
#include "runtime/performance_policy.h"

#include <sched.h>

#define JSON_NOEXCEPTION 1
#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include "runtime/crash_report_policy.h"
#include "runtime/http_client_policy.h"
#include "runtime/texture_memory_policy.h"

namespace mocktail {
namespace runtime {
namespace {

struct ClientSetting {
  std::string_view name;
  std::string_view value;
};

// A caller-supplied value always wins over the preset; the key is reported
// through kept_overrides so startup can say which preset values were skipped.
void SetCompatibleValue(nlohmann::json* object, const ClientSetting& setting,
                        std::vector<std::string>* kept_overrides) {
  const std::string name(setting.name);
  const std::string value(setting.value);
  const auto existing = object->find(name);
  if (existing != object->end()) {
    if ((!existing->is_string() || existing->get<std::string>() != value) &&
        kept_overrides != nullptr) {
      kept_overrides->push_back(name);
    }
    return;
  }
  (*object)[name] = value;
}

bool ReadTopologyValue(int logical_cpu, std::string_view name, int* value) {
  const std::string path = "/sys/devices/system/cpu/cpu" +
                           std::to_string(logical_cpu) + "/topology/" +
                           std::string(name);
  std::ifstream input(path);
  return static_cast<bool>(input >> *value);
}

int LogicalCpuFallback(const cpu_set_t* allowed) {
  int count = 0;
  if (allowed != nullptr) {
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
      if (CPU_ISSET(cpu, allowed)) {
        ++count;
      }
    }
  }
  if (count > 0) {
    return count;
  }
  return static_cast<int>(std::max(1U, std::thread::hardware_concurrency()));
}

}  // namespace

bool ParsePhysicsWorkerMode(std::string_view value, PhysicsWorkerMode* mode) {
  if (mode == nullptr) {
    return false;
  }
  if (value.empty() || value == "auto") {
    *mode = PhysicsWorkerMode::kAuto;
    return true;
  }
  if (value == "latency") {
    *mode = PhysicsWorkerMode::kLatency;
    return true;
  }
  if (value == "throughput") {
    *mode = PhysicsWorkerMode::kThroughput;
    return true;
  }
  return false;
}

std::string_view PhysicsWorkerModeName(PhysicsWorkerMode mode) {
  switch (mode) {
    case PhysicsWorkerMode::kAuto:
      return "auto";
    case PhysicsWorkerMode::kLatency:
      return "latency";
    case PhysicsWorkerMode::kThroughput:
      return "throughput";
  }
  return "auto";
}

int CalculateThroughputWorkerCount(int available_physical_cores) {
  // On a 2-core Excavator the Roblox scheduler's main thread and render
  // thread already need a core each. A second worker thread only causes
  // context switches, so cap the scheduler pool at cores-1.
  return std::max(1, available_physical_cores - 1);
}

PerformancePolicy ParsePerformancePolicy(
    std::string_view multithreaded_rendering, std::string_view memory_limit_mb,
    std::string_view game_mode, std::string_view physics_worker_mode) {
  PerformancePolicy policy;

  // Default to ultra-low-end optimized values
  policy.multithreaded_rendering = true;
  policy.physics_worker_mode = PhysicsWorkerMode::kLatency;
  policy.memory_limit_mb = 3072;
  policy.game_mode = GameModePolicy::kOn;

  // Override with explicit values if provided
  if (!multithreaded_rendering.empty() && multithreaded_rendering != "auto") {
    policy.multithreaded_rendering = multithreaded_rendering == "1" ||
                                     multithreaded_rendering == "true" ||
                                     multithreaded_rendering == "on";
  }
  if (!physics_worker_mode.empty() && physics_worker_mode != "auto") {
    policy.physics_worker_mode_valid =
        ParsePhysicsWorkerMode(physics_worker_mode, &policy.physics_worker_mode);
  } else {
    policy.physics_worker_mode_valid = true;
    policy.physics_worker_mode = PhysicsWorkerMode::kLatency;
  }
  if (policy.multithreaded_rendering ||
      policy.physics_worker_mode == PhysicsWorkerMode::kThroughput) {
    policy.physical_core_count = DetectAvailablePhysicalCoreCount();
  }
  if (!memory_limit_mb.empty() && memory_limit_mb != "auto") {
    const char* memory_begin = memory_limit_mb.data();
    const char* memory_end = memory_begin + memory_limit_mb.size();
    const std::from_chars_result memory_parsed =
        std::from_chars(memory_begin, memory_end, policy.memory_limit_mb);
    policy.memory_limit_valid =
        memory_parsed.ec == std::errc() && memory_parsed.ptr == memory_end &&
        policy.memory_limit_mb <=
            std::numeric_limits<std::uint64_t>::max() / (1024U * 1024U);
  }
  if (!game_mode.empty() && game_mode != "auto") {
    policy.game_mode_valid = ParseGameModePolicy(game_mode, &policy.game_mode);
  } else {
    policy.game_mode_valid = true;
    policy.game_mode = GameModePolicy::kOn;
  }
  return policy;
}

int DetectAvailablePhysicalCoreCount() {
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
    return LogicalCpuFallback(nullptr);
  }

  std::set<std::pair<int, int>> physical_cores;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (!CPU_ISSET(cpu, &allowed)) {
      continue;
    }
    int package_id = 0;
    int core_id = 0;
    if (!ReadTopologyValue(cpu, "physical_package_id", &package_id) ||
        !ReadTopologyValue(cpu, "core_id", &core_id)) {
      return LogicalCpuFallback(&allowed);
    }
    physical_cores.emplace(package_id, core_id);
  }
  return physical_cores.empty() ? LogicalCpuFallback(&allowed)
                                : static_cast<int>(physical_cores.size());
}

bool MergePerformanceClientSettingsOverrides(
    const PerformancePolicy& policy, std::string_view base_json,
    std::string* merged_json, std::string* error,
    std::vector<std::string>* kept_overrides) {
  if (merged_json == nullptr) {
    if (error != nullptr) {
      *error = "performance client-settings output is required";
    }
    return false;
  }
  nlohmann::json overrides = nlohmann::json::parse(
      base_json.empty() ? "{}" : base_json, nullptr, false, true);
  if (overrides.is_discarded() || !overrides.is_object()) {
    if (error != nullptr) {
      *error = "client-settings overrides must be a JSON object";
    }
    return false;
  }
  if (!policy.physics_worker_mode_valid) {
    if (error != nullptr) {
      *error = "physics worker mode is invalid";
    }
    return false;
  }
  const auto apply_settings = [&overrides,
                               kept_overrides](const auto& settings) {
    for (const ClientSetting& setting : settings) {
      SetCompatibleValue(&overrides, setting, kept_overrides);
    }
  };
  if (!policy.multithreaded_rendering &&
      policy.physics_worker_mode == PhysicsWorkerMode::kAuto) {
    // Ultra-low-end: apply latency-like optimizations even in auto mode
    const std::array<ClientSetting, 5> physics_settings = {{
        {"DFIntSimMidPhaseContactPipelineBatchSize", "128"},
        {"FIntPhysicsMaxSubsteps", "2"},
        {"FIntPhysicsMaxConstraintIterations", "2"},
        {"FFlagPhysicsThrottleWhenPaused", "True"},
        {"FFlagPhysicsThrottleWhenHidden", "True"},
    }};
    apply_settings(physics_settings);
    *merged_json = overrides.dump();
    return true;
  }
  const bool throughput_mode =
      policy.physics_worker_mode == PhysicsWorkerMode::kThroughput;
  if (policy.physics_worker_mode == PhysicsWorkerMode::kLatency) {
    const std::array<ClientSetting, 5> physics_settings = {{
        {"DFIntSimMidPhaseContactPipelineBatchSize", "128"},
        // Ultra-low-end: reduce physics substeps and iterations for 2-core CPU
        {"FIntPhysicsMaxSubsteps", "2"},
        {"FIntPhysicsMaxConstraintIterations", "2"},
        {"FFlagPhysicsThrottleWhenPaused", "True"},
        {"FFlagPhysicsThrottleWhenHidden", "True"},
    }};
    apply_settings(physics_settings);
  }
  int render_worker_count = 0;
  if (throughput_mode ||
      (policy.multithreaded_rendering &&
       policy.physics_worker_mode != PhysicsWorkerMode::kLatency)) {
    const int available_workers = policy.physical_core_count > 0
                                      ? policy.physical_core_count
                                      : DetectAvailablePhysicalCoreCount();
    // Ultra-low-end: cap at 1 worker for 2-core CPU
    render_worker_count = std::min(1, std::max(1, available_workers));
  }
  if (throughput_mode) {
    // Ultra-low-end: 2C/2T CPU - keep at 1 worker, minimize threads
    render_worker_count = CalculateThroughputWorkerCount(render_worker_count);
    const std::string workers = std::to_string(render_worker_count);
    const std::string async_minimum = "1";
    const std::array<ClientSetting, 8> physics_settings = {{
        {"FIntTaskSchedulerThreadMin", "0"},
        {"FIntTaskSchedulerAsyncTasksMinimumThreadCount", async_minimum},
        {"FIntTaskSchedulerAutoThreadLimit", workers},
        {"DFIntSimMidPhaseContactPipelineBatchSize", "128"},
        // Ultra-low-end physics
        {"FIntPhysicsMaxSubsteps", "2"},
        {"FIntPhysicsMaxConstraintIterations", "2"},
        {"FFlagPhysicsThrottleWhenPaused", "True"},
        {"FFlagPhysicsThrottleWhenHidden", "True"},
    }};
    apply_settings(physics_settings);
    } else if (policy.multithreaded_rendering &&
             policy.physics_worker_mode != PhysicsWorkerMode::kLatency) {
    // Auto mode: ultra-low-end optimized (same as throughput for 2-core)
    const std::string workers = "1";
    const std::array<ClientSetting, 4> scheduler_settings = {{
        {"FIntTaskSchedulerThreadMin", "0"},
        {"FIntTaskSchedulerAsyncTasksMinimumThreadCount", "1"},
        {"FIntTaskSchedulerAutoThreadLimit", "1"},
        {"DFIntSimMidPhaseContactPipelineBatchSize", "128"},
    }};
    apply_settings(scheduler_settings);
  }
  if ((policy.multithreaded_rendering || throughput_mode) &&
      policy.physics_worker_mode != PhysicsWorkerMode::kLatency) {
    const std::string workers = std::to_string(render_worker_count);
    const std::string occlusion_workers =
        std::to_string(std::max(1, render_worker_count / 2));
    const std::uint64_t host_memory_bytes = DetectHostMemoryBytes();
    // Hardcoded: pin the graphics-quality override to the lowest level on
    // low-memory hosts. There is no environment override.
    const std::string quality_str =
        host_memory_bytes < 8ULL * 1024U * 1024U * 1024U ? "1" : "3";
    const bool manual_quality =
        quality_str == "auto" || quality_str == "0" || quality_str == "manual";

    // DEEP OPTIMIZATION: Auto-detect ultra-low-end hardware
    const bool is_ultra_low_end = host_memory_bytes < 4ULL * 1024U * 1024U * 1024U;
    const bool is_low_end = host_memory_bytes < 8ULL * 1024U * 1024U * 1024U;

    // ForceCacheSize settings are byte counts. Let Roblox size its mesh and
    // SLIM content caches; values like 256/128 would cap them to a few bytes.
    // ULTRA-AGGRESSIVE settings for Stoney Ridge / 2-core integrated graphics
    const std::array<ClientSetting, 194> rendering_settings = {{
        {"FIntSmoothClusterTaskQueueMaxParallelTasks", workers},
        {"FIntOcclusionWorkerThreadCount", occlusion_workers},
        {"FFlagMovePrerenderV2", "True"},
        {"FFlagGcInParallelWithRenderPrepare3", "True"},
        // MAXIMUM AGGRESSIVE mip bias - reduces texture memory 87%+ (each level = 4x reduction)
        // Level 5 = 1/1024 texture size, acceptable at 1366x768 with compression
        {"FIntRenderTextureMipBias", is_ultra_low_end ? "5" : (is_low_end ? "4" : "3")},
        // FORCE the lowest in-game texture quality through FastFlags.
        // TextureQualityOverride: 0 = automatic, 1 = lowest, 3 = highest.
        {"DFFlagTextureQualityOverrideEnabled", "True"},
        {"DFIntTextureQualityOverride", "1"},
        // Pin Roblox's own graphics-quality slider to level 1 so the client
        // never re-raises texture/render quality at runtime.
        {"FIntDebugFRMQualityLevelOverride", "1"},
        {"FFlagSimRuntimeContentTranscodeBlockingCall", "False"},
        {"FFlagRenderEnableLowEndLOD", "True"},
        {"FIntTerrainArraySliceSize", "1"},
        {"FFlagFastGPULightGrid", "True"},
        {"FFlagRenderOptimizeLightGrid", "True"},
        {"FFlagRenderFixParticlesCulling", "True"},
        {"FFlagLuauIncrementalGC", "True"},
        {"FIntLuauGcStepMultiplier", "400"},
        {"FIntLuauGcGoalRatio", "400"},
        {"FIntAssetProviderThreads", "1"},
        {"FFlagUITextureCompressionDesktop", "True"},
        {"FFlagTCTextureCompressionDesktop", "True"},
        {"FFlagUITextureUncompressed", "False"},
        {"FFlagGpuVoxelCompression", "True"},
        {"FFlagMeshCompression", "True"},
        {"FFlagPhysicsMeshCompression", "True"},
        {"FFlagSupportMeshLOD", "True"},
        {"FFlagMeshLOD", "True"},
        {"FFlagMeshLodApplyGeomMeshOptimizer", "True"},
        {"FFlagAdaptiveMeshCacheSizeForAndroid", "True"},
        {"FIntDefaultMeshCacheSizeMB", is_ultra_low_end ? "4" : "8"},
        {"FIntRenderMeshOrphanBudgetMB", "1"},
        {"FIntTerrainGpuMeshMemoryTargetMib", is_ultra_low_end ? "2" : "4"},
        {"FIntAvatarMeshMemoryMax", is_ultra_low_end ? "2097152" : "4194304"},
        {"FIntSmoothTerrainPhysicsCacheSize", is_ultra_low_end ? "2" : "4"},
        // MINIMUM texture budgets for integrated graphics
        {"FIntRenderTextureTotalBudgetMB", is_ultra_low_end ? "8" : (is_low_end ? "16" : "32")},
        {"FIntRenderTextureOrphanBudgetMB", "1"},
        {"FFlagDebugTerrainVTCompressedTextures", "True"},
        {"FFlagStreamingObserverMemoryOptimization", "True"},
        {"FIntNetworkStreamingGCMaxMicroSecondLimit", "1000"},
        {"FFlagfeatureFastClusterCacheEnabled", "True"},
        {"FFlagEnableMultiVETextureManagerSharing", "True"},
        {"FFlagVulkanCompressedTextureAliasing", "True"},
        {"FFlagCSGPublishDedupSolidMesh", "True"},
        {"FFlagDeduplicateLodRemovalRegions", "True"},
        {"FFlagCurlUsePools", "True"},
        {"FIntRenderTextureCompositorBudget", is_ultra_low_end ? "2" : "4"},
        {"FIntTextureCompositorActiveJobs", "1"},
        {"FIntTextureCompositorLowResFactor", "16"},
        {"FIntTextureCompositorMeshCache", is_ultra_low_end ? "1" : "2"},
        {"FFlagContentProviderMmapAssets", "True"},
        {"FIntAssetProviderAssetCacheReadThreadCount", "1"},
        {"FIntAssetProviderAssetCacheWriteThreadCount", "1"},
        {"FIntInitialAudioAssetCacheSize", is_ultra_low_end ? "2" : "4"},
        {"FIntAvatarTextureMemoryMax", is_ultra_low_end ? "1048576" : "2097152"},
        {"FFlagEnableSLIMAvatars", "True"},
        {"FFlagEnableSlimAvatarsDefaultEnabled", "True"},
        {"FFlagLayeredClothingCacheOptimizations", "True"},
        {"FFlagRenderAllocateShadowMapResourcesOnDemand", "True"},
        {"FIntRenderShadowMapDepthCacheMemLimit", is_ultra_low_end ? "1" : "2"},
        {"FIntTM2ShadowMapMaxMips", "0"},
        // Force 1x MSAA - critical for integrated graphics
        {"FIntDebugForceMSAASamples", "1"},
        {"FFlagLuauStartupGcSuppression", "True"},
        {"FIntLuauGcStepMul", "400"},
        {"FIntLuauGcGoalCore", "150"},
        {"FIntLuauGcStartupWorkingSetMb", is_ultra_low_end ? "8" : "16"},
        {"FIntLuaGcMaxKb", "32768"},
        {"FIntTaskSchedulerMaxTempArenaSizeBytes", "4194304"},
        {"FFlagLocalStorageArenaOptimization", "True"},
        {"FIntProjectedMaxBytesUsedForSoundsMB", is_ultra_low_end ? "4" : "8"},
        {"FIntAudioMetadataCacheSizeKB", "1024"},
        {"FIntDefaultAudioDecodeBufferSizeMs", "100"},
        {"FIntMaxAudibleSoundChannels", is_ultra_low_end ? "4" : "8"},
        // Minimize task scheduler threads for 2-core CPU
        {"FIntTaskSchedulerThreadMin", "0"},
        {"FIntTaskSchedulerAsyncTasksMinimumThreadCount", "1"},
        {"FIntTaskSchedulerAutoThreadLimit", "1"},
        // Reduce physics overhead aggressively
        {"FIntPhysicsMaxSubsteps", "2"},
        {"FIntPhysicsMaxConstraintIterations", "2"},
        {"FFlagPhysicsThrottleWhenPaused", "True"},
        {"FFlagPhysicsThrottleWhenHidden", "True"},
        {"FFlagPhysicsThrottleWhenUnfocused", "True"},
        // Reduce particle/emitter overhead aggressively
        {"FIntMaxParticles", "50"},
        {"FIntMaxParticleEmitters", "4"},
        {"FFlagParticleThrottle", "True"},
        {"FFlagParticleThrottleWhenPaused", "True"},
        {"FFlagParticleThrottleWhenHidden", "True"},
        {"FFlagParticleThrottleWhenUnfocused", "True"},
        // Disable heavy Roblox systems
        {"FFlagDisableClothingSimulation", "True"},
        {"FFlagDisableHairSimulation", "True"},
        {"FFlagDisableAccessorySimulation", "True"},
        {"FFlagDisableFacialAnimation", "True"},
        {"FFlagDisableChatBubbles", "True"},
        {"FFlagDisableNameTags", "True"},
        {"FFlagDisableHealthBars", "True"},
        {"FFlagDisableDistanceCulling", "False"},
        {"FFlagDisableFrustumCulling", "False"},
        {"FFlagDisableOcclusionCulling", "False"},
        // Reduce UI overhead
        {"FFlagUIThrottleWhenHidden", "True"},
        {"FFlagUIThrottleWhenBackground", "True"},
        {"FIntUIMaxRenderCallsPerFrame", "8"},
        {"FFlagUIThrottleAnimations", "True"},
        {"FFlagUIThrottleWhenUnfocused", "True"},
        // Reduce network overhead aggressively
        {"FIntNetworkTickRate", "15"},
        {"FFlagNetworkThrottleWhenBackground", "True"},
        {"FFlagNetworkThrottleWhenUnfocused", "True"},
        {"FIntNetworkMaxPacketSize", "512"},
        // Disable analytics/telemetry services
        {"FFlagDisableTelemetryService", "True"},
        {"FFlagDisableAnalyticsService", "True"},
        {"FFlagDisableCrashReportingService", "True"},
        {"FFlagDisableEventIngestService", "True"},
        {"FFlagDisablePerformanceStatsService", "True"},
        // Reduce asset streaming aggressively
        {"FFlagAssetStreamingThrottle", "True"},
        {"FIntAssetStreamingMaxConcurrentRequests", "1"},
        {"FIntAssetStreamingMaxBandwidthKBps", "256"},
        // Disable avatar heavy features
        {"FFlagDisableAvatarClothing", "True"},
        {"FFlagDisableAvatarAccessories", "True"},
        {"FFlagDisableAvatarEmotes", "True"},
        {"FFlagDisableAvatarChat", "True"},
        {"FFlagDisableAvatarAnimations", "True"},
        {"FFlagDisableAvatarScaling", "True"},
        // Reduce memory pressure
        {"FFlagMemoryPressureThrottle", "True"},
        {"FIntMemoryPressureThresholdMB", "128"},
        {"FFlagMemoryPressureAggressiveGC", "True"},
        // Reduce input polling
        {"FIntInputPollRateHz", "20"},
        // Disable background tasks aggressively
        {"FFlagDisableBackgroundTasks", "True"},
        {"FFlagDisableIdleTasks", "True"},
        {"FFlagDisablePeriodicTasks", "True"},
        {"FFlagDisableHousekeepingTasks", "True"},
        // Reduce render thread work
        {"FIntRenderMaxCommandBuffers", "1"},
        {"FFlagRenderSkipRedundantCommands", "True"},
        {"FFlagRenderMergeDrawCalls", "True"},
        {"FFlagRenderThrottleWhenUnfocused", "True"},
        // ULTRA-AGGRESSIVE rendering optimizations - DISABLE expensive features
        {"FFlagRenderForceDeferredLighting", "False"},
        {"FFlagRenderDeferredShading", "False"},
        {"FFlagRenderTileBasedDeferred", "False"},
        {"FFlagVulkanEnableRenderPassOptimization", "True"},
        {"FFlagVulkanUseTimelineSemaphores", "True"},
        {"FFlagTextureManager2Enabled", "True"},
        {"FFlagTextureManager2AsyncUpload", "True"},
        {"FFlagTextureManager2MipGen", "True"},
        {"FFlagRenderGPUAsyncCompute", "False"},
        {"FFlagRenderParallelBatchSubmission", "False"},
        {"FFlagRenderParallelWorkSubmission", "False"},
        {"FIntRenderMaxDrawCallsPerBatch", "32"},
        {"FIntRenderMaxVerticesPerBatch", "16384"},
        {"FFlagRenderCullTransparentObjects", "True"},
        {"FFlagRenderEarlyZPass", "True"},
        {"FFlagRenderHiZCulling", "True"},
        {"FFlagRenderOcclusionQuery", "True"},
        {"FIntRenderOcclusionQueryFrameDelay", "8"},
        {"FFlagRenderFrustumCulling", "True"},
        {"FFlagRenderPortalCulling", "True"},
        {"FFlagRenderDistanceCulling", "True"},
        {"FIntRenderMaxLightsPerTile", "4"},
        {"FIntRenderMaxShadowsPerTile", "2"},
        {"FFlagRenderVariableRateShading", "False"},
        {"FFlagRenderMeshShader", "False"},
        {"FFlagRenderTaskShader", "False"},
        // Additional ultra-low-end optimizations
        {"FFlagRenderDisableSpecular", "True"},
        {"FFlagRenderDisableReflections", "True"},
        {"FFlagRenderDisableShadows", "True"},
        {"FFlagRenderDisablePostProcessing", "True"},
        {"FFlagRenderDisableBloom", "True"},
        {"FFlagRenderDisableMotionBlur", "True"},
        {"FFlagRenderDisableSSAO", "True"},
        {"FFlagRenderDisableVolumetricLighting", "True"},
        {"FFlagRenderDisableParticleLighting", "True"},
        // DISABLE HEAVY/MODERN LIGHTING FEATURES
        {"FFlagFutureIsBright", "False"},
        {"FFlagFutureIsBrightShadows", "False"},
        {"FFlagFutureIsBrightPhase2", "False"},
        {"FFlagLightGrid", "False"},
        {"FFlagVoxelLighting", "False"},
        {"FFlagDynamicLighting", "False"},
        {"FFlagGlobalIllumination", "False"},
        {"FFlagAmbientOcclusion", "False"},
        {"FFlagScreenSpaceReflections", "False"},
        {"FFlagScreenSpaceAmbientOcclusion", "False"},
        {"FFlagGodRays", "False"},
        {"FFlagLightShafts", "False"},
        {"FFlagCrepuscularRays", "False"},
        {"FFlagSubsurfaceScattering", "False"},
        {"FFlagAtmosphericEffects", "False"},
        {"FFlagEnvironmentLighting", "False"},
        {"FFlagHDR", "False"},
        {"FFlagEyeAdaptation", "False"},
        {"FFlagToneMapping", "False"},
        {"FFlagColorCorrection", "False"},
        {"FFlagVignette", "False"},
        {"FFlagDepthOfField", "False"},
        {"FFlagChromaticAberration", "False"},
        {"FFlagLensFlare", "False"},
        {"FFlagDebugSkyGray", "True"},
        {"FFlagRenderForceForwardRendering", "True"},
        {"FFlagRenderMinimizeStateChanges", "True"},
        {"FFlagRenderBatchSimilarObjects", "True"},
        {"FFlagRenderUseInstancing", "True"},
    }};
    apply_settings(rendering_settings);
    // The main ultra_low_memory_settings block below handles ultra-low-end overrides
    if (!manual_quality) {
      SetCompatibleValue(&overrides,
                         {"FIntDebugFRMQualityLevelOverride", quality_str},
                         kept_overrides);
    }
  }

  // ALWAYS apply ultra-low-end rendering optimizations (DEFAULT, no env needed)
  // Critical for Stoney Ridge / 2-core integrated graphics (AMD A4-9125, Radeon R3)
  // Hardcoded absolute minimum. Every host gets the lowest texture, mesh and
  // audio-cache budgets, so the documented defaults are the real defaults.
  // This fork does not tier these values by installed RAM.
  const bool is_ultra_low_end = true;
  const bool is_low_end = true;

  // DEFAULT to ultra-low graphics (1) - no env var needed
  const std::string quality_str = "1";
  // const bool manual_quality = quality_str == "auto" || quality_str == "0" || quality_str == "manual"; // reserved for future use

  // ForceCacheSize settings are byte counts. Let Roblox size its mesh and
  // SLIM content caches; values like 256/128 would cap them to a few bytes.
  // ULTRA-AGGRESSIVE settings for Stoney Ridge / 2-core integrated graphics
  const std::string workers = std::to_string(std::max(1, render_worker_count));
  const std::string occlusion_workers =
      std::to_string(std::max(1, (render_worker_count > 0 ? render_worker_count : 1) / 2));
  const std::array<ClientSetting, 374> rendering_settings = {{
      {"FIntSmoothClusterTaskQueueMaxParallelTasks", workers},
      {"FIntOcclusionWorkerThreadCount", occlusion_workers},
      {"FFlagMovePrerenderV2", "True"},
      {"FFlagGcInParallelWithRenderPrepare3", "True"},
      // MAXIMUM AGGRESSIVE mip bias - reduces texture memory 87%+ (each level = 4x reduction)
      // Level 5 = 1/1024 texture size, acceptable at 1366x768 with compression
      {"FIntRenderTextureMipBias", is_ultra_low_end ? "5" : (is_low_end ? "4" : "3")},
      // FORCE the lowest in-game texture quality through FastFlags.
      // TextureQualityOverride: 0 = automatic, 1 = lowest, 3 = highest.
      {"DFFlagTextureQualityOverrideEnabled", "True"},
      {"DFIntTextureQualityOverride", "1"},
      // Pin Roblox's own graphics-quality slider to level 1 so the client
      // never re-raises texture/render quality at runtime.
      {"FIntDebugFRMQualityLevelOverride", "1"},
      {"FFlagSimRuntimeContentTranscodeBlockingCall", "False"},
      {"FFlagRenderEnableLowEndLOD", "True"},
      {"FIntTerrainArraySliceSize", "1"},
      {"FFlagFastGPULightGrid", "True"},
      {"FFlagRenderOptimizeLightGrid", "True"},
      {"FFlagRenderFixParticlesCulling", "True"},
      {"FFlagLuauIncrementalGC", "True"},
      {"FIntLuauGcStepMultiplier", "300"},
      {"FIntLuauGcGoalRatio", "400"},
      {"FIntAssetProviderThreads", "1"},
      {"FFlagUITextureCompressionDesktop", "True"},
      {"FFlagTCTextureCompressionDesktop", "True"},
      {"FFlagUITextureUncompressed", "False"},
      {"FFlagGpuVoxelCompression", "True"},
      {"FFlagMeshCompression", "True"},
      {"FFlagPhysicsMeshCompression", "True"},
      {"FFlagSupportMeshLOD", "True"},
      {"FFlagMeshLOD", "True"},
      {"FFlagMeshLodApplyGeomMeshOptimizer", "True"},
      {"FFlagAdaptiveMeshCacheSizeForAndroid", "True"},
      {"FIntDefaultMeshCacheSizeMB", is_ultra_low_end ? "8" : "16"},
      {"FIntRenderMeshOrphanBudgetMB", "1"},
      {"FIntTerrainGpuMeshMemoryTargetMib", is_ultra_low_end ? "4" : "8"},
      {"FIntAvatarMeshMemoryMax", is_ultra_low_end ? "4194304" : "8388608"},
      {"FIntSmoothTerrainPhysicsCacheSize", is_ultra_low_end ? "4" : "8"},
      // MINIMUM texture budgets for integrated graphics
      {"FIntRenderTextureTotalBudgetMB", is_ultra_low_end ? "16" : (is_low_end ? "32" : "48")},
      {"FIntRenderTextureOrphanBudgetMB", "1"},
      {"FFlagDebugTerrainVTCompressedTextures", "True"},
      {"FFlagStreamingObserverMemoryOptimization", "True"},
      {"FIntNetworkStreamingGCMaxMicroSecondLimit", "1000"},
      {"FFlagfeatureFastClusterCacheEnabled", "True"},
      {"FFlagEnableMultiVETextureManagerSharing", "True"},
      {"FFlagVulkanCompressedTextureAliasing", "True"},
      {"FFlagCSGPublishDedupSolidMesh", "True"},
      {"FFlagDeduplicateLodRemovalRegions", "True"},
      {"FFlagCurlUsePools", "True"},
      {"FIntRenderTextureCompositorBudget", is_ultra_low_end ? "4" : "8"},
      {"FIntTextureCompositorActiveJobs", "1"},
      {"FIntTextureCompositorLowResFactor", "8"},
      {"FIntTextureCompositorMeshCache", is_ultra_low_end ? "2" : "4"},
      {"FFlagContentProviderMmapAssets", "True"},
      {"FIntAssetProviderAssetCacheReadThreadCount", "1"},
      {"FIntAssetProviderAssetCacheWriteThreadCount", "1"},
      {"FIntInitialAudioAssetCacheSize", is_ultra_low_end ? "4" : "8"},
      {"FIntAvatarTextureMemoryMax", is_ultra_low_end ? "4194304" : "8388608"},
      {"FFlagEnableSLIMAvatars", "True"},
      {"FFlagEnableSlimAvatarsDefaultEnabled", "True"},
      {"FFlagLayeredClothingCacheOptimizations", "True"},
      {"FFlagRenderAllocateShadowMapResourcesOnDemand", "True"},
      {"FIntRenderShadowMapDepthCacheMemLimit", is_ultra_low_end ? "2" : "4"},
      {"FIntTM2ShadowMapMaxMips", "0"},
      // Force 1x MSAA - critical for integrated graphics
      {"FIntDebugForceMSAASamples", "1"},
      {"FFlagLuauStartupGcSuppression", "True"},
      {"FIntLuauGcStepMul", "400"},
      {"FIntLuauGcGoalCore", "200"},
      {"FIntLuauGcStartupWorkingSetMb", is_ultra_low_end ? "32" : "64"},
      {"FIntLuaGcMaxKb", "131072"},
      {"FIntTaskSchedulerMaxTempArenaSizeBytes", "8388608"},
      {"FFlagLocalStorageArenaOptimization", "True"},
      {"FIntProjectedMaxBytesUsedForSoundsMB", is_ultra_low_end ? "8" : "16"},
      {"FIntAudioMetadataCacheSizeKB", "1024"},
      {"FIntDefaultAudioDecodeBufferSizeMs", "100"},
      {"FIntMaxAudibleSoundChannels", is_ultra_low_end ? "8" : "16"},
      // Balanced CPU/GPU REDUCTION SETTINGS (no FPS cap - let game run at natural rate)
      // Balanced GC settings - reduce pause spikes without excessive per-frame work
      {"FIntLuauGcStepMultiplier", "300"},
      {"FIntLuauGcGoalRatio", "300"},
      {"FIntLuauGcGoalCore", "200"},
      {"FIntLuauGcStartupWorkingSetMb", is_ultra_low_end ? "16" : "32"},
      {"FIntLuaGcMaxKb", "65536"},
      {"FFlagLuauIncrementalGC", "True"},
      {"FFlagLuauStartupGcSuppression", "True"},
      // Minimize task scheduler threads for 2-core CPU
      {"FIntTaskSchedulerThreadMin", "0"},
      {"FIntTaskSchedulerAsyncTasksMinimumThreadCount", "1"},
      {"FIntTaskSchedulerAutoThreadLimit", "1"},
      // Reduce physics overhead aggressively
      {"FIntPhysicsMaxSubsteps", "2"},
      {"FIntPhysicsMaxConstraintIterations", "2"},
      {"FFlagPhysicsThrottleWhenPaused", "True"},
      {"FFlagPhysicsThrottleWhenHidden", "True"},
      {"FFlagPhysicsThrottleWhenUnfocused", "True"},
      // Reduce audio overhead
      {"FIntAudioDSPBufferSize", "8192"},
      {"FFlagAudioUseNewApi", "False"},
      {"FFlagAudioEnableLowLatency", "False"},
      {"FFlagAudioThrottleWhenBackground", "True"},
      // Reduce particle/emitter overhead aggressively
      {"FIntMaxParticles", "200"},
      {"FIntMaxParticleEmitters", "8"},
      {"FFlagParticleThrottle", "True"},
      {"FFlagParticleThrottleWhenPaused", "True"},
      {"FFlagParticleThrottleWhenHidden", "True"},
      // Disable heavy Roblox systems
      {"FFlagDisableClothingSimulation", "True"},
      {"FFlagDisableHairSimulation", "True"},
      {"FFlagDisableAccessorySimulation", "True"},
      {"FFlagDisableFacialAnimation", "True"},
      {"FFlagDisableChatBubbles", "True"},
      {"FFlagDisableNameTags", "True"},
      {"FFlagDisableHealthBars", "True"},
      {"FFlagDisableDistanceCulling", "False"},
      {"FFlagDisableFrustumCulling", "False"},
      {"FFlagDisableOcclusionCulling", "False"},
      // Reduce UI overhead
      {"FFlagUIThrottleWhenHidden", "True"},
      {"FFlagUIThrottleWhenBackground", "True"},
      {"FIntUIMaxRenderCallsPerFrame", "16"},
      {"FFlagUIThrottleAnimations", "True"},
      // Reduce network overhead aggressively
      {"FIntNetworkTickRate", "20"},
      {"FFlagNetworkThrottleWhenBackground", "True"},
      {"FFlagNetworkThrottleWhenUnfocused", "True"},
      {"FIntNetworkMaxPacketSize", "1024"},
      // Disable analytics/telemetry services
      {"FFlagDisableTelemetryService", "True"},
      {"FFlagDisableAnalyticsService", "True"},
      {"FFlagDisableCrashReportingService", "True"},
      {"FFlagDisableEventIngestService", "True"},
      {"FFlagDisablePerformanceStatsService", "True"},
      // Reduce asset streaming aggressively
      {"FFlagAssetStreamingThrottle", "True"},
      {"FIntAssetStreamingMaxConcurrentRequests", "1"},
      {"FIntAssetStreamingMaxBandwidthKBps", "512"},
      // Disable avatar heavy features
      {"FFlagDisableAvatarClothing", "True"},
      {"FFlagDisableAvatarAccessories", "True"},
      {"FFlagDisableAvatarEmotes", "True"},
      {"FFlagDisableAvatarChat", "True"},
      {"FFlagDisableAvatarAnimations", "True"},
      {"FFlagDisableAvatarScaling", "True"},
      // Reduce memory pressure
      {"FFlagMemoryPressureThrottle", "True"},
      {"FIntMemoryPressureThresholdMB", "256"},
      {"FFlagMemoryPressureAggressiveGC", "True"},
      // Reduce input polling
      {"FIntInputPollRateHz", "30"},
      // Disable background tasks aggressively
      {"FFlagDisableBackgroundTasks", "True"},
      {"FFlagDisableIdleTasks", "True"},
      {"FFlagDisablePeriodicTasks", "True"},
      {"FFlagDisableHousekeepingTasks", "True"},
      // Reduce render thread work
      {"FIntRenderMaxCommandBuffers", "1"},
      {"FFlagRenderSkipRedundantCommands", "True"},
      {"FFlagRenderMergeDrawCalls", "True"},
      {"FFlagRenderThrottleWhenUnfocused", "True"},
      // ULTRA-AGGRESSIVE rendering optimizations - DISABLE expensive features
      {"FFlagRenderForceDeferredLighting", "False"},
      {"FFlagRenderDeferredShading", "False"},
      {"FFlagRenderTileBasedDeferred", "False"},
      {"FFlagVulkanEnableRenderPassOptimization", "True"},
      {"FFlagVulkanUseTimelineSemaphores", "True"},
      {"FFlagTextureManager2Enabled", "True"},
      {"FFlagTextureManager2AsyncUpload", "True"},
      {"FFlagTextureManager2MipGen", "True"},
      {"FFlagRenderGPUAsyncCompute", "False"},
      {"FFlagRenderParallelBatchSubmission", "False"},
      {"FFlagRenderParallelWorkSubmission", "False"},
      {"FIntRenderMaxDrawCallsPerBatch", "64"},
      {"FIntRenderMaxVerticesPerBatch", "32768"},
      {"FFlagRenderCullTransparentObjects", "True"},
      {"FFlagRenderEarlyZPass", "True"},
      {"FFlagRenderHiZCulling", "True"},
      {"FFlagRenderOcclusionQuery", "True"},
      {"FIntRenderOcclusionQueryFrameDelay", "4"},
      {"FFlagRenderFrustumCulling", "True"},
      {"FFlagRenderPortalCulling", "True"},
      {"FFlagRenderDistanceCulling", "True"},
      {"FIntRenderMaxLightsPerTile", "4"},
      {"FIntRenderMaxShadowsPerTile", "2"},
      {"FFlagRenderVariableRateShading", "False"},
      {"FFlagRenderMeshShader", "False"},
      {"FFlagRenderTaskShader", "False"},
      // Additional ultra-low-end optimizations
      {"FFlagRenderDisableSpecular", "True"},
      {"FFlagRenderDisableReflections", "True"},
      {"FFlagRenderDisableShadows", "True"},
      {"FFlagRenderDisablePostProcessing", "True"},
      {"FFlagRenderDisableBloom", "True"},
      {"FFlagRenderDisableMotionBlur", "True"},
      {"FFlagRenderDisableSSAO", "True"},
      {"FFlagRenderDisableVolumetricLighting", "True"},
      {"FFlagRenderDisableParticleLighting", "True"},
      // DISABLE HEAVY/MODERN LIGHTING FEATURES
      {"FFlagFutureIsBright", "False"},
      {"FFlagFutureIsBrightShadows", "False"},
      {"FFlagFutureIsBrightPhase2", "False"},
      {"FFlagLightGrid", "False"},
      {"FFlagVoxelLighting", "False"},
      {"FFlagDynamicLighting", "False"},
      {"FFlagGlobalIllumination", "False"},
      {"FFlagAmbientOcclusion", "False"},
      {"FFlagScreenSpaceReflections", "False"},
      {"FFlagScreenSpaceAmbientOcclusion", "False"},
      {"FFlagGodRays", "False"},
      {"FFlagLightShafts", "False"},
      {"FFlagCrepuscularRays", "False"},
      {"FFlagSubsurfaceScattering", "False"},
      {"FFlagAtmosphericEffects", "False"},
      {"FFlagEnvironmentLighting", "False"},
      {"FFlagHDR", "False"},
      {"FFlagEyeAdaptation", "False"},
      {"FFlagToneMapping", "False"},
      {"FFlagColorCorrection", "False"},
      {"FFlagVignette", "False"},
      {"FFlagDepthOfField", "False"},
      {"FFlagChromaticAberration", "False"},
      {"FFlagLensFlare", "False"},
      {"FFlagRenderDisableSpecular", "True"},
      {"FFlagRenderDisableReflections", "True"},
      {"FFlagRenderDisableShadows", "True"},
      {"FFlagRenderDisablePostProcessing", "True"},
      {"FFlagRenderDisableBloom", "True"},
      {"FFlagRenderDisableMotionBlur", "True"},
      {"FFlagRenderDisableSSAO", "True"},
      {"FFlagRenderDisableVolumetricLighting", "True"},
      {"FFlagRenderDisableParticleLighting", "True"},
      {"FFlagRenderForceForwardRendering", "True"},
      {"FFlagRenderMinimizeStateChanges", "True"},
      {"FFlagRenderBatchSimilarObjects", "True"},
      {"FFlagRenderUseInstancing", "True"},
      // === EVEN MORE AGGRESSIVE ULTRA-LOW-END OPTIMIZATIONS ===
      // Terrain: disable grass, reduce detail to absolute minimum
      {"FIntFRMMinGrassDistance", "0"},
      {"FIntFRMMaxGrassDistance", "0"},
      {"FIntRenderGrassDetailStrands", "0"},
      {"FIntRenderGrassHeightScaler", "0"},
      // Terrain material: force lowest quality
      {"FIntTerrainArraySliceSize", "1"},
      // Particles: reduce to absolute minimum
      {"FIntMaxParticles", "50"},
      {"FIntMaxParticleEmitters", "4"},
      {"FFlagParticleThrottleWhenUnfocused", "True"},
      // Avatar: disable ALL non-essential features
      {"FFlagDisableAvatarClothing", "True"},
      {"FFlagDisableAvatarAccessories", "True"},
      {"FFlagDisableAvatarEmotes", "True"},
      {"FFlagDisableAvatarChat", "True"},
      {"FFlagDisableAvatarAnimations", "True"},
      {"FFlagDisableAvatarScaling", "True"},
      {"FFlagDisableNameTags", "True"},
      {"FFlagDisableHealthBars", "True"},
      {"FFlagDisableChatBubbles", "True"},
      {"FFlagDisableFacialAnimation", "True"},
      // Clothing/Hair simulation: already disabled, ensure
      {"FFlagDisableClothingSimulation", "True"},
      {"FFlagDisableHairSimulation", "True"},
      {"FFlagDisableAccessorySimulation", "True"},
      // Physics: throttle even more aggressively
      {"FFlagPhysicsThrottleWhenUnfocused", "True"},
      {"FIntPhysicsMaxSubsteps", "2"},
      {"FIntPhysicsMaxConstraintIterations", "2"},
      {"DFIntSimMidPhaseContactPipelineBatchSize", "64"},
      // Network: absolute minimum
      {"FIntNetworkTickRate", "15"},
      {"FIntNetworkMaxPacketSize", "512"},
      {"FFlagNetworkThrottleWhenBackground", "True"},
      {"FFlagNetworkThrottleWhenUnfocused", "True"},
      {"FFlagNetworkThrottleWhenPaused", "True"},
      // Audio: minimal
      {"FIntMaxAudibleSoundChannels", "4"},
      {"FIntAudioDSPBufferSize", "16384"},
      {"FFlagAudioEnableLowLatency", "False"},
      {"FFlagAudioThrottleWhenBackground", "True"},
      {"FFlagAudioThrottleWhenUnfocused", "True"},
      // UI: absolute minimum
      {"FIntUIMaxRenderCallsPerFrame", "8"},
      {"FFlagUIThrottleWhenHidden", "True"},
      {"FFlagUIThrottleWhenBackground", "True"},
      {"FFlagUIThrottleAnimations", "True"},
      {"FFlagUIThrottleWhenUnfocused", "True"},
      // Asset streaming: absolute minimum
      {"FIntAssetStreamingMaxConcurrentRequests", "1"},
      {"FIntAssetStreamingMaxBandwidthKBps", "256"},
      {"FFlagAssetStreamingThrottle", "True"},
      {"FFlagAssetStreamingThrottleWhenBackground", "True"},
      // Memory pressure: aggressive
      {"FIntMemoryPressureThresholdMB", "128"},
      {"FFlagMemoryPressureAggressiveGC", "True"},
      {"FFlagMemoryPressureThrottle", "True"},
      // Background tasks: disable ALL
      {"FFlagDisableBackgroundTasks", "True"},
      {"FFlagDisableIdleTasks", "True"},
      {"FFlagDisablePeriodicTasks", "True"},
      {"FFlagDisableHousekeepingTasks", "True"},
      // Scheduler: absolute minimum threads
      {"FIntTaskSchedulerThreadMin", "0"},
      {"FIntTaskSchedulerAsyncTasksMinimumThreadCount", "1"},
      {"FIntTaskSchedulerAutoThreadLimit", "1"},
      {"FIntTaskSchedulerMaxTempArenaSizeBytes", "4194304"},
      // GC: even more aggressive for 2-core
      {"FIntLuauGcStepMultiplier", "400"},
      {"FIntLuauGcGoalRatio", "400"},
      {"FIntLuauGcGoalCore", "150"},
      {"FIntLuauGcStartupWorkingSetMb", is_ultra_low_end ? "8" : "16"},
      {"FIntLuaGcMaxKb", "32768"},
      {"FFlagLuauIncrementalGC", "True"},
      {"FFlagLuauStartupGcSuppression", "True"},
      // Render thread: absolute minimum
      {"FIntRenderMaxCommandBuffers", "1"},
      {"FFlagRenderSkipRedundantCommands", "True"},
      {"FFlagRenderMergeDrawCalls", "True"},
      {"FFlagRenderThrottleWhenUnfocused", "True"},
      {"FIntRenderMaxDrawCallsPerBatch", "32"},
      {"FIntRenderMaxVerticesPerBatch", "16384"},
      // Shadow: disable completely
      {"FIntRenderShadowIntensity", "0"},
      {"FIntRenderShadowMapDepthCacheMemLimit", "1"},
      {"FIntTM2ShadowMapMaxMips", "0"},
      {"FFlagRenderAllocateShadowMapResourcesOnDemand", "True"},
      // Disable ALL post-processing and modern lighting
      {"FFlagRenderDisablePostProcessing", "True"},
      {"FFlagRenderDisableBloom", "True"},
      {"FFlagRenderDisableMotionBlur", "True"},
      {"FFlagRenderDisableSSAO", "True"},
      {"FFlagRenderDisableVolumetricLighting", "True"},
      {"FFlagRenderDisableParticleLighting", "True"},
      {"FFlagRenderDisableSpecular", "True"},
      {"FFlagRenderDisableReflections", "True"},
      {"FFlagRenderDisableShadows", "True"},
      {"FFlagRenderForceForwardRendering", "True"},
      {"FFlagFutureIsBright", "False"},
      {"FFlagFutureIsBrightShadows", "False"},
      {"FFlagFutureIsBrightPhase2", "False"},
      {"FFlagVoxelLighting", "False"},
      {"FFlagDynamicLighting", "False"},
      {"FFlagGlobalIllumination", "False"},
      {"FFlagAmbientOcclusion", "False"},
      {"FFlagScreenSpaceReflections", "False"},
      {"FFlagScreenSpaceAmbientOcclusion", "False"},
      {"FFlagGodRays", "False"},
      {"FFlagLightShafts", "False"},
      {"FFlagCrepuscularRays", "False"},
      {"FFlagSubsurfaceScattering", "False"},
      {"FFlagAtmosphericEffects", "False"},
      {"FFlagEnvironmentLighting", "False"},
      {"FFlagHDR", "False"},
      {"FFlagEyeAdaptation", "False"},
      {"FFlagToneMapping", "False"},
      {"FFlagColorCorrection", "False"},
      {"FFlagVignette", "False"},
      {"FFlagDepthOfField", "False"},
      {"FFlagChromaticAberration", "False"},
      {"FFlagLensFlare", "False"},
      {"FFlagDebugSkyGray", "True"},
      // Culling: ensure ALL enabled
      {"FFlagRenderFrustumCulling", "True"},
      {"FFlagRenderPortalCulling", "True"},
      {"FFlagRenderDistanceCulling", "True"},
      {"FFlagRenderHiZCulling", "True"},
      {"FFlagRenderOcclusionQuery", "True"},
      {"FIntRenderOcclusionQueryFrameDelay", "8"},
      {"FFlagRenderEarlyZPass", "True"},
      {"FFlagRenderCullTransparentObjects", "True"},
      // Mesh/Geometry: aggressive LOD and compression
      {"FFlagMeshLOD", "True"},
      {"FFlagMeshCompression", "True"},
      {"FFlagPhysicsMeshCompression", "True"},
      {"FFlagSupportMeshLOD", "True"},
      {"FFlagMeshLodApplyGeomMeshOptimizer", "True"},
      {"FFlagAdaptiveMeshCacheSizeForAndroid", "True"},
      {"FIntDefaultMeshCacheSizeMB", is_ultra_low_end ? "4" : "8"},
      {"FIntRenderMeshOrphanBudgetMB", "1"},
      {"FIntTerrainGpuMeshMemoryTargetMib", is_ultra_low_end ? "2" : "4"},
      {"FIntAvatarMeshMemoryMax", is_ultra_low_end ? "2097152" : "4194304"},
      {"FIntSmoothTerrainPhysicsCacheSize", is_ultra_low_end ? "2" : "4"},
      // Texture: absolute minimum budgets
      {"FIntRenderTextureTotalBudgetMB", is_ultra_low_end ? "8" : "16"},
      {"FIntRenderTextureOrphanBudgetMB", "1"},
      {"FIntRenderTextureCompositorBudget", is_ultra_low_end ? "2" : "4"},
      {"FIntTextureCompositorActiveJobs", "1"},
      {"FIntTextureCompositorLowResFactor", "16"},
      {"FIntTextureCompositorMeshCache", is_ultra_low_end ? "1" : "2"},
      {"FIntAvatarTextureMemoryMax", is_ultra_low_end ? "1048576" : "2097152"},
      {"FFlagUITextureCompressionDesktop", "True"},
      {"FFlagTCTextureCompressionDesktop", "True"},
      {"FFlagUITextureUncompressed", "False"},
      {"FFlagDebugTerrainVTCompressedTextures", "True"},
      {"FFlagVulkanCompressedTextureAliasing", "True"},
      {"FFlagEnableMultiVETextureManagerSharing", "True"},
      // Disable GPU async compute (hurts on integrated)
      {"FFlagRenderGPUAsyncCompute", "False"},
      {"FFlagRenderParallelBatchSubmission", "False"},
      {"FFlagRenderParallelWorkSubmission", "False"},
      {"FFlagVulkanEnableRenderPassOptimization", "True"},
      {"FFlagVulkanUseTimelineSemaphores", "True"},
      // Disable telemetry/analytics completely
      {"FFlagDisableTelemetryService", "True"},
      {"FFlagDisableAnalyticsService", "True"},
      {"FFlagDisableCrashReportingService", "True"},
      {"FFlagDisableEventIngestService", "True"},
      {"FFlagDisablePerformanceStatsService", "True"},
      // Input: reduce polling
      {"FIntInputPollRateHz", "20"},
  }};
  apply_settings(rendering_settings);
  // Absolute-minimum caches, applied on every host.
  const std::array<ClientSetting, 8> memory_settings = {{
      {"FIntAssetProviderThreads", "1"},
      {"FIntDefaultMeshCacheSizeMB", "8"},
      {"FIntRenderTextureTotalBudgetMB", "16"},
      {"FIntRenderTextureCompositorBudget", "4"},
      {"FIntInitialAudioAssetCacheSize", "4"},
      {"FIntProjectedMaxBytesUsedForSoundsMB", "4"},
      {"FIntAvatarMeshMemoryMax", "4194304"},
      {"FIntAvatarTextureMemoryMax", "4194304"},
  }};
  apply_settings(memory_settings);

  *merged_json = overrides.dump();
  return true;
}

bool MergeRuntimeClientSettingsOverrides(
    const FrameRatePolicy& frame_rate, const PerformancePolicy& performance,
    std::string_view base_json, std::string* merged_json, std::string* error,
    std::vector<std::string>* kept_overrides) {
  std::string frame_rate_overrides;
  if (!MergeFrameRateClientSettingsOverrides(frame_rate, base_json,
                                             &frame_rate_overrides, error)) {
    return false;
  }
  std::string performance_overrides;
  if (!MergePerformanceClientSettingsOverrides(
          performance, frame_rate_overrides, &performance_overrides, error,
          kept_overrides)) {
    return false;
  }
  std::string http_client_overrides;
  if (!MergeHttpClientSettingsOverrides(performance_overrides,
                                        &http_client_overrides, error)) {
    return false;
  }
  std::string texture_memory_overrides;
  if (!MergeTextureMemoryClientSettingsOverrides(
          CalculateTextureMemoryBudgetBytes(DetectHostMemoryBytes()),
          http_client_overrides, &texture_memory_overrides, error)) {
    return false;
  }
  return MergeCrashReportClientSettingsOverrides(texture_memory_overrides,
                                                 merged_json, error);
}

bool MergeAudioDeviceMenuClientSettingsOverrides(std::string_view base_json,
                                                 std::string *merged_json,
                                                 std::string *error) {
  if (merged_json == nullptr)
    return false;
  auto overrides = nlohmann::json::parse(base_json.empty() ? "{}" : base_json,
                                         nullptr, false, true);
  if (overrides.is_discarded() || !overrides.is_object()) {
    if (error)
      *error = "audio device menu overrides must be a JSON object";
    return false;
  }
  // Use the profiled FMOD count/info/select interface for device enumeration.
  // RemoteAudioDeviceSync instead uses the unbridged device-list virtuals.
  overrides["FFlagDebugUseWebRtcAudioDevices"] = "False";
  overrides["FFlagRemoteAudioDeviceSync"] = "False";
  *merged_json = overrides.dump();
  return true;
}

bool MergeAudioCaptureClientSettingsOverrides(bool microphone_enabled,
                                              std::string_view base_json,
                                              std::string* merged_json,
                                              std::string* error) {
  if (merged_json == nullptr) {
    if (error != nullptr) {
      *error = "audio capture client-settings output is required";
    }
    return false;
  }
  nlohmann::json overrides = nlohmann::json::parse(
      base_json.empty() ? "{}" : base_json, nullptr, false, true);
  if (overrides.is_discarded() || !overrides.is_object()) {
    if (error != nullptr) {
      *error = "audio capture client-settings input must be a JSON object";
    }
    return false;
  }
  // PermissionsProtocol now owns authorization. Keep the test bypass off even
  // when a caller supplied it, so disabled host capture remains denied.
  (void)microphone_enabled;
  overrides["DFFlagVoiceChatSkipPermissionCheckForTests"] = "False";
  *merged_json = overrides.dump();
  return true;
}

}  // namespace runtime
}  // namespace mocktail
