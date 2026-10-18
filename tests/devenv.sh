#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/../scripts/common.sh"
work=$(scratch devenv-contract); trap 'rm -rf -- "$work"' EXIT
fake=$work/consumer
mkdir -p "$fake/scripts" "$fake/cmake" "$work/bin"
cp scripts/{common.sh,require-host-bash.sh,devenv.sh} "$fake/scripts/"
cp cmake/RenderDevenv.cmake "$fake/cmake/"
cp devenv.yaml.in "$fake/"
state=$fake/build/devenv
for kind in pathname abstract; do
  cmake "-DPROJECT_ROOT=$fake" "-DWORK=$state" -DPOD=rdb-contract "-DUID=$(id -u)" "-DGID=$(id -g)" \
    "-DKIND=$kind" -DSERVER_IMAGE=localhost/server:abcdef -DCLIENT_IMAGE=localhost/client:abcdef -P cmake/RenderDevenv.cmake
  if grep -q @@ "$state/devenv.yaml"; then die 'Unexpanded manifest placeholder'; fi
  grep -q 'imagePullPolicy: Never' "$state/devenv.yaml"
  grep -q 'restartPolicy: Never' "$state/devenv.yaml"
  grep -q 'allowPrivilegeEscalation: false' "$state/devenv.yaml"
  grep -q "runAsUser: $(id -u)" "$state/devenv.yaml"
  [[ $(stat -c %a "$state/devenv.yaml") == 600 ]]
  [[ $(grep -c '    - name: server$' "$state/devenv.yaml") == 1 && $(grep -c '    - name: client$' "$state/devenv.yaml") == 1 ]]
  if [[ $kind == pathname ]]; then
    grep -q /socket "$state/devenv.yaml"
  elif grep -q /socket "$state/devenv.yaml"; then die 'Unexpected abstract socket mount'; fi
done
if cmake "-DPROJECT_ROOT=$fake" "-DWORK=$state" -DKIND=tcp -P cmake/RenderDevenv.cmake > "$work/error" 2>&1; then die 'Invalid socket kind accepted'; fi
for size in 0 8193 -1 abc 1.5 999999999999999999999; do
  if REMOTEDB_E2E_MIB=$size "$fake/scripts/devenv.sh" up > "$work/error" 2>&1; then die 'Invalid transfer size accepted'; fi
done
ln -s "$work" "$state/state/escape"
if "$fake/scripts/devenv.sh" reset > "$work/error" 2>&1; then die 'Service symlink accepted'; fi
[[ -L $state/state/escape && -f $state/devenv.yaml ]]
rm -- "$state/state/escape"
ln -s "$work" "$state/storage"
if "$fake/scripts/devenv.sh" reset > "$work/error" 2>&1; then die 'Storage symlink accepted'; fi
rm -- "$state/storage"
mkdir "$state/storage"
printf '[engine]\n' > "$state/containers.conf"
cat > "$work/bin/podman" <<'PODMAN'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> "$TEST_PODMAN_LOG"
[[ -f $TEST_MANIFEST ]] || exit 1
PODMAN
chmod +x "$work/bin/podman"
PATH="$work/bin:$PATH" REMOTEDB_E2E_MIB=invalid TEST_PODMAN_LOG="$work/calls" TEST_MANIFEST="$state/devenv.yaml" "$fake/scripts/devenv.sh" reset
sed -n '1p' "$work/calls" | grep -q 'kube down'
sed -n '2p' "$work/calls" | grep -q 'system reset'
[[ ! -e $state && -f $fake/devenv.yaml.in ]]
echo 'Manifest, limits, isolation and unprivileged reset contracts passed'
