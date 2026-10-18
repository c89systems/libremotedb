#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/common.sh"
generated_roots
version=$(scripts/release_version.sh)
base=libremotedb-$version
archive_tree() {
  scripts/archive.sh "$@"
}
sdk() (
  local preset=$1 target=${1%-release} work name
  [[ $preset == *-release ]] || die 'SDK packaging requires a Release preset'
  scripts/build.sh "$preset"
  target_environment "$target"
  work=$(scratch sdk-stage); trap 'rm -rf -- "$work"' EXIT
  name=$base-$target
  cmake --install "build/$preset" --prefix "$work/$name"
  mkdir -p dist
  archive_tree "$work" "$name" "$root/dist/$name.tar.gz"
)
source_archive() (
  local work path name=$base-source
  work=$(scratch source-stage); trap 'rm -rf -- "$work"' EXIT
  mkdir -p "$work/$name" dist
  if [[ -e .git ]]; then
    git ls-files > "$work/files"
  else
    cat RELEASE_MANIFEST > "$work/files"
  fi
  while IFS= read -r path; do
    case "$path" in VERSION|RELEASE_MANIFEST) continue;; ''|/*|../*|*/../*|build/*|dist/*|.cache/*|.git/*) die "Unsafe source manifest entry: $path";; esac
    [[ -f $path && ! -L $path ]] || die "Missing/nonregular source input: $path"
    mkdir -p "$work/$name/$(dirname -- "$path")"
    cp -p -- "$path" "$work/$name/$path"
    printf '%s\n' "$path"
  done < "$work/files" > "$work/manifest"
  printf '%s\n' "$version" > "$work/$name/VERSION"
  printf '%s\n' VERSION RELEASE_MANIFEST >> "$work/manifest"
  LC_ALL=C sort -u "$work/manifest" > "$work/$name/RELEASE_MANIFEST"
  archive_tree "$work" "$name" "$root/dist/$name.tar.gz"
)
verify_one() { scripts/verify-package.sh "$root/dist/$base-${1%-release}.tar.gz" "${1%-release}"; }
manifest() {
  local scope=$1 output=$2 target path
  local -a names=()
  while IFS= read -r target; do names+=("$base-$target.tar.gz"); done < <(scripts/cpkt-toolchains.sh targets)
  [[ $scope == binary ]] || names+=("$base-source.tar.gz")
  for path in "${names[@]}"; do [[ -f dist/$path ]] || die "Missing release artifact: $path"; done
  if [[ $scope != binary ]]; then
    for path in dist/*; do
      [[ ${path##*/} == "$base-CHECKSUMS" ]] && continue
      local found=0 name
      for name in "${names[@]}"; do [[ ${path##*/} != "$name" ]] || found=1; done
      [[ $found == 1 ]] || die "Unlisted/stale release artifact: $path"
    done
  fi
  (cd dist; sha256sum -- "${names[@]}") > "$output"
}
verify_manifest() {
  local file=$1 line digest name target count=0
  [[ -s $file ]] || die "Missing checksum manifest: $file"
  while IFS= read -r line; do
    [[ $line =~ ^([0-9a-f]{64})\ \ ([A-Za-z0-9_.-]+\.tar\.gz)$ ]] || die "Invalid checksum entry: $line"
    digest=${BASH_REMATCH[1]} name=${BASH_REMATCH[2]}
    [[ $(sha256sum < "dist/$name" | cut -d' ' -f1) == "$digest" ]] || die "Checksum mismatch: $name"
    if [[ $name == "$base-source.tar.gz" ]]; then target=source; else target=${name#"$base-"}; target=${target%.tar.gz}; fi
    scripts/verify-package.sh "$root/dist/$name" "$target"
    count=$((count+1))
  done < "$file"
  [[ $count == 7 || $count == 8 ]] || die 'Incomplete release checksum inventory'
}
case ${1:-} in
  sdk) sdk "${2:?Release preset required}";;
  source) source_archive;;
  source-smoke)
    work=$(scratch source-smoke); trap 'rm -rf -- "$work"' EXIT
    scripts/extract.sh "$root/dist/$base-source.tar.gz" "$work" "$base-source"
    tree=$work/$base-source
    [[ $("$tree/scripts/release_version.sh") == "$version" ]] || die 'Source version mismatch'
    make -C "$tree" test-release PRESET=x86_64-linux-gnu-release
    ;;
  verify-one) verify_one "${2:?Release preset required}";;
  matrix)
    while IFS= read -r target; do
      if [[ $target == x86_64-linux-* ]]; then scripts/build.sh "$target-release" check-local; fi
      sdk "$target-release"
    done < <(scripts/cpkt-toolchains.sh targets)
    mkdir -p build
    manifest binary "$root/build/$base-binary-CHECKSUMS"
    verify_manifest "$root/build/$base-binary-CHECKSUMS"
    ;;
  checksums) manifest complete "$root/dist/$base-CHECKSUMS";;
  verify)
    work=$(scratch inventory); trap 'rm -rf -- "$work"' EXIT
    manifest complete "$work/expected"
    cmp "$work/expected" "dist/$base-CHECKSUMS" || die 'Checksum inventory differs from complete shipment'
    verify_manifest "$root/dist/$base-CHECKSUMS"
    ;;
  *) die 'Usage: package.sh sdk PRESET|source|source-smoke|verify-one PRESET|matrix|checksums|verify';;
esac
