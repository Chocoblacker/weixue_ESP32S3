#!/usr/bin/env bash
# Build only. Activate ESP-IDF v6.0.1 first; never flash automatically.
set -euo pipefail
cd "$(dirname "$0")/.."
command -v idf.py >/dev/null || { echo 'Activate ESP-IDF v6.0.1 first' >&2; exit 1; }
idf.py -B build-public -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-public/sdkconfig \
  -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-175c' build
