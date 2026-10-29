#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source_dir="$root/hardware/firmware/lk-coeff-burst-250"
target=/lib/firmware/xilinx/lk-coeff-burst-250
if [[ $EUID != 0 ]]; then
  echo "Run with sudo: $0" >&2
  exit 2
fi
"$root/scripts/verify_release.sh"
if [[ -e "$target" ]]; then
  for name in lk_coeff_triple.bit.bin pl.dtbo shell.json; do
    if ! cmp -s "$source_dir/$name" "$target/$name"; then
      echo "Existing firmware differs: $target/$name. Back it up and move it before installing." >&2
      exit 1
    fi
  done
  echo "Matching firmware is already installed: $target"
else
  install -d -m 755 "$target"
  for name in lk_coeff_triple.bit.bin pl.dtbo shell.json; do
    install -m 644 "$source_dir/$name" "$target/$name"
  done
  echo "Installed: $target"
fi
echo 'Load with: sudo xmutil unloadapp && sudo xmutil loadapp lk-coeff-burst-250'
