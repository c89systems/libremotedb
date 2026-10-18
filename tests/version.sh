#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/../scripts/common.sh"
work=$(scratch version-contract); trap 'rm -rf -- "$work"' EXIT
repo=$work/repo
mkdir -p "$repo/scripts"
cp scripts/{release_version.sh,release-check.sh,common.sh,require-host-bash.sh} "$repo/scripts/"
git init -q -b trunk "$repo"
git -C "$repo" config user.name Fixture
git -C "$repo" config user.email fixture@example.invalid
git -C "$repo" config commit.gpgsign false
git -C "$repo" add scripts
git -C "$repo" commit -qm 'test(fixture): establish version inputs'
git -C "$repo" remote add origin https://example.invalid/c89systems/libremotedb.git
git -C "$repo" symbolic-ref refs/remotes/origin/HEAD refs/remotes/origin/trunk
version() { "$repo/scripts/release_version.sh"; }
[[ $(version) == 0.0.0 ]]
printf '9.9.9\n' > "$repo/VERSION"
[[ $(version) == 0.0.0 && $(REMOTEDB_VERSION_OVERRIDE=8.7.6 version) == 8.7.6 ]]
rm -- "$repo/VERSION"
git -C "$repo" -c tag.gpgSign=false tag v1.2.3
[[ $(version) == 1.2.3 && $(REMOTEDB_VERSION_OVERRIDE=8.7.6 version) == 1.2.3 ]]
git -C "$repo" -c tag.gpgSign=false tag v1.2.4
if version > "$work/error" 2>&1; then die 'Ambiguous release tags accepted'; fi
git -C "$repo" tag -d v1.2.3 v1.2.4 > /dev/null
git -C "$repo" -c tag.gpgSign=false tag -a -m fixture v1.2.3
if version > "$work/error" 2>&1; then die 'Annotated release tag accepted'; fi
git -C "$repo" tag -d v1.2.3 > /dev/null
git -C "$repo" symbolic-ref refs/tags/v1.2.3 refs/heads/trunk
if version > "$work/error" 2>&1; then die 'Symbolic release tag accepted'; fi
git -C "$repo" symbolic-ref --delete refs/tags/v1.2.3
mkdir -p "$repo/build"
printf 'preserve generated state\n' > "$repo/build/sentinel"
printf '/build/\n' > "$repo/.gitignore"
git -C "$repo" add .gitignore
git -C "$repo" commit -qm 'test(fixture): ignore owned outputs'
if "$repo/scripts/release-check.sh" > "$work/error" 2>&1; then die 'Unsigned release accepted'; fi
[[ -f $repo/build/sentinel ]]
# Use an actual disposable signing key and trust file, confined to this fixture.
ssh-keygen -q -t ed25519 -N '' -f "$repo/.git/signing-key"
printf 'fixture@example.invalid %s\n' "$(cat "$repo/.git/signing-key.pub")" > "$repo/.git/allowed-signers"
git -C "$repo" config gpg.format ssh
git -C "$repo" config user.signingkey "$repo/.git/signing-key"
git -C "$repo" config gpg.ssh.allowedSignersFile "$repo/.git/allowed-signers"
git -C "$repo" -c commit.gpgsign=true commit --amend --no-edit -q
if "$repo/scripts/release-check.sh" > "$work/error" 2>&1; then die 'Untagged release accepted'; fi
git -C "$repo" -c tag.gpgSign=false tag v1.2.3
"$repo/scripts/release-check.sh"
[[ -f $repo/build/sentinel ]]
mkdir -p "$repo/build/source/scripts" "$repo/build/source/build"
cp scripts/{release_version.sh,release-check.sh,common.sh,require-host-bash.sh} "$repo/build/source/scripts/"
printf '1.2.3\n' > "$repo/build/source/VERSION"
printf preserve > "$repo/build/source/build/sentinel"
if "$repo/build/source/scripts/release-check.sh" > "$work/error" 2>&1; then die 'Source archive inherited enclosing Git release identity'; fi
[[ -f $repo/build/source/build/sentinel ]]
grep -q 'own Git checkout' "$work/error"
if REMOTEDB_VERSION_OVERRIDE=1.2.3 "$repo/scripts/release-check.sh" > "$work/error" 2>&1; then die 'Candidate override accepted for final release'; fi
git -C "$repo" switch -qc feature/fixture
if "$repo/scripts/release-check.sh" > "$work/error" 2>&1; then die 'Development branch accepted for final release'; fi
git -C "$repo" switch -q trunk
printf dirty >> "$repo/.gitignore"
if "$repo/scripts/release-check.sh" > "$work/error" 2>&1; then die 'Dirty final release accepted'; fi
[[ -f $repo/build/sentinel ]]
mkdir -p "$repo/build/source/scripts"
cp scripts/release_version.sh "$repo/build/source/scripts/"
printf '7.6.5\n' > "$repo/build/source/VERSION"
printf 'VERSION\n' > "$repo/build/source/RELEASE_MANIFEST"
[[ $("$repo/build/source/scripts/release_version.sh") == 7.6.5 ]]
echo 'Owned versions, lightweight tags, source isolation and signed release preflight passed'
