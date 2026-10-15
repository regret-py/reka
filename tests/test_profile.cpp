#include <doctest/doctest.h>

#include "TestHelpers.h"
#include "reka/engine/MixerModel.h"
#include "reka/profile/Profile.h"

#include <cstdio>
#include <filesystem>

using namespace reka;
using reka::test::GainDelayInsert;

namespace {
class MockFactory final : public IInsertFactory {
public:
    std::shared_ptr<IInsertProcessor> createInsert(const InsertDescriptor& descriptor, double sampleRate, int maxBlock,
                                                   std::string& error) override {
        if (descriptor.pluginId == "missing") {
            error = "not installed";
            return nullptr;
        }
        ++created;
        auto insert = std::make_shared<GainDelayInsert>(1.0f, 0, descriptor.pluginId);
        insert->prepare(sampleRate, maxBlock);
        return insert;
    }
    int created = 0;
};

Profile sampleProfile() {
    Profile p;
    p.name = "Streaming";
    StripState mic;
    mic.id = 1;
    mic.kind = StripKind::Input;
    mic.name = "Mic";
    mic.colour = 0xFF3A7BD5u;
    mic.binding = {"master", 0, -1};
    mic.gainDb = -3.5f;
    mic.mode = ChannelMode::Mono;
    mic.eq[2].enabled = true;
    mic.eq[2].gainDb = 4.0f;
    InsertState comp;
    comp.descriptor = {"VST3-Comp-1234", "Comp", "VST3", true};
    comp.state = {1, 2, 3, 4, 5};
    comp.bypassed = true;
    mic.inserts.push_back(comp);
    p.mixer.inputs.push_back(mic);

    StripState spk;
    spk.id = 100;
    spk.kind = StripKind::Bus;
    spk.name = "A1 Speakers";
    spk.binding = {"master", 0, 1};
    spk.colour = 0x80B47CFFu;
    p.mixer.buses.push_back(spk);

    RouteState route;
    route.input = 1;
    route.bus = 100;
    route.gainDb = -2.0f;
    route.preFader = true;
    p.mixer.routes.push_back(route);

    p.hardware = {{"serial", {{"enabled", true}, {"port", "COM7"}}}, {"map", {1, 100}}};
    p.ui = {{"theme", "midnight-glass"}};
    p.mixer.panLaw = PanLaw::Linear;
    return p;
}
}  // namespace

TEST_CASE("Colour hex conversion uses CSS order") {
    std::uint32_t argb = 0;
    REQUIRE(colourFromHex("#4DA3FF", argb));
    CHECK(argb == 0xFF4DA3FFu);
    REQUIRE(colourFromHex("#4DA3FF80", argb));
    CHECK(argb == 0x804DA3FFu);
    CHECK(colourToHex(0x804DA3FFu) == "#4DA3FF80");
    CHECK(colourToHex(0xFF000000u) == "#000000");
    CHECK_FALSE(colourFromHex("4DA3FF", argb));
}

TEST_CASE("Profile JSON round trip") {
    const Profile original = sampleProfile();
    const Profile copy = profileFromJson(profileToJson(original));
    CHECK(copy.name == "Streaming");
    CHECK((copy.mixer.panLaw == PanLaw::Linear));
    REQUIRE(copy.mixer.inputs.size() == 1);
    const StripState& mic = copy.mixer.inputs[0];
    CHECK(mic.binding.right == -1);
    CHECK(mic.gainDb == doctest::Approx(-3.5f));
    CHECK((mic.mode == ChannelMode::Mono));
    CHECK(mic.eq[2].enabled);
    CHECK(mic.eq[2].gainDb == doctest::Approx(4.0f));
    REQUIRE(mic.inserts.size() == 1);
    CHECK(mic.inserts[0].state == std::vector<std::uint8_t>{1, 2, 3, 4, 5});
    CHECK(mic.inserts[0].descriptor.sandboxed);
    CHECK(mic.inserts[0].bypassed);
    REQUIRE(copy.mixer.buses.size() == 1);
    CHECK(copy.mixer.buses[0].colour == 0x80B47CFFu);
    REQUIRE(copy.mixer.routes.size() == 1);
    CHECK(copy.mixer.routes[0].preFader);
    CHECK(copy.hardware["serial"]["port"] == "COM7");
    CHECK(copy.ui["theme"] == "midnight-glass");
}

