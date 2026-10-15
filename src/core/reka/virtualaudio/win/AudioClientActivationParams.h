#pragma once

// AUDIOCLIENT_ACTIVATION_PARAMS & co. for WASAPI process loopback (Windows 10 build 20348+).
//
// The Windows SDK (10.0.20348 and later, i.e. every SDK MSVC builds REKA with) ships them in
// audioclientactivationparams.h and is always used when present. The fallback below mirrors that header
// exactly, for toolchains whose headers predate it (MinGW-w64 11, older SDKs). Internal; Windows only.

#if defined(_WIN32)

#include <windows.h>

#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#else

#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
// Identifier of the virtual audio device that captures by process id instead of by endpoint.
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

typedef enum PROCESS_LOOPBACK_MODE {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;

typedef struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;

typedef enum AUDIOCLIENT_ACTIVATION_TYPE {
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
} AUDIOCLIENT_ACTIVATION_TYPE;

typedef struct AUDIOCLIENT_ACTIVATION_PARAMS {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union {
        AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    };
} AUDIOCLIENT_ACTIVATION_PARAMS;

#endif  // __has_include(<audioclientactivationparams.h>)

#endif  // _WIN32
