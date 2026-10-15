#include "audio/VirtualIoManager.h"

namespace reka::app {

VirtualIoManager::VirtualIoManager(RekaVirtualAudio& backend, MixerModel& mixer, RekaDeviceManager& devices)
    : backend_(backend), mixer_(mixer), devices_(devices) {
    devices_.addListener(this);
    startTimer(3000);
}

VirtualIoManager::~VirtualIoManager() {
    stopTimer();
    devices_.removeListener(this);
    for (auto& item : items_) stop(item);
}

juce::String VirtualIoManager::start(Item& item) {
    std::string error;
    if (item.entry.kind == Kind::Endpoint) {
        item.stream = backend_.createVirtualEndpoint(item.entry.spec, devices_.sampleRate(), devices_.blockSize(), error);
    } else {
        // Applications are remembered by executable name; attach to whichever instance is playing audio.
        for (const auto& process : backend_.listAudioProcesses()) {
            if (juce::String(process.executable).equalsIgnoreCase(item.entry.executable)) {
                item.stream = backend_.captureProcess(process.pid, item.entry.includeChildren, devices_.sampleRate(),
                                                      devices_.blockSize(), error);
                break;
            }
        }
        if (item.stream == nullptr && error.empty()) error = "waiting for " + item.entry.executable.toStdString() + " to play audio";
    }

    if (item.stream == nullptr) {
        item.entry.running = false;
        item.entry.status = juce::String(error);
        return item.entry.status;
    }
    PortInfo info = item.stream->portInfo();
    info.key = item.entry.key;  // stable across sessions (process ids are not)
    if (item.entry.kind == Kind::Application) info.displayName = item.entry.executable.toStdString();
    mixer_.registerPort(info, item.stream->port());
    item.entry.running = true;
    item.entry.status = "running";
    return {};
}

void VirtualIoManager::stop(Item& item) {
    if (item.stream != nullptr) item.stream->stop();
    item.stream.reset();
    item.entry.running = false;
}

juce::String VirtualIoManager::addVirtualEndpoint(const VirtualEndpointSpec& spec) {
    const PortKey key = endpointKey(spec.name);
    for (const auto& item : items_)
        if (item.entry.key == key) return "an endpoint with this name already exists";
    Item item;
    item.entry.kind = Kind::Endpoint;
    item.entry.spec = spec;
    item.entry.key = key;
    const juce::String error = start(item);
    items_.push_back(std::move(item));
    return error;
}

juce::String VirtualIoManager::addApplicationCapture(const juce::String& executable, bool includeChildren) {
    const PortKey key = applicationKey(executable);
    for (const auto& item : items_)
        if (item.entry.key == key) return "this application is already captured";
    Item item;
    item.entry.kind = Kind::Application;
    item.entry.executable = executable;
    item.entry.includeChildren = includeChildren;
    item.entry.key = key;
    const juce::String error = start(item);
    items_.push_back(std::move(item));
    return error;
}

void VirtualIoManager::remove(const PortKey& key) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->entry.key != key) continue;
        stop(*it);
        mixer_.unregisterPort(key);
        items_.erase(it);
        return;
    }
}

std::vector<VirtualIoManager::Entry> VirtualIoManager::entries() const {
    std::vector<Entry> out;
    for (const auto& item : items_) {
        Entry e = item.entry;
        if (item.stream != nullptr && !item.stream->isRunning()) {
            e.running = false;
            e.status = juce::String(item.stream->lastError());
        }
        out.push_back(e);
    }
    return out;
}

void VirtualIoManager::engineFormatWillChange() {
    for (auto& item : items_) stop(item);
}

void VirtualIoManager::engineFormatChanged(double, int) {
    for (auto& item : items_) start(item);
}

void VirtualIoManager::timerCallback() {
    for (auto& item : items_) {
        const bool dead = item.stream == nullptr || !item.stream->isRunning();
        if (!dead) continue;
        stop(item);
        start(item);  // app relaunched, endpoint driver reinstalled, ...
    }
}

nlohmann::json VirtualIoManager::saveState() const {
    auto endpoints = nlohmann::json::array();
    auto applications = nlohmann::json::array();
    for (const auto& item : items_) {
        if (item.entry.kind == Kind::Endpoint) {
            endpoints.push_back({{"name", item.entry.spec.name},
                                 {"flow", item.entry.spec.flow == EndpointFlow::Render ? "render" : "capture"},
                                 {"channels", item.entry.spec.channels},
                                 {"sampleRate", item.entry.spec.sampleRate}});
        } else {
            applications.push_back({{"executable", item.entry.executable.toStdString()},
                                    {"includeChildren", item.entry.includeChildren}});
        }
    }
    return {{"endpoints", endpoints}, {"applications", applications}};
}

juce::String VirtualIoManager::restoreState(const nlohmann::json& section) {
    juce::StringArray warnings;
    std::vector<Entry> wanted;
    if (section.is_object()) {
        for (const auto& e : section.value("endpoints", nlohmann::json::array())) {
            Entry entry;
            entry.kind = Kind::Endpoint;
            entry.spec.name = e.value("name", std::string("REKA Input"));
            entry.spec.flow = e.value("flow", std::string("render")) == "capture" ? EndpointFlow::Capture : EndpointFlow::Render;
            entry.spec.channels = e.value("channels", 2);
            entry.spec.sampleRate = e.value("sampleRate", 48000);
            entry.key = endpointKey(entry.spec.name);
            wanted.push_back(entry);
        }
        for (const auto& a : section.value("applications", nlohmann::json::array())) {
            Entry entry;
            entry.kind = Kind::Application;
            entry.executable = juce::String(a.value("executable", std::string()));
            entry.includeChildren = a.value("includeChildren", true);
            entry.key = applicationKey(entry.executable);
            if (entry.executable.isNotEmpty()) wanted.push_back(entry);
        }
    }

    // Keep running streams that are still wanted (hot reload must not interrupt them).
    for (const auto& existing : entries()) {
        const bool keep = std::any_of(wanted.begin(), wanted.end(), [&](const Entry& w) { return w.key == existing.key; });
        if (!keep) remove(existing.key);
    }
    for (const auto& w : wanted) {
        const bool present = std::any_of(items_.begin(), items_.end(), [&](const Item& i) { return i.entry.key == w.key; });
        if (present) continue;
        const juce::String error = w.kind == Kind::Endpoint ? addVirtualEndpoint(w.spec) : addApplicationCapture(w.executable, w.includeChildren);
        if (error.isNotEmpty()) warnings.add(juce::String(w.key) + ": " + error);
    }
    return warnings.joinIntoString("\n");
}

}  // namespace reka::app
