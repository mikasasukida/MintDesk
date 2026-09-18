# MintDesk

MintDesk is an experimental remote desktop/control project for personal campus-network use.

## Current Features

- Windows host captures desktop frames.
- GPU BGRA to NV12 conversion.
- NVIDIA NVENC H.264 stream.
- Android Kotlin client with MediaCodec decoding.
- TCP video stream on port `9000`.
- TCP input control on port `9001`.
- TCP clipboard/file sync on port `9002`.
- Mouse, keyboard, wheel, right click, function keys, and game-mode relative mouse.
- Local saved device card on Android.
- Host-side configurable stream resolution and FPS.
- Bidirectional clipboard text, screenshot, and file transfer.
- Android `F10` file panel for received files and sending files back to Windows.
- Drag files onto the Windows Host window to queue them for Android transfer.
- No application-level 100 MB file limit; practical limits still depend on available memory and storage.

## Project Layout

```text
MintDeskHost/
  Windows C++ host and desktop launcher

MintDeskClient/
  Android Kotlin client

release/
  Development build package
```

## Windows Host

Build environment used during development:

- Windows
- MinGW from CLion
- CMake
- NVIDIA Video Codec SDK
- NVIDIA driver with NVENC support

Runtime package is included under `release/`.

Start `MintDeskHostApp.exe` for the desktop interface. It shows the current
IPv4 address, the video/input/file ports, Host status, the received-file
folder, and the Host configuration file. The launcher starts the capture
engine in the background.

Host config file:

```ini
resolution=1920x1200
fps=60
```

Files sent from Android are saved on Windows under:

```text
D:\MintDesk\Received
```

To send a file from Windows to Android, drag it onto the MintDeskHost window,
then open the Android `F10` file panel and refresh the list. Small files can
also continue to use the normal clipboard copy workflow.

Supported resolution presets:

- `native`
- `2560x1600`
- `1920x1200`
- `1680x1050`
- `1280x800`

## Android Client

Build environment used during development:

- Android Studio
- Kotlin
- Android SDK 37
- NDK side by side
- CMake

The current client targets `arm64-v8a`.

## Status

This is a development build, not a polished production release.
