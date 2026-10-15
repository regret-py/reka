/* =====================================================================================================
 *  RekaDriverInterface.h - user-mode <-> kernel-mode contract of the REKA Virtual Audio driver
 *
 *  Shared verbatim by REKA.exe (RekaVirtualAudio) and the WaveRT/ACX miniport driver described in
 *  docs/VIRTUAL_AUDIO_DRIVER.md. Plain C, fixed-width types, explicit sizes, no pointers in structs, so
 *  the layout is identical for the 64-bit driver and the 64-bit app.
 *
 *  Model
 *    * The driver exposes a control device \\.\RekaVirtualAudio.
 *    * IOCTL_REKA_CREATE_ENDPOINT asks the driver to instantiate a new audio endpoint:
 *        REKA_FLOW_RENDER  -> a playback device ("REKA Input 1")   : apps play into it, REKA reads.
 *        REKA_FLOW_CAPTURE -> a recording device ("REKA Output 1") : REKA writes, apps record from it.
 *    * IOCTL_REKA_MAP_RING maps the endpoint's lock-free ring (REKA_RING_HEADER + float32 interleaved
 *      frames) into the calling process and duplicates a "data ready" event handle into it.
 *    * The driver's WaveRT position timer copies between the Windows audio engine's cyclic buffer and the
 *      ring; REKA's pump thread moves data between the ring and an AsyncEndpointPort, which absorbs the
 *      clock drift between the driver timer and REKA's master device.
 * ===================================================================================================== */
#ifndef REKA_DRIVER_INTERFACE_H
#define REKA_DRIVER_INTERFACE_H

#include <stdint.h>

#define REKA_DRIVER_INTERFACE_VERSION 1u
#define REKA_DRIVER_CONTROL_PATH L"\\\\.\\RekaVirtualAudio"

/* CTL_CODE(DeviceType, Function, Method, Access) without depending on winioctl.h */
#define REKA_DRIVER_DEVICE_TYPE 0x8A11u
#define REKA_METHOD_BUFFERED 0u
#define REKA_FILE_READ_ACCESS 1u
#define REKA_FILE_WRITE_ACCESS 2u
#define REKA_CTL_CODE(function, access) \
    ((REKA_DRIVER_DEVICE_TYPE << 16) | ((access) << 14) | ((function) << 2) | REKA_METHOD_BUFFERED)

#define IOCTL_REKA_GET_INFO        REKA_CTL_CODE(0x800u, REKA_FILE_READ_ACCESS)
#define IOCTL_REKA_CREATE_ENDPOINT REKA_CTL_CODE(0x801u, REKA_FILE_READ_ACCESS | REKA_FILE_WRITE_ACCESS)
#define IOCTL_REKA_REMOVE_ENDPOINT REKA_CTL_CODE(0x802u, REKA_FILE_READ_ACCESS | REKA_FILE_WRITE_ACCESS)
#define IOCTL_REKA_MAP_RING        REKA_CTL_CODE(0x803u, REKA_FILE_READ_ACCESS | REKA_FILE_WRITE_ACCESS)
#define IOCTL_REKA_UNMAP_RING      REKA_CTL_CODE(0x804u, REKA_FILE_READ_ACCESS | REKA_FILE_WRITE_ACCESS)
#define IOCTL_REKA_LIST_ENDPOINTS  REKA_CTL_CODE(0x805u, REKA_FILE_READ_ACCESS)

#define REKA_FLOW_RENDER  0u /* Windows playback endpoint  -> REKA input strip  */
#define REKA_FLOW_CAPTURE 1u /* REKA bus -> Windows recording endpoint          */

#define REKA_MAX_ENDPOINTS 32u
#define REKA_ENDPOINT_NAME_CHARS 64u
#define REKA_RING_MAGIC 0x47524B52u /* bytes "RKRG" in memory (little endian) */

#pragma pack(push, 8)

typedef struct REKA_DRIVER_INFO {
    uint32_t Size;              /* sizeof(REKA_DRIVER_INFO) */
    uint32_t InterfaceVersion;  /* REKA_DRIVER_INTERFACE_VERSION */
    uint32_t VersionMajor;
    uint32_t VersionMinor;
    uint32_t VersionPatch;
    uint32_t MaxEndpoints;
    uint32_t ActiveEndpoints;
    uint32_t Reserved;
} REKA_DRIVER_INFO;

typedef struct REKA_ENDPOINT_CONFIG {
    uint32_t Size;
    uint32_t Flow;              /* REKA_FLOW_* */
    uint32_t Channels;          /* 1..8 */
    uint32_t SampleRate;        /* 44100, 48000, 88200, 96000, 176400, 192000 */
    uint32_t RingFrames;        /* power of two, >= 4 x the driver timer period */
    uint32_t Reserved;
    uint16_t FriendlyName[REKA_ENDPOINT_NAME_CHARS]; /* UTF-16, NUL terminated: "REKA Input 1" */
} REKA_ENDPOINT_CONFIG;

typedef struct REKA_ENDPOINT_ID {
    uint32_t Size;
    uint32_t EndpointId;        /* returned by IOCTL_REKA_CREATE_ENDPOINT */
} REKA_ENDPOINT_ID;

typedef struct REKA_RING_MAPPING {
    uint32_t Size;
    uint32_t EndpointId;
    uint64_t UserAddress;       /* base of REKA_RING_HEADER in the caller's address space */
    uint64_t MappingBytes;
    uint64_t DataReadyEvent;    /* HANDLE valid in the caller's process; signalled every driver tick */
} REKA_RING_MAPPING;

typedef struct REKA_ENDPOINT_LIST {
    uint32_t Size;
    uint32_t Count;
    REKA_ENDPOINT_CONFIG Endpoints[REKA_MAX_ENDPOINTS];
    uint32_t EndpointIds[REKA_MAX_ENDPOINTS];
} REKA_ENDPOINT_LIST;

/* Ring shared between the driver and REKA. Single producer / single consumer:
 *   RENDER  endpoints: producer = driver, consumer = REKA
 *   CAPTURE endpoints: producer = REKA,   consumer = driver
 * Positions are monotonically increasing frame counters (never wrapped); index = position % Capacity.
 * Producer: write samples, then publish WritePosition with release semantics (InterlockedExchange64).
 * Consumer: read WritePosition with acquire semantics, read samples, then publish ReadPosition. */
typedef struct REKA_RING_HEADER {
    uint32_t Magic;             /* REKA_RING_MAGIC */
    uint32_t Version;           /* REKA_DRIVER_INTERFACE_VERSION */
    uint32_t Flow;
    uint32_t Channels;
    uint32_t SampleRate;
    uint32_t CapacityFrames;    /* power of two */
    uint32_t HeaderBytes;       /* offset of the first sample from the start of this header */
    uint32_t Reserved0;
    volatile int64_t WritePosition;
    uint8_t Pad0[56];           /* keep producer and consumer indices on separate cache lines */
    volatile int64_t ReadPosition;
    uint8_t Pad1[56];
    volatile int32_t StreamRunning; /* 1 while the Windows audio engine streams this endpoint */
    volatile uint32_t DriverUnderruns;
    volatile uint32_t DriverOverruns;
    uint32_t Reserved1[13];
    /* float32 interleaved samples follow at HeaderBytes */
} REKA_RING_HEADER;

#pragma pack(pop)

#endif /* REKA_DRIVER_INTERFACE_H */
