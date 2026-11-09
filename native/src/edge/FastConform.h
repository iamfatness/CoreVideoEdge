#pragma once

// Fits one Zoom I420 frame into the ISO's fixed 1920x1080 NV12 picture, fast
// enough for 8 ISOs at 30 fps on the recorder threads.
//
// Why not Pro's conformI420ToNv12 (modules/IsoFrameConform.h, lifted
// verbatim and kept): it is a per-pixel box filter with integer divisions per
// output sample, written for occasional downscales. Live 2026-11-09 Zoom
// delivered guests at 1280x720 into the 1080p files; at ~35 ms per frame each
// recorder thread spent 95% of its time scaling, fell 4+ frames behind, and
// the engine's 4-slot ISO ring overwrote ~20% of the frames. Upscaling with a
// one-pixel box is also nearest-neighbour: blocky.
//
// This one:
//  - same size: straight copy + chroma interleave (bit-exact, the 1080p path);
//  - otherwise separable bilinear with precomputed fixed-point tables, one
//    vertical blend per output row then one horizontal pass. Good for upscale
//    and for downscale down to 1/2;
//  - downscale beyond 2x (never seen from Zoom at a 1080p subscription) falls
//    back to Pro's box filter, which does not alias;
//  - aspect-preserving and centred via Pro's isoFitRect (even edges);
//  - bars are FULL-RANGE black (Y=0, UV=128): the ISO is BT.709 full range.
//    Pro's conform writes studio black (16), which reads as grey here.
//
// Not thread-safe per instance; one per recorder thread (it reuses buffers).

#include "modules/IsoFrameConform.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace corevideo::edge {

class FastConform {
 public:
  // `out` is resized to dstW*dstH*3/2 and fully written.
  void run(const std::uint8_t* i420, int srcW, int srcH, int dstW, int dstH, std::vector<std::uint8_t>& out) {
    const std::size_t dstLuma = static_cast<std::size_t>(dstW) * dstH;
    out.resize(dstLuma + dstLuma / 2);
    std::uint8_t* dstY = out.data();
    std::uint8_t* dstUV = dstY + dstLuma;
    if (!i420 || srcW < 2 || srcH < 2 || dstW < 2 || dstH < 2) {
      std::memset(dstY, 0, dstLuma);
      std::memset(dstUV, 128, dstLuma / 2);
      return;
    }
    const std::size_t srcLuma = static_cast<std::size_t>(srcW) * srcH;
    const std::uint8_t* srcY = i420;
    const std::uint8_t* srcU = i420 + srcLuma;
    const std::uint8_t* srcV = srcU + srcLuma / 4;

    if (srcW == dstW && srcH == dstH) {
      std::memcpy(dstY, srcY, srcLuma);
      for (std::size_t i = 0; i < srcLuma / 4; ++i) {
        dstUV[i * 2] = srcU[i];
        dstUV[i * 2 + 1] = srcV[i];
      }
      return;
    }
    const auto fit = modules::isoFitRect(srcW, srcH, dstW, dstH);
    if (fit.width <= 0 || fit.height <= 0 || srcW > fit.width * 2 || srcH > fit.height * 2) {
      // Big downscale: Pro's box filter (anti-aliased), then fix its bar black.
      modules::conformI420ToNv12(i420, srcW, srcH, dstW, dstH, out);
      fixBars(out.data(), dstW, dstH, fit);
      return;
    }
    // Bars first (only the parts outside the fit rect are left showing).
    if (fit.width != dstW || fit.height != dstH) {
      std::memset(dstY, 0, dstLuma);
      std::memset(dstUV, 128, dstLuma / 2);
    }
    // Luma.
    scalePlane(srcY, srcW, srcH, srcW, fit.width, fit.height,
               dstY + static_cast<std::size_t>(fit.y) * dstW + fit.x, dstW, 1, lumaTables_);
    // Chroma: half size both sides; write U and V interleaved (step 2).
    const int cw = srcW / 2, ch = srcH / 2, fw = fit.width / 2, fh = fit.height / 2;
    std::uint8_t* uvOrigin = dstUV + static_cast<std::size_t>(fit.y / 2) * dstW + fit.x;
    scalePlane(srcU, cw, ch, cw, fw, fh, uvOrigin, dstW, 2, chromaTables_);
    scalePlane(srcV, cw, ch, cw, fw, fh, uvOrigin + 1, dstW, 2, chromaTables_);
  }

