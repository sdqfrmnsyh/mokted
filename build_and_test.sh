#!/bin/bash
# Build and test script for optimized Mokted
# Tests both OpenGL and Vulkan backends

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
CONFIG_DIR="${SCRIPT_DIR}/test_configs"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${BLUE}[INFO]${NC} $*"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $*"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $*"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $*"
}

# Create test configs for OpenGL and Vulkan
create_test_configs() {
    mkdir -p "${CONFIG_DIR}"
    
    # Vulkan config (optimized)
    cat > "${CONFIG_DIR}/vulkan_optimized.yaml" << 'EOF'
version: 1
device: pc-windows-11

runtime:
  headless: false

appearance:
  theme: dark

graphics:
  backend: direct-vulkan
  frame_rate_limit: display
  vsync: auto
  etc2_emulation: auto

performance:
  multithreaded_rendering: true
  physics_worker_mode: throughput
  memory_limit_mb: 0
  gamemode: on

audio:
  output_device: default
  input_device: default

input:
  raw_mouse: false

integrations:
  fleasion:
    enabled: false
  discord_rpc:
    enabled: false

window:
  width: 1280
  height: 720
  title: "Roblox (Vulkan Optimized)"
  high_dpi: false

display:
  exclusive_fullscreen: false

network:
  use_system_proxy: false

updates:
  automatic: true
  source: apk-pure
  launch_after_update: false

environment_overrides:
  MOCKTAIL_SMALL_TEXTURE_UPSCALE: "1"
  MOCKTAIL_ENGINE_PUMP_REST: "sleep"
  MOCKTAIL_GRAPHICS_QUALITY: "auto"
  MOCKTAIL_FORCE_1X1_TEXTURES: "1"
  MOCKTAIL_DISABLE_TELEMETRY: "1"
EOF

    # OpenGL config (optimized)
    cat > "${CONFIG_DIR}/opengl_optimized.yaml" << 'EOF'
version: 1
device: pc-windows-11

runtime:
  headless: false

appearance:
  theme: dark

graphics:
  backend: opengl
  frame_rate_limit: display
  vsync: auto
  etc2_emulation: native

performance:
  multithreaded_rendering: true
  physics_worker_mode: throughput
  memory_limit_mb: 0
  gamemode: on

audio:
  output_device: default
  input_device: default

input:
  raw_mouse: false

integrations:
  fleasion:
    enabled: false
  discord_rpc:
    enabled: false

window:
  width: 1280
  height: 720
  title: "Roblox (OpenGL Optimized)"
  high_dpi: false

display:
  exclusive_fullscreen: false

network:
  use_system_proxy: false

updates:
  automatic: true
  source: apk-pure
  launch_after_update: false

environment_overrides:
  MOCKTAIL_SMALL_TEXTURE_UPSCALE: "1"
  MOCKTAIL_ENGINE_PUMP_REST: "sleep"
  MOCKTAIL_GRAPHICS_QUALITY: "auto"
  MOCKTAIL_FORCE_1X1_TEXTURES: "1"
  MOCKTAIL_DISABLE_TELEMETRY: "1"
EOF

    log_success "Test configurations created in ${CONFIG_DIR}"
}

# Build the project
build_project() {
    log_info "Building Mokted with optimizations..."
    
    mkdir -p "${BUILD_DIR}"
    cd "${BUILD_DIR}"
    
    # Configure with optimizations
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS_RELEASE="-O3 -march=native -mtune=native -flto=auto" \
        -DCMAKE_C_FLAGS_RELEASE="-O3 -march=native -mtune=native -flto=auto" \
        -DMOCKTAIL_ENABLE_PROFILING=OFF \
        -DMOCKTAIL_ENABLE_TESTS=OFF \
        -GNinja
    
    # Build
    ninja -j"$(nproc)"
    
    log_success "Build completed successfully"
}

# Test a graphics backend
test_backend() {
    local backend="$1"
    local config_file="$2"
    local profile_file="${BUILD_DIR}/${backend}_profile.json"
    
    log_info "Testing ${backend} backend..."
    
    # Check if executable exists
    if [[ ! -f "${BUILD_DIR}/mocktail" ]]; then
        log_error "mocktail executable not found at ${BUILD_DIR}/mocktail"
        return 1
    fi
    
    # Run with profiling for 30 seconds (or until user exits)
    log_info "Starting ${backend} test (will run for 30 seconds or until exit)..."
    log_info "Profile output: ${profile_file}"
    
    # Set up environment for clean test
    export MESA_SHADER_CACHE_DIR="${BUILD_DIR}/shader_cache_${backend}"
    export __GL_SHADER_DISK_CACHE_PATH="${BUILD_DIR}/shader_cache_${backend}"
    mkdir -p "${MESA_SHADER_CACHE_DIR}"
    
    # Run with timeout
    timeout 30s "${BUILD_DIR}/mocktail" \
        --config "${config_file}" \
        --profile "${profile_file}" \
        --launch-uri "roblox://experiences/start?placeId=1962086868" \
        2>&1 | tee "${BUILD_DIR}/${backend}_test.log" || true
    
    # Check if profile was generated
    if [[ -f "${profile_file}" ]]; then
        log_success "${backend} profile generated: ${profile_file}"
        
        # Run summary if script exists
        if [[ -f "${SCRIPT_DIR}/scripts/summarize_profile_trace.py" ]]; then
            python3 "${SCRIPT_DIR}/scripts/summarize_profile_trace.py" "${profile_file}" > "${BUILD_DIR}/${backend}_summary.md"
            log_info "Summary saved to ${BUILD_DIR}/${backend}_summary.md"
            head -50 "${BUILD_DIR}/${backend}_summary.md"
        fi
    else
        log_warn "${backend} profile not generated (may have exited early)"
    fi
    
    # Check logs for errors
    if grep -q "FATAL\|ERROR" "${BUILD_DIR}/${backend}_test.log" 2>/dev/null; then
        log_warn "${backend} test had errors/warnings (check ${BUILD_DIR}/${backend}_test.log)"
        grep -i "fatal\|error" "${BUILD_DIR}/${backend}_test.log" | head -20
    else
        log_success "${backend} test completed without fatal errors"
    fi
}

# Main execution
main() {
    log_info "Starting Mokted optimization build and test"
    
    create_test_configs
    build_project
    
    # Test Vulkan backend
    test_backend "vulkan" "${CONFIG_DIR}/vulkan_optimized.yaml"
    
    # Test OpenGL backend
    test_backend "opengl" "${CONFIG_DIR}/opengl_optimized.yaml"
    
    log_info "All tests completed. Check ${BUILD_DIR}/ for logs and profiles."
    log_info "Compare Vulkan vs OpenGL performance using the generated summaries."
}

main "$@"