#include "apex/concurrency/bounded_queue.hpp"
#include <gtest/gtest.h>
#include <array>
#include <thread>
#include <vector>

TEST(BoundedQueue, CapacityAndOwnership) {
    EXPECT_THROW(apex::BoundedQueue<int>(0), std::invalid_argument);
    apex::BoundedQueue<std::unique_ptr<int>> queue(1);
    EXPECT_TRUE(queue.try_push(std::make_unique<int>(7)));
    EXPECT_FALSE(queue.try_push(std::make_unique<int>(8)));
    EXPECT_EQ(**queue.try_pop(), 7);
    EXPECT_FALSE(queue.try_pop());
    EXPECT_EQ(queue.stats().high_watermark, 1U);
    EXPECT_EQ(queue.stats().rejected, 1U);
}
TEST(BoundedQueue, MultipleProducersPreserveEveryValueAndProducerOrder) {
    apex::BoundedQueue<std::pair<int, int>> queue(17);
    std::vector<std::jthread> producers;
    for (int producer = 0; producer < 8; ++producer) {
        producers.emplace_back([&, producer] {
            for (int i = 0; i < 5000; ++i) {
                while (!queue.try_push({producer, i})) { std::this_thread::yield(); }
            }
        });
    }
    std::array<int, 8> next{};
    for (int count = 0; count < 40000;) {
        if (auto value = queue.try_pop()) {
            EXPECT_EQ(value->second, next[static_cast<std::size_t>(value->first)]++);
            ++count;
        } else { std::this_thread::yield(); }
    }
    producers.clear();
    for (const auto count : next) { EXPECT_EQ(count, 5000); }
    EXPECT_EQ(queue.stats().size, 0U);
    EXPECT_LE(queue.stats().high_watermark, 17U);
}
