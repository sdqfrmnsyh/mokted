#!/bin/bash
# Quick benchmark runner for comparing before/after optimizations
# Usage: ./run_benchmark.sh [vulkan|opengl|both] [place_id]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BACKEND="${1:-both}"
PLACE_ID="${2:-1962086868}"  # Tower of Hell by default

run_benchmark() {
    local backend="$1"
    local profile_file="${BUILD_DIR}/benchmark_${backend}_$(date +%s).json"
    local config_file="${SCRIPT_DIR}/example.config.yaml"
    
    echo "Running ${backend} benchmark..."
    echo "Profile: ${profile_file}"
    echo "Place ID: ${PLACE_ID}"
    
    # Set up clean shader cache
    export MESA_SHADER_CACHE_DIR="${BUILD_DIR}/shader_cache_${backend}_bench"
    export __GL_SHADER_DISK_CACHE_PATH="${BUILD_DIR}/shader_cache_${backend}_bench"
    mkdir -p "${MESA_SHADER_CACHE_DIR}"
    
    # Override graphics backend via environment
    export MOCKTAIL_GRAPHICS_BACKEND="${backend}"
    
    # Run for 60 seconds
    timeout 60s "${BUILD_DIR}/mocktail" \
        --config "${config_file}" \
        --profile "${profile_file}" \
        --launch-uri "roblox://experiences/start?placeId=${PLACE_ID}" \
        2>&1 | tee "${BUILD_DIR}/benchmark_${backend}_$(date +%s).log"
    
    if [[ -f "${profile_file}" ]]; then
        echo "Benchmark complete. Profile saved to ${profile_file}"
        if [[ -f "${SCRIPT_DIR}/scripts/summarize_profile_trace.py" ]]; then
            python3 "${SCRIPT_DIR}/scripts/summarize_profile_trace.py" "${profile_file}"
        fi
    else
        echo "No profile generated"
    fi
}

case "${BACKEND}" in
    vulkan)
        run_benchmark "vulkan"
        ;;
    opengl)
        run_benchmark "opengl"
        ;;
    both)
        run_benchmark "vulkan"
        echo ""
        echo "========================================"
        echo ""
        run_benchmark "opengl"
        ;;
    *)
        echo "Usage: $0 [vulkan|opengl|both] [place_id]"
        exit 1
        ;;
esac