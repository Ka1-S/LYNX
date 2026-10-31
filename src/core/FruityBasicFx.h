#pragma once

// FL Studio's Fruity PanOMatic, Fruity Balance and Fruity Blood Overdrive as LYNX mixer effects, exact engines rebuilt
// in docs/analise/REA_rodada58.md (clean-room: layouts, formulas and behaviour in our own words; no Image-Line code or
// data in this repository). The only installation-independent tables they need (FL's host sine/triangle/square LFO
// tables, the overdrive's sine shaper) are computed here from their formulas.
//
// Every parameter is FL's control over its range (value = (raw − min) / (max − min)), so the LYNX editor and FL
// automation drive the same values FL's ProcessParam receives:
//
//   PanOMatic (EffectKind::fruityPanOMatic):  [0] pan 0…256 (128 = centre), [1] volume 0…320 (256 = 0 dB),
//     [2] LFO shape 0…2 (sine, triangle, square), [3] LFO target 0…2 (off, pan, volume), [4] LFO amount 0…128,
//     [5] LFO speed 0…55000 (higher = faster; free running, in samples — tempo is never read).
//   Balance (EffectKind::fruityBalance):  [0] balance −128…128, [1] volume 0…320 (256 = 0 dB).
//   Blood Overdrive (EffectKind::fruityBloodOverdrive):  [0] pre band 0…10000, [1] colour 0…10000,
//     [2] pre amp 0…10000, [3] ×100 0…1, [4] post filter 0…10000, [5] post gain 0…10000.
//   Send (EffectKind::fruitySend, docs/analise/REA_rodada105_send.md):  [0] pan −128…128, [1] volume 0…320
//     (256 = 0 dB), [2] dry 0…256, [3] the insert it sends to 0…127 (0 = none) as track / 127.
//
// Engine start (FL's host order is not confirmed, §4 of the document): the engines are built with the current rate
// before the state, as FL's own plugins accept; PanOMatic and Balance then start flushed (gains at their targets,
// PanOMatic's LFO at phase 0), Blood Overdrive settles its parameter smoothers for one second of samples as its state
// load does. Effect::reset() = FL's flush (Blood Overdrive has none). Host blocks longer than 512 frames are processed
// as 512-frame engine calls (the gain ramps and PanOMatic's LFO run once per call).

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Effects.h"

