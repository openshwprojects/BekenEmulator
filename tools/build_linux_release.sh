#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MANIFEST_PATH="${SCRIPT_DIR}/release_firmwares.txt"
DIST_PATH="${REPO_ROOT}/dist"

cd "${REPO_ROOT}"

mapfile -t FIRMWARE_NAMES < <(
    sed -e 's/\r$//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' "${MANIFEST_PATH}" \
        | grep -v -E '^(#|$)'
)

if [[ "${#FIRMWARE_NAMES[@]}" -eq 0 ]]; then
    echo "Error: The release firmware manifest is empty: ${MANIFEST_PATH}" >&2
    exit 1
fi

UNIQUE_COUNT="$(printf '%s\n' "${FIRMWARE_NAMES[@]}" | sort -u | wc -l | tr -d ' ')"
if [[ "${UNIQUE_COUNT}" -ne "${#FIRMWARE_NAMES[@]}" ]]; then
    echo "Error: The release firmware manifest contains duplicate names." >&2
    exit 1
fi

for name in "${FIRMWARE_NAMES[@]}"; do
    source_path="${REPO_ROOT}/firmwares/${name}"
    if [[ ! -f "${source_path}" ]]; then
        echo "Error: Release firmware does not exist: ${source_path}" >&2
        exit 1
    fi
done

make -j4 cli tools

mkdir -p "${DIST_PATH}"
BIN_PATH="${DIST_PATH}/BekenEmulator-linux-x64"
cp -f "${REPO_ROOT}/bekensim" "${BIN_PATH}"

python3 - "${BIN_PATH}" "${REPO_ROOT}/firmwares" "${FIRMWARE_NAMES[@]}" <<'PY'
import os, struct, sys

bin_path = sys.argv[1]
fw_dir = sys.argv[2]
names = sys.argv[3:]

total = 0
with open(bin_path, "ab") as f:
    for name in names:
        nb = name.encode("utf-8")
        with open(os.path.join(fw_dir, name), "rb") as src:
            data = src.read()
        f.write(struct.pack("<I", len(nb)))
        f.write(nb)
        f.write(struct.pack("<I", len(data)))
        f.write(data)
        total += 8 + len(nb) + len(data)
    f.write(struct.pack("<II8s", len(names), total, b"BKSFWB01"))
PY

chmod +x "${BIN_PATH}"
echo "Built ${BIN_PATH}"
