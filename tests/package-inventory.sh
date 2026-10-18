#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/../scripts/common.sh"
work=$(scratch package-inventory); trap 'rm -rf -- "$work"' EXIT
fake=$work/consumer
mkdir -p "$fake/scripts" "$fake/dist"
cp scripts/{package.sh,common.sh,require-host-bash.sh,release_version.sh,cpkt-toolchains.sh,cpkt-archive-cache.sh} "$fake/scripts/"
printf '1.2.3\n' > "$fake/VERSION"
base=libremotedb-1.2.3
while IFS= read -r target; do printf '%s\n' "$target" > "$fake/dist/$base-$target.tar.gz"; done < <(scripts/cpkt-toolchains.sh targets)
printf source > "$fake/dist/$base-source.tar.gz"
"$fake/scripts/package.sh" checksums
manifest=$fake/dist/$base-CHECKSUMS
[[ $(wc -l < "$manifest") == 8 ]]
(cd "$fake/dist"; sha256sum -c "$base-CHECKSUMS")
cp "$manifest" "$work/valid-manifest"
reject() {
  if "$fake/scripts/package.sh" "$1" > "$work/error" 2>&1; then die 'Invalid release inventory accepted'; fi
}
rm -- "$fake/dist/$base-source.tar.gz"
reject checksums
cmp "$manifest" "$work/valid-manifest"
printf source > "$fake/dist/$base-source.tar.gz"
for extra in libremotedb-9.9.9-source.tar.gz extra-output old-CHECKSUMS; do
  printf stale > "$fake/dist/$extra"
  reject checksums
  cmp "$manifest" "$work/valid-manifest"
  rm -- "$fake/dist/$extra"
done
printf corrupted >> "$fake/dist/$base-source.tar.gz"
reject verify
grep -q 'Checksum inventory differs from complete shipment' "$work/error"
printf source > "$fake/dist/$base-source.tar.gz"
head -n 1 "$work/valid-manifest" >> "$manifest"
reject verify
grep -q 'Checksum inventory differs from complete shipment' "$work/error"
printf malformed > "$manifest"
reject verify
grep -q 'Checksum inventory differs from complete shipment' "$work/error"
echo 'Complete eight-artifact inventory, checksums, missing/stale/unlisted/corrupt/duplicate rejection passed'
