#!/usr/bin/env bash
# Clones sfizz (BSD-2-Clause SFZ sampler library) at a pinned release, with its submodules, into
# engine/third_party/sfizz. engine/CMakeLists.txt runs this automatically when the folder is
# missing; run it by hand to re-fetch (--force).
#
# gcc 16 note: sfizz 1.2.3 compiles unpatched with gcc 16.2. The only adjustments are made from
# engine/CMakeLists.txt: CMAKE_POLICY_VERSION_MINIMUM=3.5 (CMake 4 rejects the
# cmake_minimum_required(<3.5) of its bundled libaiff/st_audiofile) and C++17 instead of sfizz's
# default C++14 (so absl::string_view is std::string_view on both sides of the parser API we use).
set -euo pipefail

TAG="1.2.3"
COMMIT="4e70dc0bef53b41f2853ed46e26f5911114c92d0"
URL="https://github.com/sfztools/sfizz.git"
DEST="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/sfizz"

if [[ "${1:-}" != "--force" && -f "$DEST/CMakeLists.txt" ]]; then
    have="$(git -C "$DEST" rev-parse HEAD 2>/dev/null || echo none)"
    if [[ "$have" == "$COMMIT" ]]; then
        echo "sfizz $TAG already present in $DEST"
        exit 0
    fi
    echo "warning: $DEST is at $have, expected $COMMIT; re-cloning" >&2
fi

rm -rf "$DEST"
echo "cloning sfizz $TAG (with submodules) into $DEST"
git -c advice.detachedHead=false clone -q --depth 1 --branch "$TAG" \
    --recurse-submodules --shallow-submodules "$URL" "$DEST"
have="$(git -C "$DEST" rev-parse HEAD)"
if [[ "$have" != "$COMMIT" ]]; then
    echo "error: tag $TAG resolved to $have, expected $COMMIT" >&2
    exit 1
fi
echo "sfizz $TAG ready"
