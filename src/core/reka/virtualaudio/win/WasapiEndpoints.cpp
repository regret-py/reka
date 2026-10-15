#include "reka/virtualaudio/win/WasapiEndpoints.h"

#if defined(_WIN32)

#include "reka/virtualaudio/win/ComUtil.h"

#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <propsys.h>

#include <algorithm>
#include <cstring>
#include <initializer_list>

namespace reka::win {

namespace {

// Spelled out instead of instantiating the DEFINE_PROPERTYKEY declarations (INITGUID games).
// PKEY_Device_FriendlyName {A45C254E-DF1C-4EFD-8020-67D146A850E0}, 14
const PROPERTYKEY kPKeyDeviceFriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};
// PKEY_AudioEngine_DeviceFormat {F19F064D-082C-4E27-BC73-6882A1BB8E4C}, 0
const PROPERTYKEY kPKeyAudioEngineDeviceFormat = {{0xF19F064D, 0x082C, 0x4E27, {0xBC, 0x73, 0x68, 0x82, 0xA1, 0xBB, 0x8E, 0x4C}}, 0};

HRESULT createDeviceEnumerator(ComPtr<IMMDeviceEnumerator>& enumerator) {
    return CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                            enumerator.putVoid());
}

std::wstring deviceId(IMMDevice* device) {
    CoTaskMemPtr<wchar_t> id;
    if (FAILED(device->GetId(id.put())) || id.get() == nullptr) return {};
    return id.get();
}

std::wstring defaultEndpointId(IMMDeviceEnumerator* enumerator, EDataFlow flow, ERole role) {
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, role, device.put())) || !device) return {};  // none
    return deviceId(device.get());
}

std::string stringProperty(IPropertyStore* store, const PROPERTYKEY& key) {
    ScopedPropVariant value;
    if (FAILED(store->GetValue(key, value.get()))) return {};
    if (value.value().vt != VT_LPWSTR || value.value().pwszVal == nullptr) return {};
    return toUtf8(value.value().pwszVal);
}

bool formatFromWaveFormat(const WAVEFORMATEX* format, std::size_t bytes, int& channels, int& sampleRate) {
    if (format == nullptr || bytes < sizeof(WAVEFORMATEX)) return false;
    WAVEFORMATEX copy{};
    std::memcpy(&copy, format, sizeof(copy));  // the blob carries no alignment guarantee
    if (copy.nChannels == 0 || copy.nSamplesPerSec == 0) return false;
    channels = copy.nChannels;
    sampleRate = static_cast<int>(copy.nSamplesPerSec);
    return true;
}

// The engine's shared-mode format, read from the property store: cheap and works while another
// application holds the device in exclusive mode.
bool deviceFormatProperty(IPropertyStore* store, int& channels, int& sampleRate) {
    ScopedPropVariant value;
    if (FAILED(store->GetValue(kPKeyAudioEngineDeviceFormat, value.get()))) return false;
    if (value.value().vt != VT_BLOB || value.value().blob.pBlobData == nullptr) return false;
    return formatFromWaveFormat(reinterpret_cast<const WAVEFORMATEX*>(value.value().blob.pBlobData),
                                value.value().blob.cbSize, channels, sampleRate);
}

bool mixFormat(IMMDevice* device, int& channels, int& sampleRate) {
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid()))) return false;
    CoTaskMemPtr<WAVEFORMATEX> format;
    if (FAILED(client->GetMixFormat(format.put())) || format.get() == nullptr) return false;
    return formatFromWaveFormat(format.get(), sizeof(WAVEFORMATEX), channels, sampleRate);
}

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// Windows names an endpoint "<endpoint name> (<adapter name>)": "REKA Input 1 (REKA Virtual Audio)".
bool isRekaEndpointName(const std::string& friendlyName, const std::vector<std::string>& rekaNames) {
    if (startsWith(friendlyName, "REKA ")) return true;
    for (const std::string& name : rekaNames) {
        if (name.empty() || !startsWith(friendlyName, name)) continue;
        if (friendlyName.size() == name.size() || friendlyName[name.size()] == ' ') return true;
    }
    return false;
}

std::string lowerAscii(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

std::string sessionDisplayName(IAudioSessionControl2* control) {
    CoTaskMemPtr<wchar_t> name;
    if (FAILED(control->GetDisplayName(name.put())) || name.get() == nullptr) return {};
    // "@%SystemRoot%\System32\AudioSrv.Dll,-202" style resource references are not display text.
    if (name.get()[0] == L'@') return {};
    return toUtf8(name.get());
}

}  // namespace

