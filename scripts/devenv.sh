#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/common.sh"
requested_action=${1:-}
work=$root/build/devenv
temporary_root=$work/tmp
name=remotedb-$(printf '%s' "$root" | sha256sum | cut -c1-12)
manifest=$work/devenv.yaml
config=$work/containers.conf
settings=$work/settings
safe_work=0
transfer_size=${REMOTEDB_E2E_MIB:-64}
case "$requested_action" in up|test|probe)
if [[ ! $transfer_size =~ ^[0-9]+$ || ${#transfer_size} -gt 4 ]]; then die 'REMOTEDB_E2E_MIB must be an integer from 1 through 8192'; fi
((10#$transfer_size>=1 && 10#$transfer_size<=8192)) || die 'REMOTEDB_E2E_MIB must be an integer from 1 through 8192'
transfer_size=$((10#$transfer_size))
;; esac
boundary() {
  safe_work=0
  generated_roots
  [[ ! -L $work ]] || die 'Refusing symlinked build/devenv root'
  if [[ ! -d $work ]]; then safe_work=1; return 0; fi
  local link
  while IFS= read -r -d '' link; do
    [[ $link == "$work/storage/"* ]] || die "Refusing service-state symlink: $link"
  done < <(find "$work" -type l -print0)
  safe_work=1
}
podman_owned() {
  CONTAINERS_CONF="$config" timeout 120 podman --root "$work/storage" --runroot "$work/run" \
    --tmpdir "$temporary_root/podman" --storage-driver vfs "$@"
}
load() {
  boundary
  [[ -f $settings ]] || die 'No rendered environment; run make dev-up'
  # Generated with Bash %q from this checkout's rendered settings.
  # shellcheck source=/dev/null
  source "$settings"
}
render() {
  boundary
  cmake "-DPROJECT_ROOT=$root" "-DWORK=$work" "-DPOD=$name" "-DUID=$(id -u)" "-DGID=$(id -g)" \
    "-DKIND=$kind" "-DSERVER_IMAGE=$server_image" "-DCLIENT_IMAGE=$client_image" -P cmake/RenderDevenv.cmake
  if [[ $kind == pathname ]]; then control=/socket/control.sock; data=/socket/data.sock; else control=$name-control; data=$name-data; fi
  printf 'kind=%q\ncontrol=%q\ndata=%q\nserver_image=%q\nclient_image=%q\n' "$kind" "$control" "$data" "$server_image" "$client_image" > "$settings"
  chmod 600 "$settings"
  printf '[engine]\ninfra_image = "%s"\ninfra_command = "/bin/client"\nevents_logger = "file"\n' "$client_image" > "$config"
}
client() {
  load
  local action=$1; shift
  podman_owned exec "$@" "$name-client" /bin/client "$action" "$kind" "$control" "$data" "$transfer_size"
}
readiness() {
  local deadline=$((SECONDS+20))
  while ((SECONDS<deadline)); do
    if timeout 2 "$root/scripts/devenv.sh" probe > /dev/null 2>&1; then return; fi
    sleep .1
  done
  die "Control readiness timeout: $control; inspect $work/logs"
}
image_context() {
  local libc=$1 binary=$2 containerfile=$3 context sysroot digest
  scripts/build.sh "e2e-$libc"
  context=$temporary_root/$libc
  mkdir -p "$context"
  cp "build/e2e-$libc/$binary" "$context/$([[ $libc == musl ]] && echo client || echo server)"
  if [[ $libc == gnu ]]; then
    sysroot=$(cache_value "build/e2e-$libc" CMAKE_SYSROOT)
    cp -L "$sysroot/lib/libc.so.6" "$sysroot/lib/ld-linux-x86-64.so.2" "$context/"
  fi
  digest=$( (sha256sum "$containerfile"; cd "$context"; sha256sum ./*) | sha256sum | cut -c1-24)
  image=localhost/$name-$libc:$digest
  podman_owned build --network=none --layers=false --pull=never -t "$image" -f "$root/$containerfile" "$context"
}
play() {
  podman_owned kube play --network=none --userns=keep-id --log-driver=k8s-file "$manifest"
  readiness
  printf 'Endpoints: %s %s %s\nState: %s\n' "$kind" "$control" "$data" "$work"
}
up() {
  [[ $(id -u) != 0 ]] || die 'Rootless Podman is required; run as the ordinary host user'
  [[ $(timeout 120 podman info --format '{{.Host.Security.Rootless}}') == true ]] || die 'Podman must be rootless'
  boundary
  if [[ -f $manifest ]]; then
    load
    [[ ${REMOTEDB_E2E_SOCKET_KIND:-$kind} == "$kind" ]] || die 'Socket kind changed; run make dev-reset first'
    if podman_owned pod exists "$name"; then readiness; else play; fi
    return
  fi
  kind=${REMOTEDB_E2E_SOCKET_KIND:-pathname}
  [[ $kind == pathname || $kind == abstract ]] || die 'REMOTEDB_E2E_SOCKET_KIND must be pathname or abstract'
  mkdir -p "$temporary_root/podman"
  printf '[engine]\nevents_logger = "file"\n' > "$config"
  image_context gnu e2e-server devenv/server.Containerfile; server_image=$image
  image_context musl e2e-client devenv/client.Containerfile; client_image=$image
  render
  play
}
down() { boundary; [[ ! -f $manifest ]] || podman_owned kube down "$manifest"; }
reset() {
  down
  boundary
  if [[ -d $work ]]; then
    [[ ! -f $config || ! -d $work/storage ]] || podman_owned system reset --force
    rm -rf -- "$work"
  fi
}
diagnostics() {
  [[ $safe_work == 1 ]] || return 0
  [[ -f $config ]] || return 0
  mkdir -p "$work/logs"
  podman_owned pod ps > "$work/logs/ps.log" 2>&1 || :
  podman_owned logs "$name-server" > "$work/logs/server.log" 2>&1 || :
  podman_owned logs "$name-client" > "$work/logs/client.log" 2>&1 || :
  cat "$work/logs/"*.log >&2
}
verify_observations() {
  awk -v "high=$((transfer_size/4096))" -v "low=$(((transfer_size%4096)*1048576+17))" \
    -v rows=6 -f tests/observations.awk "$work/state/server/observations"
}
e2e() {
  [[ ! -e $manifest ]] || die 'Existing development state; run make dev-reset before make test-e2e'
  local mode server_pid client_pid foreign
  for mode in pathname abstract; do
    REMOTEDB_E2E_SOCKET_KIND=$mode up
    server_pid=$(podman_owned inspect --format '{{.State.Pid}}' "$name-server")
    client_pid=$(podman_owned inspect --format '{{.State.Pid}}' "$name-client")
    [[ $(readlink "/proc/$server_pid/ns/net") == "$(readlink "/proc/$client_pid/ns/net")" ]] || die 'Containers do not share a network namespace'
    if [[ $mode == abstract ]]; then
      if grep -q /socket "$manifest"; then die 'Abstract mode has a filesystem socket mount'; fi
      [[ ! -e $work/state/sockets ]]
    fi
    client run
    client denied --user "$(($(id -u)+2))"
    client remember
    [[ $(stat -c %a "$temporary_root/client/restart.key") == 600 ]]
    client owner --user "$(($(id -u)+1))" -i < "$temporary_root/client/restart.key"
    down
    for owned in state tmp/client logs credentials; do
      foreign=$(find "$work/$owned" ! -uid "$(id -u)" -print -quit)
      [[ -z $foreign ]] || die "Host ownership mismatch: $foreign"
    done
    if [[ $mode == pathname ]]; then [[ -z $(find "$work/state/sockets" -mindepth 1 -print -quit) ]]; fi
    up
    client restart
    verify_observations
    down
    if [[ $mode == abstract && ${REMOTEDB_E2E_KEEP_DEVSERVICES:-0} == 1 ]]; then up; else reset; fi
  done
}
on_failure() {
  status=$?
  [[ $status == 0 ]] && return
  trap - EXIT
  diagnostics
  if [[ $safe_work == 1 ]]; then
    if [[ ${1:-} == test && ${REMOTEDB_E2E_KEEP_DEVSERVICES:-0} != 1 ]]; then reset || :; elif [[ ${1:-} == up ]]; then down || :; fi
  fi
  printf 'remotedb: devenv failed; inspect command/container output above; reset residual state with make dev-reset\n' >&2
  exit "$status"
}
trap 'on_failure "$requested_action"' EXIT
case ${1:-} in
  up) up;; down) down;; reset) reset;;
  render) load; if podman_owned pod exists "$name"; then die 'Stop services before rendering'; fi; kind=${REMOTEDB_E2E_SOCKET_KIND:-$kind}; render;;
  ps) boundary; [[ ! -e $config ]] || podman_owned pod ps --filter "name=$name";;
  logs) boundary; diagnostics;; test) e2e;; probe) client probe;;
  *) die 'Usage: devenv.sh up|down|reset|render|ps|logs|test';;
esac
