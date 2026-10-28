#pragma once

// Sorting files by a time read from the disk (issue #12's crash, LYNX 0.1.18): std::sort with a comparator that reads
// getLastAccessTime() on every comparison is undefined behaviour when the times change during the sort — and they do:
// reading a cache file updates its access time, other threads touch / delete files — and libc++'s introsort then ran
// past the array (an access violation in WaveformCache's disk-cache pruning while cycling quickly through samples).
// Each time is read once here, and the sort runs on those copies.

#include <algorithm>
#include <utility>
#include <vector>

#include <juce_core/juce_core.h>

namespace lynx::filesort
{
enum class By { accessTime, modificationTime };

// Oldest first (`newestFirst`: the other way); ties keep the path order, so the result is deterministic.
inline void byTime (juce::Array<juce::File>& files, By by, bool newestFirst = false)
{
    std::vector<std::pair<juce::int64, juce::File>> keyed;
    keyed.reserve ((std::size_t) files.size());
    for (const auto& f : files)
        keyed.emplace_back ((by == By::accessTime ? f.getLastAccessTime() : f.getLastModificationTime()).toMilliseconds(), f);
    std::sort (keyed.begin(), keyed.end(), [newestFirst] (const auto& a, const auto& b) {
        if (a.first != b.first)
            return newestFirst ? a.first > b.first : a.first < b.first;
        return a.second.getFullPathName() < b.second.getFullPathName();
    });
    files.clearQuick();
    for (auto& k : keyed)
        files.add (std::move (k.second));
}
} // namespace lynx::filesort
