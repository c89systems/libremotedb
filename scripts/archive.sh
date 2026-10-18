#!/usr/bin/env bash
set -euo pipefail
[[ $# == 3 && -d $1/$2 && $2 != */* ]] || { echo 'Usage: archive.sh STAGING_PARENT ROOT_NAME DESTINATION' >&2; exit 2; }
destination=$3
trap 'rm -f -- "$destination.partial"' EXIT
tar --sort=name --format=gnu --mtime=@0 --owner=0 --group=0 --numeric-owner \
  --mode=u+rwX,go+rX,go-w -C "$1" -cf - "$2" | gzip -n -9 > "$destination.partial"
mv -- "$destination.partial" "$destination"
