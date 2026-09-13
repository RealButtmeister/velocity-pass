# Velocity Pass + Velocity Pass Inverse

Two Windows x64 VST3 audio effects that turn incoming MIDI note velocity into audio level. Use either on its own, or send the same automation MIDI to both for opposite movement across two voices.

| MIDI note velocity | Velocity Pass | Velocity Pass Inverse |
| --- | ---: | ---: |
| 1 | 0% | 100% |
| 64 | 50% | 50% |
| 127 | 100% | 0% |

For a nonzero MIDI note-on velocity `v`, the original gain is `(v - 1) / 126`; the inverse gain is `1 - (v - 1) / 126`. Gains are clamped to 0–1. The VST3 normalized velocity is converted back to the MIDI range, with near-integer values snapped to recover exact 7-bit levels.

Both effects support mono/stereo and 32-bit/64-bit audio samples. They add no latency, gain boost, or smoothing. Note pitch is ignored, any MIDI channel is accepted, and note-offs hold the last level. Velocity-zero note-ons, controllers, and pitch bend do not change the level. Manual Level edits and Reset control the actual output gain in both effects; a simultaneous MIDI note takes priority over a host Level edit. Level and bypass are saved with the host project.

Both start at 100%. Their linear gains add to 100% after receiving the same control note, so place the first control note at the start of the arrangement. Complementary gains do not guarantee equal perceived loudness for different voices: use track faders to set their starting balance.

## Using the pair in FL Studio

1. Put **Velocity Pass** on one voice's Mixer track and **Velocity Pass Inverse** on the other. Keep each effect slot's Mix level at **100%**.
2. Add **MIDI Out** and choose an unused Port, such as **20**.
3. Set both effects' wrapper **MIDI Input port** to that same number.
4. Load the automation MIDI into MIDI Out's Piano roll, align it with the audio arrangement, and use the same BPM.

Use different ports for independent curves. If a quiet track stops following control notes, disable its wrapper's Smart disable. The effects hold the last delivered value when stopped or seeking; the next delivered control note updates it. **Reset to 100%** restores passthrough.

## Example control MIDI

The [examples](examples) folder contains two 130 BPM control-note patterns. Play audio through the effects while sending these patterns; neither plugin generates sound.

- **8-bar intro ramp.mid** rises from silence to full level in Velocity Pass, and falls from full level to silence in Inverse.
- **Full-half-silent-full.mid** gives 100%, 50%, 0%, 100% in Velocity Pass, and 0%, 50%, 100%, 0% in Inverse. Its filename describes the original plugin's result.

## Build from source

The repository contains source and binary-loading tests for both effects. The native editor and test loader use Windows APIs; other operating systems and CPU architectures are not supported by this source version.

Requirements: Windows x64, Git, CMake 3.25 or newer, and a C++17 compiler. The verified toolchain is LLVM-MinGW 20260826 (x86_64 UCRT) with Ninja. Put CMake, Ninja, and the LLVM-MinGW `bin` directory on your PATH. The SDK version used for these plugins is Steinberg VST3 SDK 3.8.1, pinned to commit `3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96`.

From the repository root in PowerShell:

```powershell
git clone https://github.com/steinbergmedia/vst3sdk.git external/vst3sdk
git -C external/vst3sdk checkout 3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96
git -C external/vst3sdk submodule update --init --recursive

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DVST3_SDK_ROOT="$PWD/external/vst3sdk"
cmake --build build --config Release --target VelocityPass VelocityPassInverse velocity_pass_tests velocity_pass_inverse_tests --parallel
ctest --test-dir build -C Release --output-on-failure
```

The SDK is added once. Each plugin and test executable has its own build target. `BUILD_TESTING=OFF` can be used for a build without the tests. Build output stays in `build`; the build does not install plugins or create links in a system plugin directory.

With the shown Release configuration, the complete bundles are:

- `build/VST3/Release/VelocityPass.vst3`
- `build/VST3/Release/VelocityPassInverse.vst3`

For a manual installation, close the audio host, copy each complete `.vst3` folder into the Windows shared VST3 directory (`%CommonProgramFiles%\VST3`), then rescan plugins in the host. Preserve each bundle's `Contents` directory. Installing into that system directory may require administrator permission.

## Tests

CTest loads the freshly built plugin binaries. The original suite covers 15 cases; the inverse suite covers 19, including paired original/inverse processing across all 127 nonzero velocities in mono/stereo with both sample formats. The suites also exercise sample timing, exact endpoints, note-off holds, bypass, state restoration, and the native editor lifecycle. Actual audio-host routing and the balance of your voices should still be checked in your project.

The original and inverse retain separate processor/controller IDs and can coexist in the same host session.

## Third-party notices

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [the VST3 SDK license](Licenses/Steinberg-VST3.txt). No license has been selected for this repository's own plugin source yet; third-party licenses apply only to their respective components.
