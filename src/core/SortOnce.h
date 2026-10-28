#pragma once

// Sorting by a key that can change while the sort runs (issue #12's crash, LYNX 0.1.18): std::sort with a comparator
// that reads a file's access time on every comparison is undefined behaviour when the times change during the sort —
// and they do (reading a cache file updates its access time, other threads touch / delete files) — and libc++'s
// introsort then ran past the array. Here every key is read exactly once, and the sort runs on those copies.

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace lynx::sortonce
{
// Sorts `items` by key (ascending; `descending`: the other way). `key (item)` is called exactly once per item; equal
// keys are ordered by `tie (a, b)` (a strict weak order on the items), so the result is deterministic.
template <class Item, class Container, class KeyFn, class TieLess>
void byKey (Container& items, KeyFn key, TieLess tie, bool descending = false)
{
    using Key = decltype (key (std::declval<const Item&>()));
    std::vector<std::pair<Key, Item>> keyed;
    keyed.reserve ((std::size_t) items.size());
    for (const auto& item : items)
        keyed.emplace_back (key (item), item);
    std::sort (keyed.begin(), keyed.end(), [&] (const auto& a, const auto& b) {
        if (a.first < b.first || b.first < a.first)
            return descending ? b.first < a.first : a.first < b.first;
        return tie (a.second, b.second);
    });
    std::size_t i = 0;
    for (auto& item : items)
        item = std::move (keyed[i++].second);
}
} // namespace lynx::sortonce
