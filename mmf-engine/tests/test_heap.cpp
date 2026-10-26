// Property tests for the indexed weighted min-heap.
#include "mmf/weighted_min_heap.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "test_support.hpp"

using mmf::WeightedMinHeap;

MMF_TEST_SUITE(heap_orders_by_ascending_key) {
  WeightedMinHeap<std::uint64_t> heap;
  const std::vector<double> keys{5.0, -1.0, 12.5, 0.0, -7.25, 3.5};
  for (std::size_t i = 0; i < keys.size(); ++i) heap.pushOrUpdate(i + 1, keys[i]);

  heap.validate();
  std::vector<double> popped;
  WeightedMinHeap<std::uint64_t>::Item item;
  while (heap.popMin(item)) popped.push_back(item.sortKey);
  std::vector<double> expected = keys;
  std::sort(expected.begin(), expected.end());
  CHECK(popped == expected);
}

MMF_TEST_SUITE(heap_update_and_remove_by_key) {
  WeightedMinHeap<std::uint64_t> heap;
  for (std::uint64_t key = 1; key <= 100; ++key) heap.pushOrUpdate(key, static_cast<double>(key));
  heap.validate();

  CHECK(heap.updateKey(90, -50.0));
  heap.validate();
  CHECK_EQ(heap.peekMin()->key, static_cast<std::uint64_t>(90));

  CHECK(heap.erase(90));
  heap.validate();
  CHECK_EQ(heap.peekMin()->key, static_cast<std::uint64_t>(1));
  CHECK(!heap.erase(90));
  CHECK(!heap.updateKey(999, 0.0));

  CHECK(heap.erase(1));
  heap.validate();
  CHECK_EQ(heap.peekMin()->key, static_cast<std::uint64_t>(2));

  while (!heap.empty()) {
    WeightedMinHeap<std::uint64_t>::Item item;
    CHECK(heap.popMin(item));
  }
  heap.validate();
  WeightedMinHeap<std::uint64_t>::Item drained;
  CHECK(!heap.popMin(drained));
}

MMF_TEST_SUITE(heap_rebuild_rekeys_everything_in_place) {
  WeightedMinHeap<std::uint64_t> heap;
  for (std::uint64_t key = 1; key <= 50; ++key) heap.pushOrUpdate(key, 0.0);
  heap.validate();

  // Aging: older key == smaller value gets a much smaller sortKey.
  heap.rebuild([](std::uint64_t key) { return static_cast<double>(100 - key); });
  heap.validate();
  CHECK_EQ(heap.peekMin()->key, static_cast<std::uint64_t>(50));

  auto items = heap.items();
  CHECK_EQ(items.size(), static_cast<std::size_t>(50));
  for (std::uint64_t key = 1; key <= 50; ++key) {
    CHECK(heap.contains(key));
    CHECK(heap.updateKey(key, static_cast<double>(key)));
  }
  heap.validate();
  CHECK_EQ(heap.peekMin()->key, static_cast<std::uint64_t>(1));
}

MMF_TEST_SUITE(heap_matches_bruteforce_minimum_under_random_ops) {
  std::mt19937_64 rng(0xBEEF1234ULL);
  WeightedMinHeap<std::uint64_t> heap;
  std::vector<std::pair<std::uint64_t, double>> model;

  auto modelRemove = [&](std::uint64_t key) {
    auto it = std::find_if(model.begin(), model.end(),
                           [&](const auto& entry) { return entry.first == key; });
    if (it != model.end()) model.erase(it);
  };

  for (int round = 0; round < 20000; ++round) {
    const std::uint64_t key = rng() % 500;
    const double value = static_cast<double>(rng() % 100000) / 100.0 - 500.0;
    switch (rng() % 4) {
      case 0:
      case 1: {
        heap.pushOrUpdate(key, value);
        modelRemove(key);
        model.emplace_back(key, value);
        break;
      }
      case 2: {
        const bool ok = heap.updateKey(key, value);
        auto it = std::find_if(model.begin(), model.end(),
                               [&](const auto& entry) { return entry.first == key; });
        CHECK_EQ(ok, it != model.end());
        if (it != model.end()) it->second = value;
        break;
      }
      default: {
        const bool ok = heap.erase(key);
        const bool had = std::any_of(model.begin(), model.end(),
                                     [&](const auto& entry) { return entry.first == key; });
        CHECK_EQ(ok, had);
        modelRemove(key);
        break;
      }
    }
    if (round % 50 == 0) {
      heap.validate();
      CHECK_EQ(heap.size(), model.size());
      if (!model.empty()) {
        const double expected =
            std::min_element(model.begin(), model.end(),
                             [](const auto& a, const auto& b) { return a.second < b.second; })
                ->second;
        CHECK_EQ(heap.peekMin()->sortKey, expected);
      }
    }
  }
}
