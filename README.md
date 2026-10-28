# gestureConductor (PixelSense)

Zero-latency pure C optical gesture conductor for real-time sound synthesis, SuperCollider orchestration, and dynamic musical interaction.

Features camera-based optical flow and connected-component centroid tracking with adaptive 1-Euro speed filtering, ictus baton strike detection, bilateral tutti bursts, and real-time CRT phosphor visualization.

## Architecture

- **Core Vision Engine (`gesture_conductor.c`)**:
  - Direct Linux V4L2 mmap DMA video capture (`/dev/video0`)
  - Real-time frame differencing & connected-component spatial labeling
  - Adaptive 1-Euro speed filter for jitter-free hand centroid coordinates
  - Vertical acceleration inflection detector for baton downbeats (*ictus*)
  - High-velocity bilateral motion analysis for *tutti* strikes
  - Hardware-accelerated Raylib 60 FPS CRT phosphor dashboard

- **In-Process Audio Synthesis (`sc_engine.cpp`)**:
  - Embedded `libscsynth.so` running inside the process
  - Direct in-memory zero-latency OSC packet dispatch (`World_SendPacket`)
  - Modular external SynthDefs loaded from `./synthdefs/*.scsyndef`
  - Lead oscillator with dynamic Moog 4-pole low-pass ladder filter + resonance
  - Polyphonic ambient wash with detuned sines and spatial stereo spread
  - Resonant chime on baton downbeats and heavy sub-bass tutti thump
  - Automatic PipeWire / JACK hardware port auto-connection

- **Android Mobile Conductor (`android/`)**:
  - Standalone APK with Camera2 front-facing camera tracking
  - Embedded real-time audio synthesizer for on-device speaker output
  - Wireless OSC transmitter over WiFi/UDP to conduct studio SuperCollider machines

## Quick Start (Desktop Linux)

### Build & Run
```bash
make clean && make
./gesture_conductor            # Full Raylib CRT dashboard
./gesture_conductor --headless # Headless daemon
```

### Recompile SynthDefs
```bash
sclang synthdefs.scd
```

## Android Mobile Conductor (APK)

The `android/` directory contains the complete native Android port:
- **Vision**: Uses Android Camera2 API (`ImageReader`) with the front-facing (selfie) camera, streaming frames into the native C connected-component tracker at 60 FPS.
- **Embedded Audio**: Includes a zero-latency real-time AAudio stereo synthesizer (`conductor_audio.c`) running 48 kHz floating-point DSP directly on the phone (Moog ladder lead tone, polyphonic ambient wash, metallic FM ictus chime, sub-bass tutti thump).
- **Wireless OSC**: Continuously broadcasts `/gesture/hand/left`, `/gesture/hand/right`, `/gesture/ictus`, and `/gesture/tutti` over UDP to any target host on the network (configurable live in the HUD, default `10.0.0.18:57120`).
- **Retro CRT Phosphor Radar**: Custom vector `OverlayView` displaying real-time tracking crosshairs, elevation meters, and expanding ripple animations for baton downbeats.

### Build & Install APK
```bash
make apk            # Builds gestureConductor.apk via Gradle + NDK
make install-apk    # Installs directly to connected phone via adb
```
