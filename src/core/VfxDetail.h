#pragma once

// Internals shared by the VFX sources (Vfx.cpp, VfxSequencer.cpp): byte reading / writing and the lock-free
// settings hand-over to the audio thread.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace lynx::vfx::detail
{

struct Reader
{
    const std::uint8_t* data;
    std::size_t size, pos = 0;
    bool failed = false;

    template <typename T>
    T get() noexcept
    {
        T v {};
        if (failed || sizeof (T) > size - pos)
        {
            failed = true;
            return v;
        }
        std::memcpy (&v, data + pos, sizeof (T));
        pos += sizeof (T);
        return v;
    }
    std::int32_t i32() noexcept { return get<std::int32_t>(); }
    std::uint32_t u32() noexcept { return get<std::uint32_t>(); }
    std::size_t left() const noexcept { return failed ? 0 : size - pos; }
};

template <typename T>
void put (std::vector<std::uint8_t>& out, T v)
{
    const auto* p = reinterpret_cast<const std::uint8_t*> (&v);
    out.insert (out.end(), p, p + sizeof (T));
}

// Settings handed from the UI thread to the audio thread without locks or allocation on the audio side (the
// Patcher graph's pattern): the UI publishes a new object, the audio thread swaps it in and parks the old one, the
// UI frees it on its next call.
template <typename T>
class Handoff
{
public:
    explicit Handoff (T initial) : active (std::make_unique<T> (std::move (initial))) {}
    explicit Handoff (std::unique_ptr<T> initial) : active (std::move (initial)) {}
    ~Handoff()
    {
        delete pending.exchange (nullptr);
        delete retired.exchange (nullptr);
    }
    void publish (T value)
    {
        delete retired.exchange (nullptr);
        delete pending.exchange (new T (std::move (value)));
    }
    void publish (std::unique_ptr<T> value)
    {
        delete retired.exchange (nullptr);
        delete pending.exchange (value.release());
    }
    // audio thread: true when new settings arrived
    bool take() noexcept
    {
        auto* incoming = pending.exchange (nullptr);
        if (incoming == nullptr)
            return false;
        auto* old = active.release();
        active.reset (incoming);
        delete retired.exchange (old); // (normally null: the UI frees it on its next call)
        return true;
    }
    const T& get() const noexcept { return *active; }

private:
    std::unique_ptr<T> active;
    std::atomic<T*> pending { nullptr };
    std::atomic<T*> retired { nullptr };
};

} // namespace lynx::vfx::detail
