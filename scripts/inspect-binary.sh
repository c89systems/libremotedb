#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "$0")/common.sh"
[[ $# -ge 3 ]] || die 'Usage: inspect-binary.sh BUILD BINARY architecture|loader|exports [client|server]'
configured_build=$1 binary=$2 action=$3
tools=$(scripts/discover_target_tools.sh "$configured_build")
eval "$tools"
case "$action" in
  architecture)
    if [[ $TARGET == *darwin ]]; then
      if [[ $binary == *.a ]]; then
        "$OTOOL" -hv "$binary" | awk '/^MH_/ { seen=1; if ($1 != "MH_MAGIC_64" || $2 != "ARM64") exit 1 } END { if (!seen) exit 1 }'
      else file -L "$binary" | grep -E 'Mach-O.*arm64' >/dev/null; fi
    else
      case "$TARGET" in x86_64*) machine=X86-64; class=ELF64;; aarch64*) machine=AArch64; class=ELF64;; armhf*) machine=ARM; class=ELF32;; *) die "Unknown target: $TARGET";; esac
      "$READELF" -h "$binary" | awk -v machine="$machine" -v class="$class" '
        /Machine:/ { seen=1; if (index($0,machine)==0) bad=1 }
        /Class:/ { if ($2 != class) bad=1 } END { exit !seen || bad }'
    fi
    ;;
  exports)
    expected=$root/cmake/${4:?client or server required}.exports
    if [[ $TARGET == *darwin ]]; then
      actual=$("$NM" -gU "$binary" | awk 'NF>=3 { sub(/^_/,"",$NF); print $NF }' | sort -u)
    else actual=$("$NM" -D --defined-only "$binary" | awk 'NF>=3 { sub(/@.*/,"",$NF); print $NF }' | sort -u); fi
    diff -u <(sort -u "$expected") <(printf '%s\n' "$actual")
    ;;
  loader)
    if [[ $TARGET == *darwin ]]; then
      while IFS= read -r dependency; do
        case "$dependency" in @rpath/*|@loader_path/*|@executable_path/*|/usr/lib/*|/System/Library/*) ;; *) die "Nonrelocatable Mach-O dependency: $dependency";; esac
      done < <("$OTOOL" -L "$binary" | tail -n +2 | sed -E 's/^[[:space:]]+//; s/ \(.*//')
      while IFS= read -r path; do
        case "$path" in @loader_path*|@executable_path*) ;; *) die "Nonrelocatable Mach-O runtime path: $path";; esac
      done < <("$OTOOL" -l "$binary" | sed -nE 's/^[[:space:]]*path (.*) \(offset.*/\1/p')
    else
      metadata=$("$READELF" -l -d "$binary")
      while IFS= read -r interpreter; do
        case "$interpreter" in /lib/*|/lib64/*|/usr/lib/*) ;; *) die "Non-system ELF interpreter: $interpreter";; esac
        [[ /$interpreter/ != */../* ]] || die "Unsafe ELF interpreter: $interpreter"
      done < <(sed -nE 's/.*Requesting program interpreter: (.*)\]/\1/p' <<< "$metadata")
      while IFS= read -r paths; do
        IFS=: read -ra parts <<< "$paths"
        for path in "${parts[@]}"; do [[ $path == "\$ORIGIN"* ]] || die "Nonrelocatable ELF runtime path: $path"; done
      done < <(sed -nE 's/.*\((RPATH|RUNPATH)\).*\[(.*)\].*/\2/p' <<< "$metadata")
    fi
    ;;
  *) die "Unknown inspection: $action";;
esac
