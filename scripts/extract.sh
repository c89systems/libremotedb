#!/usr/bin/env bash
set -euo pipefail
[[ $# == 3 ]] || { echo 'Usage: extract.sh ARCHIVE OWNED_DESTINATION ROOT_NAME' >&2; exit 2; }
archive=$1 destination=$2 expected=$3
while IFS= read -r path; do
  case "$path" in "$expected"|"$expected/"|"$expected/"*) ;; *) echo "Unexpected archive root: $path" >&2; exit 1;; esac
  clean=${path%/}
  case "/$clean/" in */../*|*//*) echo "Unsafe archive member: $path" >&2; exit 1;; esac
 done < <(tar -tf "$archive")
listing=$(tar -tvf "$archive")
[[ -n $listing ]] || { echo 'Empty archive' >&2; exit 1; }
while IFS= read -r line; do
  case ${line:0:1} in
    -|d) ;;
    l) link=${line##* -> }; case "/$link/" in */../*|//*|*//*) echo "Unsafe archive link: $link" >&2; exit 1;; esac;;
    *) echo "Unsupported archive entry: $line" >&2; exit 1;;
  esac
done <<< "$listing"
tar --extract --gzip --file "$archive" --directory "$destination" --no-same-owner
