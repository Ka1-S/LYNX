#pragma once

// FL Studio's Fruity WaveShaper and Fruity Soft Clipper as LYNX mixer effects, exact engines rebuilt in
// docs/analise/REA_rodada51.md (clean-room: layouts, formulas and behaviour in our own words; no Image-Line code or
// data in this repository).
//
// Soft Clipper (EffectKind::fruitySoftClipper): stateless. Parameters = FL's two controls over their ranges:
//   [0] threshold 1…127, [1] post 0…160 (value = (raw − min) / (max − min)).
//
// WaveShaper (EffectKind::fruityWaveShaper): the curve (FL's map editor points) travels in EffectSlot::state as FL's
// own saved state (plugin record 0xD5); the controls are slot parameters so the LYNX UI can change them:
//   [0] pre amp 64…192, [1] mix 0…128, [2] post gain 0…128, [3] oversampling order 0…4 (×1…×16),
//   [4] HQ (interpolated table), [5] smooth (per-sample parameter glide), [6] CENTER (DC removal),
//   [7] unipolar curve. The saved state's own values are copied into the parameters at import.
// While LYNX exports (offlineRendering()), the WaveShaper behaves as FL's does when it renders: the interpolated
// table and, when oversampling is on, order 4 (×16).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Effects.h"

namespace lynx::flshape
{
// ------------------------------------------------------------------------------------------------- Soft Clipper
constexpr int softClipperParams = 2;
struct SoftClipperState
{
    int threshold = 100, post = 128; // FL's raw controls (defaults of a new instance)
};
// record 0xD5: two i32 (threshold, post)
std::optional<SoftClipperState> parseSoftClipper (const std::vector<std::uint8_t>& state);
void softClipperToParams (const SoftClipperState& s, float* params) noexcept;
SoftClipperState softClipperFromParams (const float* params) noexcept;
// the engine on one channel, in place (it has no memory)
void softClip (float* x, int n, int threshold, int post) noexcept;
const char* softClipperParamName (int param) noexcept;
std::string softClipperParamText (int param, float value01);
std::unique_ptr<Effect> makeSoftClipper();

// ------------------------------------------------------------------------------------------------------ WaveShaper
constexpr int waveShaperParams = 8;
struct WsPoint
{
    float dt = 0.0f, y = 0.0f, tension = 0.0f; // Δt from the previous point (the map spans 1), level 0…1
    std::uint8_t mode = 0;                     // segment shape (0 single curve … 12), REA_rodada51 §2.4
};
struct WaveShaperState
{
    int version = 4;
    int pre = 128, mix = 128, post = 128, oversampling = 0;
    bool unipolar = false, center = false, hq = false, smooth = false;
    std::vector<WsPoint> points; // after FL's load-time normalisation (first point at 0, total time 1)
};
std::optional<WaveShaperState> parseWaveShaper (const std::vector<std::uint8_t>& state);
void waveShaperToParams (const WaveShaperState& s, float* params) noexcept;
const char* waveShaperParamName (int param) noexcept;
std::string waveShaperParamText (int param, float value01);

// The shaping table: entries −1 … N+1 (stored from index 0), N = 16383 (bipolar) or 32767 (unipolar).
std::vector<std::int16_t> shaperTable (const std::vector<WsPoint>& points, bool unipolar);

// --- the curve editor (round 165: FL's point editor, edited in the plugin window) ---
// The slot's state read for editing: an empty or unreadable state gives FL's default instance (the default curve,
// controls from `params` when given).
WaveShaperState waveShaperForEditing (const std::vector<std::uint8_t>& state, const float* params = nullptr);
// FL's state layout 4 with these points (Δt normalised to a total of 1); the map's kind, its 20-byte header and the
// editor flag are kept from `base` when it is a WaveShaper state, FL's defaults otherwise.
std::vector<std::uint8_t> writeWaveShaper (const WaveShaperState& s, const std::vector<std::uint8_t>& base = {});
// The points as positions on the map: x = time 0…1 (cumulative Δt), y = level 0…1
std::vector<float> waveShaperTimes (const std::vector<WsPoint>& points);
// Edits on the point list; times are absolute (0…1). The first point stays at 0 and the last at 1 (their level
// moves); a point can't pass its neighbours. Each returns the index of the point it touched (−1 = nothing done).
int addWaveShaperPoint (std::vector<WsPoint>& points, float x, float y);
int moveWaveShaperPoint (std::vector<WsPoint>& points, int index, float x, float y);
bool removeWaveShaperPoint (std::vector<WsPoint>& points, int index);
// The level of the curve at map time x (0…1), as the engine shapes it (from the table)
float waveShaperLevelAt (const std::vector<std::int16_t>& table, bool unipolar, float x) noexcept;
// FL's segment shapes (WsPoint::mode) in the order of FL's point menu, with their names
struct WsModeName { std::uint8_t mode; const char* name; };
const std::vector<WsModeName>& waveShaperModes();

class WaveShaperEngine
{
public:
    WaveShaperEngine();
    ~WaveShaperEngine();
    // points: the state's curve (tables for both polarities are built here, off the audio thread)
    void setCurve (const std::vector<WsPoint>& points);
    // audio thread: takes these tables (built by shaperTable, bipolar and unipolar); the old ones come back in them
    void swapCurve (std::vector<std::int16_t>& bipolar, std::vector<std::int16_t>& unipolar) noexcept;
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    // FL's controls (raw values) and options; renderMode = FL's "rendering" process mode
    void setControls (int pre, int mix, int post, int oversampling, bool hq, bool smooth, bool center, bool unipolar,
                      bool renderMode) noexcept;
    // stereo, in place; any n (split into FL-sized blocks of 512 frames)
    void process (float* left, float* right, int n) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// the mixer effect; flState = FL's saved state (an empty or unreadable state gives FL's default curve)
std::unique_ptr<Effect> makeWaveShaper (const std::vector<std::uint8_t>& flState);
} // namespace lynx::flshape
