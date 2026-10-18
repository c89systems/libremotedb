#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/../scripts/common.sh"
work=$(scratch archive-contract); trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/stage/sdk/bin" "$work/stage/sdk/include" "$work/extracted"
printf '#!/bin/sh\nexit 0\n' > "$work/stage/sdk/bin/tool"
chmod 755 "$work/stage/sdk/bin/tool"
printf 'header\n' > "$work/stage/sdk/include/api.h"
scripts/archive.sh "$work/stage" sdk "$work/first.tar.gz"
find "$work/stage" -exec touch -m -d @1234567890 {} +
(umask 077; scripts/archive.sh "$work/stage" sdk "$work/second.tar.gz")
cmp "$work/first.tar.gz" "$work/second.tar.gz"
scripts/extract.sh "$work/first.tar.gz" "$work/extracted" sdk
[[ -x $work/extracted/sdk/bin/tool && $(stat -c %a "$work/extracted/sdk/include/api.h") == 644 ]]
mkdir -p "$work/unsafe"
ln -s / "$work/unsafe/escape"
tar -C "$work" -czf "$work/unsafe.tar.gz" unsafe
if scripts/extract.sh "$work/unsafe.tar.gz" "$work/extracted" unsafe > "$work/error" 2>&1; then die 'Unsafe link accepted'; fi
if scripts/extract.sh "$work/first.tar.gz" "$work/extracted" wrong > "$work/error" 2>&1; then die 'Wrong archive root accepted'; fi
# Source verification rejects missing inputs and unmanifested objects, including
# empty directories and links which a file-only inventory would miss.
source_tree=$work/source-fixture
mkdir "$source_tree"
printf '%s\n' "$(scripts/release_version.sh)" > "$source_tree/VERSION"
printf 'fixture license\n' > "$source_tree/LICENSE"
printf 'cmake_minimum_required(VERSION 3.25)\n' > "$source_tree/CMakeLists.txt"
printf 'CMakeLists.txt\nLICENSE\nRELEASE_MANIFEST\nVERSION\n' > "$source_tree/RELEASE_MANIFEST"
source_archive() { scripts/archive.sh "$work" source-fixture "$work/source-fixture.tar.gz"; }
source_archive
scripts/verify-package.sh "$work/source-fixture.tar.gz" source
reject_source() {
  source_archive
  if scripts/verify-package.sh "$work/source-fixture.tar.gz" source > "$work/error" 2>&1; then die 'Invalid source inventory accepted'; fi
  grep -q "$1" "$work/error"
}
mkdir "$source_tree/unlisted"
reject_source 'Source directory inventory mismatch'
rmdir "$source_tree/unlisted"
ln -s LICENSE "$source_tree/unlisted"
reject_source 'Unexpected source symlink'
rm -- "$source_tree/unlisted" "$source_tree/LICENSE"
reject_source 'Missing source input: LICENSE'
printf 'fixture license\n' > "$source_tree/LICENSE"
printf unlisted > "$source_tree/unlisted"
reject_source 'Source manifest does not match archive payload'
rm -- "$source_tree/unlisted"
printf 'not-a-version\n' > "$source_tree/VERSION"
reject_source 'Source version mismatch'
