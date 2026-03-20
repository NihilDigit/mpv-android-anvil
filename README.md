# ANVIL VFI Demo — Real-Time NPU Video Frame Interpolation on Android

Demo application for **ANVIL** (Accelerator-Native Video InterpoLation), a video frame interpolation system designed for mobile NPU deployment. This app performs real-time 30→60fps frame doubling on 1080p H.264 video using a three-accelerator pipeline (CPU + GPU + NPU) on Qualcomm Snapdragon SoCs.

> **Paper**: *ANVIL: Accelerator-Native Video InterpoLation for Mobile NPU Deployment* (under review)

## How It Works

```
H.264 Software Decode (with MV side-data export)
  → CPU:  ZOH densify + 4× downsample               (~3 ms)
  → GPU:  Vulkan compute — median + Gauss + warp      (~3 ms)
  → NPU:  QNN HTP INT8 residual network inference    (~13 ms, async)
  → GPU:  Vulkan compute — dequant + residual + YUV   (~3 ms)
  → Display: 50fps interpolated output
```

The key insight: H.264 encoder motion vectors (MVs) provide a free coarse motion prior. ANVIL prealigns frames using these MVs, then a tiny pure-Conv residual network (852K params) refines the result. The network uses only NPU-friendly operators (Conv + ReLU), achieving 77% compute-bound ratio on Hexagon HTP — compared to 5% for RIFE.

## Performance (SM8650 / Snapdragon 8 Gen 3)

| Stage | Hardware | Latency | Description |
|-------|----------|---------|-------------|
| P1a | CPU | 3.3 ms | MV densify + downsample + YUV pack |
| P1b+P2 | GPU (Adreno 750) | 2.7 ms | Prealign v2 + quantized warp |
| Copy | CPU | 0.9 ms | 12 MB uint8 NHWC memcpy |
| P3 | HTP V75 (INT8) | 13.5 ms | ANVIL-S inference (async) |
| P4 | GPU (Adreno 750) | 2.7 ms | Residual + RGB→YUV420 |
| **Total** | | **25 ms** | < 40 ms budget for 25→50 fps |

INT8 quantization loss: **-0.19 dB** (negligible).

## Supported Devices

| SoC | NPU | 1080p INT8 | Status |
|-----|-----|:----------:|--------|
| Snapdragon 8 Gen 3 (SM8650) | HTP V75 | 11.6 ms | Tested |
| Snapdragon 8 Gen 2 (SM8550) | HTP V73 | 15.5 ms | Tested |
| Snapdragon 7+ Gen 2 (SM7475) | HTP V69 | 720p only | Tested |
| Dimensity 9300 | APU 790 | 24.4 ms | Tested |
| Dimensity 9400+ | APU 890 | 25.5 ms | Tested |

## Building

### Prerequisites

- Android SDK & NDK (the build script downloads them automatically)
- Qualcomm QAIRT SDK (for QNN HTP headers and runtime libraries)

### QNN Runtime Setup

This repo does not include proprietary Qualcomm libraries. Provide them from a QAIRT SDK install:

```sh
export QAIRT_SDK_ROOT=/opt/qcom/aistack/qairt/<version>

# ARM64 runtime libraries → bundled in APK
mkdir -p app/src/main/qnnLibs/arm64-v8a
cp "$QAIRT_SDK_ROOT/lib/aarch64-android/libQnnHtp.so"        app/src/main/qnnLibs/arm64-v8a/
cp "$QAIRT_SDK_ROOT/lib/aarch64-android/libQnnSystem.so"     app/src/main/qnnLibs/arm64-v8a/
cp "$QAIRT_SDK_ROOT/lib/aarch64-android/libQnnHtpPrepare.so" app/src/main/qnnLibs/arm64-v8a/
cp "$QAIRT_SDK_ROOT/lib/aarch64-android/libQnnHtpV75Stub.so" app/src/main/qnnLibs/arm64-v8a/

# Hexagon Skel → deployed to device at first run
mkdir -p app/src/main/assets/anvil
cp "$QAIRT_SDK_ROOT/lib/hexagon-v75/unsigned/libQnnHtpV75Skel.so" app/src/main/assets/anvil/
```

The INT8 model context binary (`app/src/main/assets/anvil/context.serialized.bin`) is included in the repo.

### Build

```sh
cd buildscripts
export QAIRT_SDK_ROOT=/opt/qcom/aistack/qairt/<version>
bash buildall.sh --arch arm64
# Output: app/build/outputs/apk/default/debug/app-default-arm64-v8a-debug.apk
```

### Usage

1. Install the APK
2. Open any H.264 video file with the app
3. The ANVIL filter activates automatically on H.264 content with `hwdec=no` and MV export enabled (configured in `mpv.conf`)

The filter logs per-frame timing to logcat under tag `mpv` with prefix `ANVIL[...]`.

## Architecture

The ANVIL VFI filter (`anvil/filter/vf_anvil.c`, ~2800 lines) implements:

- **4 Vulkan compute shaders** for GPU-accelerated prealignment and post-processing
  - `median5.comp` — 5×5 median filter on 1/4-res flow
  - `gauss_sep.comp` — Separable Gaussian σ=2
  - `warp_pack_quant.comp` — Fused warp + YUV→RGB + blend + INT8 quantize
  - `residual_yuv.comp` — Dequant + residual + RGB→YUV420
- **QNN HTP integration** via dlopen (no subprocess) with double-buffered async inference
- **Three-state frame doubling** state machine with correct PTS ordering
- **CPU fallback** path when Vulkan/HTP unavailable (MV blend only)

## Acknowledgments

This demo is built on [mpv-android](https://github.com/mpv-android/mpv-android) (LGPL 2.1+). The ANVIL VFI filter and Vulkan shaders are original work.

## License

GPL (inherited from mpv-android fork). ANVIL filter code: LGPL 2.1+.
