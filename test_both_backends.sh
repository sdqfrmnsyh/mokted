#!/bin/bash
# Quick test runner for both backends

set -euo pipefail

BUILD_DIR="/home/modz/mokted/build"
EXE="$BUILD_DIR/mokted"
PLACE_ID="1962086868"  # Tower of Hell

run_test() {
    local backend="$1"
    local profile_file="$BUILD_DIR/${backend}_test_profile.json"
    local log_file="$BUILD_DIR/${backend}_test.log"
    
    echo "=========================================="
    echo "Testing $backend backend..."
    echo "=========================================="
    
    # Set up clean shader cache
    export MESA_SHADER_CACHE_DIR="$BUILD_DIR/shader_cache_$backend"
    export __GL_SHADER_DISK_CACHE_PATH="$BUILD_DIR/shader_cache_$backend"
    mkdir -p "$MESA_SHADER_CACHE_DIR"
    
    # Common optimizations
    export MOCKTAIL_SMALL_TEXTURE_UPSCALE=1
    export MOCKTAIL_ENGINE_PUMP_REST=sleep
    export MOCKTAIL_GRAPHICS_QUALITY=auto
    export MOCKTAIL_FORCE_1X1_TEXTURES=1
    export MOCKTAIL_DISABLE_TELEMETRY=1
    export MOCKTAIL_MULTITHREADED_RENDERING=1
    export MOCKTAIL_PHYSICS_WORKER_MODE=throughput
    
    if [[ "$backend" == "vulkan" ]]; then
        export MOCKTAIL_GRAPHICS_BACKEND=direct-vulkan
    else
        export MOCKTAIL_GRAPHICS_BACKEND=opengl
    fi
    
    # Run for 30 seconds
    timeout 30s "$EXE" \
        --profile "$profile_file" \
        --launch-uri "roblox://experiences/start?placeId=$PLACE_ID" \
        2>&1 | tee "$log_file"
    
    if [[ -f "$profile_file" ]]; then
        echo ""
        echo "Profile generated: $profile_file"
        if [[ -f "/home/modz/mokted/scripts/summarize_profile_trace.py" ]]; then
            python3 /home/modz/mokted/scripts/summarize_profile_trace.py "$profile_file"
        fi
    else
        echo "No profile generated for $backend"
    fi
}

echo "Testing Vulkan backend..."
run_test "vulkan"

echo ""
echo ""
echo "Testing OpenGL backend..."
run_test "opengl"

echo ""
echo "=========================================="
echo "All tests completed!"
echo "=========================================="