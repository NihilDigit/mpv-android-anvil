# mpv for Android

[![Build Status](https://github.com/mpv-android/mpv-android/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/mpv-android/mpv-android/actions/workflows/build.yml)

mpv-android is a video player for Android based on [libmpv](https://github.com/mpv-player/mpv).

## Features

* Hardware and software video decoding
* Gesture-based seeking, volume/brightness control and more
* libass support for styled subtitles
* Secondary (or dual) subtitle support
* High-quality rendering with advanced settings (scalers, debanding, interpolation, ...)
* Play network streams with the "Open URL" function
* Background playback, Picture-in-Picture, keyboard input supported

### Library?

mpv-android is **not** a library/module (AAR) you can import into your app.

If you'd like to use libmpv in your app you can use our code as inspiration.
The important parts are [`MPVLib`](app/src/main/java/is/xyz/mpv/MPVLib.kt), [`BaseMPVView`](app/src/main/java/is/xyz/mpv/BaseMPVView.kt) and the [native code](app/src/main/jni/).
Native code is built by [these scripts](buildscripts/).

## Downloads

You can download mpv-android from the [Releases section](https://github.com/mpv-android/mpv-android/releases) or

[<img src="https://play.google.com/intl/en_us/badges/images/generic/en-play-badge.png" alt="Get it on Google Play" height="80">](https://play.google.com/store/apps/details?id=is.xyz.mpv)

[<img src="https://fdroid.gitlab.io/artwork/badge/get-it-on.png" alt="Get it on F-Droid" height="80">](https://f-droid.org/packages/is.xyz.mpv)

**Note**: Android TV is supported, but only available on F-Droid or by installing the APK manually.

## Building from source

Take a look at the [README](buildscripts/README.md) inside the `buildscripts` directory.

### Qualcomm QNN / QAIRT runtime setup

This demo does not ship Qualcomm QAIRT/QNN runtime libraries in Git.
You must provide them from a local Qualcomm AI Engine Direct (QAIRT) SDK install.

Expected files:

* `app/src/main/qnnLibs/arm64-v8a/libQnnHtp.so`
* `app/src/main/qnnLibs/arm64-v8a/libQnnSystem.so`
* `app/src/main/qnnLibs/arm64-v8a/libQnnHtpPrepare.so`
* `app/src/main/qnnLibs/arm64-v8a/libQnnHtpV75Stub.so`
* `app/src/main/assets/anvil/libQnnHtpV75Skel.so`

Example setup:

```sh
export QAIRT_SDK_ROOT=/opt/qcom/aistack/qairt/<version>
mkdir -p app/src/main/qnnLibs/arm64-v8a app/src/main/assets/anvil
cp "$QAIRT_SDK_ROOT/lib/aarch64-android/libQnnHtp.so" app/src/main/qnnLibs/arm64-v8a/
cp "$QAIRT_SDK_ROOT/lib/aarch64-android/libQnnSystem.so" app/src/main/qnnLibs/arm64-v8a/
cp "$QAIRT_SDK_ROOT/lib/aarch64-android/libQnnHtpPrepare.so" app/src/main/qnnLibs/arm64-v8a/
cp "$QAIRT_SDK_ROOT/lib/aarch64-android/libQnnHtpV75Stub.so" app/src/main/qnnLibs/arm64-v8a/
cp "$QAIRT_SDK_ROOT/lib/hexagon-v75/unsigned/libQnnHtpV75Skel.so" app/src/main/assets/anvil/
```

`app/src/main/assets/anvil/context.serialized.bin` is still expected separately for the model context.
The QNN/QAIRT `.so` files above are intentionally ignored by Git.

Some other documentation can be found at this [link](http://mpv-android.github.io/mpv-android/).