TEST_CASE("Profile validation rejects bad files") {
    auto json = profileToJson(sampleProfile());
    SUBCASE("wrong format tag") {
        json["format"] = "voicemeeter";
        CHECK_THROWS_AS(profileFromJson(json), ProfileError);
    }
    SUBCASE("newer version") {
        json["version"] = kProfileFormatVersion + 1;
        CHECK_THROWS_AS(profileFromJson(json), ProfileError);
    }
    SUBCASE("duplicate ids") {
        json["mixer"]["buses"][0]["id"] = 1;
        CHECK_THROWS_AS(profileFromJson(json), ProfileError);
    }
    SUBCASE("broken plugin state") {
        json["mixer"]["inputs"][0]["inserts"][0]["state"] = "%%%";
        CHECK_THROWS_AS(profileFromJson(json), ProfileError);
    }
    SUBCASE("dangling routes are dropped, not fatal") {
        json["mixer"]["routes"].push_back({{"from", 1}, {"to", 999}});
        CHECK(profileFromJson(json).mixer.routes.size() == 1);
    }
}

TEST_CASE("Profile file save/load") {
    const auto path = std::filesystem::temp_directory_path() / "reka_test_profile.reka";
    std::string error;
    REQUIRE(saveProfileFile(path, sampleProfile(), error));
    Profile loaded;
    REQUIRE(loadProfileFile(path, loaded, error));
    CHECK(loaded.mixer.inputs[0].name == "Mic");
    std::filesystem::remove(path);
    CHECK_FALSE(loadProfileFile(path, loaded, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Hot reload keeps unchanged strips, processors and plugin instances") {
    RekaAudioEngine engine;
    MixerModel model(engine);
    model.prepare(48000.0, 256);
    MockFactory factory;

    MixerState state = sampleProfile().mixer;
    ApplyReport first = applyMixerState(model, state, &factory);
    CHECK(first.stripsAdded == 2);
    CHECK(first.insertsCreated == 1);
    CHECK(factory.created == 1);
    CHECK(model.parameters(1)->gainDb() == doctest::Approx(-3.5f));
    CHECK(model.findRoute(1, 100)->enabled());

    const auto micProcessor = model.processor(1);
    const auto micParams = model.parameters(1);
    const auto pluginSlot = model.strip(1)->inserts.at(0);

    // Same profile again: nothing is recreated.
    ApplyReport same = applyMixerState(model, captureMixerState(model), &factory);
    CHECK(same.stripsAdded == 0);
    CHECK(same.insertsReused == 1);
    CHECK(factory.created == 1);
    CHECK(model.processor(1) == micProcessor);
    CHECK(model.strip(1)->inserts.at(0) == pluginSlot);

    // Edited profile: gain change, new strip, plugin state change, missing plugin.
    state.inputs[0].gainDb = -10.0f;
    state.inputs[0].inserts[0].state = {9, 9, 9};
    InsertState missing;
    missing.descriptor = {"missing", "Ghost", "VST3", false};
    state.inputs[0].inserts.push_back(missing);
    StripState music;
    music.id = 2;
    music.name = "Music";
    state.inputs.push_back(music);
    ApplyReport edited = applyMixerState(model, state, &factory);
    CHECK(edited.stripsAdded == 1);
    CHECK(edited.insertsCreated == 1);
    CHECK(edited.warnings.size() == 1);
    CHECK(model.processor(1) == micProcessor);  // DSP state survives
    CHECK(model.parameters(1) == micParams);
    CHECK(micParams->gainDb() == doctest::Approx(-10.0f));
    CHECK(model.strip(1)->inserts.at(0) != pluginSlot);  // new state -> new instance
    CHECK(model.strips(StripKind::Input).size() == 2);

    // Removing a strip removes its routes too.
    state.inputs.erase(state.inputs.begin());
    state.routes.clear();
    ApplyReport removed = applyMixerState(model, state, &factory);
    CHECK(removed.stripsRemoved == 1);
    CHECK(model.routes().empty());
}
