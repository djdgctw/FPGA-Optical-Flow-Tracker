# FPGA Optical Flow Tracker — KV260 Burst-Read Release

This release provides the board-tested three-lane coefficient-domain pyramidal Lucas–Kanade accelerator for the AMD Kria KV260, together with its standalone C++ host benchmark. The hardware image runs at 250 MHz and uses the KV260 base platform without VCU.

The release contains one accelerator configuration. Hardware implementation sources, alternative accelerator builds, and SLAM software are outside this release.

## Contents

| Path | Content |
| --- | --- |
| `hardware/lk_coeff_triple.xclbin` | Formal, uninstrumented three-lane burst-read image |
| `hardware/firmware/lk-coeff-burst-250/` | Matching PL firmware, device-tree overlay, and XRT shell descriptor |
| `host/benchmark_kv260_lk.cpp` | Exact host source used for the board benchmarks |
| `host/kv260_patch.hpp`, `host/kv260_flow.hpp` | NEON patch extraction and signed fixed-point result conversion |
| `host/build.sh` | Native aarch64 build helper |
| `scripts/run_benchmarks.py` | Three-round serial/pipeline benchmark and output-equivalence checks |
| `RELEASE-MANIFEST.json`, `SHA256SUMS` | Artifact identity, configuration, and checksums |

## Tested environment

- AMD Kria KV260, `xck26-sfvc784-2LV-c`, four Cortex-A53 cores online at approximately 1.333 GHz.
- aarch64 Linux with XRT/ZOCL 2.13.0, `xmutil`, OpenCV 4.5.4, GCC with C++17, and Python 3.
- Vitis 2022.2 / Vivado 2022.2.2 base-platform build; this binary is not a 2022.1 platform build.
- Kernel `lk_coeff_triple`: three lanes inside one compute unit, at most 150 features per frame pair.

The firmware and device tree must be used together. An xclbin alone does not create the required PS–PL device-tree bindings on this edge platform. This image is specific to the stated platform and runtime environment.

## Build and load

On the KV260, install the OpenCV and XRT development packages supplied for your board image. For an environment where the XRT runtime is already configured:

```bash
sudo apt-get install build-essential pkg-config libopencv-dev python3
git clone https://github.com/djdgctw/FPGA-Optical-Flow-Tracker.git
cd FPGA-Optical-Flow-Tracker
./scripts/verify_release.sh
./host/build.sh
sudo ./scripts/install_firmware.sh
sudo xmutil unloadapp
sudo xmutil loadapp lk-coeff-burst-250
```

If XRT headers or libraries are installed elsewhere, set `XRT_INCLUDE_DIR` and `XRT_LIBRARY_DIR` before building. The default header directory is `/usr/include/xrt`; the library is `libxrt_coreutil`. The board user needs permission to access the render node created by ZOCL.

## Benchmark EuRoC MH_05

Download EuRoC separately. Point the benchmark at the directory containing the grayscale cam0 PNG files, normally `MH_05_difficult/mav0/cam0/data`. Images are sorted by filename. A 1,000-pair run needs at least 1,001 images.

```bash
python3 scripts/run_benchmarks.py   /path/to/MH_05_difficult/mav0/cam0/data   results/mh05 --pairs 1000 --rounds 3
```

The runner executes both serial and overlapped tracking with the same features. Its first serial run selects features before the timer and saves them. Subsequent runs import those saved coordinates. To reuse an existing compatible feature set, pass `--points-prefix /path/to/reference_prefix`; that prefix must have `.counts.bin`, `.points.bin`, and `.files.txt` files in the format described below. Exact reproduction of an earlier experiment requires its original coordinates and image order; the default run does not claim to regenerate those coordinates identically across OpenCV versions.

The pipeline uses three preparation workers, one main thread, eight preallocated buffer slots, NEON patch extraction, normal BOs, main-thread synchronization, and blocking XRT waits. The serial path uses one active slot. The runner fixes these settings and rejects differing outputs. Results must use a new output directory.

