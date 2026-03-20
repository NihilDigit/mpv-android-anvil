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

## Building from Source

### Prerequisites

| Requirement | Notes |
|-------------|-------|
| Linux x86_64 host | Tested on Arch Linux; Ubuntu 22.04+ should work |
| Android SDK | Auto-downloaded by build script if `ANDROID_HOME` not set |
| Android NDK r29 | Auto-downloaded by build script |
| Qualcomm QAIRT SDK | **Manual install required** — provides QNN headers + HTP runtime .so files |
| ~20 GB disk space | For NDK, SDK, and built dependencies |

### Step 1: Install Qualcomm QAIRT SDK

Download the Qualcomm AI Engine Direct SDK (QAIRT) from [Qualcomm AI Hub](https://aihub.qualcomm.com/) or the Qualcomm package manager. Version 2.42+ is recommended.

After installation, set the environment variable:

```sh
export QAIRT_SDK_ROOT=/opt/qcom/aistack/qairt/2.42.0.251225  # adjust to your version
# Verify:
ls "$QAIRT_SDK_ROOT/include/QNN/QnnInterface.h"  # should exist
```

### Step 2: Copy QNN Runtime Libraries

The QAIRT SDK provides proprietary shared libraries that must be bundled in the APK. These are **not** included in the repo (they are `.gitignore`d).

```sh
# ARM64 runtime libraries → packaged into APK's jniLibs
mkdir -p app/src/main/qnnLibs/arm64-v8a
cp "$QAIRT_SDK_ROOT/lib/aarch64-android/libQnnHtp.so"        app/src/main/qnnLibs/arm64-v8a/
cp "$QAIRT_SDK_ROOT/lib/aarch64-android/libQnnSystem.so"     app/src/main/qnnLibs/arm64-v8a/
cp "$QAIRT_SDK_ROOT/lib/aarch64-android/libQnnHtpPrepare.so" app/src/main/qnnLibs/arm64-v8a/
cp "$QAIRT_SDK_ROOT/lib/aarch64-android/libQnnHtpV75Stub.so" app/src/main/qnnLibs/arm64-v8a/

# Hexagon Skel binary (32-bit DSP code) → packaged into APK assets
mkdir -p app/src/main/assets/anvil
cp "$QAIRT_SDK_ROOT/lib/hexagon-v75/unsigned/libQnnHtpV75Skel.so" app/src/main/assets/anvil/
```

> **Note:** The V75 Skel/Stub targets Snapdragon 8 Gen 3 (SM8650). For other SoCs, replace with the appropriate version (e.g., `v73` for SD 8 Gen 2, `v69` for SD 7+ Gen 2).

### Step 3: Build

The build script downloads Android SDK/NDK (if needed), compiles FFmpeg, libplacebo, mpv, and the ANVIL filter, then produces the APK:

```sh
cd buildscripts
export QAIRT_SDK_ROOT=/opt/qcom/aistack/qairt/2.42.0.251225
bash buildall.sh --arch arm64
```

First build takes ~15-30 minutes. Subsequent builds are incremental (~5 seconds if only ANVIL filter changed).

**Output APKs:**
```
app/build/outputs/apk/default/debug/app-default-arm64-v8a-debug.apk
app/build/outputs/apk/default/release/app-default-arm64-v8a-release-unsigned.apk
```

### Step 4: Install and First Run

```sh
adb install -r app/build/outputs/apk/default/debug/app-default-arm64-v8a-debug.apk
```

On first launch, the app extracts QNN assets (context binary + Skel) from the APK to its private storage. The demo videos are also extracted on first tap.

**Required device-side config** (`/data/data/com.nihildigit.anvildemo/files/mpv.conf`):
```
vf=anvil
vd-lavc-o=flags2=+export_mvs
hwdec=no
pause=no
loop=inf
```

Create it via:
```sh
adb shell "run-as com.nihildigit.anvildemo sh -c '
echo vf=anvil > files/mpv.conf
echo vd-lavc-o=flags2=+export_mvs >> files/mpv.conf
echo hwdec=no >> files/mpv.conf
echo pause=no >> files/mpv.conf
echo loop=inf >> files/mpv.conf
'"
```

### Step 5: Verify

Check logcat for successful initialization:
```sh
adb logcat -v brief -s mpv | grep ANVIL
```

Expected output:
```
ANVIL VFI frame-doubler (30fps -> 60fps, Vulkan GPU + HTP)
QNN: graph 'D_unet_v3bs_nomv_1080p', 1 inputs, 1 outputs
QNN: HTP perf profile = power_saver (err=0x0)
ANVIL: QNN HTP ready at /data/data/com.nihildigit.anvildemo/files/anvil
ANVIL: Vulkan GPU compute ready (1920x1080)
ANVIL: HTP async thread started (pipeline parallelism)
ANVIL[GPU/Q/async]: total=25.5ms  P1a=3.3  GPU=2.7  copy=0.9  P3=13.5  P4(GPU)=2.7
```

### Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `QNN: cannot open .../context.serialized.bin` | Assets not extracted | Clear app data and relaunch |
| `QNN: dlopen libQnnHtp.so: ...` | Missing QNN .so files | Re-run Step 2 and rebuild |
| `QNN: contextCreateFromBinary failed` | Skel file missing or wrong version | Check `libQnnHtpV*Skel.so` in assets |
| `qnn=0` in frame logs | QNN init failed | Check all above; verify device has Hexagon DSP |
| No `ANVIL[...]` timing logs | Filter not active | Verify `mpv.conf` has `vf=anvil` and `hwdec=no` |
| Video plays but no interpolation | H.265/VP9 codec or non-1080p | ANVIL requires H.264 at 1920×1080 |

### Usage

- **Demo videos**: Tap any of the 4 scenario cards on the launcher screen
- **Custom video**: Tap "Load Custom Video" — the app validates H.264 codec and 1080p resolution before playing
- **A/B comparison**: During playback, tap the screen to show controls, then tap the green **VFI** button to toggle interpolation on/off
- **Timing data**: Per-frame latency is logged to logcat with tag `mpv` and prefix `ANVIL[GPU/Q/async]`

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