 private:
  struct Tables {
    int srcW = 0, srcH = 0, dstW = 0, dstH = 0;
    std::vector<int> x0, y0;               // left / top source index
    std::vector<std::uint16_t> xw, yw;     // weight of the right / bottom neighbour, 0..256
    std::vector<std::uint16_t> row;        // vertically blended source row, x256
  };

  static void build(Tables& t, int srcW, int srcH, int dstW, int dstH) {
    if (t.srcW == srcW && t.srcH == srcH && t.dstW == dstW && t.dstH == dstH) return;
    t.srcW = srcW; t.srcH = srcH; t.dstW = dstW; t.dstH = dstH;
    // Centre-aligned mapping: src = (dst + 0.5) * src/dst - 0.5, clamped.
    const auto axis = [](int src, int dst, std::vector<int>& index, std::vector<std::uint16_t>& weight) {
      index.resize(static_cast<size_t>(dst));
      weight.resize(static_cast<size_t>(dst));
      for (int d = 0; d < dst; ++d) {
        const long long num = (2LL * d + 1) * src - dst;  // (2d+1)*src/dst - 1, over 2*dst
        long long pos256 = num * 256 / (2LL * dst);       // source position x256
        if (pos256 < 0) pos256 = 0;
        const long long max256 = static_cast<long long>(src - 1) * 256;
        if (pos256 > max256) pos256 = max256;
        int i = static_cast<int>(pos256 >> 8);
        int w = static_cast<int>(pos256 & 255);
        if (i >= src - 1) { i = src - 2; w = 256; }
        index[static_cast<size_t>(d)] = i;
        weight[static_cast<size_t>(d)] = static_cast<std::uint16_t>(w);
      }
    };
    axis(srcW, dstW, t.x0, t.xw);
    axis(srcH, dstH, t.y0, t.yw);
    t.row.resize(static_cast<size_t>(srcW));
  }

  // Scales one 8-bit plane (srcW x srcH, row pitch srcPitch) into a dstW x
  // dstH region at `dst` (row pitch dstPitch, sample step `step`).
  static void scalePlane(const std::uint8_t* src, int srcW, int srcH, int srcPitch, int dstW, int dstH,
                         std::uint8_t* dst, int dstPitch, int step, Tables& t) {
    build(t, srcW, srcH, dstW, dstH);
    std::uint16_t* row = t.row.data();
    const int* x0 = t.x0.data();
    const std::uint16_t* xw = t.xw.data();
    for (int y = 0; y < dstH; ++y) {
      const int sy = t.y0[static_cast<size_t>(y)];
      const unsigned wy = t.yw[static_cast<size_t>(y)];
      const std::uint8_t* a = src + static_cast<std::size_t>(sy) * srcPitch;
      const std::uint8_t* b = a + srcPitch;
      const unsigned ia = 256 - wy;
      for (int x = 0; x < srcW; ++x) row[x] = static_cast<std::uint16_t>(a[x] * ia + b[x] * wy);  // x256
      std::uint8_t* out = dst + static_cast<std::size_t>(y) * dstPitch;
      for (int x = 0; x < dstW; ++x) {
        const int sx = x0[x];
        const unsigned wx = xw[x];
        const unsigned v = row[sx] * (256 - wx) + row[sx + 1] * wx;  // x65536
        out[static_cast<std::size_t>(x) * step] = static_cast<std::uint8_t>((v + 32768) >> 16);
      }
    }
  }

  static void fixBars(std::uint8_t* nv12, int dstW, int dstH, const modules::IsoFitRect& fit) {
    if (fit.width <= 0 || fit.height <= 0) return;
    for (int y = 0; y < dstH; ++y) {
      std::uint8_t* row = nv12 + static_cast<std::size_t>(y) * dstW;
      if (y < fit.y || y >= fit.y + fit.height) { std::memset(row, 0, static_cast<size_t>(dstW)); continue; }
      std::memset(row, 0, static_cast<size_t>(fit.x));
      std::memset(row + fit.x + fit.width, 0, static_cast<size_t>(dstW - fit.x - fit.width));
    }
  }

  Tables lumaTables_, chromaTables_;
};

}  // namespace corevideo::edge
