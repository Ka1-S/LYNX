#pragma once

// Sorting files by a time read from the disk (issue #12's crash, LYNX 0.1.18): the times are read once per file and
// the sort runs on those copies (core/SortOnce.h has the why; a comparator reading getLastAccessTime() on every
// comparison crashed WaveformCache's disk-cache pruning while the user cycled quickly through samples).

#include "core/SortOnce.h"

#include <juce_core/juce_core.h>

namespace lynx::filesort
{
enum class By { accessTime, modificationTime };

// Oldest first (`newestFirst`: the other way); ties keep the path order, so the result is deterministic.
inline void byTime (juce::Array<juce::File>& files, By by, bool newestFirst = false)
{
    sortonce::byKey<juce::File> (
        files,
        [by] (const juce::File& f) {
            return (by == By::accessTime ? f.getLastAccessTime() : f.getLastModificationTime()).toMilliseconds();
        },
        [] (const juce::File& a, const juce::File& b) { return a.getFullPathName() < b.getFullPathName(); },
        newestFirst);
}
} // namespace lynx::filesort
