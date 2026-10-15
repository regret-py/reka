#include <doctest/doctest.h>

#include "TestHelpers.h"
#include "reka/dsp/Biquad.h"
#include "reka/dsp/DelayLine.h"
#include "reka/dsp/FaderLaw.h"
#include "reka/dsp/LoudnessMeter.h"
#include "reka/dsp/MeterTap.h"
#include "reka/dsp/PanLaw.h"
#include "reka/dsp/ParametricEq.h"
#include "reka/dsp/Smoothing.h"

#include <cmath>
#include <vector>

using namespace reka;
using reka::test::kPi;

TEST_CASE("Fader law round-trips and hits the console scale") {
    CHECK(FaderLaw::positionToDb(0.75f) == doctest::Approx(0.0f));
    CHECK(FaderLaw::positionToDb(1.0f) == doctest::Approx(12.0f));
    CHECK(FaderLaw::positionToDb(0.0f) == kSilenceDb);
    for (float p = 0.01f; p < 1.0f; p += 0.01f) CHECK(FaderLaw::dbToPosition(FaderLaw::positionToDb(p)) == doctest::Approx(p).epsilon(1e-4));
    CHECK(dbToGain(FaderLaw::positionToDb(0.0f)) == 0.0f);
}

TEST_CASE("Constant power pan keeps power at centre") {
    const StereoGains centre = panMono(0.0f, PanLaw::ConstantPower);
    CHECK(centre.left * centre.left + centre.right * centre.right == doctest::Approx(1.0f));
    const StereoGains hardLeft = panMono(-1.0f, PanLaw::ConstantPower);
    CHECK(hardLeft.left == doctest::Approx(1.0f));
    CHECK(hardLeft.right == doctest::Approx(0.0f).epsilon(1e-6));
    const StereoGains unity = panMono(0.0f, PanLaw::ConstantPowerUnityCentre);
    CHECK(unity.left == doctest::Approx(1.0f));
    const StereoGains bal = balance(0.5f);
    CHECK(bal.right == doctest::Approx(1.0f));
    CHECK(bal.left == doctest::Approx(std::cos(0.25 * kPi)));
}

TEST_CASE("Biquad designs match their nominal response") {
    const double fs = 48000.0;
    auto bell = designBiquad(FilterType::Bell, fs, 1000.0, 1.0, 6.0);
    CHECK(magnitudeDb(bell, 1000.0, fs) == doctest::Approx(6.0).epsilon(1e-3));
    CHECK(magnitudeDb(bell, 20.0, fs) == doctest::Approx(0.0).epsilon(0.05));

    auto lowCut = designBiquad(FilterType::LowCut, fs, 100.0, 0.7071, 0.0);
    CHECK(magnitudeDb(lowCut, 100.0, fs) == doctest::Approx(-3.01).epsilon(0.01));
    CHECK(magnitudeDb(lowCut, 10000.0, fs) == doctest::Approx(0.0).epsilon(0.01));

    auto shelf = designBiquad(FilterType::HighShelf, fs, 5000.0, 0.7071, -9.0);
    CHECK(magnitudeDb(shelf, 20000.0, fs) == doctest::Approx(-9.0).epsilon(0.05));

    auto notch = designBiquad(FilterType::Notch, fs, 50.0, 10.0, 0.0);
    CHECK(magnitudeDb(notch, 50.0, fs) < -60.0);
}

TEST_CASE("ParametricEq processes a sine with the designed gain") {
    const double fs = 48000.0;
    EqParameters params;
    EqBand band;
    band.enabled = true;
    band.type = FilterType::Bell;
    band.frequency = 1000.0f;
    band.gainDb = -12.0f;
    band.q = 2.0f;
    params.setBand(3, band);

    EqProcessor eq;
    eq.prepare(fs);
    CHECK(eq.update(params));

    std::vector<float> l(48000), r(48000);
    for (std::size_t i = 0; i < l.size(); ++i) l[i] = r[i] = static_cast<float>(std::sin(2.0 * kPi * 1000.0 * i / fs));
    float* ch[2] = {l.data(), r.data()};
    for (int offset = 0; offset < 48000; offset += 480) {
        float* block[2] = {ch[0] + offset, ch[1] + offset};
        eq.process(block, 2, 480);
    }
    const double outRms = reka::test::rms(l, 24000, 48000);
    CHECK(20.0 * std::log10(outRms / std::sqrt(0.5)) == doctest::Approx(-12.0).epsilon(0.01));

    // A flat EQ is skipped entirely.
    params.setBand(3, EqBand{});
    CHECK_FALSE(eq.update(params));
}

