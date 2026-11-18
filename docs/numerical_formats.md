# Numerical Formats of the Main Datapath Quantities

**Source: reviewer-response PDF, Q2.7, Table IX.**

| Quantity | Representation | Scale |
| --- | --- | --- |
| Input and pyramid pixels | `ap_uint<8>` | 1 |
| Sampling flow and fractional components α, β | `ap_fixed<40,8>` | 2⁻³² |
| Interpolation-weight product αβ | `ap_fixed<40,8>` | 2⁻³² |
| Image gradients and pixel differences | `ap_fixed<25,12>` | 2⁻¹³ |
| Cx/Cy coefficients and their accumulations | `ap_fixed<56,30>` | 2⁻²⁶ |
| Normal-matrix accumulations and right-hand side | `ap_fixed<56,30>` | 2⁻²⁶ |
| 2 × 2 solver and within-level flow updates | 32-bit single-precision floating point | — |

For `ap_fixed<W,I>`, W is the total bit width and I includes the sign bit, so the represented value is $x = q\,2^{-(W-I)}$, where q is the stored signed integer. The 32-bit fractional part of the sampling flow keeps the truncation error in α and β negligible, while the 26 fractional bits of the accumulated coefficients limit error growth over the tracking window. The normal matrix and right-hand side are converted to single precision before the 2 × 2 solve, and the updated flow is converted back to fixed point for sampling, coefficient evaluation, and interlevel propagation.
