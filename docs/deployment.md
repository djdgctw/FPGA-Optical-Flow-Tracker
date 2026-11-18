# KV260 reproduction instructions

```bash
sudo xmutil unloadapp
sudo ./scripts/install_firmware.sh
sudo xmutil loadapp lk-coeff-burst-250
./host/build.sh
```

```bash
./build/lk_optical_flow hardware/lk_coeff_triple.xclbin \
  /path/to/MH_05_difficult/mav0/cam0/data \
  1000 results/mh05.csv
```

## On-Board Latency and Throughput

**Source: reviewer-response PDF, Q1.3.3, Section VII.C.**

On-Board Latency and Throughput: The implemented system overlaps PS preparation with PL execution across batches. Measurements are performed on KV260 with the PL at 250 MHz. Given feature coordinates, we measure latency from block preparation to result retrieval. Tests use 1000 consecutive EuRoC MH 05 frame pairs, each with up to 150 features, and are repeated three times.

In the serial baseline shown in Fig. 1, block preparation, input synchronization, PL execution, and result retrieval proceed sequentially. Block preparation dominates the latency at 2.664 ms, while PL execution takes 0.778 ms. The mean time per frame pair is 3.642 ms, corresponding to a throughput of 274.58 fps.

Fig. 2, three PS threads prepare successive batches across eight reusable shared-DDR buffer slots while the PL processes one batch at a time. Input synchronization for batch k overlaps with PLk−1 , and its result retrieval overlaps with PLk+1 . This overlap reduces the mean time per frame pair to 1.096 ms, achieving 912.51 fps and a 3.32× throughput improvement over the serial baseline.

[On-Board Latency and Throughput — CSV](../data/mh05_performance.csv)
