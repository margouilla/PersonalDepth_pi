#!/bin/bash
# PersonalDepth_pi - compilation Linux dans un conteneur Debian (GitHub Actions)
# Usage : tools/ci/build-linux.sh <target> <target-version> <target-arch> [pré-version] [tests 0/1]
# ex.   : tools/ci/build-linux.sh debian-arm64 12 arm64 beta.4 1
set -euo pipefail
target=$1 tversion=$2 tarch=$3 pre=${4:-} tests=${5:-1}
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q --no-install-recommends \
  build-essential cmake git ca-certificates pkg-config libwxgtk3.2-dev libgl-dev
cmake -S . -B build-ci -DCMAKE_BUILD_TYPE=Release -DPD_WITH_GDAL=OFF \
  -DPD_TARGET="$target" -DPD_TARGET_VERSION="$tversion" -DPD_TARGET_ARCH="$tarch" -DPD_PRE="$pre"
cmake --build build-ci -j"$(nproc)"
if [ "$tests" = 1 ]; then
  (cd build-ci && ctest --output-on-failure)
fi
cmake --build build-ci --target pd-package
ls -l dist
