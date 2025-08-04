# JAY-D Firmware

Core firmware for the JAY-D portable DJ mixing device by CircuitMess.

## Overview

This firmware provides complete DJ functionality including:
- Dual-channel audio mixing with crossfader
- SD card-based music playback with hot-swap support
- Real-time audio effects and filtering
- Track browsing with enhanced search and sorting
- Position-accurate playback restoration
- Smart audio continuity during media changes
- This firmware bypasses the first loading screen and intro graphics and goes straight into DJ mode.

## Features

### Audio Effects
Each player has three effect slots:
- **Effect 1**: Speed (default)
- **Effect 2**: High pass filter (default)
- **Effect 3**: User-selectable

### Track Management
- **Smart Indexing**: AAC files are scanned once and stored as a persistent list on device
- **Browse While Playing**: Continue playback while browsing tracks without interruption
- **Circular Navigation**: Song list scrolls seamlessly from beginning to end and vice versa
- **Manual Updates**: Refresh track list via last entry in song list menu

## Recent Improvements

- **Optimized Loading**: Eliminated repeated SD card scanning for faster track selection
- **Persistent Library**: Track list maintained in device memory for instant access
- **Enhanced Navigation**: Circular scrolling for seamless song list browsing
- **Hot-swap System**: Stable SD card hot-swapping with automatic re-indexing
- **Audio Continuity**: Better playback restoration and position preservation
- **Performance**: Reduced audio dropouts and improved responsiveness

![](https://circuitmess.com/wp-content/uploads/2021/05/jayd-nobg-resized-min.png)

# Compiling

The firmware is based on the [Jay-D Library](https://github.com/CircuitMess/JayD-Library). It is, along other required libraries, automatically installed when you install the CircuitMess ESP32 Arduino platform. More info on [CircuitMess/Arduino-Packages](https://github.com/CircuitMess/Arduino-Packages).

## Using Arduino IDE

Simply open JayD-Firmware.ino using Arduino IDE, set the board to Jay-D, and compile.

## Using CMake

To compile and upload you need to have [CMake](https://cmake.org/) and [arduino-cli](https://github.com/arduino/arduino-cli)  installed. You also need to have both of them registered in the PATH.

In the CMakeLists.txt file change the port to your desired COM port (default is /dev/ttyUSB0):
```
set(PORT /dev/ttyUSB0)
```
Then in the root directory of the repository type:
```
mkdir cmake
cd cmake
cmake ..
cmake --build . --target CMBuild
```
This will compile the binaries, and place the .bin and .elf files in the build/ directory located in the root of the repository.

To compile the binary, and upload it according to the port set in CMakeLists.txt, run

```cmake --build . --target CMBuild```

in the cmake directory.

# Meta


**CircuitMess**  - https://circuitmess.com/

**Facebook** - https://www.facebook.com/thecircuitmess/

**Instagram** - https://www.instagram.com/thecircuitmess/

**Twitter** - https://twitter.com/circuitmess

**YouTube** - https://www.youtube.com/channel/UCVUvt1CeoZpCSnwg3oBMsOQ

----
Copyright © 2021 CircuitMess

Licensed under [MIT License](https://opensource.org/licenses/MIT).
