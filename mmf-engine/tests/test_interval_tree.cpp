// Property tests for the augmented interval tree.
#include "mmf/interval_tree.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <vector>

#include "test_support.hpp"

using mmf::IntervalTree;

namespace {

struct ReferenceModel {
  std::map<std::uint64_t, std::pair<std::int64_t, std::int64_t>> intervals;

  void upsert(std::uint64_t key, std::int64_t lo, std::int64_t hi) { intervals[key] = {lo, hi}; }

  bool erase(std::uint64_t key) { return intervals.erase(key) > 0; }

  std::set<std::uint64_t> query(std::int64_t qlo, std::int64_t qhi) const {
    std::set<std::uint64_t> hits;
    for (const auto& [key, bounds] : intervals) {
      if (bounds.first <= qhi && bounds.second >= qlo) hits.insert(key);
    }
    return hits;
  }
};

void crossCheck(const IntervalTree<std::uint64_t>& tree, const ReferenceModel& model,
                mmftest::TestContext& ctx, std::mt19937_64& rng) {
  tree.validate();
  CHECK_EQ(tree.size(), model.intervals.size());
  for (int i = 0; i < 40; ++i) {
    const auto a = static_cast<std::int64_t>(rng() % 2000);
    const auto b = a + static_cast<std::int64_t>(rng() % 400);
    std::set<std::uint64_t> expected = model.query(a, b);
    std::set<std::uint64_t> actual;
    tree.query(a, b, [&](const auto& iv) {
      actual.insert(iv.key);
      return true;
    });
    CHECK(expected == actual);
  }
}

}  // namespace

MMF_TEST_SUITE(interval_tree_basic_overlap) {
  IntervalTree<std::uint64_t> tree;
  tree.upsert({100, 200, 1});
  tree.upsert({150, 250, 2});
  tree.upsert({400, 500, 3});
  tree.validate();

  std::vector<std::uint64_t> hits;
  const auto result = tree.query(180, 190, [&](const auto& iv) {
    hits.push_back(iv.key);
    return true;
  });
  CHECK_EQ(result.matches, static_cast<std::size_t>(2));
  CHECK(hits == (std::vector<std::uint64_t>{1, 2}));

  std::vector<std::uint64_t> lonely;
  const auto none = tree.query(260, 399, [&](const auto& iv) {
    lonely.push_back(iv.key);
    return false;
  });
  CHECK_EQ(none.matches, static_cast<std::size_t>(0));
  CHECK(lonely.empty());

  // touching intervals overlap on their shared endpoint
  CHECK_EQ(tree.query(250, 400, [](const auto&) { return true; }).matches,
           static_cast<std::size_t>(2));
}

MMF_TEST_SUITE(interval_tree_removal) {
  IntervalTree<std::uint64_t> tree;
  for (std::uint64_t key = 1; key <= 200; ++key) {
    tree.upsert({static_cast<std::int64_t>(key * 10), static_cast<std::int64_t>(key * 10 + 50), key});
  }
  tree.validate();
  for (std::uint64_t key = 1; key <= 200; ++key) {
    if (key % 2 == 0) CHECK(tree.erase(key));
  }
  tree.validate();
  CHECK_EQ(tree.size(), static_cast<std::size_t>(100));

  // Re-inserting an existing key is an in-place upsert, not a duplicate.
  tree.upsert({5000, 5100, 7});
  tree.validate();
  const auto* found = tree.find(7);
  CHECK(found != nullptr);
  if (found != nullptr) {
    CHECK_EQ(found->lo, static_cast<std::int64_t>(5000));
    CHECK_EQ(found->hi, static_cast<std::int64_t>(5100));
  }
  CHECK_EQ(tree.size(), static_cast<std::size_t>(100));

  // A brand new key grows the structure.
  tree.upsert({5000, 5100, 9999});
  tree.validate();
  CHECK_EQ(tree.size(), static_cast<std::size_t>(101));

  tree.clear();
  tree.validate();
  CHECK(tree.empty());
  CHECK(!tree.erase(7));
}

MMF_TEST_SUITE(interval_tree_empty_and_edge_cases) {
  IntervalTree<std::uint64_t> tree;
  CHECK(tree.empty());
  CHECK_EQ(tree.height(), 0);
  CHECK(!tree.find(1));
  CHECK(!tree.erase(1));
  tree.query(0, 100, [](const auto&) { return true; });

  tree.upsert({5, 5, 42});  // degenerate point interval
  tree.validate();
  CHECK_EQ(tree.query(5, 5, [](const auto&) { return true; }).matches, static_cast<std::size_t>(1));
  CHECK_EQ(tree.query(6, 10, [](const auto&) { return true; }).matches, static_cast<std::size_t>(0));

  // Reversed query bounds are normalised.
  CHECK_EQ(tree.query(10, 6, [](const auto&) { return true; }).matches, static_cast<std::size_t>(0));

  CHECK(!tree.forEachOrdered([](const auto&) { return false; }));
  CHECK(tree.forEachOrdered([](const auto&) { return true; }));
}

MMF_TEST_SUITE(interval_tree_randomised_against_reference) {
  std::mt19937_64 rng(0xC0FFEEULL);
  IntervalTree<std::uint64_t> tree;
  ReferenceModel model;

  for (int round = 0; round < 4000; ++round) {
    const std::uint64_t key = rng() % 900;
    if (rng() % 3 == 0) {
      const bool removed = tree.erase(key);
      CHECK_EQ(removed, model.erase(key));
    } else {
      const auto center = static_cast<std::int64_t>(rng() % 1800);
      const auto half = static_cast<std::int64_t>(rng() % 200);
      tree.upsert({center - half, center + half, key});
      model.upsert(key, center - half, center + half);
    }
    if (round % 200 == 0) crossCheck(tree, model, ctx, rng);
  }
  crossCheck(tree, model, ctx, rng);
  CHECK(tree.height() <= 2 * static_cast<int>(std::log2(std::max<std::size_t>(tree.size(), 2))));
}

MMF_TEST_SUITE(interval_tree_query_cost_is_logarithmic_plus_matches) {
  // 20k non-overlapping windows: a point query must touch O(log N) nodes.
  IntervalTree<std::uint64_t> tree;
  for (std::uint64_t key = 0; key < 20000; ++key) {
    const auto base = static_cast<std::int64_t>(key) * 1000;
    tree.upsert({base, base + 100, key});
  }
  tree.validate();

  std::size_t worst = 0;
  for (std::uint64_t key = 0; key < 20000; key += 97) {
    const auto probe = static_cast<std::int64_t>(key) * 1000 + 50;
    const auto result = tree.query(probe, probe, [](const auto&) { return true; });
    CHECK_EQ(result.matches, static_cast<std::size_t>(1));
    worst = std::max(worst, result.nodesVisited);
  }
  // log2(20000) ~= 14.3; allow a generous constant factor for AVL overhead.
  CHECK_MSG(worst <= 8 * 15, "point query visited " + std::to_string(worst) + " nodes");
}
