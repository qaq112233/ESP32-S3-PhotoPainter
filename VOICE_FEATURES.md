# ESP32-S3-PhotoPainter Voice Wake-up and Speech Recognition

This document provides a detailed explanation of how voice wake-up and speech recognition features are implemented in the ESP32-S3-PhotoPainter project.

## Table of Contents
- [Overview](#overview)
- [Voice Wake-up (Wake Word Detection)](#voice-wake-up-wake-word-detection)
- [Speech Recognition](#speech-recognition)
- [Technical Architecture](#technical-architecture)
- [Audio Processing Pipeline](#audio-processing-pipeline)
- [Key Technical Components](#key-technical-components)
- [FAQ](#faq)

## Overview

The ESP32-S3-PhotoPainter project is based on Brother Xia's [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) open-source project, implementing comprehensive voice interaction features. The system includes two core functionalities:

1. **Voice Wake-up (Wake Word Detection)**: The device continuously listens for specific wake words (e.g., "你好，小智" or "Hi, ESP") in a low-power state. When a wake word is detected, it activates the speech recognition feature.

2. **Speech Recognition**: After being awakened, the system starts recording and processing user voice commands, converting them to text and sending them to the cloud for processing.

## Voice Wake-up (Wake Word Detection)

### How It Works

Voice wake-up is an always-on, low-power voice detection technology. The system continuously monitors the audio stream from the microphone, using a specially trained neural network model (WakeNet) to identify preset wake words.

### Implementation Methods

The project supports three wake word detection implementations:

1. **AFE Wake Word (`AfeWakeWord`)** (Recommended)
   - Uses ESP-ADF's Audio Front-End (AFE) framework
   - Integrates WakeNet neural network model
   - Supports multiple wake word detection simultaneously
   - Provides better noise suppression and echo cancellation

2. **ESP Wake Word (`EspWakeWord`)**
   - Uses ESP-SR speech recognition framework
   - Lightweight implementation with lower resource usage

3. **Custom Wake Word (`CustomWakeWord`)**
   - Supports custom wake word models
   - Can train specific wake words as needed

### Wake-up Flow

```
Microphone captures audio
    ↓
Audio digitization (I2S interface)
    ↓
AudioCodec reads raw PCM data
    ↓
Feeds into WakeWord detector
    ↓
Real-time analysis using neural network
    ↓
Wake word detected?
    ├─ Yes → Trigger callback, start speech recognition
    └─ No → Continue listening
```

### Technical Features

- **Low Latency**: Typically completes detection within 100-300ms
- **Low Power**: Optimized algorithms ensure long-term operation
- **High Accuracy**: Uses deep learning models with low false wake rate
- **Multiple Wake Words**: Can recognize multiple different wake words simultaneously

## Speech Recognition

### How It Works

When triggered by a wake word, the system enters speech recognition mode. At this point, the system:
1. Activates the complete audio processing pipeline
2. Records user voice commands
3. Performs real-time audio processing (noise reduction, echo cancellation, etc.)
4. Encodes the processed audio and sends it to the cloud
5. Receives recognition results and responses from the cloud

### Audio Processing Pipeline

Speech recognition involves complex audio signal processing, mainly including the following steps:

#### 1. Audio Capture
- Reads raw PCM data from the Audio Codec via I2S interface
- Sample rate typically 16kHz (standard for speech recognition)
- Mono or stereo, depending on hardware configuration

#### 2. Audio Front-End Processing (AFE)

**`AudioProcessor`** handles real-time audio processing with main functions:

- **Acoustic Echo Cancellation (AEC)**
  - Eliminates speaker playback interference with the microphone
  - Supports device-side AEC and server-side AEC
  - Uses reference signals for adaptive filtering

- **Noise Suppression (NS)**
  - Filters background noise (fan, air conditioning, etc.)
  - Improves signal-to-noise ratio of voice signals

- **Voice Activity Detection (VAD)**
  - Real-time detection of speech and silence segments
  - Encodes and transmits only when speech is detected
  - Saves bandwidth and computing resources

- **Automatic Gain Control (AGC)**
  - Automatically adjusts audio signal strength
  - Ensures clear recognition of speech at different distances

#### 3. Audio Encoding

Processed audio is compressed using the **Opus codec**:

- **Encoding Format**: Opus
- **Frame Duration**: 60ms (configurable)
- **Sample Rate**: 16kHz
- **Channels**: Mono
- **Advantages**:
  - High compression ratio: Significantly reduces data transmission
  - Low latency: Suitable for real-time voice communication
  - High quality: Maintains voice clarity

#### 4. Network Transmission

Encoded Opus packets are sent to cloud servers via network:
- Uses WebSocket or HTTP protocol
- Supports reconnection
- Timestamp synchronization for server-side AEC

#### 5. Cloud Recognition

Cloud servers (such as DeepSeek AI, OpenAI, etc.):
- Receive audio stream
- Use advanced speech recognition models (ASR) to convert speech to text
- Understand user intent
- Generate responses (text or voice)

#### 6. Speech Synthesis and Playback

- Receives voice data from cloud (Opus format)
- Decodes back to PCM data using Opus decoder
- Sends to Audio Codec for playback

## Technical Architecture

### System Architecture Diagram

```
┌─────────────────────────────────────────────────────────┐
│                    AudioService                         │
│  (Audio Service - Core Coordinator)                     │
└─────────────────────────────────────────────────────────┘
         │
         ├─────────────────────────────────────────────────┐
         │                                                 │
┌────────▼──────────┐                          ┌──────────▼─────────┐
│   WakeWord        │                          │   AudioProcessor   │
│  (Wake Detection) │                          │   (Audio Processor)│
│                   │                          │                    │
│ - AfeWakeWord     │                          │ - AEC (Echo Cancel)│
│ - EspWakeWord     │                          │ - NS (Noise Supp.) │
│ - CustomWakeWord  │                          │ - VAD (Activity)   │
└───────────────────┘                          │ - AGC (Gain Ctrl)  │
                                               └────────────────────┘
         │
         │
┌────────▼──────────┐
│   AudioCodec      │
│  (Audio Codec)    │
│                   │
│ - I2S Interface   │
│ - ES8311/ES8388   │
│ - Mic Input       │
│ - Speaker Output  │
└───────────────────┘
         │
         │
    ┌────▼────┐
    │ Hardware│
    │         │
    │  Mic 🎤 │
    │Speaker🔊│
    └─────────┘
```

### Threading Model

AudioService uses three independent FreeRTOS tasks for the audio pipeline:

1. **AudioInputTask (Audio Input Task)**
   - Reads raw PCM data from AudioCodec
   - Feeds data to WakeWord or AudioProcessor based on current state
   - Priority: High

2. **AudioOutputTask (Audio Output Task)**
   - Gets decoded PCM data from playback queue
   - Sends to AudioCodec for playback
   - Priority: High

3. **OpusCodecTask (Opus Codec Task)**
   - Handles both Opus encoding and decoding
   - Takes data from encode queue for encoding
   - Takes data from decode queue for decoding
   - Priority: Medium

## Audio Processing Pipeline

### Uplink Flow (Audio Input)

Complete flow from microphone capture to server transmission:

```
Microphone → I2S → AudioCodec → Raw PCM
                                    ↓
                    [AudioInputTask reads]
                                    ↓
                         Audio Processor
                         (AEC/NS/VAD)
                                    ↓
                         Processed PCM
                                    ↓
                      [Encode queue buffer]
                                    ↓
                      [OpusCodecTask]
                                    ↓
                         Opus Encoder
                                    ↓
                          Opus packets
                                    ↓
                      [Send queue buffer]
                                    ↓
                      Application layer retrieves
                                    ↓
                      Network send to server
```

### Downlink Flow (Audio Output)

Complete flow from server reception to speaker playback:

```
Server → Network → Application receives
                      ↓
               Opus packets
                      ↓
            [Decode queue buffer]
                      ↓
            [OpusCodecTask]
                      ↓
             Opus Decoder
                      ↓
                 PCM data
                      ↓
            [Playback queue buffer]
                      ↓
           [AudioOutputTask]
                      ↓
              AudioCodec
                      ↓
               I2S → Speaker
```

## Key Technical Components

### 1. AudioCodec (Audio Codec)

**Purpose**: Hardware abstraction layer, handles communication with physical audio chips

**Supported Chips**:
- ES8311 (Mono)
- ES8388 (Stereo)
- ES8374
- ES8389
- Other compatible chips

**Main Functions**:
- I2S interface configuration and data transfer
- Sample rate configuration (typically 16kHz)
- Volume control
- Microphone and speaker on/off control

### 2. WakeWord (Wake Word Detection)

**Purpose**: Real-time detection of preset wake words

**Core Technology**:
- Deep learning neural network model (WakeNet)
- Real-time audio stream analysis
- Low power design

**Working Characteristics**:
- Runs independently without affecting main system
- Triggers callback upon successful detection
- Can support multiple wake words simultaneously

### 3. AudioProcessor (Audio Processor)

**Purpose**: Real-time processing and enhancement of audio signals

**Core Technology**:
- **AEC (Echo Cancellation)**: Adaptive filtering algorithms
- **NS (Noise Suppression)**: Spectral subtraction or Wiener filtering
- **VAD (Voice Activity Detection)**: Energy detection or machine learning methods
- **AGC (Automatic Gain Control)**: Dynamic range compression

**Implementation Classes**:
- `AfeAudioProcessor`: Uses ESP-ADF's AFE framework (recommended)
- `NoAudioProcessor`: Pass-through mode, no processing

### 4. Opus Codec

**Purpose**: Audio compression and decompression

**Technical Parameters**:
- Codec Standard: Opus (RFC 6716)
- Sample Rate: 16kHz
- Frame Length: 60ms
- Bit Rate: Adaptive
- Latency: < 100ms

**Advantages**:
- High compression ratio (typically 10:1)
- Good audio quality
- Low latency
- Suitable for real-time communication

### 5. OpusResampler (Resampler)

**Purpose**: Convert audio data between different sample rates

**Application Scenarios**:
- Convert hardware sample rate to processing sample rate (e.g., 48kHz → 16kHz)
- Convert processing sample rate to playback sample rate (e.g., 16kHz → 48kHz)

## Data Queue Management

The system uses multiple queues for asynchronous data flow:

1. **audio_encode_queue_**: PCM data to be encoded
2. **audio_send_queue_**: Encoded Opus packets (to send)
3. **audio_decode_queue_**: Opus packets to decode (received)
4. **audio_playback_queue_**: Decoded PCM data (to play)
5. **timestamp_queue_**: Timestamp queue (for server-side AEC)

Design advantages:
- Decouples processing stages
- Supports concurrent processing
- Buffers data to handle network jitter
- Improves system real-time performance

## Power Management

To conserve energy, the system implements intelligent power management:

- **Auto Sleep**: Automatically disables ADC (microphone) and DAC (speaker) after 15 seconds of inactivity
- **Auto Wake**: Automatically re-enables when activity is detected
- **Periodic Check**: Checks audio activity status every second

## FAQ

### 1. How to change the wake word?

Wake words are determined by model files. You can:
- Use the project's default models (supports "你好，小智", "Hi, ESP", etc.)
- Train custom wake word models
- Configure to use different pre-trained models

### 2. Why is wake-up sometimes insensitive?

Possible reasons:
- Excessive environmental noise
- Microphone quality or position issues
- Non-standard wake word pronunciation
- Volume too low

Suggestions:
- Test in a quiet environment
- Adjust microphone gain
- Pronounce clearly and standardly
- Adjust wake-up sensitivity parameters

### 3. How to reduce false wake-ups?

- Increase wake threshold
- Use more unique wake words
- Enable stricter wake models
- Ensure no similar-sounding interference in environment

### 4. What if speech recognition is inaccurate?

Check the following:
- Is network connection stable
- Is microphone working properly
- Are AEC, NS, etc. correctly configured
- Is speech clear with appropriate volume
- Is cloud service functioning normally

### 5. How to optimize recognition latency?

- Use wired network instead of Wi-Fi
- Choose cloud services with lower latency
- Reduce Opus frame length (e.g., from 60ms to 20ms)
- Enable device-side AEC (reduces server-side processing)
- Optimize network transmission protocol

### 6. Which development boards are supported?

The project supports over 70 ESP32-S3 series development boards, including but not limited to:
- ESP32-S3-BOX
- M5Stack series
- LilyGO series
- Waveshare series
- Various custom development boards

For a detailed list, see the `01_Example/xiaozhi-esp32/main/boards/` directory.

### 7. How to disable voice wake-up?

In the configuration menu:
```
Xiaozhi Assistant -> [ ] Enable Wake Word Detection and Audio Processing -> Unselect
```

Or in code:
```cpp
audio_service.EnableWakeWordDetection(false);
```

### 8. How to enable audio debugging?

Using the `AudioDebugger` component, you can:
- Record raw audio
- Save processed audio
- Analyze audio quality
- Debug AEC effects

## Technical References

### Related Projects
- [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) - Brother Xia's Xiaozhi AI Assistant
- [ESP-ADF](https://github.com/espressif/esp-adf) - ESP Audio Development Framework
- [ESP-SR](https://github.com/espressif/esp-sr) - ESP Speech Recognition Framework

### Technical Documentation
- ESP-ADF Programming Guide
- ESP32-S3 Technical Reference Manual
- Opus Codec Specification (RFC 6716)
- I2S Audio Interface Specification

### Product Links
- [Chinese Wiki](https://www.waveshare.net/wiki/ESP32-S3-PhotoPainter)
- [English Wiki](https://www.waveshare.com/wiki/ESP32-S3-PhotoPainter)

## Summary

The voice features of ESP32-S3-PhotoPainter form a complex and sophisticated system involving multiple technical domains:

- **Hardware Layer**: Microphones, speakers, audio codec chips
- **Driver Layer**: I2S interface, Audio Codec HAL
- **Algorithm Layer**: Wake word detection, AEC, NS, VAD
- **Encoding Layer**: Opus codec, resampling
- **Application Layer**: Audio service, queue management, network communication

Through modular design and multi-task concurrency, the system achieves a low-latency, high-quality real-time voice interaction experience.

---

**Last Updated**: 2025-12-10
**Project Version**: Based on latest xiaozhi-esp32
