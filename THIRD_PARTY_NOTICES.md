# Third-party notices

## Steinberg VST3 SDK

These plugins build against the [Steinberg VST3 SDK](https://github.com/steinbergmedia/vst3sdk), version 3.8.1 at commit `3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96`. The SDK is fetched separately and is not vendored in this repository.

The SDK's MIT license is reproduced in [Licenses/Steinberg-VST3.txt](Licenses/Steinberg-VST3.txt). Preserve applicable notices when distributing builds that include SDK code. VST is a trademark of Steinberg Media Technologies GmbH.

## Build toolchain

The verified Windows toolchain is [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw), release 20260826, using its x86_64 UCRT distribution. Compiler and runtime components have their own licenses. This repository does not distribute a compiler, runtime, plugin binary, or installer. Consult the toolchain's bundled notices when distributing compiled binaries, particularly when using its static runtime.

## Repository source

These third-party notices do not grant a license to the Velocity Pass or Velocity Pass Inverse source. No license has been selected for that source yet.
