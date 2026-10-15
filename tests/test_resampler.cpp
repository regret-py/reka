#include <doctest/doctest.h>

#include "TestHelpers.h"
#include "reka/dsp/SincResampler.h"
#include "reka/io/AsyncEndpointPort.h"

#include <cmath>
#include <vector>

using namespace reka;
using reka::test::kPi;

namespace {
// Goertzel power at `freq` relative to a full-scale sine.
double toneLevelDb(const std::vector<float>& x, std::size_t begin, std::size_t end, double freq, double fs) {
    double s1 = 0, s2 = 0;
    const double coeff = 2.0 * std::cos(2.0 * kPi * freq / fs);
    for (std::size_t i = begin; i < end; ++i) {
        const double s = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    const double n = static_cast<double>(end - begin);
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 10.0 * std::log10(power / (n * n / 4.0) + 1e-30);
}
}  // namespace

TEST_CASE("SincResampler converts 44.1 kHz to 48 kHz with low distortion") {
    const double inRate = 44100.0, outRate = 48000.0;
    SincResampler rs;
    rs.prepare(1, inRate / outRate, 1024);

    std::vector<float> output;
    std::vector<float> chunk(441), out(2048);
    std::size_t n = 0;
    for (int block = 0; block < 400; ++block) {  // 4 s
        for (float& s : chunk) s = 0.5f * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * static_cast<double>(n++) / inRate));
        const float* in[1] = {chunk.data()};
        REQUIRE(rs.pushInput(in, 441) == 441);
        float* o[1] = {out.data()};
        const int produced = rs.process(o, 2048);
        output.insert(output.end(), out.begin(), out.begin() + produced);
    }
    // 4 s of input -> ~4 s of output
    CHECK(std::abs(static_cast<double>(output.size()) - 4.0 * outRate) < 64.0);

    // The tone lands at 1 kHz, at the right level, with residual well below -80 dB.
    const std::size_t begin = 4800, end = begin + 96000;
    CHECK(toneLevelDb(output, begin, end, 1000.0, outRate) == doctest::Approx(-6.02).epsilon(0.01));
    std::vector<float> residual(output.begin() + static_cast<long>(begin), output.begin() + static_cast<long>(end));
    // Remove the fundamental by least squares fit of sin/cos at 1 kHz.
    double a = 0, b = 0;
    for (std::size_t i = 0; i < residual.size(); ++i) {
        const double t = 2.0 * kPi * 1000.0 * static_cast<double>(i) / outRate;
        a += residual[i] * std::sin(t);
        b += residual[i] * std::cos(t);
    }
    a *= 2.0 / static_cast<double>(residual.size());
    b *= 2.0 / static_cast<double>(residual.size());
    double err = 0;
    for (std::size_t i = 0; i < residual.size(); ++i) {
        const double t = 2.0 * kPi * 1000.0 * static_cast<double>(i) / outRate;
        const double e = residual[i] - (a * std::sin(t) + b * std::cos(t));
        err += e * e;
    }
    const double thdNoiseDb = 10.0 * std::log10(err / static_cast<double>(residual.size()) / 0.125);
    CHECK(thdNoiseDb < -80.0);
}

TEST_CASE("DriftController locks onto a clock that runs 150 ppm fast") {
    // Simulates a capture device whose crystal is 150 ppm fast feeding a 48 kHz engine.
    const double nominal = 1.0;
    const double deviceRate = 48000.0 * (1.0 + 150e-6);
    DriftController controller;
    const double target = 1024.0;
    controller.reset(nominal, target, 48000.0);
    double fill = target;
    const int block = 256;
    for (int i = 0; i < 48000 * 300 / block; ++i) {  // five minutes
        fill += deviceRate * block / 48000.0;            // produced by the device during one engine block
        fill -= controller.ratio() * block;              // consumed by the resampler
        controller.update(fill, block / 48000.0);
        REQUIRE(fill > 0.0);
        REQUIRE(fill < 4.0 * target);
    }
    CHECK(controller.correctionPpm() == doctest::Approx(150.0).epsilon(0.05));
    CHECK(std::abs(fill - controller.targetFill()) < 16.0);
    CHECK(std::abs(controller.targetFill() - target) < 32.0);
}

TEST_CASE("AsyncEndpointPort bridges a 44.1 kHz endpoint into a 48 kHz engine") {
    AsyncEndpointPort port;
    AsyncPortFormat format;
    format.captureChannels = 2;
    format.renderChannels = 2;
    format.endpointSampleRate = 44100.0;
    format.endpointMaxBlock = 441;
    port.configure(format, 48000.0, 256);

    std::vector<float> endpointL(441), endpointR(441), renderL(441), renderR(441);
    std::vector<float> captured;
    std::size_t n = 0;
    double endpointClock = 0.0;
    // Interleave endpoint callbacks (10 ms) and engine blocks (256 frames) in time order for 3 s.
    for (int engineBlock = 0; engineBlock < 48000 * 3 / 256; ++engineBlock) {
        const double engineTime = (engineBlock + 1) * 256 / 48000.0;
        while (endpointClock < engineTime) {
            for (std::size_t i = 0; i < 441; ++i, ++n)
                endpointL[i] = endpointR[i] = 0.25f * static_cast<float>(std::sin(2.0 * kPi * 440.0 * static_cast<double>(n) / 44100.0));
            const float* src[2] = {endpointL.data(), endpointR.data()};
            port.endpointWriteCapture(src, 2, 441);
            float* dst[2] = {renderL.data(), renderR.data()};
            port.endpointReadRender(dst, 2, 441);
            endpointClock += 0.01;
        }
        port.beginBlock(256);
        const float* in = port.inputChannel(0);
        captured.insert(captured.end(), in, in + 256);
        // Loop the captured audio back out through the render side.
        std::copy(in, in + 256, port.outputChannel(0));
        std::copy(in, in + 256, port.outputChannel(1));
        port.endBlock(256);
    }
    const auto stats = port.stats();
    CHECK(stats.captureOverruns == 0);
    CHECK(stats.renderOverruns == 0);
    // After priming, the engine sees a clean 440 Hz tone at the endpoint's level.
    CHECK(toneLevelDb(captured, 48000, 48000 + 96000, 440.0, 48000.0) == doctest::Approx(-12.04).epsilon(0.02));
    CHECK(stats.captureUnderruns <= 1);
    CHECK(std::fabs(stats.captureDriftPpm) < 150.0f);  // identical clocks: only a tiny, inaudible trim
}
