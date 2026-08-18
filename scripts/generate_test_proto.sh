#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
PROTO_DIR="${REPO_ROOT}/test"
OUTPUT_DIR="${1:-${PROTO_DIR}}"

if [[ $# -gt 1 ]]; then
    printf 'usage: %s [output-directory]\n' "$0" >&2
    exit 2
fi

if ! command -v protoc >/dev/null 2>&1; then
    printf 'error: protoc is required\n' >&2
    exit 127
fi

printf 'protoc: %s\n' "$(protoc --version)"

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/kairpc-test-proto.XXXXXX")"
trap 'rm -rf -- "$tmp_dir"' EXIT

protoc \
    -I "${PROTO_DIR}" \
    --cpp_out="${tmp_dir}" \
    "${PROTO_DIR}/order.proto"

mkdir -p "${OUTPUT_DIR}"
for generated in order.pb.cc order.pb.h; do
    sed -E -i 's/[[:space:]]+$//' "${tmp_dir}/${generated}"
    cp "${tmp_dir}/${generated}" "${OUTPUT_DIR}/${generated}"
done
