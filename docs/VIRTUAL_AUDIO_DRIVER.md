# REKA virtual audio: endpoints, cables and per-app capture

REKA needs to exchange audio with other applications: games, browsers and Spotify into REKA; REKA's buses out to
Discord, OBS, Zoom or a DAW. Windows offers three ways to do that, and REKA supports all of them through the
`RekaVirtualAudio` bridge (`src/core/reka/virtualaudio/`) and `VirtualIoManager` (`src/app/audio/`).

| Path | Needs | Direction | Status in this repository |
|---|---|---|---|
| **1. REKA Virtual Audio driver** (`REKA Input N` / `REKA Output N`) | a signed kernel driver | both | user-mode client **implemented**. The kernel driver is **specified here, not included** (see below) |
| **2. Third-party virtual cables** (VB-CABLE, Virtual Audio Cable, Voicemeeter, BlackHole) | the cable installed | both | **implemented**: cables are detected and listed; add each end as a secondary device |
| **3. WASAPI process loopback** (capture one app) | Windows 10 build 20348+ / Windows 11 | app → REKA | **implemented**: no driver needed |

## Why the kernel driver is not in this repository

A device that appears in Windows' Sound settings, so that Discord can select "REKA Output 1" as its microphone,
**must be a kernel-mode audio driver** (PortCls WaveRT miniport or ACX). Windows has no user-mode API that creates
audio endpoints. That driver:

* is built with the Windows Driver Kit (WDK), not CMake/MSVC, and cannot be built or tested in this project's
  toolchain;
* must be **attestation-signed through Microsoft Partner Center** (EV code-signing certificate) to load on Secure
  Boot machines;
* runs in the kernel, where a bug is a blue screen rather than a crash.

Shipping a driver that has never been compiled, signed or run would be irresponsible. This repository therefore
defines the exact contract (`RekaDriverInterface.h`) and implements and compile-tests the whole user-mode side
against it. The driver itself is a separate deliverable. Until it exists, paths 2 and 3 cover the use cases.

## Driver contract (`RekaDriverInterface.h`)

* Control device: `\\.\RekaVirtualAudio`, `IOCTL_REKA_GET_INFO` → `REKA_DRIVER_INFO` (interface version check).
* `IOCTL_REKA_CREATE_ENDPOINT(REKA_ENDPOINT_CONFIG)` → endpoint id. `REKA_FLOW_RENDER` creates a **playback** device
  (apps play into it, REKA reads). `REKA_FLOW_CAPTURE` creates a **recording** device (REKA writes, apps record).
* `IOCTL_REKA_MAP_RING(REKA_ENDPOINT_ID)` → `REKA_RING_MAPPING`: the endpoint's ring buffer mapped into REKA's
  address space, plus a "data ready" event handle duplicated into REKA's process.
* Ring = `REKA_RING_HEADER` + float32 interleaved frames. Single producer, single consumer, with monotonic 64-bit
  positions published with release semantics (`InterlockedExchange64`):
  * render endpoints: the driver produces, REKA consumes;
  * capture endpoints: REKA produces, the driver consumes.
* `IOCTL_REKA_UNMAP_RING`, `IOCTL_REKA_REMOVE_ENDPOINT`, `IOCTL_REKA_LIST_ENDPOINTS`.

Conventions the client implements where the header leaves room (`win/RekaDriverClient.cpp`):
* `GET_INFO` fills `Size` and `InterfaceVersion` in both directions.
* `MAP_RING`, `UNMAP_RING` and `REMOVE_ENDPOINT` take `REKA_ENDPOINT_ID` as input.
* Each stream opens its own control handle.
* The data-ready event may be manual-reset: the client resets it after each wake.
* While `StreamRunning == 0` on a capture endpoint, the pump keeps draining REKA's side at the nominal rate, so the
  engine never backs up.

## Recommended driver implementation

1. Start from Microsoft's **SYSVAD** sample (PortCls WaveRT) or the **ACX** samples (`Windows-driver-samples/audio`).
2. Make each endpoint a WaveRT pin with a cyclic buffer. A high-resolution timer (or the position-reporting DPC)
   copies between the WaveRT buffer and the shared ring at the endpoint's nominal rate, then signals the data-ready
   event.
3. Allocate the ring from non-paged pool, describe it with an MDL and map it into the requesting process with
   `MmMapLockedPagesSpecifyCache(UserMode)` inside `IOCTL_REKA_MAP_RING`. Unmap it on `UNMAP_RING` and on handle close
   (`IRP_MJ_CLEANUP`), so a crashed REKA never leaks mappings.
4. Create and remove endpoints dynamically with KS filter factories (SYSVAD) or `AcxCircuit` creation (ACX), named
   from `REKA_ENDPOINT_CONFIG::FriendlyName`.
5. Clock drift between the driver timer and REKA's master device is **not** the driver's problem: REKA's
   `AsyncEndpointPort` resamples with a drift-locked ratio (see ARCHITECTURE.md §5).
6. Sign with attestation signing. For development, test-signing mode (`bcdedit /set testsigning on`) works.

## Third-party cables (path 2)

`RekaVirtualAudio::enumerateEndpoints()` flags endpoints whose names match known cables (`CABLE Input/Output`,
`VB-Audio`, `Virtual Audio Cable`, `Voicemeeter`, `BlackHole`). Settings → Virtual I/O lists them. To use one, add
its two ends under Settings → Audio → Secondary devices. Each end becomes a drift-corrected port like any other
device, at any sample rate.

## Per-application capture (path 3)

Settings → Virtual I/O → *Per-application capture* lists the processes that currently have an audio session.
Capturing one uses `ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK)` in
`PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE` mode, at float32 stereo 48 kHz (`win/ProcessLoopbackCapture.cpp`).

* Profiles remember the **executable name**, not the process id. REKA re-attaches when the app is restarted (it
  retries every 3 s).
* Windows always captures the whole process tree: there is no "this process only" mode, so the `includeChildren`
  flag in the API is informational.
* The application still plays to its own output device. Point it at an unused device (or a cable) if you only want
  to hear it through REKA.

## Testing status

The Windows layer (driver client, WASAPI enumeration, process loopback, serial ports, MMCSS, DWM backdrop) compiles
and links warning-free with MinGW-w64 during development and with MSVC in CI. **It has not been run on Windows
hardware in this environment.** Runtime behaviour of COM activation, process loopback and device hot-plug must be
validated on a real Windows 10/11 machine.
