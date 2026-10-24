# Cableloom

Experimental Windows virtual audio cables with a Rust manager and a C++ PortCls/WaveRT driver.

The source implements up to eight independent root-enumerated cable devices, each with a render/capture pair, its own queue, clock accounting, notification state and statistics. The manager provides a native Windows GUI and a CLI for driver staging, cable creation/removal, status and queue-delay selection.

**Development status:** the transfer core has host-side tests. The Rust manager and the kernel driver have not yet been built and validated together. This is not a production-ready or signed driver release. No latency, fidelity or stability advantage over existing products has been established.

## Requirements and build

Target: Windows 10 version 2004 or later, x64. Build the manager with an x64 MSVC Rust toolchain and Visual Studio 2022 C++ tools:

```powershell
cargo test
cargo build --release
```

The manager has no external crate dependencies. Install matching Windows SDK and WDK components to compile the driver:

```powershell
.\build-driver.ps1 -WdkRoot 'C:\Program Files (x86)\Windows Kits\10'
```

Use -WdkVersion if the installed include/library version differs. The script emits an **unsigned** driver and INF in build/driver. Package validation, catalog generation, signing and signature verification are required before Windows can load it. Use a dedicated driver-test machine for kernel validation. The script does not change boot settings, certificate trust or installed devices.

Run the portable transfer-core tests with CMake and a C++17 compiler:

```powershell
cmake -S . -B build/core
cmake --build build/core --config Release
ctest --test-dir build/core -C Release --output-on-failure
```

## Usage

Run target/release/cableloom.exe for the GUI. Install/add/remove actions request elevation through a short-lived helper. CLI equivalents must run in an administrator terminal:

```powershell
cableloom install C:\signed-package\cableloom.inf
cableloom add 1
cableloom add 2
cableloom list
cableloom diagnose
cableloom delay 1 96
cableloom remove 2
```

Select the cable's Send endpoint in the producing application and its Receive endpoint in the receiving application. The list --json command provides machine-readable status; diagnose also reports registered devices whose driver is unavailable. Removing a cable stops its streams; it does not remove the staged driver package.

## Constraints

- Audio is 48 kHz stereo integer PCM16, packed PCM24 or PCM32. Both sides of a cable must use the same sample width. There is no driver-side resampling, mixing or gain processing.
- One hardware stream per direction is supported. Windows shared mode handles application mixing.
- Queue delay is 48–480 frames (default 96), changes only while both streams are stopped, and resets to the default when the device restarts. Queue delay is not total application latency.
- RUN transitions flush queued audio. Underflow emits silence, overflow drops oldest queued frames, and excessive timer delays trigger bounded resynchronization with counters.
- A 1 ms high-resolution timer is requested only while streams run. Actual timing and minimum usable buffer size depend on Windows scheduling and must be measured.
- Dynamic endpoint naming, ARM64, other sample rates, float PCM and production driver certification are not implemented.

Licensed under MIT; see [LICENSE](LICENSE). Include the license with redistributed packages.
