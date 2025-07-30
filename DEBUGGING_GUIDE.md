# JAY-D Firmware Debugging Guide

## Overview
This guide documents the systematic debugging approach for the JAY-D firmware, specifically targeting power management issues and brownout resets that occur during track loading and hot-swapping operations.

## Current Debug Features Added

### 1. Boot Sequence Logging
- **Location**: `JayD-Firmware_clean.ino`
- **Features**:
  - Reset cause detection (POWERON, BROWNOUT, PANIC, etc.)
  - System specs logging (chip info, memory, frequency)
  - Startup phase tracking
  - Loop performance monitoring

### 2. Memory Tracking
- **Locations**: Throughout MixScreen.cpp, IntroScreen.cpp, SongList.cpp
- **Features**:
  - Heap and PSRAM monitoring at critical points
  - Memory usage before/after major operations
  - Allocation failure detection

### 3. MixScreen Debugging
- **Location**: `src/Screens/MixScreen/MixScreen.cpp`
- **Features**:
  - Constructor/destructor logging
  - File loading validation
  - Hot-swap operation tracking
  - System state monitoring
  - Power management delays

### 4. Track Loading Flow
- **Locations**: SongList.cpp, MixScreen.cpp
- **Features**:
  - File selection tracking
  - Hot-swap state management
  - Audio preservation logging
  - System recreation monitoring

## Testing Protocol

### Phase 1: Basic Boot and Single Track Loading
1. **Monitor Serial Output** for boot sequence
2. **Load first track** (should assign to f1/left deck)
3. **Verify playback** works without issues
4. **Check memory usage** remains stable

Expected Serial Output:
```
=== JAY-D FIRMWARE STARTUP ===
Reset reason: [POWERON/BROWNOUT/etc.]
=== INTROSCREEN CONSTRUCTOR ===
=== INTRO COMPLETE - LAUNCHING MIXSCREEN ===
=== MIXSCREEN CONSTRUCTOR START ===
```

### Phase 2: Hot-Swap Testing (Critical)
1. **Load first track and start playback**
2. **Hold encoder button 6** to trigger hot-swap mode
3. **Select second track** from SongList
4. **Monitor for crashes** during MixSystem recreation

Critical Debug Points:
- Memory levels before/after system deletion
- Power management delays
- File validation
- Audio state preservation

### Phase 3: Full Dual-Deck Operation
1. **Load tracks on both decks**
2. **Test crossfading between tracks**
3. **Test effects and speed controls**
4. **Monitor long-term stability**

## Common Crash Points and Mitigations

### 1. Power Supply Brownouts
**Symptoms**: ESP_RST_BROWNOUT, POWERON_RESET
**Mitigations Added**:
- Power management delays during MixSystem operations
- Audio pausing during file operations
- Staggered power-hungry operations

### 2. Memory Allocation Failures
**Symptoms**: malloc/ps_malloc failures, heap exhaustion
**Mitigations Added**:
- Memory monitoring at allocation points
- Fallback strategies for buffer allocation
- Proper cleanup during hot-swaps

### 3. File System Issues
**Symptoms**: SD card access failures, file corruption
**Mitigations Added**:
- File validation before processing
- Proper file handle management
- SD card insertion detection

## Debug Output Analysis

### Normal Boot Sequence
```
JAY-D FIRMWARE STARTUP
ESP32 Chip: ESP32 Rev X
Reset reason: POWERON (cold boot)
Free heap: ~300000 bytes
Free PSRAM: ~4000000 bytes
=== INITIALIZING JAYD ===
=== LAUNCHING MAIN APPLICATION ===
=== INTROSCREEN CONSTRUCTOR ===
=== INTRO COMPLETE - LAUNCHING MIXSCREEN ===
=== MIXSCREEN CONSTRUCTOR START ===
MixScreen instance set: 0x3fxxxxxx
```

### Problematic Patterns
- Reset reason: BROWNOUT → Power supply issue
- Memory allocation failures → Heap/PSRAM exhaustion
- Hot-swap hangs → System recreation deadlock
- Repeated resets → Watchdog or panic loop

## Recovery Strategies

### Git Restore Point
```bash
# Current state committed as:
git reset --hard dfc21ba  # "Initial commit: JAY-D Firmware with power management fixes"
```

### If Crashes Persist
1. **Check power supply** - ensure adequate current capability
2. **Monitor memory usage** - look for leaks or fragmentation
3. **Test with minimal configuration** - disable effects, single track
4. **Add more delays** in power-critical sections

## Next Steps
1. Flash firmware with new debugging
2. Test complete flow: load track 1 → play → load track 2
3. Document any new crash patterns
4. Refine power management based on findings

## Hardware Considerations
- ESP32 power supply stability
- SD card power consumption during access
- Audio amplifier power draw
- LED matrix power requirements
- Total system power budget