namespace lynx::flbasic
{
constexpr int panOMaticParams = 6, balanceParams = 2, bloodOverdriveParams = 6;

// FL's raw controls; parse = the plugins' saved states (record 0xD5)
struct PanOMaticState { int pan = 128, vol = 256, shape = 0, target = 0, amount = 64, speed = 27500; };
struct BalanceState { int pan = 0, vol = 256; };
struct BloodOverdriveState { std::array<int, 6> raw { 0, 5000, 0, 0, 0, 10000 }; };
struct SendState { int pan = 0, vol = 256, dry = 256, track = 0; };
constexpr int sendParams = 4;

std::optional<PanOMaticState> parsePanOMatic (const std::vector<std::uint8_t>& state); // 6 × i32
std::optional<BalanceState> parseBalance (const std::vector<std::uint8_t>& state);     // 2 × i32
std::optional<BloodOverdriveState> parseBloodOverdrive (const std::vector<std::uint8_t>& state); // version ≤ 3, 6 × i32, …
std::optional<SendState> parseSend (const std::vector<std::uint8_t>& state);                 // 3 × i32 controls, i32 track

void panOMaticToParams (const PanOMaticState& s, float* params) noexcept;
void balanceToParams (const BalanceState& s, float* params) noexcept;
void bloodOverdriveToParams (const BloodOverdriveState& s, float* params) noexcept;
PanOMaticState panOMaticFromParams (const float* params) noexcept;
BalanceState balanceFromParams (const float* params) noexcept;
BloodOverdriveState bloodOverdriveFromParams (const float* params) noexcept;
void sendToParams (const SendState& s, float* params) noexcept;
SendState sendFromParams (const float* params) noexcept;
float sendTrackValue (int track) noexcept; // the "Send to" parameter of an insert (0 = none)
int sendTrack (float value) noexcept;      // and back
// "Send to" is normalised over 0..maxMixerTracks - 1. Files saved before round 166d (issue #14: 500 inserts) used
// 0..127 and carry no "sendSpan": their value is rescaled to the current span (the same insert).
constexpr int legacySendSpan = 127;
float rescaleSendTrackValue (float value, int savedSpan) noexcept;

std::string panOMaticParamText (int param, float value01);
std::string balanceParamText (int param, float value01);
std::string bloodOverdriveParamText (int param, float value01);
std::string sendParamText (int param, float value01);

// ------------------------------------------------------------------------------------------------------- engines
class PanOMaticEngine
{
public:
    void prepare (int sampleRate) noexcept;                // FL's sample-rate notification
    void set (const PanOMaticState& s) noexcept;           // FL's ProcessParam for every control that changed
    void flush() noexcept;                                 // FL's flush: gains snap to their targets, LFO phase 0
    void prime() noexcept;                                 // LYNX's start: one silent frame, then a flush
    void process (float* left, float* right, int n) noexcept; // one FL render call (any n; no splitting here)

private:
    void gains (float pan, float vol) noexcept;
    PanOMaticState raw {};
    bool known = false;
    float pan = 0, vol = 1, amount = 0.25f, step = 0.001f;
    const float* table = nullptr;
    std::uint32_t phase = 0, inc = 0;
    int rate = 44100;
    bool dirty = true;
    std::array<float, 4> cur {}, target {};
};

class BalanceEngine
{
public:
    void prepare (int sampleRate) noexcept;
    void set (const BalanceState& s) noexcept;
    void flush() noexcept;
    void process (float* left, float* right, int n) noexcept;

private:
    BalanceState raw {};
    bool known = false;
    float step = 0.01f;
    std::array<double, 4> target {};
    std::array<float, 4> cur {};
};

class BloodOverdriveEngine
{
public:
    // rate first, then the state's controls; the smoothers settle for `rate` samples as FL's state load does
    void start (int sampleRate, const BloodOverdriveState& s) noexcept;
    void set (const BloodOverdriveState& s) noexcept;
    void process (float* left, float* right, int n) noexcept;

private:
    void derive() noexcept;
    void smooth() noexcept;
    float channel (float* st, float x) noexcept;
    BloodOverdriveState raw {};
    std::array<float, 6> p { 0, 0.5f, 0, 0, 0, 1 };
    float s158 = 1, s15c = 1, s160 = 1, s178 = 0;
    float c14c = 0, c150 = 0, c154 = 0, c17c = 0, c164 = 0, c168 = 0;
    float L[6] {}, R[6] {}, lp[4] {};
};

// Fruity Send: the send (input × the 2×2 pan matrix × volume, ramped) is added into `sendLeft/Right`, then the
// signal continues × dry (not ramped). Nothing is sent while the insert is 0 (none).
class SendEngine
{
public:
    void prepare (int sampleRate) noexcept;
    void set (const SendState& s) noexcept;
    void flush() noexcept;
    void process (const float* left, const float* right, float* sendLeft, float* sendRight, int n) noexcept;
    float dryGain() const noexcept { return dry; }

private:
    SendState raw {};
    bool known = false, dirty = true;
    float step = 0.001f, pan = 0.0f, volume = 1.0f, dry = 1.0f;
    std::array<float, 4> target {}, cur {};
};

std::unique_ptr<Effect> makePanOMatic();
std::unique_ptr<Effect> makeSend();
std::unique_ptr<Effect> makeBalance();
std::unique_ptr<Effect> makeBloodOverdrive();
} // namespace lynx::flbasic
