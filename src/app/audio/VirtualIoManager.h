#pragma once

#include "audio/RekaDeviceManager.h"
#include "reka/virtualaudio/RekaVirtualAudio.h"

#include <juce_events/juce_events.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <vector>

namespace reka::app {

// Owns the REKA virtual endpoints and per-application captures, registers their ports with the mixer and
// recreates them whenever the engine format changes (their drift-corrected ports are format specific).
class VirtualIoManager final : public RekaDeviceManager::Listener, private juce::Timer {
public:
    enum class Kind { Endpoint, Application };

    struct Entry {
        Kind kind = Kind::Endpoint;
        VirtualEndpointSpec spec;    // Endpoint
        juce::String executable;     // Application ("Discord.exe")
        bool includeChildren = true; // Application
        PortKey key;
        bool running = false;
        juce::String status;
    };

    VirtualIoManager(RekaVirtualAudio& backend, MixerModel& mixer, RekaDeviceManager& devices);
    ~VirtualIoManager() override;

    RekaVirtualAudio& backend() noexcept { return backend_; }

    juce::String addVirtualEndpoint(const VirtualEndpointSpec& spec);
    juce::String addApplicationCapture(const juce::String& executable, bool includeChildren);
    void remove(const PortKey& key);
    std::vector<Entry> entries() const;

    nlohmann::json saveState() const;
    juce::String restoreState(const nlohmann::json& virtualSection);

    void engineFormatWillChange() override;
    void engineFormatChanged(double sampleRate, int blockSize) override;

    static PortKey endpointKey(const std::string& name) { return "virtual:" + name; }
    static PortKey applicationKey(const juce::String& executable) {
        return "app:" + executable.toLowerCase().toStdString();
    }

private:
    struct Item {
        Entry entry;
        std::unique_ptr<IVirtualStream> stream;
    };

    void timerCallback() override;
    juce::String start(Item& item);
    void stop(Item& item);

    RekaVirtualAudio& backend_;
    MixerModel& mixer_;
    RekaDeviceManager& devices_;
    std::vector<Item> items_;
};

}  // namespace reka::app
