# Cableloom

Experimental Windows virtual audio cables with a Rust manager and a C++ PortCls/WaveRT driver.

The source implements up to eight independent root-enumerated cable devices, each with a render/capture pair, its own queue, clock accounting, notification state and statistics. The manager provides a native Windows GUI and a CLI for driver staging, cable creation/removal, status and queue-delay selection.

**Development status:** the transfer core and Rust manager tests pass. The manager builds with Rust 1.99.1, and its CLI and native GUI have been exercised. The x64 kernel driver builds with Visual Studio 2026 and SDK/WDK 10.0.28000.0, and its INF passes the matching InfVerif Windows, Universal and signature-requirements checks. The optional VST3 worker builds against SDK 3.8.1 and has processed the official ADelay example in one- and two-plug-in chains. Driver loading, live device management and live VST audio remain unverified. This is not a production-ready release. No performance advantage over existing products has been established.

## Requirements and build

Target: Windows 10 version 2004 or later, x64. Build the manager with an x64 MSVC Rust toolchain:

```powershell
cargo test
cargo build --release
```

The manager has no external crate dependencies. Install matching Windows SDK and WDK components to compile the driver:

```powershell
.\build-driver.ps1 -WdkRoot 'C:\Program Files (x86)\Windows Kits\10'
```

Use -WdkVersion if the installed include/library version differs. The driver script selects VS2022 for kit 26100 and VS2026 for kit 28000. For separately located kits, pass -SdkRoot and -SdkVersion; SDK/WDK build numbers must match. The script emits an **unsigned** driver and INF in build/driver. Package validation, catalog generation, signing and signature verification are required before Windows can load it. Use a dedicated driver-test machine for kernel validation. The script does not change boot settings, certificate trust or installed devices.

Cableloom does not require or recommend disabling Secure Boot, enabling BCD test-signing, or disabling signature enforcement. Load the driver only after Microsoft signing. For a Windows 10/11 Desktop testing submission, generate a catalog with the matching WDK tools and prepare the Partner Center CAB after validation:

```powershell
.\build-attestation-cab.ps1 -PackageDirectory C:\build\cableloom-package -Output C:\build\cableloom-attestation
```

The input directory must contain `cableloom.inf`, `cableloom.sys`, `cableloom.pdb` and `cableloom.cat`. The resulting CAB is still unsigned: an organization with a Hardware Developer Program account must sign it using a Partner Center-registered EV certificate and submit it for Microsoft attestation signing. Production/Windows Update distribution requires the applicable HLK/WHCP path.

Run the portable transfer-core tests with CMake and a C++17 compiler:

```powershell
cmake -S . -B build/core
cmake --build build/core --config Release
ctest --test-dir build/core -C Release --output-on-failure
```

### Optional VST3 worker

The VST3 host runs in a process separate from the manager and driver and never requests elevation. Obtain the official Steinberg VST 3 SDK recursively and check out the tested commit before building:

```powershell
git clone --recursive https://github.com/steinbergmedia/vst3sdk.git
git -C vst3sdk checkout 3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96
git -C vst3sdk submodule update --init --recursive
.\build-vst-worker.ps1 -Vst3SdkRoot C:\src\vst3sdk
```

Place `cableloom-vst-worker.exe` beside `cableloom.exe`. The manager forwards `cableloom vst ...` commands to that isolated worker. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) before redistribution.

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

### Direct and VST routes

The low-latency direct route remains entirely inside one cable's driver queue: select that cable's Send and Receive endpoints and do not start the VST worker. It keeps PCM16/24/32 samples as integers and does not depend on a plug-in process.

The optional live VST route uses two different Cableloom devices:

```text
source -> Cable A Send/Receive -> VST worker -> Cable B Send/Receive -> sink
```

List endpoint IDs, scan a module, and inspect a chain offline before starting the live route:

```powershell
cableloom vst list-endpoints
cableloom vst scan C:\VST3\effect.vst3
cableloom vst benchmark --plugin C:\VST3\effect.vst3 --block 96 --blocks 10000
cableloom vst live --capture <Cable-A-capture-id> --render <Cable-B-render-id> --plugin C:\VST3\effect.vst3 --period 96
```

Repeat `--plugin` to build a serial chain. Omitting it runs a user-mode bypass route for measuring bridge overhead; this is distinct from the lower-latency in-driver direct route. The worker rejects non-Cableloom endpoints and endpoints belonging to the same cable.

Every benchmark and periodic live snapshot reports the chain and each plug-in separately: declared latency samples, average processing time, p50/p95/p99 histogram upper bounds, maximum processing time, deadline misses, restart flags, dropped/underflow frames and clipped or non-finite samples. This distinguishes plug-in look-ahead latency from host processing cost and transport buffering. The worker enables flush-to-zero handling to avoid denormal-number CPU spikes. A plug-in latency, I/O or reload notification stops the route instead of reconfiguring it on the real-time thread; restart the route to activate the new configuration.

## Constraints

- Audio is 48 kHz stereo integer PCM16, packed PCM24 or PCM32. Both sides of a cable must use the same sample width. There is no driver-side resampling, mixing or gain processing.
- One hardware stream per direction is supported. Windows shared mode handles application mixing.
- Queue delay is 48–480 frames (default 96), changes only while both streams are stopped, and resets to the default when the device restarts. Queue delay is not total application latency.
- RUN transitions flush queued audio. Underflow emits silence, overflow drops oldest queued frames, and excessive timer delays trigger bounded resynchronization with counters.
- A 1 ms high-resolution timer is requested only while streams run. Actual timing and minimum usable buffer size depend on Windows scheduling and must be measured.
- Dynamic endpoint naming, ARM64, other sample rates, float PCM and production driver certification are not implemented.
- The VST route currently supports Windows x64 VST3 effects with one stereo input/output bus, float32 processing and 48 kHz PCM24 WASAPI endpoints. PCM24 converts exactly to float32 at the host boundary, but plug-in processing is not lossless. Plug-in editors, state persistence, parameter automation, instruments, MIDI, sidechains, float64 chains and automatic crash restart are not implemented.

Licensed under MIT; see [LICENSE](LICENSE). Include the license with redistributed packages.
