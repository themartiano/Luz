#!/usr/bin/env bash
set -euo pipefail

# Builds the optional offline VDB importer. This is intentionally separate from
# Makefile/CMakeLists.txt: Luz and luz_tests never include or link OpenVDB.
root="$(cd "$(dirname "$0")/.." && pwd)"
output="${1:-$root/tools/vdb-to-luzvol}"
cxx="${CXX:-c++}"

if [[ -z "${OPENVDB_PREFIX:-}" ]]; then
	if command -v brew >/dev/null 2>&1; then
		OPENVDB_PREFIX="$(brew --prefix openvdb)"
	else
		OPENVDB_PREFIX="/usr"
	fi
fi

if command -v brew >/dev/null 2>&1; then
	IMATH_PREFIX="${IMATH_PREFIX:-$(brew --prefix imath)}"
	TBB_PREFIX="${TBB_PREFIX:-$(brew --prefix tbb)}"
else
	IMATH_PREFIX="${IMATH_PREFIX:-/usr}"
	TBB_PREFIX="${TBB_PREFIX:-/usr}"
fi

"$cxx" -std=c++20 -O3 \
	-I"$root/include/luz" \
	-I"$OPENVDB_PREFIX/include" \
	-I"$IMATH_PREFIX/include" \
	-I"$IMATH_PREFIX/include/Imath" \
	-I"$TBB_PREFIX/include" \
	"$root/tools/vdb-to-luzvol.cpp" \
	"$root/src/core/SparseVolumeGrid.cpp" \
	-L"$OPENVDB_PREFIX/lib" \
	-L"$TBB_PREFIX/lib" \
	-lopenvdb -ltbb \
	-o "$output"

printf 'Ready: %s\n' "$output"
