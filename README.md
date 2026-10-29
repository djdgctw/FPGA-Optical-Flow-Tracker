# FPGA Optical Flow Tracker

An FPGA accelerator for sparse pyramidal Lucas–Kanade optical-flow tracking. It tracks up to 150 feature points between image pairs using a three-level pyramid and three parallel processing lanes.

| Item | Specification |
| --- | --- |
| Platform | AMD Kria KV260 |
| FPGA device | XCK26-SFVC784-2LV-C |
| Clock frequency | 250 MHz |
| Tracking throughput | **887.35 FPS** on EuRoC MH_05 |

Performance is measured over 1,000 adjacent frame pairs, with up to 150 features per pair. Timing includes patch extraction, packing, input/output synchronization, hardware execution, and result recovery, starting from decoded images and prepared feature coordinates.

This repository provides the accelerator xclbin and matching firmware in `hardware/`, C++ host benchmark code in `host/`, and test scripts in `scripts/`.

**The complete FPGA IP source code will be open-sourced after the paper is accepted.**
