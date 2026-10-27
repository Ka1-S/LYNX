#pragma once

// FL's Fruity X-Y Controller and X-Y-Z Controller (docs/analise/REA_rodada164a.md §3; REA_rodada15 §11.1, REA_rodada17
// §4): mixer "controller" effects that make no change to the sound. A target follows the goal the user drags on a pad
// (speed and acceleration smooth its movement); each axis is published as a control output — X-Y: 0 = X, 1 = Y;
// X-Y-Z: 0 = X, 1 = Y, 2 = Z — that any parameter can be linked to ("Link to internal controller", Patcher cables).
// Clean-room: our own code from FL's documented and decompiled behaviour.
//
// Parameters (EffectSlot::params, 0..1), per axis a (X 0, Y 1, Z 2): 4a + 0 level (the goal), + 1 min, + 2 max,
// + 3 tension (0.5 = straight; FL's −128…128); then speed and acceleration (X-Y: 8, 9; X-Y-Z: 12, 13).

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Song.h"

namespace lynx
{
class Effect;
}

namespace lynx::xy
{
int axesOf (EffectKind kind) noexcept; // 2 or 3 (0: not an X-Y controller)
inline bool isXyController (EffectKind kind) noexcept { return axesOf (kind) > 0; }
constexpr int level (int axis) noexcept { return 4 * axis; }
constexpr int minimum (int axis) noexcept { return 4 * axis + 1; }
constexpr int maximum (int axis) noexcept { return 4 * axis + 2; }
constexpr int tension (int axis) noexcept { return 4 * axis + 3; }
inline int speed (EffectKind kind) noexcept { return 4 * axesOf (kind); }
inline int acceleration (EffectKind kind) noexcept { return 4 * axesOf (kind) + 1; }
int paramCount (EffectKind kind) noexcept;

// The default parameters (goal centred, full range, straight, FL's default speed / acceleration).
EffectParams defaults (EffectKind kind) noexcept;

// FL's tension curve (REA_rodada17 §4): t = (tension − 0.5) × 256 in −128…128, k = 0.1 (1001^(|t|/128) − 1);
// t > 0: 1 − ((k + 1)^(1 − p) − 1)/k (concave), t < 0: ((k + 1)^p − 1)/k (convex), t = 0: p.
double shape (double position, float tension01) noexcept;
// An axis' output for a position 0..1: min + shape (position) × (max − min), clipped to 0..1.
float output (const EffectParams& params, int axis, double position) noexcept;

// The target's movement towards the goal (per axis), one step per block of `seconds`.
// The laws are FL's, per tick of a 96 PPQ song at `bpm` (REA_rodada164a §3, §3b).
struct Mover
{
    std::array<double, 3> position { 0.5, 0.5, 0.5 }, velocity {};
    std::array<double, 3> stage1 { 0.5, 0.5, 0.5 }, stage2 { 0.5, 0.5, 0.5 }; // (X-Y-Z: the smoother's first stages)
    double ticks = 0.0; // (the fraction of a tick left from the last block)
    bool started = false;
    void step (const EffectParams& params, EffectKind kind, double seconds, double bpm) noexcept;
};

std::unique_ptr<Effect> makeEffect (EffectKind kind);

// FL's saved state (0xD5) → LYNX's slot (nullopt when it doesn't parse). X-Y: i32 version + 10 × i32 parameters
// (levels / min / max 0…65536, tension −128…128, speed, acceleration); X-Y-Z: version 3 (levels, speed, acceleration,
// each axis' output curve as Min / Max).
std::optional<EffectSlot> fromFl (const std::string& pluginName, const std::vector<std::uint8_t>& state);

// The text a parameter shows ("X level 50%", "Tension +32").
std::string paramText (EffectKind kind, int param, float value);
} // namespace lynx::xy
