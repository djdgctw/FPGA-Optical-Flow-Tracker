# Coefficient-State Reuse and Kernel Speedup across Eight EuRoC Sequences

**Source: reviewer-response PDF, Q2.6, Table VII.**

| Sequence | Mean LK iters. per featureᵃ | Cell change rate (%)ᵇ | Coeff. reuse rate (%) | No reuseᶜ (ms) | With reuseᶜ (ms) | Kernel speedupᵈ |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| MH 01 | 6.766 | 49.411 | 28.157 | 0.991217 | 0.849060 | 1.167× |
| MH 02 | 8.422 | 52.114 | 30.828 | 1.153890 | 0.970630 | 1.189× |
| MH 03 | 8.496 | 53.774 | 29.903 | 1.148264 | 0.962406 | 1.193× |
| MH 04 | 7.583 | 48.133 | 31.346 | 1.088398 | 0.898524 | 1.211× |
| MH 05 | 7.305 | 47.903 | 30.702 | 0.880494 | 0.777739 | 1.132× |
| V1 01 | 8.407 | 44.169 | 35.908 | 1.153971 | 0.918685 | 1.256× |
| V1 02 | 12.719 | 58.105 | 32.013 | 1.572724 | 1.274669 | 1.234× |
| V1 03 | 11.422 | 63.342 | 27.030 | 1.319656 | 1.118748 | 1.180× |
| Overall | 8.859 | 53.486 | 30.762 | 1.184985 | 0.985045 | 1.203× |

- **a** For each feature, LK iterations are summed over the three pyramid levels. The totals are then averaged over all features.
- **b** Percentage of consecutive LK iteration pairs that use different interpolation cells.
- **c** Mean kernel execution time per call (ms).
- **d** Speedup relative to the configuration without coefficient-state reuse.

We added tests on the eight EuRoC sequences to provide statistics on the average number of LK iterations per feature, the interpolation-cell change rate, and the coefficient-reuse ratio. The results are shown in Table VII. They show that the proposed coefficient-state reuse achieves an average speedup of 1.2× across different datasets, demonstrating its effectiveness.

[Table VII — CSV](../data/coefficient_reuse.csv)
