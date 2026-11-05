#!/bin/bash
set -e

ROOT="$(dirname "$(realpath "$0")")/.."

"$ROOT/scripts/build.sh"

exec "$ROOT/build/miniagent" --desktop "$@"
