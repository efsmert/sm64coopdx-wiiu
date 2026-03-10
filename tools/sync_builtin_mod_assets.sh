#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SYNC_CONTENT_DIR="${SYNC_CONTENT_DIR:-${PROJECT_ROOT}/build/us_wiiu/wuhb_content}"
SYNC_STATIC_CONTENT_DIR="${SYNC_STATIC_CONTENT_DIR:-${PROJECT_ROOT}/content}"

mkdir -p "${SYNC_CONTENT_DIR}"

if [[ -d "${SYNC_STATIC_CONTENT_DIR}" ]]; then
    rsync -a --delete "${SYNC_STATIC_CONTENT_DIR}/" "${SYNC_CONTENT_DIR}/"
fi

for asset_dir in mods lang palettes dynos; do
    src="${PROJECT_ROOT}/${asset_dir}"
    dst="${SYNC_CONTENT_DIR}/${asset_dir}"

    if [[ -d "${src}" ]]; then
        mkdir -p "${dst}"
        rsync -a --delete "${src}/" "${dst}/"
    fi
done

echo "synced built-in assets into ${SYNC_CONTENT_DIR}"
