#pragma once
#include <opencv2/imgproc.hpp>
#include <cmath>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

// Specialize only the exact integer-center, 64x64, interior, grayscale case.
// The even patch width still samples at half pixels. Fractional centers and
// boundary patches must retain OpenCV's general interpolation/border handling.
inline void halfpixel_patch(const cv::Mat& image, cv::Point2f center, cv::Mat& patch) {
  const int x = int(std::floor(center.x))-32, y = int(std::floor(center.y))-32;
  if (center.x != std::floor(center.x) || center.y != std::floor(center.y) ||
      x < 0 || y < 0 || x+64 >= image.cols || y+64 >= image.rows ||
      image.type()!=CV_8UC1 || patch.type()!=CV_8UC1 || patch.rows!=64 || patch.cols!=64) {
    cv::getRectSubPix(image,cv::Size(64,64),center,patch);
    return;
  }
  for(int row=0;row<64;++row) {
    auto a=image.ptr<uint8_t>(y+row)+x, b=image.ptr<uint8_t>(y+row+1)+x;
    auto out=patch.ptr<uint8_t>(row);
    for(int col=0;col<64;col+=16) {
#ifdef __aarch64__
      auto a0=vld1q_u8(a+col), a1=vld1q_u8(a+col+1);
      auto b0=vld1q_u8(b+col), b1=vld1q_u8(b+col+1);
      auto lo=vaddq_u16(vaddl_u8(vget_low_u8(a0),vget_low_u8(a1)),
                        vaddl_u8(vget_low_u8(b0),vget_low_u8(b1)));
      auto hi=vaddq_u16(vaddl_u8(vget_high_u8(a0),vget_high_u8(a1)),
                        vaddl_u8(vget_high_u8(b0),vget_high_u8(b1)));
      vst1q_u8(out+col,vcombine_u8(vrshrn_n_u16(lo,2),vrshrn_n_u16(hi,2)));
#else
      for(int j=col;j<col+16;++j) out[j]=uint8_t((int(a[j])+a[j+1]+b[j]+b[j+1]+2)>>2);
#endif
    }
  }
}