TEST_CASE("SmoothedValue ramps without discontinuity") {
    SmoothedValue v;
    v.prepare(48000.0, 10.0);
    v.reset(0.0f);
    float last = 0.0f;
    for (int block = 0; block < 200; ++block) {
        const GainSpan span = v.advance(1.0f, 64);
        CHECK(span.start == doctest::Approx(last));
        CHECK(span.end >= span.start);
        last = span.end;
    }
    CHECK(last == doctest::Approx(1.0f));
}

TEST_CASE("DelayLine delays by exactly N samples") {
    DelayLine line(100);
    std::vector<float> l(256), r(256);
    for (int i = 0; i < 256; ++i) l[static_cast<std::size_t>(i)] = r[static_cast<std::size_t>(i)] = static_cast<float>(i + 1);
    float* ch[2] = {l.data(), r.data()};
    float* a[2] = {ch[0], ch[1]};
    float* b[2] = {ch[0] + 128, ch[1] + 128};
    line.process(a, 2, 128, 37);
    line.process(b, 2, 128, 37);
    CHECK(l[36] == 0.0f);
    CHECK(l[37] == 1.0f);
    CHECK(l[200] == 164.0f);
}

TEST_CASE("MeterTap gives independent peak readers and RMS") {
    MeterTap meter;
    meter.prepare(48000.0, 300.0);
    std::vector<float> l(4800), r(4800, 0.0f);
    for (std::size_t i = 0; i < l.size(); ++i) l[i] = 0.5f * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * i / 48000.0));
    const float* ch[2] = {l.data(), r.data()};
    for (int i = 0; i < 20; ++i) meter.process(ch, 2, 4800);
    const MeterReading ui = meter.read(MeterReader::Ui);
    const MeterReading hw = meter.read(MeterReader::Hardware);
    CHECK(ui.peak[0] == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK(hw.peak[0] == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK(ui.rms[0] == doctest::Approx(0.5f / std::sqrt(2.0f)).epsilon(0.01));
    CHECK(meter.read(MeterReader::Ui).peak[0] == 0.0f);  // reset on read
    CHECK_FALSE(ui.clipped);
}

TEST_CASE("Loudness meter matches EBU Tech 3341 case 1 (stereo 1 kHz at -23 dBFS)") {
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        LoudnessMeter meter;
        meter.prepare(fs);
        LoudnessIntegrator integrator;
        const double amplitude = std::pow(10.0, -23.0 / 20.0);
        const int total = static_cast<int>(fs * 20.0);
        std::vector<float> l(1024), r(1024);
        std::size_t n = 0;
        for (int done = 0; done < total; done += 1024) {
            for (std::size_t i = 0; i < 1024; ++i, ++n)
                l[i] = r[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(n) / fs));
            const float* ch[2] = {l.data(), r.data()};
            meter.process(ch, 2, 1024);
            integrator.drain(meter);
        }
        CHECK(meter.momentaryLufs() == doctest::Approx(-23.0).epsilon(0.005));
        CHECK(meter.shortTermLufs() == doctest::Approx(-23.0).epsilon(0.005));
        CHECK(integrator.integratedLufs() == doctest::Approx(-23.0).epsilon(0.005));
    }
}

TEST_CASE("Integrated loudness applies the relative gate") {
    // 10 s at -20 LUFS then 10 s of very quiet signal (-50 LUFS) -> the quiet part is gated out.
    LoudnessIntegrator integrator;
    const double loud = std::pow(10.0, (-20.0 + 0.691) / 10.0);
    const double quiet = std::pow(10.0, (-50.0 + 0.691) / 10.0);
    for (int i = 0; i < 100; ++i) integrator.addBlock(loud);
    for (int i = 0; i < 100; ++i) integrator.addBlock(quiet);
    CHECK(integrator.integratedLufs() == doctest::Approx(-20.0).epsilon(0.01));
}