std::vector<SystemEndpoint> enumerateSystemEndpoints(const std::vector<std::string>& rekaEndpointNames, std::string* error) {
    std::vector<SystemEndpoint> endpoints;
    auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return endpoints;
    };

    ScopedCoInit com;
    if (!com.ok()) return fail("COM initialisation failed: " + hresultMessage(com.result()));

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = createDeviceEnumerator(enumerator);
    if (FAILED(hr)) return fail("Could not open the Windows audio device list: " + hresultMessage(hr));

    std::string partialError;
    for (const EDataFlow dataFlow : {eRender, eCapture}) {
        const std::wstring defaultConsole = defaultEndpointId(enumerator.get(), dataFlow, eConsole);
        const std::wstring defaultCommunications = defaultEndpointId(enumerator.get(), dataFlow, eCommunications);

        ComPtr<IMMDeviceCollection> collection;
        hr = enumerator->EnumAudioEndpoints(dataFlow, DEVICE_STATE_ACTIVE, collection.put());
        if (FAILED(hr)) {
            partialError = std::string("Could not list the ") + (dataFlow == eRender ? "playback" : "recording") +
                           " devices: " + hresultMessage(hr);
            continue;
        }
        UINT count = 0;
        if (FAILED(collection->GetCount(&count))) continue;

        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(i, device.put())) || !device) continue;

            SystemEndpoint endpoint;
            const std::wstring id = deviceId(device.get());
            endpoint.id = toUtf8(id);
            endpoint.flow = dataFlow == eRender ? EndpointFlow::Render : EndpointFlow::Capture;
            endpoint.isDefault = !id.empty() && id == defaultConsole;
            endpoint.isDefaultCommunications = !id.empty() && id == defaultCommunications;

            ComPtr<IPropertyStore> properties;
            bool haveFormat = false;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.put())) && properties) {
                endpoint.name = stringProperty(properties.get(), kPKeyDeviceFriendlyName);
                haveFormat = deviceFormatProperty(properties.get(), endpoint.channels, endpoint.sampleRate);
            }
            if (!haveFormat) mixFormat(device.get(), endpoint.channels, endpoint.sampleRate);
            if (endpoint.name.empty()) endpoint.name = endpoint.id;

            endpoint.isRekaVirtual = isRekaEndpointName(endpoint.name, rekaEndpointNames);
            endpoint.isThirdPartyCable = !endpoint.isRekaVirtual && RekaVirtualAudio::looksLikeThirdPartyCable(endpoint.name);
            endpoints.push_back(std::move(endpoint));
        }
    }
    if (error != nullptr) *error = partialError;
    return endpoints;
}

std::vector<AudioProcess> listAudioSessionProcesses() {
    std::vector<AudioProcess> processes;
    ScopedCoInit com;
    if (!com.ok()) return processes;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(createDeviceEnumerator(enumerator))) return processes;
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.put())) || !collection)
        return processes;
    UINT deviceCount = 0;
    if (FAILED(collection->GetCount(&deviceCount))) return processes;

    const DWORD self = GetCurrentProcessId();
    for (UINT d = 0; d < deviceCount; ++d) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(d, device.put())) || !device) continue;
        ComPtr<IAudioSessionManager2> manager;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.putVoid())) || !manager)
            continue;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(manager->GetSessionEnumerator(sessions.put())) || !sessions) continue;
        int sessionCount = 0;
        if (FAILED(sessions->GetCount(&sessionCount))) continue;

        for (int s = 0; s < sessionCount; ++s) {
            ComPtr<IAudioSessionControl> control;
            if (FAILED(sessions->GetSession(s, control.put())) || !control) continue;
            ComPtr<IAudioSessionControl2> control2;
            if (FAILED(control.as(control2)) || !control2) continue;
            if (control2->IsSystemSoundsSession() == S_OK) continue;
            AudioSessionState state = AudioSessionStateInactive;
            if (SUCCEEDED(control2->GetState(&state)) && state == AudioSessionStateExpired) continue;
            DWORD pid = 0;
            // AUDCLNT_S_NO_SINGLE_PROCESS (a success code) still reports the session's creator.
            if (FAILED(control2->GetProcessId(&pid)) || pid == 0 || pid == self) continue;

            const std::string displayName = sessionDisplayName(control2.get());
            auto existing = std::find_if(processes.begin(), processes.end(),
                                         [pid](const AudioProcess& p) { return p.pid == pid; });
            if (existing != processes.end()) {
                if (existing->displayName.empty()) existing->displayName = displayName;
                continue;
            }
            AudioProcess process;
            process.pid = pid;
            process.executable = processExecutableName(pid);
            if (process.executable.empty()) process.executable = "Process " + std::to_string(pid);
            process.displayName = displayName;
            processes.push_back(std::move(process));
        }
    }

    std::sort(processes.begin(), processes.end(), [](const AudioProcess& a, const AudioProcess& b) {
        const std::string la = lowerAscii(a.executable);
        const std::string lb = lowerAscii(b.executable);
        return la != lb ? la < lb : a.pid < b.pid;
    });
    return processes;
}

std::string processExecutableName(std::uint32_t pid) {
    if (pid == 0) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return {};
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD size = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
            path.resize(size);
            break;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || path.size() >= 32768) {
            path.clear();
            break;
        }
        path.resize(path.size() * 2);
    }
    CloseHandle(process);
    const std::size_t slash = path.find_last_of(L"\\/");
    return toUtf8(slash == std::wstring::npos ? path : path.substr(slash + 1));
}

}  // namespace reka::win

#endif  // _WIN32
