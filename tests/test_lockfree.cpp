#include <doctest/doctest.h>

#include "reka/common/Base64.h"
#include "reka/lockfree/AudioRingBuffer.h"
#include "reka/lockfree/SpscQueue.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace reka;

TEST_CASE("SpscQueue preserves order across threads") {
    SpscQueue<int, 64> queue;
    constexpr int kCount = 200000;
    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i)
            while (!queue.push(i)) std::this_thread::yield();
    });
    int expected = 0;
    while (expected < kCount) {
        int value = -1;
        if (queue.pop(value)) {
            REQUIRE(value == expected);
            ++expected;
        }
    }
    producer.join();
    CHECK(queue.empty());
}

TEST_CASE("SpscQueue reports full") {
    SpscQueue<int, 4> queue;
    CHECK(queue.push(1));
    CHECK(queue.push(2));
    CHECK(queue.push(3));
    CHECK(queue.push(4));
    CHECK_FALSE(queue.hasSpace());
    CHECK_FALSE(queue.push(5));
    int v = 0;
    CHECK(queue.pop(v));
    CHECK(v == 1);
    CHECK(queue.hasSpace());
}

TEST_CASE("AudioRingBuffer wraps and handles mono sources") {
    AudioRingBuffer ring;
    ring.allocate(2, 100);  // rounded up to 128
    CHECK(ring.capacity() == 128);

    std::vector<float> mono(90);
    for (int i = 0; i < 90; ++i) mono[static_cast<std::size_t>(i)] = static_cast<float>(i);
    const float* src[1] = {mono.data()};
    CHECK(ring.write(src, 1, 90) == 90);

    std::vector<float> l(64), r(64);
    float* dst[2] = {l.data(), r.data()};
    CHECK(ring.read(dst, 2, 64) == 64);
    CHECK(l[10] == 10.0f);
    CHECK(r[10] == 10.0f);  // mono duplicated

    CHECK(ring.write(src, 1, 90) == 90);  // wraps around the end
    CHECK(ring.availableToRead() == 116);
    CHECK(ring.write(src, 1, 90) == 12);  // only 12 frames of space left
    CHECK(ring.discard(26) == 26);
    CHECK(ring.read(dst, 2, 64) == 64);
    CHECK(l[0] == 0.0f);
    CHECK(l[63] == 63.0f);
}

TEST_CASE("AudioRingBuffer interleaved round trip with downmix") {
    AudioRingBuffer ring;
    ring.allocate(2, 64);
    std::vector<float> interleaved = {1, -1, 2, -2, 3, -3};
    CHECK(ring.writeInterleaved(interleaved.data(), 2, 3) == 3);
    std::vector<float> mono(3);
    CHECK(ring.readInterleaved(mono.data(), 1, 3) == 3);
    CHECK(mono[0] == doctest::Approx(0.0));
    CHECK(mono[2] == doctest::Approx(0.0));
}

TEST_CASE("Base64 round trip") {
    for (std::size_t size : {0u, 1u, 2u, 3u, 4u, 5u, 255u}) {
        std::vector<std::uint8_t> data(size);
        for (std::size_t i = 0; i < size; ++i) data[i] = static_cast<std::uint8_t>(i * 37 + 11);
        const std::string text = base64Encode(data);
        const auto decoded = base64Decode(text);
        REQUIRE(decoded.has_value());
        CHECK(*decoded == data);
    }
    CHECK(base64Encode(std::vector<std::uint8_t>{'M', 'a', 'n'}) == "TWFu");
    CHECK_FALSE(base64Decode("TW$u").has_value());
}
