// MMF Engine — indexed weighted min-heap.
//
// The matchmaking loop needs three operations on the same collection:
//   1. find the highest-priority ticket                        -> O(1) peek
//   2. remove it                                               -> O(log N)
//   3. delete/refresh an arbitrary ticket (disconnect, aging)  -> O(log N)
//
// A plain std::priority_queue supports none of (3). This heap keeps a
// key -> array-position index so every mutation is O(log N) and stays coherent.
//
// Direction: it is a *min-heap* on `sortKey`. The aging policy returns a
// priority where higher means "more urgent", and the engine pushes
// `sortKey = -priority`. Popping the minimum sortKey therefore pops the most
// urgent ticket, which keeps the classic min-heap mechanics intact while the
// documented priority score rises with wait time.
#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mmf {

template <class Key>
class WeightedMinHeap {
 public:
  struct Item {
    Key key{};
    double sortKey{0.0};
  };

  WeightedMinHeap() = default;

  [[nodiscard]] std::size_t size() const noexcept { return heap_.size(); }
  [[nodiscard]] bool empty() const noexcept { return heap_.empty(); }
  [[nodiscard]] bool contains(const Key& key) const { return index_.find(key) != index_.end(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return heap_.capacity(); }

  /// Inserts the key, or repositions it when it is already present.
  bool pushOrUpdate(const Key& key, double sortKey) {
    auto it = index_.find(key);
    if (it != index_.end()) return updateKey(key, sortKey);
    heap_.push_back(Item{key, sortKey});
    index_.emplace(key, heap_.size() - 1);
    siftUp(heap_.size() - 1);
    return true;
  }

  /// Changes a key's sortKey and restores heap order in O(log N).
  bool updateKey(const Key& key, double sortKey) {
    auto it = index_.find(key);
    if (it == index_.end()) return false;
    const std::size_t position = it->second;
    const double previous = heap_[position].sortKey;
    heap_[position].sortKey = sortKey;
    if (sortKey < previous) {
      siftUp(position);
    } else if (sortKey > previous) {
      siftDown(position);
    }
    return true;
  }

  bool erase(const Key& key) {
    auto it = index_.find(key);
    if (it == index_.end()) return false;
    const std::size_t position = it->second;
    const std::size_t last = heap_.size() - 1;
    if (position != last) swapItems(position, last);
    heap_.pop_back();
    index_.erase(key);
    if (position < heap_.size()) {
      siftUp(position);
      siftDown(index_[heap_[position].key]);
    }
    return true;
  }

  [[nodiscard]] const Item* peekMin() const { return heap_.empty() ? nullptr : &heap_.front(); }

  bool popMin(Item& out) {
    if (heap_.empty()) return false;
    out = heap_.front();
    const std::size_t last = heap_.size() - 1;
    swapItems(0, last);
    heap_.pop_back();
    index_.erase(out.key);
    if (!heap_.empty()) siftDown(0);
    return true;
  }

  /// Recomputes every key and re-heapifies in O(N) (Floyd's algorithm).
  /// The aging policy is time dependent, so the worker calls this on a fixed
  /// cadence (default 100 ms) which bounds key staleness by that interval —
  /// far below the multi-second granularity of the aging curve.
  template <class KeyFunction>
  void rebuild(KeyFunction&& keyOf) {
    for (Item& item : heap_) item.sortKey = keyOf(item.key);
    for (std::size_t i = heap_.size() / 2; i-- > 0;) siftDown(i);
  }

  void clear() noexcept {
    heap_.clear();
    index_.clear();
  }

  [[nodiscard]] std::vector<Item> items() const { return heap_; }

  void validate() const {
    if (index_.size() != heap_.size()) throw std::logic_error("heap: index size mismatch");
    for (std::size_t i = 0; i < heap_.size(); ++i) {
      auto it = index_.find(heap_[i].key);
      if (it == index_.end() || it->second != i)
        throw std::logic_error("heap: index position mismatch");
      const std::size_t left = 2 * i + 1;
      const std::size_t right = 2 * i + 2;
      if (left < heap_.size() && heap_[left].sortKey < heap_[i].sortKey)
        throw std::logic_error("heap: min-heap property violated (left)");
      if (right < heap_.size() && heap_[right].sortKey < heap_[i].sortKey)
        throw std::logic_error("heap: min-heap property violated (right)");
    }
  }

 private:
  void swapItems(std::size_t a, std::size_t b) {
    std::swap(heap_[a], heap_[b]);
    index_[heap_[a].key] = a;
    index_[heap_[b].key] = b;
  }

  void siftUp(std::size_t position) {
    while (position > 0) {
      const std::size_t parent = (position - 1) / 2;
      if (heap_[parent].sortKey <= heap_[position].sortKey) break;
      swapItems(parent, position);
      position = parent;
    }
  }

  void siftDown(std::size_t position) {
    const std::size_t count = heap_.size();
    while (true) {
      const std::size_t left = 2 * position + 1;
      if (left >= count) break;
      std::size_t smallest = left;
      const std::size_t right = left + 1;
      if (right < count && heap_[right].sortKey < heap_[left].sortKey) smallest = right;
      if (heap_[position].sortKey <= heap_[smallest].sortKey) break;
      swapItems(position, smallest);
      position = smallest;
    }
  }

  std::vector<Item> heap_;
  std::unordered_map<Key, std::size_t> index_;
};

}  // namespace mmf
