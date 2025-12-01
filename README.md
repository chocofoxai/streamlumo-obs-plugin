# StreamLumo OBS Plugin

GPL-2.0 licensed OBS plugin that captures video frames at 60 FPS and writes them to shared memory.

## Overview

This plugin runs inside OBS Studio and provides high-performance video frame streaming to the StreamLumo Electron application via shared memory.

## Features

- ✅ **60 FPS Capture**: Uses `obs_add_raw_video_callback` for direct frame access
- ✅ **Format Conversion**: Converts OBS video formats (NV12/I420) to RGBA
- ✅ **Shared Memory**: Zero-copy IPC with triple buffering
- ✅ **GPL-Compliant**: Maintains separation from proprietary StreamLumo code
- ✅ **Cross-Platform**: POSIX (macOS/Linux) and Win32 (Windows)

## Build Instructions

### macOS/Linux

```bash
mkdir build && cd build
cmake ..
make
sudo make install
```

### Windows

```bash
mkdir build && cd build
cmake -G "Visual Studio 16 2019" -A x64 ..
cmake --build . --config Release
cmake --install .
```

## Installation

The plugin will be installed to:

- **macOS**: `~/Library/Application Support/obs-studio/plugins/streamlumo-plugin/`
- **Linux**: `~/.config/obs-studio/plugins/streamlumo-plugin/`
- **Windows**: `C:/Program Files/obs-studio/obs-plugins/64bit/`

## Usage

1. Build and install the plugin
2. Start OBS Studio
3. The plugin will automatically start capturing frames
4. Start the StreamLumo Electron app to consume frames

## Architecture

```
┌──────────────────────────────────────────────┐
│         OBS Studio (GPL-2.0)                 │
│                                              │
│  ┌────────────────────────────────────────┐ │
│  │  Video Pipeline                        │ │
│  │  (Scenes, Sources, Filters)            │ │
│  └──────────────────┬─────────────────────┘ │
│                     │                        │
│  ┌──────────────────▼─────────────────────┐ │
│  │  obs_add_raw_video_callback            │ │
│  │  (60 FPS, NV12/I420/RGBA)              │ │
│  └──────────────────┬─────────────────────┘ │
│                     │                        │
│  ┌──────────────────▼─────────────────────┐ │
│  │  Frame Writer                          │ │
│  │  - YUV → RGBA Conversion               │ │
│  │  - Write to Shared Memory              │ │
│  └──────────────────┬─────────────────────┘ │
└────────────────────┬────────────────────────┘
                     │
         ┌───────────▼──────────┐
         │  Shared Memory       │
         │  /dev/shm            │
         │  Triple Buffered     │
         │  23.7 MB             │
         └───────────┬──────────┘
                     │
┌────────────────────▼────────────────────────┐
│   StreamLumo Electron App (Proprietary)     │
│   - Reads frames from shared memory         │
│   - Renders with WebGL                      │
└─────────────────────────────────────────────┘
```

## GPL Compliance

This plugin is GPL-2.0 licensed because it links with libobs (GPL-2.0). The communication with the proprietary StreamLumo application happens via shared memory, which acts as an IPC boundary:

- **GPL Code**: This OBS plugin
- **IPC Boundary**: POSIX/Win32 shared memory
- **Proprietary Code**: StreamLumo Electron application

This architecture is legally sound and follows the same pattern used by commercial products like Streamlabs Desktop.

## Performance

- **Frame Rate**: 60 FPS capture
- **Resolution**: 1920x1080 (configurable)
- **Format**: RGBA (4 bytes per pixel)
- **Throughput**: ~474 MB/s
- **Latency**: <20ms capture + conversion
- **CPU Usage**: <5% (optimized YUV→RGBA conversion)

## Troubleshooting

### Plugin not loading

```bash
# Check OBS logs
tail -f ~/Library/Application\ Support/obs-studio/logs/*.txt
```

### Shared memory not created

```bash
# List shared memory objects (macOS/Linux)
ls -la /dev/shm/

# Should see: streamlumo_frames
```

### Check plugin is active

In OBS logs, look for:
```
[StreamLumo] Plugin loaded successfully - 60 FPS capture active
[StreamLumo] Target resolution: 1920x1080 RGBA
```

## Development

### Debug Build

```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
make
```

### Enable Verbose Logging

Edit `plugin_main.cpp` and change log level:
```cpp
blog(LOG_DEBUG, "[StreamLumo] Verbose logging enabled");
```

## License

GPL-2.0 - See LICENSE file

This plugin is free software. You can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 2 of the License, or (at your option) any later version.
