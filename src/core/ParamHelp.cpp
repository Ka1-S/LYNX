#include "core/ParamHelp.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <utility>

#include "core/Effects.h"
#include "core/EmberClassic.h"
#include "core/MixSuite.h"

#ifndef TR_NOOP
 #define TR_NOOP(text) text // marks a literal for translation (tools/i18n_keys.py scans core too)
#endif

namespace lynx::paramhelp
{
namespace
{
using K = EffectKind;

// shared wording, by the folded parameter name
const std::map<std::string, const char*>& shared()
{
    static const std::map<std::string, const char*> help {
        // levels, mix
        { "mix", TR_NOOP ("Balance between the dry signal and the effect") },
        { "dry", TR_NOOP ("Level of the unprocessed signal mixed back in") },
        { "output", TR_NOOP ("Output level") },
        { "input", TR_NOOP ("Level going into the effect") },
        { "trim", TR_NOOP ("Output trim after everything else") },
        { "gain", TR_NOOP ("Gain") },
        { "level", TR_NOOP ("Level") },
        { "width", TR_NOOP ("Stereo width of the effect: 0 = mono") },
        { "phase", TR_NOOP ("Inverts the polarity") },
        { "voicing", TR_NOOP ("Which model the engine follows: LYNX's own or the one matched to a reference") },
        { "matched", TR_NOOP ("Uses the model matched to the reference instead of LYNX's own") },
        { "oversampling", TR_NOOP ("Processes at a higher rate: less aliasing, more CPU") },
        { "auto gain", TR_NOOP ("Keeps the loudness about the same while the effect is changed") },
        { "listen", TR_NOOP ("Plays only what the effect removes or reacts to") },
        { "lookahead", TR_NOOP ("Looks ahead in the signal so the effect can react before a peak (adds latency)") },
        // reverbs
        { "predelay", TR_NOOP ("Time before the reverb starts") },
        { "decay", TR_NOOP ("How long the reverb takes to die away") },
        { "size", TR_NOOP ("Size of the space; larger spaces build up more slowly") },
        { "mod rate", TR_NOOP ("Speed of the modulation that keeps the tail moving") },
        { "mod depth", TR_NOOP ("Amount of modulation: chorus-like movement in the tail") },
        { "high cut", TR_NOOP ("Removes the effect's highs above this frequency") },
        { "low cut", TR_NOOP ("Removes the effect's lows below this frequency") },
        { "bass mult", TR_NOOP ("Decay of the lows relative to the mids (above 1 = longer)") },
        { "bass xover", TR_NOOP ("Where the lows end for the bass multiplier") },
        { "high mult", TR_NOOP ("Decay of the highs relative to the mids (below 1 = shorter)") },
        { "high xover", TR_NOOP ("Where the highs start for the high multiplier") },
        { "high shelf", TR_NOOP ("How much the highs are damped inside the reverb") },
        { "high freq", TR_NOOP ("Where the damping of the highs starts") },
        { "early diffusion", TR_NOOP ("Density of the first reflections: low = audible echoes, high = smooth") },
        { "late diffusion", TR_NOOP ("Density of the tail: low = grainy, high = smooth") },
        { "diffusion", TR_NOOP ("Smears the repeats into a wash") },
        { "diffusion size", TR_NOOP ("Length of the smearing") },
        { "color", TR_NOOP ("Character of the era the algorithm imitates") },
        { "colour", TR_NOOP ("Bright or dark tone") },
        { "mode", TR_NOOP ("The algorithm: each mode is a different space or effect") },
        { "type", TR_NOOP ("The kind of room or plate") },
        { "reverb mode", TR_NOOP ("The kind of reverb") },
        { "early/late", TR_NOOP ("Balance between the early reflections and the tail") },
        { "cross", TR_NOOP ("How much the left and right sides feed each other") },
        { "send", TR_NOOP ("How much of the early reflections feed the tail") },
        { "space", TR_NOOP ("Feeds the early reflections back for a bigger, longer space") },
        { "low eq freq", TR_NOOP ("Frequency of the low shelf") },
        { "low eq gain", TR_NOOP ("Boost or cut of the lows") },
        { "high eq freq", TR_NOOP ("Frequency of the high shelf") },
        { "high eq gain", TR_NOOP ("Boost or cut of the highs") },
        { "feedback", TR_NOOP ("How much of the output is fed back: more repeats, a longer tail") },
        { "density", TR_NOOP ("From discrete echoes (low) to a dense reverb (high)") },
        { "warp", TR_NOOP ("Bends the delay lines against each other for an evolving tail") },
        { "clear", TR_NOOP ("Empties the delay network at once") },
        // delays
        { "delay", TR_NOOP ("Delay time") },
        { "time", TR_NOOP ("Delay time in milliseconds") },
        { "note", TR_NOOP ("Delay time as a note value at the project tempo") },
        { "sync", TR_NOOP ("Delay time in milliseconds or as a note value (straight, dotted, triplet)") },
        { "shift", TR_NOOP ("Pitch or frequency shift of every repeat") },
        { "shift mode", TR_NOOP ("Which voices the shifter adds (up, up and down, none)") },
        { "shift detune", TR_NOOP ("Small detune added to the shifted repeats") },
        { "pitch", TR_NOOP ("Pitch shift of the repeats in semitones") },
        { "pitch detune", TR_NOOP ("Fine detune of the pitch-shifted repeats in cents") },
        { "style", TR_NOOP ("The effect's character") },
        { "spread", TR_NOOP ("Offsets the two sides in time for width") },
        { "spacing", TR_NOOP ("Spacing of the four taps") },
        { "drive", TR_NOOP ("Saturation of the repeats") },
        { "age", TR_NOOP ("Wear of the machine the mode imitates (noise, darkening)") },
        { "wow", TR_NOOP ("Slow pitch wobble of a tape transport") },
        { "flutter", TR_NOOP ("Fast pitch wobble of a tape transport") },
        { "ducking", TR_NOOP ("Turns the echoes down while you play") },
        { "pairs", TR_NOOP ("Ping-pong in pairs: each left echo is repeated right at full level") },
        { "hybrid", TR_NOOP ("The hybrid echo loop of the delay this engine stands in for") },
        // modulation
        { "rate", TR_NOOP ("Speed of the modulation") },
        { "depth", TR_NOOP ("Amount of modulation") },
        { "manual", TR_NOOP ("Offset of the modulated delay") },
        { "taps", TR_NOOP ("How many delay taps a side") },
        { "stereo", TR_NOOP ("Mono or stereo repeats (stereo: the right side shifts the other way)") },
        { "slope", TR_NOOP ("How the taps' levels fall from the first to the last") },
        { "skew", TR_NOOP ("Moves the taps towards the start or the end of the delay") },
        { "random", TR_NOOP ("Random variation of the tap times") },
        { "tap gain", TR_NOOP ("Level of the taps against each other") },
        { "diffusion rate", TR_NOOP ("Speed of the modulation inside the diffuser") },
        { "diffusion depth", TR_NOOP ("Amount of modulation inside the diffuser") },
        { "diffusion on", TR_NOOP ("Switches the diffuser on") },
        { "crossover", TR_NOOP ("Frequency below which the signal stays unmodulated") },
        { "detune rate", TR_NOOP ("Speed of the slow detune") },
        { "detune depth", TR_NOOP ("Amount of the slow detune") },
        { "vibrato rate", TR_NOOP ("Speed of the vibrato") },
        { "vibrato depth", TR_NOOP ("Amount of the vibrato") },
        { "over-mod", TR_NOOP ("Pushes the modulation past its normal range (can sound unstable)") },
        { "drive in", TR_NOOP ("Saturation before the delay lines") },
        { "drive out", TR_NOOP ("Saturation after the delay lines") },
        { "drive on", TR_NOOP ("Switches the saturation on") },
        { "drive post", TR_NOOP ("Saturates after the delay lines instead of before") },
        { "noise", TR_NOOP ("Adds the noise of an old analog unit") },
        { "rotate", TR_NOOP ("Rotates the taps around the stereo field") },
        { "smoothing", TR_NOOP ("Rounds off the modulation's corners") },
        { "half speed", TR_NOOP ("Halves every modulation rate") },
        // dynamics
        { "threshold", TR_NOOP ("Level above which the effect starts working") },
        { "ratio", TR_NOOP ("How strongly the level beyond the threshold is changed") },
        { "attack", TR_NOOP ("How fast the effect reacts once the signal crosses the threshold") },
        { "range", TR_NOOP ("The most the effect may turn the signal down") },
        { "knee", TR_NOOP ("How gradually the effect starts around the threshold: 0 = at once") },
        { "release", TR_NOOP ("How fast the effect lets go once the signal falls") },
        { "hold", TR_NOOP ("Time the effect stays engaged before it starts to release") },
        { "auto release", TR_NOOP ("Release that follows the material: fast on peaks, slow on long notes") },
        { "makeup", TR_NOOP ("Gain after the compression") },
        { "external sc", TR_NOOP ("Uses the side-chain input instead of the signal itself as the trigger") },
        { "sc listen", TR_NOOP ("Plays the side-chain signal (after its filters) so you hear what triggers the effect") },
        { "stereo link", TR_NOOP ("How much both sides share one detector: 100 % = the image stays put") },
        { "expand", TR_NOOP ("Expands instead of compressing: quiet parts get quieter") },
        { "character", TR_NOOP ("Adds the colour of analog circuits") },
        { "solo", TR_NOOP ("Plays only this band") },
        { "on", TR_NOOP ("Switches this band on or off") },
        { "bypass", TR_NOOP ("Bypasses this band; its settings are kept") },
        { "state", TR_NOOP ("Normal, solo or mute for this band") },
        { "crossover 1", TR_NOOP ("Split frequency between band 1 and band 2") },
        { "crossover 2", TR_NOOP ("Split frequency between band 2 and band 3") },
        { "crossover 3", TR_NOOP ("Split frequency between band 3 and band 4") },
        { "crossover slope", TR_NOOP ("Steepness of the band splits") },
        { "bands", TR_NOOP ("How many frequency bands are processed separately") },
        // EQ
        { "freq", TR_NOOP ("Centre or corner frequency of the band") },
        { "q", TR_NOOP ("Bandwidth: high values affect a narrow range") },
        { "shape", TR_NOOP ("Filter shape of the band (bell, shelf, cut, notch...)") },
        { "placement", TR_NOOP ("Where the band works: both sides, left, right, mid or side") },
        { "pan", TR_NOOP ("Stereo position") },
        // saturation
        { "tone", TR_NOOP ("Tilts the tone towards dark or bright") },
        { "bass", TR_NOOP ("Boost or cut of the band's lows after the saturation") },
        { "mid", TR_NOOP ("Boost or cut of the band's mids after the saturation") },
        { "treble", TR_NOOP ("Boost or cut of the band's highs after the saturation") },
        { "presence", TR_NOOP ("Boost or cut of the band's presence range after the saturation") },
        { "fb freq", TR_NOOP ("Centre of the feedback band") },
        { "dynamics", TR_NOOP ("Compresses (left) or expands (right) the band after the drive") },
    };
    return help;
}

// an engine's own wording where the shared one would be wrong or too vague
const std::map<std::pair<K, std::string>, const char*>& own()
{
    static const std::map<std::pair<K, std::string>, const char*> help {
        // Epoch Verb
        { { K::epochVerb, "mode" }, TR_NOOP ("The reverb algorithm: halls, rooms, plates, chambers and unreal spaces") },
        { { K::epochVerb, "color" }, TR_NOOP ("The era of the algorithm: 1970s = dark and grainy, 1980s = brighter, Now = full range") },
        { { K::epochVerb, "attack" }, TR_NOOP ("How the reverb builds up: low = immediate, high = a slow swell") },
        { { K::epochVerb, "size" }, TR_NOOP ("Size of the space; also sets how dense the tail gets") },
        // Atrium
        { { K::atrium, "type" }, TR_NOOP ("The kind of room, chamber or space") },
        { { K::atrium, "early/late" }, TR_NOOP ("Depth: balance between the early reflections (close) and the tail (far)") },
        { { K::atrium, "late size" }, TR_NOOP ("Size of the tail's space") },
        { { K::atrium, "early size" }, TR_NOOP ("Length of the early reflections") },
        { { K::atrium, "diffusion" }, TR_NOOP ("Density of the reflections: low = grainy, high = smooth") },
        // Foil Plate
        { { K::foilPlate, "type" }, TR_NOOP ("The plate's metal: each one has its own brightness, density and decay") },
        { { K::foilPlate, "size" }, TR_NOOP ("Size of the plate: larger plates sound deeper") },
        { { K::foilPlate, "width" }, TR_NOOP ("Stereo width of the plate: 0 = mono, above 100 % = wider than the speakers") },
        { { K::foilPlate, "view" }, TR_NOOP ("Full: every control of the plate; Plate: the one-knob panel of the Little Plate role (Low Cut, Decay, Mod, Mix)") },
        { { K::foilPlate, "infinite" }, TR_NOOP ("Holds the plate's tail for ever (the decay at its top): play into it to layer, switch off to let it fade") },
        // Nebula
        { { K::nebula, "mode" }, TR_NOOP ("The delay network: from clean echoes to huge, slowly building reverbs") },
        { { K::nebula, "delay" }, TR_NOOP ("Base time of the delay network; long times give echoes, short ones a reverb") },
        { { K::nebula, "feedback" }, TR_NOOP ("How long the network keeps sounding") },
        { { K::nebula, "density" }, TR_NOOP ("From separate echoes (low) to a smooth wash (high)") },
        { { K::nebula, "width" }, TR_NOOP ("Stereo width: negative values swap the sides") },
        // Halo
        { { K::halo, "shift" }, TR_NOOP ("Pitch shift inside the reverb's feedback: each pass climbs (shimmer)") },
        { { K::halo, "feedback" }, TR_NOOP ("How much of the shifted tail is fed back: more shimmer, a longer tail") },
        { { K::halo, "diffusion" }, TR_NOOP ("Density of the reverb: low = grainy echoes, high = smooth") },
        { { K::halo, "reverb mode" }, TR_NOOP ("Mono or stereo reverb, and how big the stereo space is") },
        { { K::halo, "shift mode" }, TR_NOOP ("Which shifted voices feed the tail (up, up and down, bright, none)") },
        { { K::halo, "color" }, TR_NOOP ("Bright or dark tail") },
        // Horizon
        { { K::horizon, "echo mode" }, TR_NOOP ("The echo's character (clean, tape, analog, reverse...); only its own controls are shown") },
        { { K::horizon, "reverb mode" }, TR_NOOP ("The reverb's space after (or before) the echo") },
        { { K::horizon, "attack" }, TR_NOOP ("How the reverb builds up: low = immediate, high = a slow swell") },
        { { K::horizon, "routing" }, TR_NOOP ("Order of the two effects: echo into reverb, or reverb into echo") },
        { { K::horizon, "echo level" }, TR_NOOP ("Level of the echo; 0 = reverb only") },
        { { K::horizon, "reverb level" }, TR_NOOP ("Level of the reverb") },
        { { K::horizon, "echo detune" }, TR_NOOP ("Detune of the echo's repeats in cents") },
        { { K::horizon, "color" }, TR_NOOP ("Overall tone: bright, neutral, dark or vintage") },
        // Spiral Echo
        { { K::spiralEcho, "shift" }, TR_NOOP ("Frequency shift of every repeat: the echoes spiral up or down") },
        { { K::spiralEcho, "delay" }, TR_NOOP ("Time between the repeats") },
        { { K::spiralEcho, "stereo" }, TR_NOOP ("Mono or stereo repeats (stereo: the right side shifts the other way)") },
        // Orbit
        { { K::orbit, "mode" }, TR_NOOP ("The modulation effect: flangers, chorus, doubler, ensemble") },
        { { K::orbit, "manual" }, TR_NOOP ("Base delay of the flanger or chorus the modulation sweeps around") },
        { { K::orbit, "feedback" }, TR_NOOP ("Resonance of the modulated delay: higher = more metallic") },
        // Lattice Mod
        { { K::latticeMod, "delay" }, TR_NOOP ("Delay of the modulated taps") },
        { { K::latticeMod, "depth" }, TR_NOOP ("Amount of the taps' modulation") },
        { { K::latticeMod, "width" }, TR_NOOP ("Stereo spread of the taps") },
        { { K::latticeMod, "spread" }, TR_NOOP ("How far the taps are spread in time") },
        { { K::latticeMod, "colour" }, TR_NOOP ("Tone of the modulated signal") },
        { { K::latticeMod, "sync" }, TR_NOOP ("Locks the modulation to the project tempo") },
        { { K::latticeMod, "delay sync" }, TR_NOOP ("Sets the delay to a note value at the project tempo: straight, dotted or triplet (up to 2.75 s)") },
        { { K::latticeMod, "input pan" }, TR_NOOP ("What enters the taps: stereo, swapped sides, or the mono sum to both, the left or the right") },
        // Echoform
        { { K::echoform, "mode" }, TR_NOOP ("The machine the echo imitates (tape, bucket brigade, digital, lo-fi, pitch...)") },
        { { K::echoform, "style" }, TR_NOOP ("Echo layout: single, ratio, four taps, dual or ping-pong") },
        { { K::echoform, "ratio" }, TR_NOOP ("Time of the second echo as a share of the first") },
        { { K::echoform, "output" }, TR_NOOP ("Level of the echoes") },
        { { K::echoform, "rhythm" }, TR_NOOP ("Rhythm echo: a pattern of evenly spaced taps that the feedback repeats every Length") },
        { { K::echoform, "repeats" }, TR_NOOP ("Slots in the rhythm pattern (the first one is the input itself)") },
        { { K::echoform, "decay" }, TR_NOOP ("How much quieter the last tap of the pattern is than the first") },
        { { K::echoform, "length" }, TR_NOOP ("Length of the rhythm pattern in beats; the feedback replays it") },
        { { K::echoform, "rhythm edit" }, TR_NOOP ("Where the rhythm taps sit: evenly spaced, each at its own time (Free) or snapped to the grid") },
        { { K::echoform, "rhythm grid" }, TR_NOOP ("The note value the taps snap to in the Grid edit") },
        { { K::echoform, "input level" }, TR_NOOP ("Level into the echoes only (the dry signal is not touched): hotter drives the loop harder") },
        { { K::echoform, "fb balance" }, TR_NOOP ("Feedback of the left side against the right side") },
        { { K::echoform, "diffusion post" }, TR_NOOP ("Diffusion inside the loop (each repeat smears more) or only on the echoes' way out") },
        { { K::echoform, "mod shape" }, TR_NOOP ("Wave of the modulation: sine, triangle, square, saw or random") },
        { { K::echoform, "mod sync" }, TR_NOOP ("Locks the modulation rate to a note value of the tempo") },
        { { K::echoform, "loop bass" }, TR_NOOP ("Low shelf inside the loop: every repeat gets darker or fuller") },
        { { K::echoform, "loop mid" }, TR_NOOP ("Mid bell inside the loop: every repeat gets more or less of the middle") },
        { { K::echoform, "loop treble" }, TR_NOOP ("High shelf inside the loop: every repeat gets brighter or duller") },
        { { K::echoform, "rhythm shape" }, TR_NOOP ("How Decay spreads over the taps: falling, rising, a swell, a fade or a late drop") },
        // Ridge Strip
        { { K::ridgeStrip, "input" }, TR_NOOP ("Level into the channel strip") },
        { { K::ridgeStrip, "hpf" }, TR_NOOP ("High-pass filter: removes rumble below this frequency") },
        { { K::ridgeStrip, "lpf" }, TR_NOOP ("Low-pass filter: removes hiss above this frequency") },
        { { K::ridgeStrip, "lf gain" }, TR_NOOP ("Boost or cut of the low band") },
        { { K::ridgeStrip, "lf freq" }, TR_NOOP ("Frequency of the low band") },
        { { K::ridgeStrip, "lf bell" }, TR_NOOP ("Low band as a bell instead of a shelf") },
        { { K::ridgeStrip, "lmf gain" }, TR_NOOP ("Boost or cut of the low-mid band") },
        { { K::ridgeStrip, "lmf freq" }, TR_NOOP ("Frequency of the low-mid band") },
        { { K::ridgeStrip, "lmf q" }, TR_NOOP ("Bandwidth of the low-mid band") },
        { { K::ridgeStrip, "hmf gain" }, TR_NOOP ("Boost or cut of the high-mid band") },
        { { K::ridgeStrip, "hmf freq" }, TR_NOOP ("Frequency of the high-mid band") },
        { { K::ridgeStrip, "hmf q" }, TR_NOOP ("Bandwidth of the high-mid band") },
        { { K::ridgeStrip, "hf gain" }, TR_NOOP ("Boost or cut of the high band") },
        { { K::ridgeStrip, "hf freq" }, TR_NOOP ("Frequency of the high band") },
        { { K::ridgeStrip, "hf bell" }, TR_NOOP ("High band as a bell instead of a shelf") },
        { { K::ridgeStrip, "eq curve" }, TR_NOOP ("The EQ's character: the brown (gentle) or the black (steeper, with cut) console curves") },
        { { K::ridgeStrip, "eq on" }, TR_NOOP ("Switches the EQ section on") },
        { { K::ridgeStrip, "ratio" }, TR_NOOP ("Compression ratio; at the minimum the compressor is off") },
        { { K::ridgeStrip, "comp threshold" }, TR_NOOP ("Level above which the compressor works") },
        { { K::ridgeStrip, "comp release" }, TR_NOOP ("How fast the compressor lets go") },
        { { K::ridgeStrip, "fast attack" }, TR_NOOP ("A fast attack instead of the automatic one") },
        { { K::ridgeStrip, "gate range" }, TR_NOOP ("How far the gate or expander turns the signal down") },
        { { K::ridgeStrip, "gate threshold" }, TR_NOOP ("Level below which the gate closes") },
        { { K::ridgeStrip, "gate release" }, TR_NOOP ("How fast the gate closes") },
        { { K::ridgeStrip, "gate fast" }, TR_NOOP ("A fast gate attack") },
        { { K::ridgeStrip, "expander" }, TR_NOOP ("Gentle expander instead of a gate") },
        { { K::ridgeStrip, "dynamics on" }, TR_NOOP ("Switches the dynamics section on") },
        { { K::ridgeStrip, "order" }, TR_NOOP ("Dynamics before or after the EQ") },
        { { K::ridgeStrip, "filters to sc" }, TR_NOOP ("The filters shape only the dynamics' side chain") },
        { { K::ridgeStrip, "comp mix" }, TR_NOOP ("Parallel compression: blends the uncompressed signal back in") },
        { { K::ridgeStrip, "eq to sc" }, TR_NOOP ("The filters and the EQ shape only what the dynamics hear (de-essing, frequency-conscious compression); the audio keeps neither") },
        { { K::ridgeStrip, "filter split" }, TR_NOOP ("The filters go before the dynamics, even when the EQ is after them") },
        // round 145: the Vocal strip voicing
        { { K::ridgeStrip, "vocal strip" }, TR_NOOP ("The vocal channel strip: de-esser, de-ploser, three-band EQ and compander in any order") },
        { { K::ridgeStrip, "process order" }, TR_NOOP ("The order of the four vocal modules") },
        { { K::ridgeStrip, "de-esser" }, TR_NOOP ("Turns down sibilance: the highs when they get loud compared with the rest of the voice") },
        { { K::ridgeStrip, "de-ess threshold" }, TR_NOOP ("How strong the highs must be, compared with the whole voice, before the de-esser works") },
        { { K::ridgeStrip, "de-ess freq" }, TR_NOOP ("The frequency above which the de-esser listens") },
        { { K::ridgeStrip, "de-ess split" }, TR_NOOP ("Split: only the highs are turned down; broadband: the whole voice") },
        { { K::ridgeStrip, "de-ess lookahead" }, TR_NOOP ("Delays the audio 1 ms so the de-esser acts before the sibilant starts") },
        { { K::ridgeStrip, "de-ess listen" }, TR_NOOP ("Plays only the band the de-esser acts on") },
        { { K::ridgeStrip, "de-ploser" }, TR_NOOP ("Turns down plosives: the lows when they get loud compared with the rest of the voice") },
        { { K::ridgeStrip, "de-plos threshold" }, TR_NOOP ("How strong the lows must be, compared with the whole voice, before the de-ploser works") },
        { { K::ridgeStrip, "de-plos freq" }, TR_NOOP ("The frequency below which the de-ploser listens") },
        { { K::ridgeStrip, "de-plos split" }, TR_NOOP ("Split: only the lows are turned down; broadband: the whole voice") },
        { { K::ridgeStrip, "de-plos lookahead" }, TR_NOOP ("Delays the audio 10 ms so the de-ploser acts before the pop starts") },
        { { K::ridgeStrip, "de-plos listen" }, TR_NOOP ("Plays only the band the de-ploser acts on") },
        { { K::ridgeStrip, "vocal eq" }, TR_NOOP ("Switches the vocal EQ on") },
        { { K::ridgeStrip, "vocal hpf" }, TR_NOOP ("High-pass filter with a small lift above its frequency") },
        { { K::ridgeStrip, "notch" }, TR_NOOP ("A narrow cut (or boost) for one resonance") },
        { { K::ridgeStrip, "high band" }, TR_NOOP ("A wide boost or cut for presence and air") },
        { { K::ridgeStrip, "compander" }, TR_NOOP ("Expander, then compressor, make-up and drive") },
        { { K::ridgeStrip, "expander threshold" }, TR_NOOP ("Below this level the expander turns the signal down (fully left = off)") },
        { { K::ridgeStrip, "compander threshold" }, TR_NOOP ("Level above which the compressor works") },
        { { K::ridgeStrip, "make-up" }, TR_NOOP ("Gain after the compressor") },
        { { K::ridgeStrip, "soft knee" }, TR_NOOP ("A gradual start of the compression around the threshold") },
        { { K::ridgeStrip, "drive" }, TR_NOOP ("Soft clipping after the make-up gain: warmer, louder; stronger with more make-up") },
        // Glow Leveler
        { { K::glowLeveler, "peak reduction" }, TR_NOOP ("How much the leveler turns peaks down: the main control") },
        { { K::glowLeveler, "gain" }, TR_NOOP ("Gain after the leveling") },
        { { K::glowLeveler, "limit" }, TR_NOOP ("Limit instead of compress: a harder ratio") },
        { { K::glowLeveler, "emphasis" }, TR_NOOP ("Makes the detector more sensitive to highs (less reduction on bass)") },
        { { K::glowLeveler, "auto makeup" }, TR_NOOP ("Turns the gain up as the peak reduction rises") },
        { { K::glowLeveler, "analog" }, TR_NOOP ("The reference's analog hum and noise (50 or 60 Hz mains), added after the gain") },
        { { K::glowLeveler, "opto law" }, TR_NOOP ("Measured: the reference's Limit and Auto makeup; old: the law of projects saved before round 152") },
        { { K::glowLeveler, "opto release" }, TR_NOOP ("Measured: the reference's attack and gain recovery; old: the timing of projects saved before round 165") },
        { { K::glowLeveler, "makeup ref" }, TR_NOOP ("The peak reduction where Auto makeup was switched on: the make-up follows the dial from there") },
        // Hush De-esser
        { { K::hushDeEsser, "frequency" }, TR_NOOP ("Where the sibilance is detected and reduced") },
        { { K::hushDeEsser, "input pan" }, TR_NOOP ("Balance of the input before the de-esser hears it") },
        { { K::hushDeEsser, "output pan" }, TR_NOOP ("Balance of the output") },
        { { K::hushDeEsser, "range law" }, TR_NOOP ("Extended: levels from silence to +36 dB, the band up to 20 kHz and the reference's stereo link") },
        { { K::hushDeEsser, "link only" }, TR_NOOP ("Stereo link past 100 %: the share processed on the mid (or side) signal only") },
        { { K::hushDeEsser, "link only side" }, TR_NOOP ("Which signal the processing past 100 % link acts on alone: the mid or the side") },
        { { K::hushDeEsser, "threshold" }, TR_NOOP ("Level of sibilance above which the de-esser works") },
        { { K::hushDeEsser, "range" }, TR_NOOP ("The most the sibilance is turned down") },
        { { K::hushDeEsser, "wideband" }, TR_NOOP ("Turns the whole signal down instead of only the sibilant band") },
        { { K::hushDeEsser, "band detector" }, TR_NOOP ("Detects in a band around the frequency instead of everything above it") },
        { { K::hushDeEsser, "listen" }, TR_NOOP ("Plays only the detected sibilance") },
        { { K::hushDeEsser, "allround" }, TR_NOOP ("For any material, not only a single voice") },
        { { K::hushDeEsser, "low-pass freq" }, TR_NOOP ("Upper limit of the detection band") },
        { { K::hushDeEsser, "lookahead time" }, TR_NOOP ("How far ahead the de-esser looks") },
        { { K::hushDeEsser, "link mid" }, TR_NOOP ("Detects on the middle of the stereo image") },
        { { K::hushDeEsser, "display" }, TR_NOOP ("What the display shows") },
        // Aether
        { { K::aether, "air" }, TR_NOOP ("Amount of air: a soft lift of the highest frequencies") },
        { { K::aether, "air freq" }, TR_NOOP ("Where the air starts") },
        { { K::aether, "harmonics" }, TR_NOOP ("Adds generated harmonics above the exciter frequency") },
        { { K::aether, "exciter freq" }, TR_NOOP ("Above this frequency the harmonics are generated") },
        { { K::aether, "character" }, TR_NOOP ("Smooth or crisp air") },
        { { K::aether, "dynamic" }, TR_NOOP ("The air follows the signal: less on loud, harsh moments") },
        { { K::aether, "mid air" }, TR_NOOP ("Air on the upper mids") },
        { { K::aether, "high air" }, TR_NOOP ("Air on the very top") },
        // Ember Saturator
        { { K::ember, "drive" }, TR_NOOP ("How hard the signal hits the saturation") },
        { { K::ember, "style" }, TR_NOOP ("The saturation's character: tube, tape, transistor or fuzz") },
        { { K::ember, "tone" }, TR_NOOP ("Tilts the saturated tone towards dark or bright") },
        { { K::ember, "low cut" }, TR_NOOP ("Removes lows before the saturation") },
        { { K::ember, "high cut" }, TR_NOOP ("Removes highs after the saturation") },
        { { K::ember, "auto gain" }, TR_NOOP ("Keeps the level about the same while the drive changes") },
        { { K::ember, "feedback" }, TR_NOOP ("Feeds a band of the saturated signal back into the saturation: resonant, singing distortion") },
        { { K::ember, "level" }, TR_NOOP ("Level of the band after the saturation") },
        // Crest Compressor
        { { K::crestCompressor, "style" }, TR_NOOP ("The compressor's character: how it reacts to peaks and how it recovers") },
        { { K::crestCompressor, "ratio" }, TR_NOOP ("How strongly the level above the threshold is reduced") },
        { { K::crestCompressor, "max lookahead" }, TR_NOOP ("Allows the longest lookahead (more latency)") },
        { { K::crestCompressor, "character pre" }, TR_NOOP ("The analog colour goes before the compression") },
        { { K::crestCompressor, "drive" }, TR_NOOP ("Amount of the analog colour") },
        { { K::crestCompressor, "sc level" }, TR_NOOP ("Level of the side-chain signal into the detector") },
        { { K::crestCompressor, "slope" }, TR_NOOP ("Steepness of the side-chain filter") },
        { { K::crestCompressor, "auto threshold" }, TR_NOOP ("The threshold follows the input level") },
        { { K::crestCompressor, "link mode" }, TR_NOOP ("How the two sides are linked: stereo or mid / side") },
        { { K::crestCompressor, "meter scale" }, TR_NOOP ("Range of the level display") },
        { { K::crestCompressor, "input meter" }, TR_NOOP ("Shows the input level in the display") },
        // Prism Multiband
        { { K::prismMultiband, "level" }, TR_NOOP ("Output level of this band") },
        { { K::prismMultiband, "slope" }, TR_NOOP ("Steepness of the band splits") },
        { { K::prismMultiband, "pan mode" }, TR_NOOP ("What each band's pan balances: left / right, or mid / side") },
        { { K::prismMultiband, "processing" }, TR_NOOP ("Dynamic phase: the crossovers' phase only where a band works; linear phase: no phase change (more latency); minimum phase: the classic crossover sum") },
        { { K::prismMultiband, "band layout" }, TR_NOOP ("Split: the bands sit side by side on crossovers; free: each band has its own edges and slopes, the room between bands passes untouched") },
        { { K::prismMultiband, "low" }, TR_NOOP ("Free layout: where this band starts") },
        { { K::prismMultiband, "high" }, TR_NOOP ("Free layout: where this band ends") },
        { { K::prismMultiband, "low slope" }, TR_NOOP ("Free layout: the steepness of the band's lower edge (6 to 48 dB/oct)") },
        { { K::prismMultiband, "high slope" }, TR_NOOP ("Free layout: the steepness of the band's upper edge (6 to 48 dB/oct)") },
        { { K::prismMultiband, "sc input" }, TR_NOOP ("What this band's detector hears: the plug-in's input or the external side chain (global: the External SC switch)") },
        { { K::prismMultiband, "revision" }, TR_NOOP ("Round 152: the reference's attack in the gain stage, its narrow Dynamic-phase bands and the 0.8-octave minimum band; round 150: projects saved before") },
        { { K::prismMultiband, "detector" }, TR_NOOP ("Measured: the reference's attack, release and soft range (a range-limited band recovers at once); classic: the detector of projects saved before round 150") },
        // Sentry Gate
        { { K::sentryGate, "style" }, TR_NOOP ("Gate, expander or ducker, and how it opens") },
        { { K::sentryGate, "threshold" }, TR_NOOP ("Level below which the gate closes") },
        { { K::sentryGate, "range" }, TR_NOOP ("How far the gate turns the signal down when closed") },
        { { K::sentryGate, "lookahead on" }, TR_NOOP ("Opens the gate just before a hit (adds latency)") },
        { { K::sentryGate, "high pass" }, TR_NOOP ("Ignores lows in the trigger signal") },
        { { K::sentryGate, "low pass" }, TR_NOOP ("Ignores highs in the trigger signal") },
        { { K::sentryGate, "up threshold" }, TR_NOOP ("Level above which the upward expansion starts") },
        { { K::sentryGate, "up ratio" }, TR_NOOP ("Amount of the upward expansion") },
        { { K::sentryGate, "channel mode" }, TR_NOOP ("Stereo, or mid / side gating") },
        { { K::sentryGate, "sc gain" }, TR_NOOP ("Gain of the trigger signal") },
        { { K::sentryGate, "sc mix" }, TR_NOOP ("Blend of the signal itself and the side chain as the trigger") },
        { { K::sentryGate, "wet pan" }, TR_NOOP ("Stereo position of the gated signal") },
        { { K::sentryGate, "dry pan" }, TR_NOOP ("Stereo position of the dry signal") },
        { { K::sentryGate, "ducking" }, TR_NOOP ("Turns the signal down while the trigger plays (a ducker)") },
        // Quell Denoiser
        { { K::quellDenoiser, "amount" }, TR_NOOP ("How much noise is removed: the only control you usually need") },
        { { K::quellDenoiser, "listen" }, TR_NOOP ("Plays only the removed noise") },
        // Gemini Doubler
        { { K::geminiDoubler, "direct" }, TR_NOOP ("Keeps the original voice in the output") },
        { { K::geminiDoubler, "direct gain" }, TR_NOOP ("Level of the original voice") },
        { { K::geminiDoubler, "direct pan" }, TR_NOOP ("Position of the original voice") },
        { { K::geminiDoubler, "align" }, TR_NOOP ("Delays the original voice to line it up with the doubles") },
        { { K::geminiDoubler, "low shelf" }, TR_NOOP ("Switches the low shelf of the doubles on") },
        { { K::geminiDoubler, "low gain" }, TR_NOOP ("Boost or cut of the doubles' lows") },
        { { K::geminiDoubler, "low freq" }, TR_NOOP ("Frequency of the low shelf") },
        { { K::geminiDoubler, "high shelf" }, TR_NOOP ("Switches the high shelf of the doubles on") },
        { { K::geminiDoubler, "high gain" }, TR_NOOP ("Boost or cut of the doubles' highs") },
        { { K::geminiDoubler, "high freq" }, TR_NOOP ("Frequency of the high shelf") },
        { { K::geminiDoubler, "feedback cut" }, TR_NOOP ("Darkens the doubles' feedback") },
        { { K::geminiDoubler, "range" }, TR_NOOP ("Maximum delay of the doubles") },
        { { K::geminiDoubler, "eq law" }, TR_NOOP ("Shelf law: measured (cuts down to -88 dB) or the older round-124 shelves") },
        { { K::geminiDoubler, "on" }, TR_NOOP ("Switches this double on") },
        { { K::geminiDoubler, "gain" }, TR_NOOP ("Level of this double") },
        { { K::geminiDoubler, "delay" }, TR_NOOP ("How late this double comes") },
        { { K::geminiDoubler, "detune" }, TR_NOOP ("Pitch offset of this double in cents") },
        { { K::geminiDoubler, "depth" }, TR_NOOP ("Amount of this double's pitch and timing drift") },
        { { K::geminiDoubler, "rate" }, TR_NOOP ("Speed of this double's drift") },
        { { K::geminiDoubler, "octave" }, TR_NOOP ("Shifts this double an octave up or down") },
        { { K::geminiDoubler, "source" }, TR_NOOP ("What this double is made from: the input or another double") },
        // Compressor (the generic stand-in)
        { { K::compressor, "ratio" }, TR_NOOP ("How strongly the level above the threshold is reduced") },
        { { K::compressor, "attack" }, TR_NOOP ("How fast the compressor reacts to a peak") },
        { { K::compressor, "makeup" }, TR_NOOP ("Gain after the compression") },
        { { K::compressor, "vintage curve" }, TR_NOOP ("Measured: the reference's static curve at every ratio; old: the curve of projects saved before round 152") },
        // Apex Limiter
        { { K::apexLimiter, "gain" }, TR_NOOP ("Pushes the signal into the limiter: louder, with more limiting") },
        { { K::apexLimiter, "ceiling" }, TR_NOOP ("The output never goes above this level") },
        { { K::apexLimiter, "lookahead" }, TR_NOOP ("How early the limiter sees a peak: longer = cleaner, more latency") },
        { { K::apexLimiter, "release" }, TR_NOOP ("How fast the limiter recovers") },
        { { K::apexLimiter, "attack" }, TR_NOOP ("How fast the limiter reacts within the lookahead") },
        { { K::apexLimiter, "style" }, TR_NOOP ("The limiting algorithm: from clean and transparent to dense and loud") },
        { { K::apexLimiter, "true peak" }, TR_NOOP ("Also limits the peaks between samples (what a converter would play)") },
        { { K::apexLimiter, "link" }, TR_NOOP ("How much the two sides are limited together") },
        { { K::apexLimiter, "link release" }, TR_NOOP ("How much the two sides recover together") },
        { { K::apexLimiter, "dither" }, TR_NOOP ("Adds dither when the output is reduced to fewer bits") },
        { { K::apexLimiter, "noise shaping" }, TR_NOOP ("Moves the dither noise to where it is least audible") },
        { { K::apexLimiter, "unity gain" }, TR_NOOP ("Plays the result turned down by the gain, to compare at the same loudness") },
        { { K::apexLimiter, "audition" }, TR_NOOP ("Plays only what the limiter removes") },
        { { K::apexLimiter, "dc filter" }, TR_NOOP ("Removes DC offset before the limiter") },
        { { K::apexLimiter, "meter scale" }, TR_NOOP ("Range of the level display") },
        // Facet EQ
        { { K::facetEq, "gain" }, TR_NOOP ("Boost or cut of the band") },
        { { K::facetEq, "slope" }, TR_NOOP ("Steepness of a cut or shelf band") },
        { { K::facetEq, "range" }, TR_NOOP ("Dynamic EQ: how far the band may move with the signal") },
        { { K::facetEq, "threshold" }, TR_NOOP ("Dynamic EQ: level at which the band starts to move") },
        { { K::facetEq, "attack" }, TR_NOOP ("Dynamic EQ: how fast the bands react") },
        { { K::facetEq, "release" }, TR_NOOP ("Dynamic EQ: how fast the bands return") },
        { { K::facetEq, "output" }, TR_NOOP ("Output level after the EQ") },
        { { K::facetEq, "gain scale" }, TR_NOOP ("Scales the gain of every band at once") },
        { { K::facetEq, "detector" }, TR_NOOP ("What the dynamic bands listen to") },
        { { K::facetEq, "analyzer" }, TR_NOOP ("What the spectrum analyser shows: before, after or both") },
        { { K::facetEq, "auto gain" }, TR_NOOP ("Compensates the loudness change of the EQ") },
        { { K::facetEq, "invert" }, TR_NOOP ("Inverts the gain of every band") },
        { { K::facetEq, "pan mode" }, TR_NOOP ("How the output pan works: balance or stereo") },
        { { K::facetEq, "analyzer range" }, TR_NOOP ("Range of the spectrum analyser") },
        { { K::facetEq, "analyzer speed" }, TR_NOOP ("How fast the spectrum analyser reacts") },
        { { K::facetEq, "analyzer tilt" }, TR_NOOP ("Tilts the analyser so that a balanced mix looks flat") },
        { { K::facetEq, "display range" }, TR_NOOP ("Range of the EQ curve display") },
        // LYNX Tune
        { { K::lynxTune, "key" }, TR_NOOP ("The song's key: the notes are corrected towards its scale") },
        { { K::lynxTune, "scale" }, TR_NOOP ("The scale whose notes the voice is pulled to") },
        { { K::lynxTune, "retune" }, TR_NOOP ("How fast the pitch moves to the target note: 0 = the robotic effect") },
        { { K::lynxTune, "humanize" }, TR_NOOP ("Slower correction on long notes so they sound natural") },
        { { K::lynxTune, "formant preserve" }, TR_NOOP ("Keeps the voice's character when the pitch moves") },
        { { K::lynxTune, "formant shift" }, TR_NOOP ("Makes the voice sound bigger or smaller without changing the pitch") },
        { { K::lynxTune, "transpose" }, TR_NOOP ("Shifts the corrected voice in semitones") },
        { { K::lynxTune, "reference" }, TR_NOOP ("Pitch of A: 440 Hz unless the song is tuned differently") },
        { { K::lynxTune, "range" }, TR_NOOP ("The voice range the pitch detector expects") },
        { { K::lynxTune, "low latency" }, TR_NOOP ("Shorter delay for live use, slightly less accurate detection") },
        { { K::lynxTune, "midi" }, TR_NOOP ("Notes played on MIDI set the target pitch") },
        { { K::lynxTune, "tracking" }, TR_NOOP ("How strict the pitch detection is: relaxed for noisy or breathy takes") },
        { { K::lynxTune, "flex-tune" }, TR_NOOP ("Corrects only notes close to a target, so slides and expression stay") },
        { { K::lynxTune, "natural vibrato" }, TR_NOOP ("Turns the singer's own vibrato up or down") },
        { { K::lynxTune, "ignore vibrato" }, TR_NOOP ("Does not chase the singer's vibrato") },
        { { K::lynxTune, "vibrato shape" }, TR_NOOP ("Shape of the added vibrato") },
        { { K::lynxTune, "vibrato rate" }, TR_NOOP ("Speed of the added vibrato") },
        { { K::lynxTune, "vibrato depth" }, TR_NOOP ("Amount of the added vibrato") },
        { { K::lynxTune, "onset delay" }, TR_NOOP ("Time into a note before the added vibrato starts") },
        { { K::lynxTune, "onset rate" }, TR_NOOP ("How fast the added vibrato fades in") },
        { { K::lynxTune, "more scales" }, TR_NOOP ("Shows the extra scales in the scale list") },
        { { K::lynxTune, "note" }, TR_NOOP ("Whether this note is a target of the correction") },
        { { K::lynxTune, "note bypass" }, TR_NOOP ("Leaves this note uncorrected") },
    };
    return help;
}

std::string fold (const char* s)
{
    std::string r (s);
    std::transform (r.begin(), r.end(), r.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    while (! r.empty() && (r.back() == ' ' || r.back() == '.'))
        r.pop_back();
    // "Comp. threshold" -> "comp threshold", "LF freq." -> "lf freq"
    std::string out;
    for (std::size_t i = 0; i < r.size(); ++i)
        if (! (r[i] == '.' && (i + 1 == r.size() || r[i + 1] == ' ')))
            out += r[i];
    return out;
}

bool startsWith (const std::string& s, const char* prefix) { return s.rfind (prefix, 0) == 0; }

const char* lookup (K kind, const std::string& key)
{
    if (const auto it = own().find ({ kind, key }); it != own().end())
        return it->second;
    if (const auto it = shared().find (key); it != shared().end())
        return it->second;
    return nullptr;
}

// the name without one prefix / suffix (a band, voice, side or section), or "" when nothing can be dropped
std::string shorter (const std::string& s)
{
    // "b2 ratio", "v1 pan", "sc1 freq", "3 gain"
    if (s.size() > 3 && (s[0] == 'b' || s[0] == 'v') && std::isdigit ((unsigned char) s[1]) && s[2] == ' ')
        return s.substr (3);
    if (s.size() > 4 && startsWith (s, "sc") && std::isdigit ((unsigned char) s[2]) && s[3] == ' ')
        return s.substr (4);
    if (s.size() > 2 && std::isdigit ((unsigned char) s[0]) && s[1] == ' ')
        return s.substr (2);
    // a note switch of LYNX Tune: "c#", "c# bypass"
    for (const auto* prefix : { "echo ", "early ", "late ", "reverb ", "comp ", "gate " })
        if (startsWith (s, prefix) && s.size() > std::strlen (prefix))
            return s.substr (std::strlen (prefix));
    if (s.size() > 2 && (s.compare (s.size() - 2, 2, " l") == 0 || s.compare (s.size() - 2, 2, " r") == 0))
        return s.substr (0, s.size() - 2);
    return {};
}

bool isNoteName (const std::string& s)
{
    static const char* names[] = { "c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b" };
    for (const auto* n : names)
        if (s == n) return true;
    return false;
}

bool contains (const std::string& s, const char* word) { return s.find (word) != std::string::npos; }

// what a named mode is, from the words in its name (the reverbs' and echoes' mode names are descriptive)
Summary fromName (K kind, const std::string& name)
{
    Summary s;
    const auto reverb = kind == K::epochVerb || kind == K::atrium || kind == K::foilPlate || kind == K::halo || kind == K::horizon;
    if (reverb)
    {
        if (contains (name, "cathedral") || contains (name, "nave")) s.text = TR_NOOP ("a very large stone space: a long, dark tail");
        else if (contains (name, "hall")) s.text = TR_NOOP ("a concert hall: a slow build-up and a long, smooth tail");
        else if (contains (name, "plate")) s.text = TR_NOOP ("a plate: dense from the first moment, bright, no distinct early echoes");
        else if (contains (name, "chamber")) s.text = TR_NOOP ("a chamber: a dense, warm tail with few distinct echoes");
        else if (contains (name, "room")) s.text = TR_NOOP ("a room: early reflections close to the source and a short tail");
        else if (contains (name, "ambience")) s.text = TR_NOOP ("mostly early reflections: room sound without a long tail");
        else if (contains (name, "gated")) s.text = TR_NOOP ("a dense tail that stops abruptly");
        else if (contains (name, "freeze")) s.text = TR_NOOP ("an almost endless tail that builds up slowly");
        else if (contains (name, "space")) s.text = TR_NOOP ("a space larger than any real room: long, diffuse and moving");
        else if (contains (name, "mono")) s.text = TR_NOOP ("a mono reverb: the tail stays in the middle");
        else if (contains (name, "stereo")) s.text = TR_NOOP ("a stereo reverb; the bigger the space, the wider and slower the tail");
        if (contains (name, "bright")) s.detail = TR_NOOP ("brighter than the plain version");
        else if (contains (name, "dark")) s.detail = TR_NOOP ("darker than the plain version");
        else if (contains (name, "grit")) s.detail = TR_NOOP ("with the grain of early digital units");
        else if (contains (name, "smooth")) s.detail = TR_NOOP ("smoother, with no grain");
        else if (contains (name, "drift")) s.detail = TR_NOOP ("with a slowly drifting pitch");
        else if (contains (name, "chorus")) s.detail = TR_NOOP ("with a chorused tail");
        else if (contains (name, "random")) s.detail = TR_NOOP ("with randomly moving reflections");
        else if (contains (name, "vintage")) s.detail = TR_NOOP ("with the character of older digital units");
        else if (contains (name, "deep") || contains (name, "vast") || contains (name, "distant")) s.detail = TR_NOOP ("very large and far away");
        else if (contains (name, "dense")) s.detail = TR_NOOP ("denser than the plain version");
        else if (contains (name, "mirror")) s.detail = TR_NOOP ("with strong, mirror-like early reflections");
        if (s.text == nullptr && contains (name, "wide")) s.text = TR_NOOP ("a wide stereo reverb");
        else if (s.text == nullptr && contains (name, "dense")) s.text = TR_NOOP ("a dense, close reverb"), s.detail = nullptr;
        else if (s.text == nullptr && contains (name, "bright")) s.text = TR_NOOP ("a bright reverb"), s.detail = nullptr;
        if (kind == K::foilPlate && s.text == nullptr)
            s.text = TR_NOOP ("a plate of this metal: each has its own brightness, density and decay");
        return s;
    }
    if (kind == K::echoform)
    {
        if (contains (name, "hi-fi")) s.text = TR_NOOP ("a clean tape machine: full-range repeats with a little wow");
        else if (contains (name, "tape")) s.text = TR_NOOP ("a tape echo: darker repeats with wow, flutter and saturation");
        else if (contains (name, "lo-fi")) s.text = TR_NOOP ("a lo-fi echo: reduced bandwidth and bit depth");
        else if (contains (name, "bucket")) s.text = TR_NOOP ("a bucket-brigade echo: each repeat darker and grainier");
        else if (contains (name, "analog")) s.text = TR_NOOP ("an analog echo: warm repeats with a soft high cut");
        else if (contains (name, "digital")) s.text = TR_NOOP ("a digital echo: precise repeats with a gentle filter");
        else if (contains (name, "clean")) s.text = TR_NOOP ("clean repeats, unchanged");
        else if (contains (name, "phaser")) s.text = TR_NOOP ("repeats through a phaser");
        else if (contains (name, "shifter")) s.text = TR_NOOP ("each repeat is frequency-shifted");
        else if (contains (name, "pitch")) s.text = TR_NOOP ("each repeat is pitch-shifted");
        else if (contains (name, "reverse")) s.text = TR_NOOP ("the repeats play backwards");
        else if (contains (name, "detune")) s.text = TR_NOOP ("slightly detuned repeats for width");
        return s;
    }
    if (kind == K::orbit)
    {
        if (contains (name, "zero")) s.text = TR_NOOP ("a through-zero flanger: the sweep passes the dry signal for a deep jet sound");
        else if (contains (name, "rise")) s.text = TR_NOOP ("a flanger whose sweep rises (and falls) like a barber-pole");
        else if (contains (name, "doubler")) s.text = TR_NOOP ("a doubler: a second, slightly moving copy of the signal");
        else if (contains (name, "ensemble") || contains (name, "symphonic")) s.text = TR_NOOP ("an ensemble: several chorus voices for a wide, lush sound");
        else if (contains (name, "tri") || contains (name, "octo")) s.text = TR_NOOP ("a multi-voice chorus");
        return s;
    }
    if (kind == K::apexLimiter)
    {
        if (contains (name, "clean")) s.text = TR_NOOP ("the most transparent limiting, for small amounts");
        else if (contains (name, "glue")) s.text = TR_NOOP ("gentle limiting that holds a mix together");
        else if (contains (name, "punch")) s.text = TR_NOOP ("keeps the attack of drums and transients");
        else if (contains (name, "dynamic")) s.text = TR_NOOP ("keeps as much of the dynamics as possible");
        else if (contains (name, "dense")) s.text = TR_NOOP ("dense, even limiting for a full sound");
        else if (contains (name, "loud")) s.text = TR_NOOP ("the loudest result, with audible density");
        else if (contains (name, "bus")) s.text = TR_NOOP ("for groups and buses");
        else if (contains (name, "safe")) s.text = TR_NOOP ("a safety limiter: only catches the overs");
        return s;
    }
    if (kind == K::ember)
    {
        if (contains (name, "tube")) s.text = TR_NOOP ("tube saturation: soft, mostly even harmonics");
        else if (contains (name, "tape")) s.text = TR_NOOP ("tape saturation: rounded peaks, odd harmonics");
        else if (contains (name, "transistor")) s.text = TR_NOOP ("transistor saturation: a harder, brighter edge");
        else if (contains (name, "fuzz")) s.text = TR_NOOP ("fuzz: heavy clipping");
        return s;
    }
    return s;
}

// a kind's own line when the mode's name says nothing
const char* kindSummary (K kind)
{
    switch (kind)
    {
        case K::nebula: return TR_NOOP ("one of the delay networks: each has its own density, build-up and decay");
        case K::crestCompressor: return TR_NOOP ("the compressor's character: how it reacts to peaks and how it recovers");
        case K::sentryGate: return TR_NOOP ("how the gate opens and closes");
        default: return nullptr;
    }
}
} // namespace

const char* describe (EffectKind kind, const char* param)
{
    if (param == nullptr || *param == 0)
        return nullptr;
    auto key = fold (param);
    // LYNX Tune's note switches ("C", "C# bypass")
    if (kind == K::lynxTune)
    {
        if (isNoteName (key)) return lookup (kind, "note");
        if (key.size() > 7 && key.compare (key.size() - 7, 7, " bypass") == 0 && isNoteName (key.substr (0, key.size() - 7)))
            return lookup (kind, "note bypass");
    }
    for (int step = 0; step < 4 && ! key.empty(); ++step)
    {
        if (const auto* text = lookup (kind, key))
            return text;
        key = shorter (key);
    }
    return nullptr;
}

int modeParam (EffectKind kind)
{
    const char* name = nullptr;
    switch (kind)
    {
        case K::epochVerb: case K::nebula: case K::echoform: case K::orbit: name = "Mode"; break;
        case K::atrium: case K::foilPlate: name = "Type"; break;
        case K::halo: case K::horizon: name = "Reverb mode"; break;
        case K::ember: case K::crestCompressor: case K::sentryGate: case K::apexLimiter: name = "Style"; break;
        default: return -1;
    }
    const auto& info = effectInfo (kind);
    for (std::size_t i = 0; i < info.params.size(); ++i)
        if (std::strcmp (info.params[i].name, name) == 0)
            return (int) i;
    return -1;
}

Summary summary (EffectKind kind, const EffectParams& params)
{
    // round 165x: Ember's Classic voicing has its own one-screen panel and its own five styles; the footer names the
    // Classic style (Ember's own Style is not on that panel)
    if (kind == K::ember && params[(std::size_t) mixsuite::ember::voicing] > 0.5f)
    {
        Summary s;
        s.param = (int) mixsuite::ember::classicStyle;
        static const char* texts[] = { TR_NOOP ("a tape preamp: warm, rounded, the gentlest of the five"),
                                       TR_NOOP ("a vintage console channel: bright and forward"),
                                       TR_NOOP ("a transformer console input: thick, punchy mids"),
                                       TR_NOOP ("a triode valve: smooth, rich even harmonics"),
                                       TR_NOOP ("a pentode valve: the most aggressive, with an edge") };
        s.text = texts[emberclassic::styleOf (params[(std::size_t) mixsuite::ember::classicStyle])];
        return s;
    }
    const auto p = modeParam (kind);
    if (p < 0)
        return {};
    auto name = formatEffectParam (kind, p, params[(std::size_t) p], 120.0, params);
    std::transform (name.begin(), name.end(), name.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    auto s = fromName (kind, name);
    s.param = p;
    if (s.text == nullptr)
        s.text = kindSummary (kind);
    return s;
}
} // namespace lynx::paramhelp
