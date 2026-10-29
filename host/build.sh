#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $(uname -m) != aarch64 ]]; then
  echo 'Build natively on the KV260 aarch64 Linux system.' >&2
  exit 2
fi
mkdir -p "$root/build"
read -r -a opencv_flags <<< "$(pkg-config --cflags --libs opencv4)"
xrt_include=${XRT_INCLUDE_DIR:-/usr/include/xrt}
xrt_flags=()
if [[ -n ${XRT_LIBRARY_DIR:-} ]]; then
  xrt_flags+=(-L"$XRT_LIBRARY_DIR" -Wl,-rpath,"$XRT_LIBRARY_DIR")
fi
g++ -O3 -mcpu=cortex-a53 -std=c++17 -pthread -Wall -Wextra   -DLK_BENCH_LANES=3 -I"$xrt_include"   "$root/host/benchmark_kv260_lk.cpp" "${opencv_flags[@]}"   "${xrt_flags[@]}" -lxrt_coreutil -o "$root/build/benchmark_kv260_lk"
echo "Built $root/build/benchmark_kv260_lk"