### Timing boundary and reference performance

Prepared-feature tracking starts from decoded images and available feature coordinates. Its wall timer includes patch extraction with subpixel sampling and border handling, layout, input synchronization, parameter setup and launch, PL execution, output synchronization, fixed-point conversion, and restoration of feature order and image coordinates. The pipeline timer includes fill and drain. Image decoding, feature detection/import, and BO allocation occur before that timer.

The formal MH_05 reference is **887.35 tracking FPS** over the first 1,000 adjacent frame pairs, pooled across three rounds at 250 MHz. Each round contains 149,801 valid features (149.801 per pair on average), with 149,805 executed tasks including four padding tasks. This is the capacity to process supplied frame pairs; it is not a complete vision-system frame rate. Host scheduling and the runtime environment affect repeated results.

FPS is total processed frame pairs divided by total directly measured wall time. Do not average per-frame FPS or sum overlapping stage timers to derive pipeline throughput. `execute_host_wait_ms` measures a host wait interval, not a PL hardware cycle counter; the released formal image has no added measurement counters.

Full PL implementation resources are 52,786 total LUTs, 64,279 FFs, 247 DSPs, and 106 BRAM36 equivalents. Total LUT includes LUTRAM and SRL. The accelerator CU itself uses 45,535 total LUTs, 51,390 FFs, 247 DSPs, and 97.5 BRAM36 equivalents.

## Direct host invocation

```bash
LK_HALFPIX_NEON=1 ./build/benchmark_kv260_lk   hardware/lk_coeff_triple.xclbin /path/to/cam0/data 1000   prepared-pipeline 3 8 direct reuse results/example
```

Use `prepared-serial 1 2 direct reuse` for serial stage timing; the serial implementation allocates one active slot despite the shared argument parser requiring a slot argument of at least two. `detect-serial` / `detect-pipeline` additionally include feature detection on predecoded images. `full-serial` / `full-pipeline` include PNG decoding and detection. These broader modes have different timing boundaries from the reference above.

### Output formats

Each run writes `.json` aggregate timings and `.csv` per-frame timings, plus:

- `.counts.bin`: one little-endian int32 valid-feature count per frame pair.
- `.points.bin`: interleaved little-endian float32 source coordinates `(x,y)` for valid features, frame by frame.
- `.coords.bin`: corresponding float32 tracked coordinates, in the same order.
- `.flows.bin`: two signed little-endian int64 fixed-point displacements per valid feature. Values use 32 fractional bits and are sign-extended from the hardware's 40-bit representation.
- `.files.txt`: ordered input basenames, including the final image.

The runner saves `summary.json`, verifies raw flows and recovered coordinates bit for bit, records valid/executed/padded task counts per frame, and calculates pooled FPS from the measured wall times.

## Data interface

Each feature supplies two adjacent 64×64 uint8 patches, `[previous][current]`, totaling 8,192 bytes. Feature index `i` maps to lane `i % 3` and slot `i / 3`. Each lane executes `ceil(valid_features / 3)` tasks; unused final tasks are zero-padded. Feature IDs and original coordinates stay on the host and are recovered by array position.

Each lane builds a three-level local pyramid (64/32/16), then runs coarse-to-fine LK with an 11×11 window, at most seven iterations per level, the formal squared-update convergence threshold of 0.01, and zero coarse-level initialization. Input AXI interfaces are 512 bits wide; outputs are 64 bits wide and store 40-bit Q8.32 values. The accelerator reads DDR directly through its m_axi interfaces; no separate AXI DMA is required. BO input and output synchronization remains necessary.

## License

Host code and helper scripts use the repository's existing GPL-3.0-only license; see `LICENSE`. The hardware image and matching firmware are supplied as prebuilt binary artifacts. This release does not include the FPGA implementation source.
