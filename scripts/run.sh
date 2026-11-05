#!/bin/bash
set -e

ROOT="$(dirname "$(realpath "$0")")/.."
clear

"$ROOT/scripts/build.sh"

# export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json
# export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
"$ROOT/build/miniagent"
