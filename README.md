# FPGA Optical Flow Tracker

An FPGA accelerator for sparse pyramidal Lucas–Kanade optical-flow tracking. It tracks up to 150 feature points between image pairs using a three-level pyramid and three parallel processing lanes.

| Item | Specification |
| --- | --- |
| Platform | AMD Kria KV260 |
| FPGA device | XCK26-SFVC784-2LV-C |
| Clock frequency | 250 MHz |
| Tracking throughput | **912.51 FPS** on EuRoC MH_05 |

Performance is measured over 1,000 adjacent frame pairs, with up to 150 features per pair. Timing includes patch extraction, packing, input/output synchronization, hardware execution, and result recovery, starting from decoded images and prepared feature coordinates.

This repository provides the accelerator xclbin and matching firmware in `hardware/`, C++ host code in `host/`, and the firmware installation script in `scripts/`.

The final three-core design uses 56,573 LUTs, 69,156 FFs, 247 DSPs, and 477 KB of BRAM, achieves 912.51 fps, and keeps an AEE of 0.3911 px on Middlebury and 0.0991 px on EuRoC.

## Reviewer 1 — Q1.3.3: Host code, bitstream, and deployment files

> The host code, bitstream, and deployment files are publicly available at https://github.com/djdgctw/FPGA-Optical-Flow-Tracker.

[Host code](host/) · [Bitstream and deployment files](hardware/) · [KV260 reproduction instructions](docs/deployment.md) · [On-Board Latency and Throughput — CSV](data/mh05_performance.csv)

## Reviewer 2 — Q2.6: Coefficient-State Reuse and Kernel Speedup across Eight EuRoC Sequences

> We added tests on the eight EuRoC sequences to provide statistics on the average number of LK iterations per feature, the interpolation-cell change rate, and the coefficient-reuse ratio. The results are shown in Table VII.

> To make the data fully accessible, we will upload these statistics to our open-source repository at https://github.com/djdgctw/FPGA-Optical-Flow-Tracker.

[Table VII and footnotes](docs/coefficient_reuse.md) · [Table VII — CSV](data/coefficient_reuse.csv)

## Reviewer 2 — Q2.7: Numerical Formats of the Main Datapath Quantities

> Table IX specifies the bit widths and scaling choices of the main datapath quantities, and these formats are identical across the compared ablation configurations.

> To make the details fully accessible, they will be uploaded to our open-source repository at https://github.com/djdgctw/FPGA-Optical-Flow-Tracker.

[Table IX and numerical-format explanation](docs/numerical_formats.md)

Upon acceptance of the manuscript, the complete FPGA design source code and the KV260 reproduction instructions will be released in the same repository.
