#include "core/PluginSubstitutes.h"
#include "core/VoiceMatch.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

#include "core/Effects.h"
#include "core/FabFilterStates.h"
#include "core/FlEffects.h"
#include "core/FlPluginDb.h"
#include "core/FlWrapper.h"
#include "core/LynxTune.h"
#include "core/MixSuite.h"
#include "core/VocalPitch.h"
#include "core/VoiceTools.h"
#include "core/VoiceShift.h"
#include "core/VoxStrip.h"
#include "core/EchoSuite.h"
#include "core/DynamicsSuite.h"

namespace lynx
{
namespace
{
// The built-in table (format: docs/PLUGIN_FALLBACK.md). Seeded with the third-party plugins of tmp/gf/PLUGIN_QUEUE.md
// and common mixing staples, mapped to EXISTING LYNX effects. Identifiers marked [DATA] were read from real projects'
// FL wrapper records; the others match by name. Parameter maps only where the saved state shows the layout and a
// public manual gives the meaning (each one says so). Modules added later (pitch corrector, vocal tools...) append
// their own entries here or through registerPluginSubstitutes.
constexpr std::string_view builtInTable = R"TABLE(
# ---------------------------------------------------------------- reverbs -> LYNX's reverbs (round 100)
# Valhalla DSP: the saved state is a JUCE XML block whose attributes are the plugin's normalised (0..1) VST3 values
# [DATA, round 98]. Round 100 measured each parameter's value -> unit law and enumeration breakpoints on the official
# demos (black-box, REA_rodada100 §0-§6) [EXEC]; LYNX's own reverbs use the same laws, so most maps are identities;
# enumerations use "steps", and Atrium's decay (LYNX's own law) a "curve". The Mix controls are equal-power
# dry / wet inside the effect (measured), so the slot mix stays at 100 %.
[valhalla-vintageverb]
product = ValhallaVintageVerb
vendor = Valhalla DSP
lynx = Epoch Verb
# [DATA] class ID from FL wrapper records of real projects
vst3 = 7654535665657633616c68616c6c6176
names = ValhallaVintageVerb
map = xml:Mix -> Mix
map = xml:PreDelay -> Predelay
map = xml:Decay -> Decay
map = xml:Size -> Size
map = xml:Attack -> Attack
map = xml:BassMult -> Bass mult
map = xml:BassXover -> Bass xover
map = xml:HighShelf -> High shelf
map = xml:HighFreq -> High freq
map = xml:EarlyDiffusion -> Early diffusion
map = xml:LateDiffusion -> Late diffusion
map = xml:ModRate -> Mod rate
map = xml:ModDepth -> Mod depth
map = xml:HighCut -> High cut
map = xml:LowCut -> Low cut
map = xml:ColorMode -> Color | steps 0:0 0.6662:0.5 0.9995:1
map = xml:ReverbMode -> Mode | steps 0:0 0.0828:0.0476 0.1245:0.0952 0.1662:0.1429 0.2078:0.1905 0.2495:0.2381 0.2912:0.2857 0.3328:0.3333 0.3745:0.381 0.4162:0.4286 0.4578:0.4762 0.4995:0.5238 0.5412:0.5714 0.5828:0.619 0.6245:0.6667 0.6662:0.7143 0.7078:0.7619 0.7495:0.8095 0.7912:0.8571 0.8328:0.9048 0.8745:0.9524 0.9162:1 0.9578:0

[valhalla-room]
product = ValhallaRoom
vendor = Valhalla DSP
lynx = Atrium
names = ValhallaRoom
map = xml:mix -> Mix
map = xml:predelay -> Predelay
map = xml:HiCut -> High cut
map = xml:earlyLateMix -> Early/late
map = xml:lateSize -> Late size
map = xml:lateCross -> Late cross
map = xml:lateModRate -> Late mod rate
map = xml:lateModDepth -> Late mod depth
map = xml:RTBassMultiply -> Bass mult
map = xml:RTXover -> Bass xover
map = xml:RTHighMultiply -> High mult
map = xml:RTHighXover -> High xover
map = xml:earlySize -> Early size
map = xml:earlyCross -> Early cross
map = xml:earlyModRate -> Early mod rate
map = xml:earlyModDepth -> Early mod depth
map = xml:earlySend -> Early send
map = xml:diffusion -> Diffusion
map = xml:space -> Space
map = xml:LoCut -> Low cut
map = xml:decay -> Decay | curve 0:0 0.0002:0.2254 0.0005:0.2646 0.001:0.2987 0.002:0.3372 0.004:0.3807 0.008:0.4297 0.012:0.4613 0.019:0.4999 0.03:0.5415 0.05:0.5921 0.08:0.6429 0.13:0.6999 0.2:0.7546 0.3:0.8101 0.45:0.8696 0.65:0.9274 1:1
map = xml:type -> Type | steps 0:0 0.1662:0.0909 0.2495:0.1818 0.3328:0.2727 0.4162:0.3636 0.4995:0.4545 0.5828:0.5455 0.6662:0.6364 0.7495:0.7273 0.8328:0.8182 0.9162:0.9091 0.9995:1

[valhalla-plate]
product = ValhallaPlate
vendor = Valhalla DSP
lynx = Foil Plate
names = ValhallaPlate
map = xml:Mix -> Mix
map = xml:PreDelay -> Predelay
map = xml:Decay -> Decay
map = xml:Size -> Size
map = xml:Width -> Width
map = xml:ModRate -> Mod rate
map = xml:ModDepth -> Mod depth
map = xml:LowEQFreq -> Low EQ freq
map = xml:LowEQGain -> Low EQ gain
map = xml:HighEQFreq -> High EQ freq
map = xml:HighEQGain -> High EQ gain
map = xml:Type -> Type | steps 0:0 0.0828:0.0909 0.1245:0.1818 0.1662:0.2727 0.2078:0.3636 0.2495:0.4545 0.2912:0.5455 0.3328:0.6364 0.3745:0.7273 0.4162:0.8182 0.4578:0.9091 0.4995:1 0.5412:0

[valhalla-supermassive]
product = Valhalla Supermassive
vendor = Valhalla DSP
lynx = Nebula
names = ValhallaSupermassive
map = xml:Mix -> Mix
map = xml:Delay_Ms -> Delay
map = xml:DelayWarp -> Warp
map = xml:Feedback -> Feedback
map = xml:Density -> Density
map = xml:Width -> Width
map = xml:LowCut -> Low cut
map = xml:HighCut -> High cut
map = xml:ModRate -> Mod rate
map = xml:ModDepth -> Mod depth
map = xml:DelaySync -> Sync | steps 0:0 0.4995:0.3333 0.7495:0.6667 0.9995:1
map = xml:DelayNote -> Note | steps 0:0 0.1453:0.1667 0.287:0.3333 0.4287:0.5 0.5745:0.6667 0.7162:0.8333 0.8578:1
map = xml:Mode -> Mode | steps 0:0 0.0828:0.0476 0.1245:0.0952 0.1662:0.1429 0.2078:0.1905 0.2495:0.2381 0.2912:0.2857 0.3328:0.3333 0.3745:0.381 0.4162:0.4286 0.4578:0.4762 0.4995:0.5238 0.5412:0.5714 0.5828:0.619 0.6245:0.6667 0.6662:0.7143 0.7078:0.7619 0.7495:0.8095 0.7912:0.8571 0.8328:0.9048 0.8745:0.9524 0.9162:1 0.9578:0

[valhalla-shimmer]
product = ValhallaShimmer
vendor = Valhalla DSP
lynx = Halo
names = ValhallaShimmer
# the reference saves bare parameterN attributes (0..1); the order is its VST3 parameter list [EXEC]
map = xml:parameter0 -> Mix
map = xml:parameter1 -> Shift
map = xml:parameter2 -> Feedback
map = xml:parameter3 -> Diffusion
map = xml:parameter4 -> Size
map = xml:parameter5 -> Low cut
map = xml:parameter6 -> High cut
map = xml:parameter7 -> Mod rate
map = xml:parameter8 -> Mod depth
# round 135: its four reverb modes (Mono / three stereo sizes, steps at 0.25 / 0.5 / 0.75 [EXEC]) -> Halo's same four
map = xml:parameter9 -> Reverb mode | steps 0:0.125 0.25:0.25 0.5:0.375 0.75:0.625
map = xml:parameter10 -> Shift mode | steps 0:0 0.25:0.25 0.45:0.5 0.65:0.75 0.85:1
map = xml:parameter11 -> Color | steps 0:0 0.525:1

[valhalla-futureverb]
product = ValhallaFutureVerb
vendor = Valhalla DSP
lynx = Horizon
names = ValhallaFutureVerb
map = xml:Mix -> Mix
map = xml:Width -> Width
map = xml:Echo_Ms -> Echo time
map = xml:EchoFeedback -> Echo feedback
map = xml:EchoSpread -> Echo spread
map = xml:EchoDrive -> Echo drive
map = xml:EchoDetune -> Echo detune
map = xml:EchoLevel -> Echo level
map = xml:ReverbDecay -> Decay
map = xml:ReverbSize -> Size
map = xml:ReverbAttack -> Attack
map = xml:ReverbEarlyLate -> Early/late
map = xml:ReverbDensity -> Density
map = xml:ReverbLevel -> Reverb level
map = xml:ModRate -> Mod rate
map = xml:ModDepth -> Mod depth
map = xml:LowEQFreq -> Low EQ freq
map = xml:HighEQFreq -> High EQ freq
map = xml:EchoSync -> Echo sync | steps 0:0 0.4995:0.3333 0.7495:0.6667 0.9995:1
map = xml:EchoNote -> Echo note | steps 0:0 0.1662:0.2 0.3328:0.4 0.4995:0.6 0.6662:0.8 0.8328:1
map = xml:Routing -> Routing | steps 0:0 0.4995:1
map = xml:EchoMode -> Echo mode | steps 0:0 0.0828:0.0909 0.1245:0.1818 0.1662:0.2727 0.2078:0.3636 0.2495:0.4545 0.2912:0.5455 0.3328:0.6364 0.3745:0.7273 0.4162:0.8182 0.4578:0.9091 0.4995:1 0.5412:0
map = xml:ReverbMode -> Reverb mode | steps 0:0 0.0828:0.1429 0.1245:0.2857 0.1662:0.4286 0.2078:0.5714 0.2495:0.7143 0.2912:0.8571 0.3328:1 0.3745:0
map = xml:Color -> Color | steps 0:0 0.1662:0.3333 0.2495:0.6667 0.3328:1

# ---------------------------------------------------------------- other reverbs -> Reeverb 2 / LuxeVerb
[fabfilter-pro-r]
product = FabFilter Pro-R
vendor = FabFilter
lynx = LuxeVerb
names = FabFilter Pro-R; FabFilter Pro-R 2; Pro-R; Pro-R 2
mix = 1

[soundtoys-littleplate]
product = Little Plate
vendor = Soundtoys
lynx = Foil Plate
names = LittlePlate; Little Plate
# round 129 (docs/PARITY.md): LYNX's plate reverb plays a plate's role (was Reeverb 2); its state is encoded: a
# 2 s plate, 100 % wet, at the slot's dry / wet 25 % as before. Round 150: in the measured plate voicing (the
# trial's onset and decay per band, REA_rodada150a §7). Round 152: the editor opens in the one-knob Plate view
preset = Decay=0.3; Mix=1; Voicing=1; View=1
mix = 0.25

[waves-reverbs]
product = Waves reverb (RVerb / H-Reverb / TrueVerb)
vendor = Waves
lynx = Reeverb 2
waves = RVerb; H-Reverb; TrueVerb
preset = Dry=0; Wet=0.8
mix = 0.25

# ---------------------------------------------------------------- delays -> Fruity Delay 3 (FL's "Default" preset)
[waves-h-delay]
product = H-Delay
vendor = Waves
lynx = Echoform
# [DATA] class ID (stereo component) and <PluginName> from real projects. Round 119: LYNX's echo (was Fruity Delay 3
# at FL's default); the RealWorld list is translated (sync host / BPM / ms, note, time, feedback, ping-pong or
# stereo, mix, output, filters), measured on the official trial (docs/analise/REA_rodada119.md §2). Without a
# readable state: a clean quarter-note ping-pong at 30 % feedback
vst3 = 48545356444268532d64656c61792073
waves = H-Delay
translate = waves-h-delay
preset = Mix=1; Style=0.9; Sync L=0.625; Note L=0.45; Sync R=0.625; Note R=0.45; Feedback=0.0465; Mode=0.5417; Age=0; Mod depth=0; Width=1; Pairs=1; Output=0.4611

[waves-supertap]
product = SuperTap
vendor = Waves
lynx = Fruity Delay 3
waves = SuperTap

[soundtoys-echoboy]
product = EchoBoy
vendor = Soundtoys
# round 129 (docs/PARITY.md): LYNX's multi-mode echo plays EchoBoy's role. Round 136: her states play Echoform's Tape
# voicing (the loop structures measured on the official trial, REA_rodada136 §4), recognised by the hash of the encoded
# preset (not decoded here); any other state: round 129's tape echo, 1/4 note at the project tempo, 25 % feedback, 25 % mix
# (round 147: in the Studio reel voicing — EchoBoy's default style, measured black-box, REA_rodada147a)
lynx = Echoform
preset = Mode=0.0415; Sync L=0.6248; Note L=0.4498; Feedback=0.125; Mix=0.25; Echo voicing=0.064516
# [DATA] VST2 unique ID from real projects
vst2 = StEB
names = EchoBoy; EchoBoyJr; EchoBoy Jr
translate = soundtoys-echoboy

[soundtoys-primaltap]
product = PrimalTap
vendor = Soundtoys
# round 129: Echoform (as EchoBoy), a darker lo-fi tape echo
lynx = Echoform
preset = Mode=0.125; Sync L=0.6248; Note L=0.4498; Feedback=0.125; Mix=0.25
names = PrimalTap; Little PrimalTap; LittlePrimalTap

# Valhalla delay / modulation plugins -> LYNX's delay / modulation suite (round 101). Their saved state is a JUCE
# XML block holding every parameter as its normalised value [DATA, EXEC]; the LYNX effects use the same value laws,
# measured black-box on the official demos (docs/analise/REA_rodada101.md §2-§6), so most settings map one to one.
# [derived] VST3 class IDs: Steinberg's VST2-compatible UID ("VST" + the VST2 ID found in the state's FBCh header +
# the first 9 letters of the lower-case name), in FL's byte order; the same rule reproduces ValhallaVintageVerb's ID
# read from real projects.
[valhalla-delay]
product = ValhallaDelay
vendor = Valhalla DSP
lynx = Echoform
vst3 = 64545356614c7679616c68616c6c6164
names = ValhallaDelay
mix = 1
map = xml:Mix -> Mix
# styles: Single, Dual, Ratio, PingPong, Quad onto Echoform's order (Single, Ratio, Quad, Dual, Ping-pong)
map = xml:DelayStyle -> Style | steps 0:0.1 0.19:0.7 0.39:0.3 0.59:0.9 0.79:0.5
map = xml:DelayLSync -> Sync L
map = xml:DelayLNote -> Note L
map = xml:DelayL_Ms -> Time L
map = xml:DelayRSync -> Sync R
map = xml:DelayRNote -> Note R
map = xml:DelayR_Ms -> Time R
map = xml:DelaySpread -> Spread
map = xml:DelaySpacing -> Spacing
map = xml:DelayRatio -> Ratio
# round 137: the quad taps and Repeat / Swell (Echoform has them since round 135w; the same on / off values)
map = xml:TapA -> Tap A
map = xml:TapB -> Tap B
map = xml:TapC -> Tap C
map = xml:TapD -> Tap D
map = xml:RepeatSwell -> Swell
map = xml:Feedback -> Feedback
map = xml:Width -> Width
map = xml:DriveIn -> Drive
map = xml:Age -> Age
map = xml:Diffusion -> Diffusion
map = xml:DiffSize -> Diffusion size
map = xml:LowCut -> Low cut
map = xml:HighCut -> High cut
map = xml:ModRate -> Mod rate
map = xml:ModDepth -> Mod depth
map = xml:Wow -> Wow
map = xml:Flutter -> Flutter
map = xml:FreqShift -> Shift
map = xml:FreqDetune -> Shift detune
map = xml:PitchShift -> Pitch
map = xml:PitchDetune -> Pitch detune
map = xml:Ducking -> Ducking
# round 137: Era (Past below 2/3, Present, Future at the top; measured on the demo's own value texts)
map = xml:Era -> Era | steps 0:0 0.6662:0.5 0.9995:1
# the 17 modes (index floor (24 v)) onto Echoform's 12 characters: Tape, HiFi, BBD, Digital, Ghost (shifter), Pitch,
# RevPitch (reverse), BBDuck (bucket), Clarity (clean), DuckTape (tape), PitchDuck (pitch), LoFi, Quartz (clean),
# PhaserDDL (phaser), RichPitch (detune), ChromeTape (tape), Analog; the "duck" variants' built-in ducking is not kept
map = xml:Mode -> Mode | steps 0:0.041667 0.0828:0.208333 0.1245:0.291667 0.1662:0.458333 0.2078:0.708333 0.2495:0.791667 0.2912:0.875 0.3328:0.291667 0.3745:0.541667 0.4162:0.041667 0.4578:0.791667 0.4995:0.125 0.5412:0.541667 0.5828:0.625 0.6245:0.958333 0.6662:0.041667 0.7078:0.375 0.7495:0.041667

[valhalla-freqecho]
product = ValhallaFreqEcho
vendor = Valhalla DSP
lynx = Spiral Echo
vst3 = 4654535645717668616c68616c6c6166
names = ValhallaFreqEcho
mix = 1
map = xml:wetDryMix -> Mix
map = xml:shift -> Shift
map = xml:delay -> Delay
map = xml:sync -> Sync
map = xml:feedback -> Feedback
map = xml:lowCut -> Low cut
map = xml:highCut -> High cut
map = xml:stereo -> Stereo

[valhalla-spacemodulator]
product = ValhallaSpaceModulator
vendor = Valhalla DSP
lynx = Orbit
vst3 = 535453564d707664616c68616c6c6173
names = ValhallaSpaceModulator
mix = 1
map = xml:wetDry -> Mix
map = xml:rate -> Rate
map = xml:depth -> Depth
map = xml:feedback -> Feedback
map = xml:manual -> Manual
map = xml:Mode -> Mode

[valhalla-ubermod]
product = ValhallaUberMod
vendor = Valhalla DSP
lynx = Lattice Mod
vst3 = 7554535665627672616c68616c6c6175
names = ValhallaUberMod
mix = 1
map = xml:Mix -> Mix
map = xml:Depth -> Depth
map = xml:StereoWidth -> Width
map = xml:Delay -> Delay
map = xml:Feedback -> Feedback
map = xml:Spread -> Spread
map = xml:Slope -> Slope
map = xml:Skew -> Skew
map = xml:Random -> Random
map = xml:TapGain -> Tap gain
map = xml:Diffusion -> Diffusion
map = xml:DiffSize -> Diffusion size
map = xml:DiffModRate -> Diffusion rate
map = xml:DiffModDepth -> Diffusion depth
map = xml:LowCut -> Low cut
map = xml:HighCut -> High cut
map = xml:SpatialXover -> Crossover
map = xml:DetuneRate -> Detune rate
map = xml:DetuneDepth -> Detune depth
map = xml:VibratoRate -> Vibrato rate
map = xml:VibratoDepth -> Vibrato depth
map = xml:OverMod -> Over-mod
map = xml:DriveInGain -> Drive in
map = xml:DriveOutGain -> Drive out
map = xml:DriveNoiseGain -> Noise
map = xml:FeedbackRotate -> Rotate
map = xml:SmoothingTime -> Smoothing
map = xml:ColorMode -> Colour
map = xml:type -> Taps
map = xml:DiffEnable -> Diffusion on
map = xml:Drive -> Drive on
map = xml:DrivePrePost -> Drive post
map = xml:Speed -> Half speed
# round 137: InputPan (0-8, the same value law; its routes measured black-box, REA_rodada137 §4)
map = xml:InputPan -> Input pan
# round 144: DelaySync (floor (17 v): free, straight, dotted, triplet groups — measured, REA_rodada144a §1), the same value law
map = xml:DelaySync -> Delay sync

[fabfilter-timeless]
product = FabFilter Timeless
vendor = FabFilter
lynx = Fruity Delay 3
names = FabFilter Timeless 2; FabFilter Timeless 3; Timeless 2; Timeless 3

# ---------------------------------------------------------------- limiters -> Limiter (ceiling -0.3 dB)
[waves-l-series]
product = Waves L1 / L2 / L3
vendor = Waves
lynx = Limiter
waves = L1; L2; L3; L3-16; L3-LL Multi; L4

[fabfilter-pro-l]
product = FabFilter Pro-L
vendor = FabFilter
lynx = Apex Limiter
names = FabFilter Pro-L; FabFilter Pro-L 2; Pro-L; Pro-L 2

# ---------------------------------------------------------------- compressors -> Compressor
[waves-rcomp]
product = Renaissance Compressor
vendor = Waves
lynx = Compressor
# [DATA] class ID (stereo component) and <PluginName> from real projects
vst3 = 5554535643527253636f6d7072657373
waves = RComp
# round 113: its RealWorld list is translated (threshold, ratio, attack, release, gain; measured on the official
# trial, docs/analise/REA_rodada113.md §3c). Without a readable state: a gentle default (2:1 above -15 dBFS, auto gain)
translate = waves-rcomp
preset = Threshold=0.75; Ratio=0.0526; Auto gain=1

[waves-cla-2a]
product = CLA-2A
vendor = Waves
lynx = Glow Leveler
# [DATA] class ID (stereo component) and <PluginName> from real projects
vst3 = 4c545356324163536c612d3261207374
waves = CLA-2A
# LYNX's optical leveler (round 96). Round 113: gain, peak reduction and comp / limit are translated from the
# RealWorld list (measured on the official trial, REA_rodada113 §3c); without a readable state light leveling
preset = Peak reduction=0.30; Gain=0.067
translate = waves-cla-2a

[waves-cla-76]
product = CLA-76
vendor = Waves
lynx = Crest Compressor
waves = CLA-76
# round 127: LYNX's style compressor fitted to the official trial at her one state (the factory "Start Me Up":
# Input -8.7 dB, Output -16.9 dB, attack 4.3, release 4.31, all buttons, Mix 41.3 %); the RealWorld list is
# translated (REA_rodada127 §3). Without a readable state: a light 4:1 at Waves' defaults
translate = waves-cla-76
preset = Style=0.9; Threshold=0.6; Ratio=0.546; Knee=0.13; Range=0.53; Auto gain=1

[waves-compressors]
product = Waves compressor (CLA-3A / C1 / SSLComp / API-2500)
vendor = Waves
lynx = Compressor
waves = CLA-3A; C1 comp; C1comp; SSLComp; API-2500
preset = Threshold=0.75; Ratio=0.0526; Auto gain=1

[fabfilter-pro-c]
product = FabFilter Pro-C
vendor = FabFilter
lynx = Crest Compressor
names = FabFilter Pro-C 2; FabFilter Pro-C 3; Pro-C 2; Pro-C 3
# LYNX's style compressor (round 108), fitted to our black-box measurements (MEASURE_fabfilter §2); a Pro-C 3 state
# (component state "FFBS", 100 values) is translated: style, threshold, ratio, knee, range, attack, release, hold,
# look-ahead, character, side-chain EQ (3 bands), levels, mix, oversampling. Pro-C 2 states keep the defaults.
translate = pro-c3
# auto gain on by default (round 107): keeps the level when the state is not readable
preset = Auto gain=1

[fabfilter-pro-mb]
product = FabFilter Pro-MB
vendor = FabFilter
lynx = Prism Multiband
names = FabFilter Pro-MB; Pro-MB
# LYNX's 4-band dynamics (round 108, MEASURE_fabfilter §6 / §12.4): the enabled bands are laid out low → high on
# Prism's four bands (gaps become bypassed bands), with threshold, ratio, range, mode, knee, attack / release %,
# level, solo, look-ahead, mix and levels. Without a readable state the bands start at a gentle -12 dB range.
translate = pro-mb

[fabfilter-pro-g]
product = FabFilter Pro-G
vendor = FabFilter
lynx = Sentry Gate
names = FabFilter Pro-G; Pro-G
# LYNX's gate / expander (round 108, MEASURE_fabfilter §7): every setting of the state is translated (style,
# thresholds, ratios, range, attack, release, hold, knee, look-ahead, side-chain filters, wet / dry, output)
translate = pro-g

# ---------------------------------------------------------------- EQs and channel strips -> Parametric EQ (flat)
[waves-ssl-channel]
product = SSL E-Channel
vendor = Waves
lynx = Ridge Strip
# [DATA] class ID (stereo component) and <PluginName> from real projects. Round 123: Ridge Strip's "E" voicing is the
# console measured black-box on the official trial (every EQ band / filter from impulse responses, the feedback
# compressor from stairs, bursts and her takes), so the RealWorld list maps one to one (REA_rodada123.md)
vst3 = 5354535648437353736c6368616e6e65
waves = SSLChannel
translate = waves-ssl-channel

[ssl-native-channel]
product = SSL Native Channel Strip / 4K E
vendor = Solid State Logic
lynx = Ridge Strip
names = SSL Native Channel Strip 2; SSL Native Channel Strip; SSL 4K E; SSL 4K B; Vocalstrip 2; SSL Native Vocalstrip 2
# round 145: a Vocalstrip 2 state (XML, the original's own units) is translated to Ridge's Vocal strip voicing, measured
# black-box on the official trial (de-esser, de-ploser, EQ, compander, order; REA_rodada145 §2); round 148: a 4K E state
# (the same XML form) to Ridge's 4K voicing, measured on the 4K E trial (REA_rodada148 §2); the other strips' states are
# not read (Ridge at its defaults)
translate = ssl-vocalstrip-2

# round 145: the rest of Complete Access (Slate Digital, Solid State Logic, LiquidSonics, sonible, Harrison; official
# trials on this PC) by role only — their states are not read (not in her corpus, not measured): LYNX's effect for
# the same job at its defaults, so a project that used them still plays a sensible chain
[complete-access-strips]
product = channel strips (SSL 4K G, Harrison 32C / MPC, Slate VCC / Virtual Mix Rack)
vendor = Complete Access (role)
lynx = Ridge Strip
names = SSL 4K G; Harrison 32C; Harrison_32ClassicChannelStrip; Harrison 32Classic; Harrison_MPCChannelStrip; VM VCC Channel; Virtual Mix Rack; VBC Rack; SSL Guitarstrip; SSL Native Drumstrip v6

[complete-access-bus-comps]
product = bus / smart compressors (SSL Bus Compressor 2, G3, VBC, sonible smart:comp, pure:comp)
vendor = Complete Access (role)
lynx = Crest Compressor
names = SSL Native Bus Compressor 2; SSL G3 MultiBusComp; SSL 360 Link Bus Compressor; SSL Native X-Comp v6; SSL Fusion HF Compressor; VBC FG-Grey; VBC FG-MU; VBC FG-Red; smartcomp3; smartcomp2; purecomp; learncomp; Harrison_LookaheadCompressor; Harrison_MPCCompressor; VM FG-Stress; VM FG-Bomber

[complete-access-vintage-comps]
product = vintage compressor models (Slate FG-116 / FG-76 / FG-401 / FG-A / FG-Dynamics, SSL X-ValveComp)
vendor = Complete Access (role)
lynx = Compressor
names = VM FG-116; VM FG-116 Modern; VM FG-116 Vintage; VM FG-76; VM FG-401; VM FG-A; VM FG-Dynamics; VM The Monster; SSL Native X-ValveComp v6; MO-TT

[complete-access-optos]
product = optical levelers (Slate FG-2A, SD-3A, Custom Opto)
vendor = Complete Access (role)
lynx = Glow Leveler
names = VM FG-2A; VM SD-3A; VM Custom Opto

[complete-access-limiters]
product = limiters (Slate FG-X 2, SSL X-Limit, sonible smart:limit, pure:limit)
vendor = Complete Access (role)
lynx = Apex Limiter
names = FG-X 2; FG-X; SSL X-Limit; smartlimit; purelimit; learnlimit; Bus Clipper

[complete-access-eqs]
product = EQs (Slate Infinity EQ, FG-73 / FG-N / FG-S / SD-PE1 / CS-EQ, Eiosis AirEQ, SSL X-EQ 2, sonible smart:EQ)
vendor = Complete Access (role)
lynx = Facet EQ
names = Inf EQ; Infinity EQ; VM FG-73; VM FG-N; VM FG-S; VM SD-PE1; VM CS-EQ; VM CS-Lift; VM AirEQ Air; VM AirEQ Earth; Eiosis AirEQ; Eiosis AirEQ 5.1; SSL Native X-EQ 2; SSL Fusion Violet EQ; smartEQ4; smartEQ3; pureeq; learneq; Harrison_LegacyEQ; Harrison_MasteringEQ

[complete-access-deessers]
product = de-essers (SSL DeEss, Slate FG-DS 902, Eiosis E2Deesser, sonible smart:deess)
vendor = Complete Access (role)
lynx = Hush De-esser
names = SSL DeEss; VM FG-DS 902; Eiosis E2Deesser; smartdeess; puredeess; Harrison_De-Esser

[complete-access-gates]
product = gates (SSL X-Gate, Slate Gate Classic / Drums, Harrison Spectral Gate)
vendor = Complete Access (role)
lynx = Sentry Gate
names = SSL X-Gate; VM Gate Classic; VM Gate Drums; Harrison_SpectralGate

[complete-access-reverbs]
product = reverbs (LiquidSonics Lustrous Plates / Seventh Heaven / Cinematic Rooms, Slate VerbSuite Classics, SSL FlexVerb / PlateVerb / SpringVerb, sonible smart:reverb)
vendor = Complete Access (role)
lynx = Foil Plate
names = Lustrous Plates; Seventh Heaven; Seventh Heaven Professional; Cinematic Rooms; Cinematic Rooms Professional; Illusion; VerbSuite Classics; SSL Native FlexVerb; SSL PlateVerb; SSL SpringVerb; SSL GateVerb; smartreverb2; smartreverb; pureverb; learnreverb; Vocal Reverb
preset = Decay=0.3; Mix=1
mix = 0.25

[complete-access-delays]
product = delays (SSL X-Echo / X-Delay, Slate Stellar Echo SD-201, Harrison 3D Delay)
vendor = Complete Access (role)
lynx = Echoform
preset = Mode=0.0415; Sync L=0.6248; Note L=0.4498; Feedback=0.125; Mix=0.25
names = SSL Native X-Echo; SSL X-Delay; Stellar Echo SD-201; Harrison_3DDelay

[complete-access-saturators]
product = saturators (Slate Heatwave / Revival, SSL Fusion Vintage Drive / X-Saturator, Harrison Tape Saturator)
vendor = Complete Access (role)
lynx = Ember Saturator
names = Heatwave; VM Revival; SSL Fusion Vintage Drive; SSL Native X-Saturator v6; SSL Fusion Transformer; Harrison Tape Saturator

[complete-access-tuners]
product = pitch correction (Slate MetaTune)
vendor = Complete Access (role)
lynx = LYNX Tune
names = MetaTune

[waves-s1-imager]
product = S1 Imager
vendor = Waves
lynx = Stereo Shaper
# round 124: the width / rotation / gain matrix measured on the official trial is set exactly on the Stereo Shaper's
# matrix (docs/analise/REA_rodada124_extras.md §4); without a readable state the slot stays neutral
waves = S1 Imager; S1 Imager Stereo
translate = waves-s1-imager

[waves-eqs]
product = Waves EQ (Q10 / Renaissance EQ / SSL EQ)
vendor = Waves
lynx = Parametric EQ
waves = Q10; REQ; SSLEQ

[fabfilter-pro-q]
product = FabFilter Pro-Q
vendor = FabFilter
lynx = Facet EQ
names = FabFilter Pro-Q 2; FabFilter Pro-Q 3; FabFilter Pro-Q 4; Pro-Q 2; Pro-Q 3; Pro-Q 4
# round 124: a Pro-Q 4 state (component state "FFBS", 600 values) is translated band by band (shape, frequency, gain,
# Q, slope; processing mode, gain scale, output) — Facet's shapes are fitted to the official trial, so her curves
# null against the reference (docs/analise/REA_rodada124_extras.md §2). Other versions keep a flat curve.
translate = pro-q4

# ---------------------------------------------------------------- vocal tools -> LYNX's mixing suite (round 96)
[waves-deesser]
product = DeEsser
vendor = Waves
lynx = Hush De-esser
waves = DeEsser; DeEsser Stereo
# round 113: frequency, threshold, split / wideband, side-chain filter and monitor are translated from the RealWorld
# list (measured on the official trial, REA_rodada113 §3c); without a readable state a gentle default (5.5 kHz,
# -20 dBFS, at most 4 dB)
translate = waves-deesser
# round 165x: 5.5 kHz on the extended law's 2 … 20 kHz scale (0.487 on the old one)
preset = Frequency=0.4393; Threshold=0.667; Range=0.167

[fabfilter-pro-ds]
product = FabFilter Pro-DS
vendor = FabFilter
lynx = Hush De-esser
names = FabFilter Pro-DS; Pro-DS
# round 148: her two states (Single Vocal, wide band) -> Hush's measured vocal voicing (REA_rodada148 §3)
translate = pro-ds

[harrison-deesser]
product = Harrison De-Esser
vendor = Harrison
lynx = Hush De-esser
names = Harrison De-Esser; Harrison DeEsser

[slate-fresh-air]
product = Fresh Air
vendor = Slate Digital
lynx = Aether
names = Fresh Air; FreshAir
# round 113: its two air amounts are read from the state (freshAirAmounts); round 124: they drive Aether's dynamic
# air model, measured black-box on the official trial to match it sample for sample (REA_rodada124 §3)
translate = fresh-air

[soundtoys-decapitator]
product = Decapitator
vendor = Soundtoys
lynx = Ember Saturator
# [DATA] VST2 unique ID from real projects
vst2 = StDc
names = Decapitator
# round 136: Ember's Classic voicing, measured black-box on the official trial (REA_rodada136 §1). Its state is
# encoded (not decoded here); her 63 saved states are known by the hash of the encoded preset and translated to the
# settings the trial reports for them; any other state: style A, drive 2, output -3.2 dB (her usual light drive)
preset = Voicing=1; Classic style=0; Classic drive=0.2; Classic tone=0.5; Classic high cut=1; Classic mix=1; Classic output=0.866667
translate = soundtoys-decapitator

[fabfilter-saturn]
product = FabFilter Saturn
vendor = FabFilter
lynx = Ember Saturator
names = FabFilter Saturn; FabFilter Saturn 2; Saturn 2
preset = Drive=0.15
# a Saturn 2 state ("FFBS", 951 values) sets Ember's multiband mode (round 108): up to four bands with their
# crossovers, style family, drive, mix and level, the global mix and output
translate = saturn2

# ---------------------------------------------------------------- modulation
[soundtoys-panman]
product = PanMan
vendor = Soundtoys
lynx = Auto-pan
# [DATA] VST2 unique ID from real projects
vst2 = StPn
names = PanMan
# round 136: Auto-pan's Rhythm voicing (core/RhythmPan.h), measured black-box on the official trial (REA_rodada136
# §2). Its state is encoded (not decoded here); her 50 saved states are known by the hash of the encoded preset and
# translated to the pattern, rate / beats, width, smoothing, start phase and analog stage measured for them; any other
# state: her most used one (a 3.56 Hz left / right triangle over 151 degrees, analog). (Round 135's rhythmic-panner
# sources — free LFO, rhythm steps — stay available on the same effect; vox138 kept round 136's measured voicing for
# the substitute.)
preset = Depth=0.918; Source=0.5; Rate=0.8505; Smoothing=1; Analog=1; Voicing=1; Rhythm rate=0.8505; Sync=0; Width=0.7171; Offset=0.5; Rhythm smoothing=0.9925; Pattern=0; Amount=0.9981; Start phase=0.9917; Rhythm analog=0.5; Rhythm output=0.5
translate = soundtoys-panman

[soundtoys-littlealterboy]
product = Little AlterBoy
vendor = Soundtoys
lynx = LYNX Tune
# [DATA] VST2 unique ID from real projects
vst2 = StLa
names = Little AlterBoy; LittleAlterBoy; AlterBoy
# round 136 (REA_rodada136 §3): LYNX Tune as the voice shifter — Quantize = chromatic snapping at the fastest retune,
# Transpose = every note bypassed (pitch shift only), the formant shift, the measured Drive stage and the mix. Its
# state is encoded (not decoded here); her 52 saved states are known by the hash of the encoded preset; any other
# state: her most used one (Quantize, formant +1.2 st, drive 4.6). (Round 135's Voice Shift engine — the same role as
# its own effect — stays in LYNX's effect list; the substitute keeps round 136's measured model.)
preset = Scale=0; Retune=0; Humanize=0; Formant preserve=1; Formant shift=0.585714; Transpose=0.5; Mix=1; Drive stage=1; Drive=0.46; Output=0.5
translate = soundtoys-littlealterboy

# ---------------------------------------------------------------- vocal pitch correction -> LYNX Tune (round 95)
[antares-auto-tune]
product = Auto-Tune Pro
vendor = Antares
lynx = LYNX Tune
# [DATA] VST3 class ID from FL wrapper records of real projects (round 98). Round 113: the state's key and scale are
# read (autoTuneProKey); round 120: its retune speed, humanize and flex-tune too (measured on the official trial);
# without a key the project's key (another readable pitch corrector) or, failing that, a gentle chromatic correction
# (50 ms, humanize 50 %) instead of forcing C major
vst3 = 415453563854413175746f2d54756e65
names = Auto-Tune Pro; Auto-Tune Pro X; Auto-Tune Artist; Auto-Tune Access; Auto-Tune EFX+; Auto-Tune Hybrid; Auto-Tune
translate = auto-tune-pro
preset = Scale=0; Retune=0.3536; Humanize=0.5
mix = 1

# ---------------------------------------------------------------- noise suppressor / doubler (round 119)
[waves-ns1]
product = NS1
vendor = Waves
lynx = Quell Denoiser
# LYNX's one-knob noise suppressor, fitted to the official trial (docs/analise/REA_rodada119.md §3); the amount is
# read from the RealWorld list (value 229, %)
waves = NS1
translate = waves-ns1
preset = Amount=0.25

[waves-doubler]
product = Doubler
vendor = Waves
lynx = Gemini Doubler
# LYNX's two-voice doubler, laws measured on the official trial (REA_rodada119 §4); every control of the two-voice
# Doubler is translated from the RealWorld list (voices 3 and 4 of the four-voice one are left out)
waves = Doubler; Doubler2; Doubler4
translate = waves-doubler

[waves-tune]
product = Waves Tune Real-Time
vendor = Waves
lynx = LYNX Tune
waves = WavesTune Real-Time; Waves Tune Real-Time; WavesTune
names = WavesTune Real-Time; Waves Tune Real-Time
# round 120: its saved scale (root + note statuses) and its Note Transition time (= LYNX Tune's retune, measured)
translate = waves-tune
preset = Scale=0; Retune=0.3536; Humanize=0.5
mix = 1
)TABLE";

std::string lowerAscii (std::string s)
{
    for (auto& c : s)
        c = (char) std::tolower ((unsigned char) c);
    return s;
}

std::string trim (std::string_view s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace ((unsigned char) s[a]))
        ++a;
    while (b > a && std::isspace ((unsigned char) s[b - 1]))
        --b;
    return std::string (s.substr (a, b - a));
}

std::vector<std::string> splitList (std::string_view s, char separator)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == separator)
        {
            if (auto item = trim (s.substr (start, i - start)); ! item.empty())
                out.push_back (std::move (item));
            start = i + 1;
        }
    return out;
}

// a name key without the channel-layout words Waves and others append ("RComp Stereo", "L2 Mono")
std::string matchKey (const std::string& name)
{
    auto key = pluginNameKey (name);
    for (bool again = true; again;)
    {
        again = false;
        for (const std::string suffix : { "stereo", "mono" })
            if (key.size() >= suffix.size() + 2 && key.compare (key.size() - suffix.size(), suffix.size(), suffix) == 0)
            {
                key.resize (key.size() - suffix.size());
                again = true;
            }
    }
    return key;
}

std::optional<EffectKind> effectKindNamed (const std::string& name)
{
    const auto wanted = lowerAscii (trim (name));
    for (int k = 0; k <= (int) lastEffectKind; ++k) // the last EffectKind: extend when one is appended
    {
        const auto kind = (EffectKind) k;
        if (kind == EffectKind::plugin || kind == EffectKind::parallel)
            continue;
        if (lowerAscii (effectInfo (kind).name) == wanted)
            return kind;
    }
    return std::nullopt;
}

int paramNamed (EffectKind kind, const std::string& name)
{
    const auto wanted = lowerAscii (trim (name));
    const auto& params = effectInfo (kind).params;
    for (std::size_t i = 0; i < params.size(); ++i)
        if (lowerAscii (params[i].name) == wanted)
            return (int) i;
    return -1;
}

bool parseFloat (const std::string& s, float& out)
{
    try
    {
        std::size_t used = 0;
        out = std::stof (s, &used);
        return used == s.size() && std::isfinite (out);
    }
    catch (...)
    {
        return false;
    }
}

bool parseHex16 (const std::string& s, std::array<std::uint8_t, 16>& out)
{
    std::string hex;
    for (auto c : s)
        if (std::isxdigit ((unsigned char) c))
            hex += c;
        else if (c != '-' && c != ' ')
            return false;
    if (hex.size() != 32)
        return false;
    for (std::size_t i = 0; i < 16; ++i)
        out[i] = (std::uint8_t) std::stoul (hex.substr (i * 2, 2), nullptr, 16);
    return true;
}

// "StEB" (four characters, as VST2 IDs are usually written) or a number ("0x53744542", "1400128834")
bool parseVst2Id (const std::string& s, std::uint32_t& out)
{
    if (s.size() == 4 && ! (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')))
    {
        out = ((std::uint32_t) (unsigned char) s[0] << 24) | ((std::uint32_t) (unsigned char) s[1] << 16)
              | ((std::uint32_t) (unsigned char) s[2] << 8) | (std::uint32_t) (unsigned char) s[3];
        return true;
    }
    try
    {
        std::size_t used = 0;
        const auto v = std::stoull (s, &used, 0);
        if (used != s.size() || v > 0xFFFFFFFFull)
            return false;
        out = (std::uint32_t) v;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// "<source> -> <target> [| inLo inHi outLo outHi]"; source = vst2:<n> | xml:<attribute> | waves:<n>; target =
// "mix" or a parameter name of the LYNX effect
bool parseMap (const std::string& text, EffectKind kind, SubstituteParamMap& out)
{
    const auto arrow = text.find ("->");
    if (arrow == std::string::npos)
        return false;
    const auto source = trim (std::string_view (text).substr (0, arrow));
    auto rest = std::string (text.substr (arrow + 2));
    std::string range;
    if (const auto bar = rest.find ('|'); bar != std::string::npos)
    {
        range = rest.substr (bar + 1);
        rest = rest.substr (0, bar);
    }
    const auto target = trim (rest);

    const auto colon = source.find (':');
    if (colon == std::string::npos)
        return false;
    const auto type = lowerAscii (source.substr (0, colon));
    const auto arg = trim (std::string_view (source).substr (colon + 1));
    if (arg.empty())
        return false;
    if (type == "xml")
    {
        out.source = SubstituteParamMap::Source::xml;
        out.attribute = arg;
    }
    else if (type == "vst2" || type == "waves")
    {
        out.source = type == "vst2" ? SubstituteParamMap::Source::vst2 : SubstituteParamMap::Source::waves;
        float index = 0;
        if (! parseFloat (arg, index) || index < 0 || index != std::floor (index))
            return false;
        out.index = (int) index;
    }
    else
        return false;

    if (target == "mix") // lower case: the slot's dry/wet ("Mix" is an effect parameter, e.g. of LYNX's reverbs)
        out.toSlotMix = true;
    else if ((out.target = paramNamed (kind, target)) < 0)
        return false;

    if (const auto words = splitList (range, ' '); ! words.empty() && (lowerAscii (words[0]) == "curve" || lowerAscii (words[0]) == "steps"))
    {
        out.shape = lowerAscii (words[0]) == "curve" ? SubstituteParamMap::Shape::curve : SubstituteParamMap::Shape::steps;
        for (std::size_t i = 1; i < words.size(); ++i)
        {
            const auto colon = words[i].find (':');
            float x = 0, y = 0;
            if (colon == std::string::npos || ! parseFloat (words[i].substr (0, colon), x) || ! parseFloat (words[i].substr (colon + 1), y))
                return false;
            if (! out.points.empty() && x <= out.points.back().first)
                return false; // x must ascend
            out.points.push_back ({ x, y });
        }
        return out.points.size() >= (out.shape == SubstituteParamMap::Shape::curve ? 2u : 1u);
    }
    if (! range.empty())
    {
        std::vector<float> v;
        for (const auto& token : splitList (range, ' '))
        {
            float f = 0;
            if (! parseFloat (token, f))
                return false;
            v.push_back (f);
        }
        if (v.size() != 4 || v[0] == v[1])
            return false;
        out.inLo = v[0];
        out.inHi = v[1];
        out.outLo = v[2];
        out.outHi = v[3];
    }
    return true;
}

std::vector<SubstituteEntry>& registry()
{
    static std::vector<SubstituteEntry> entries = parseSubstituteTable (builtInTable);
    return entries;
}

std::size_t findBytes (const std::vector<std::uint8_t>& data, std::string_view what, std::size_t from = 0)
{
    if (what.empty() || data.size() < what.size())
        return std::string::npos;
    for (std::size_t i = from; i + what.size() <= data.size(); ++i)
        if (std::memcmp (data.data() + i, what.data(), what.size()) == 0)
            return i;
    return std::string::npos;
}

std::string xmlUnescape (const std::string& s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '&')
        {
            out += s[i];
            continue;
        }
        const auto end = s.find (';', i);
        if (end == std::string::npos)
        {
            out += s[i];
            continue;
        }
        const auto entity = s.substr (i + 1, end - i - 1);
        if (entity == "amp") out += '&';
        else if (entity == "lt") out += '<';
        else if (entity == "gt") out += '>';
        else if (entity == "quot") out += '"';
        else if (entity == "apos") out += '\'';
        else
        {
            out += s.substr (i, end - i + 1);
        }
        i = end;
    }
    return out;
}

// the text of the active setup's <Parameters> block of a Waves preset chunk
std::string wavesParametersText (const std::vector<std::uint8_t>& state)
{
    const auto start = findBytes (state, "<PresetChunkXMLTree");
    if (start == std::string::npos)
        return {};
    const std::string xml (state.begin() + (std::ptrdiff_t) start, state.end());
    std::string setup = "SETUP_A";
    if (const auto a = xml.find ("<ActiveSetup>"); a != std::string::npos)
        if (const auto b = xml.find ("</ActiveSetup>", a); b != std::string::npos)
            setup = trim (std::string_view (xml).substr (a + 13, b - a - 13));
    const auto data = xml.find ("<PresetData Setup=\"" + setup + "\"");
    if (data == std::string::npos)
        return {};
    const auto params = xml.find ("<Parameters", data);
    if (params == std::string::npos)
        return {};
    const auto open = xml.find ('>', params);
    const auto close = xml.find ("</Parameters>", params);
    if (open == std::string::npos || close == std::string::npos || close < open)
        return {};
    return xml.substr (open + 1, close - open - 1);
}

float mapValue (const SubstituteParamMap& m, double value)
{
    if (m.shape == SubstituteParamMap::Shape::steps)
    {
        auto y = m.points.front().second;
        for (const auto& [px, py] : m.points)
            if (value >= (double) px - 1e-6)
                y = py;
        return std::clamp (y, 0.0f, 1.0f);
    }
    if (m.shape == SubstituteParamMap::Shape::curve)
    {
        const auto& p = m.points;
        if (value <= (double) p.front().first)
            return std::clamp (p.front().second, 0.0f, 1.0f);
        for (std::size_t i = 1; i < p.size(); ++i)
            if (value <= (double) p[i].first)
            {
                const auto t = (value - (double) p[i - 1].first) / ((double) p[i].first - (double) p[i - 1].first);
                return std::clamp ((float) ((double) p[i - 1].second + t * ((double) p[i].second - (double) p[i - 1].second)), 0.0f, 1.0f);
            }
        return std::clamp (p.back().second, 0.0f, 1.0f);
    }
    auto u = (value - (double) m.inLo) / ((double) m.inHi - (double) m.inLo);
    u = std::clamp (u, 0.0, 1.0);
    return (float) ((double) m.outLo + u * ((double) m.outHi - (double) m.outLo));
}

// --- binary writer / reader for foreignToText ----------------------------------------------------------------------
void putU32 (std::vector<std::uint8_t>& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back ((std::uint8_t) (v >> (8 * i)));
}

void putBytes (std::vector<std::uint8_t>& out, const std::uint8_t* p, std::size_t n)
{
    putU32 (out, (std::uint32_t) n);
    out.insert (out.end(), p, p + n);
}

void putString (std::vector<std::uint8_t>& out, const std::string& s)
{
    putBytes (out, (const std::uint8_t*) s.data(), s.size());
}

struct BinReader
{
    const std::vector<std::uint8_t>& d;
    std::size_t pos = 0;
    bool ok = true;

    std::uint32_t u32()
    {
        if (pos + 4 > d.size())
        {
            ok = false;
            return 0;
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= (std::uint32_t) d[pos + (std::size_t) i] << (8 * i);
        pos += 4;
        return v;
    }

    std::vector<std::uint8_t> bytes()
    {
        const auto n = u32();
        if (! ok || n > d.size() - pos)
        {
            ok = false;
            return {};
        }
        std::vector<std::uint8_t> v (d.begin() + (std::ptrdiff_t) pos, d.begin() + (std::ptrdiff_t) (pos + n));
        pos += n;
        return v;
    }

    std::string str()
    {
        const auto b = bytes();
        return std::string (b.begin(), b.end());
    }
};

constexpr std::uint32_t foreignTextMagic = 0x31464C4Cu; // "LLF1"

// --- translators of states whose layout is readable without a vendor library (round 113) ------------------------
using RawTranslator = int (*) (const std::vector<std::uint8_t>& state, EffectSlot& slot);

// Auto-Tune Pro: key and scale (autoTuneProKey) and the Auto-mode controls of its ParamsData (autoTuneProParams;
// [EXEC] REA round 120 §5: the X<n> entries are the plugin's parameter n in real units — 4 Retune Speed (ms),
// 61 Humanize, 90 Flex-Tune (0…100)). Measured black-box on the official trial: a held note is pulled to its target
// along a first-order curve whose time constant is 1.36 × the retune speed − 3 ms (LYNX Tune's retune is that time
// constant); Humanize and Flex-Tune map 1:1 onto LYNX Tune's (fitted to the same measurements, LynxTune.cpp).
// Round 127 (docs/specs/auto-tune-pro.md §3, §7.5): the remaining cheap controls, units from the trial's own
// parameter texts — X3 Detune (cents, A4 = 440·2^(c/1200); LYNX Tune's reference reaches ±39 c), X74 Transpose
// (semitones), X92 Wet-Dry Mix (%), X10 Input Type (Soprano, Alto-Tenor, Low Male, Instrument, Bass Instrument →
// LYNX Tune's ranges, the two instrument types on Instrument), X70 Formant Correction off → formants move with the
// pitch, X71 Throat Length (%: LYNX Tune's Throat length, measured in round 165hb),
// X11 Classic mode ("Auto-Tune 5": no formant correction, no Flex-Tune — the manual) and X9 Tracking (50 = LYNX
// Tune's default strictness; the rest of the law [model]). All 143 states of her main projects use Detune 0,
// Transpose 0, Mix 100 %, Alto-Tenor, Throat 100 %, Tracking 50 (round 120 §5), so these only matter elsewhere.
// Without a readable key the slot keeps the entry's gentle preset.
int translateAutoTuneEfx (const std::vector<std::uint8_t>& state, EffectSlot& slot);
std::string attributeIn (const std::string& text, std::size_t from, const std::string& name);

int translateAutoTunePro (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    if (slot.kind != EffectKind::lynxTune)
        return 0;
    if (const auto efx = translateAutoTuneEfx (state, slot); efx > 0)
        return efx;
    const auto key = autoTuneProKey (state);
    if (! key)
        return 0;
    slot.params[lynxtune::key] = (float) key->first / 11.0f;
    if (key->second >= vocal::scaleCount) // whole tone / diminished (round 138: modern scale lists): "More scales"
    {
        slot.params[lynxtune::scale] = 0.0f;
        slot.params[lynxtune::moreScales] = key->second == (int) vocal::Scale::wholeTone ? 0.5f : 1.0f;
    }
    else
        slot.params[lynxtune::scale] = (float) key->second / (float) (vocal::scaleCount - 1);
    const auto x = autoTuneProParams (state);
    auto has = [&x] (int n) {
        const auto it = x.find (n);
        return it != x.end() && std::isfinite (it->second);
    };
    auto at = [&x, &has] (int n, double fallback) { return has (n) ? x.at (n) : fallback; };
    int n = 2;
    auto set = [&slot, &n] (int param, double v) {
        slot.params[(std::size_t) param] = (float) std::clamp (v, 0.0, 1.0);
        ++n;
    };
    const auto retune = std::clamp (at (4, 20.0), 0.0, 400.0);
    set (lynxtune::retune, std::sqrt (std::clamp (1.36 * retune - 3.0, 0.0, 400.0) / 400.0));
    set (lynxtune::humanize, at (61, 0.0) / 100.0);
    const auto classic = at (11, 0.0) >= 0.5;
    set (lynxtune::flexTune, classic ? 0.0 : at (90, 0.0) / 100.0);
    if (has (3))
        set (lynxtune::reference, (440.0 * std::exp2 (std::clamp (at (3, 0.0), -100.0, 100.0) / 1200.0) - 430.0) / 20.0);
    if (has (74) && ! classic)
        set (lynxtune::transpose, (std::clamp (std::round (at (74, 0.0)), -12.0, 12.0) + 12.0) / 24.0);
    if (has (92))
        set (lynxtune::mix, at (92, 100.0) / 100.0);
    if (has (10))
    {
        static constexpr int rangeOf[5] { 0, 1, 2, 3, 3 };
        set (lynxtune::range, rangeOf[std::clamp ((int) std::lround (at (10, 1.0)), 0, 4)] / (double) (lynxtune::rangeCount - 1));
    }
    if (has (70) || classic)
        set (lynxtune::formantPreserve, ! classic && at (70, 1.0) >= 0.5 ? 1.0 : 0.0);
    // round 166c: X71 Throat Length → LYNX Tune's own Throat length (the law measured in REA_rodada165hb §2 lives in
    // the engine; round 138's half-size law on Formant shift, fitted on a synthetic vowel, is superseded)
    if (has (71) && ! classic)
        set (lynxtune::throatLength, lynxtune::throatValue (at (71, 100.0)));
    if (has (9))
        set (lynxtune::tracking, std::clamp (at (9, 50.0), 1.0, 100.0) * 0.4 / 50.0);
    // round 128: natural vibrato (X62, dB), ignore vibrato (X44), Create Vibrato (X5 shape, X7 rate, X6 cents, X46 / X50
    // onset delay / rate, ms) and the active scale's per-note Bypass / Remove edits (autoTuneProNoteEdits)
    if (has (62)) set (lynxtune::naturalVibrato, at (62, 0.0) / 24.0 + 0.5);
    if (has (44)) set (lynxtune::ignoreVibrato, at (44, 0.0) >= 0.5 ? 1.0 : 0.0);
    if (has (5)) set (lynxtune::vibratoShape, std::clamp (std::round (at (5, 0.0)), 0.0, 3.0) / 3.0);
    if (has (7)) set (lynxtune::vibratoRate, (at (7, 5.5) - 0.1) / 9.9);
    if (has (6)) set (lynxtune::vibratoDepth, at (6, 18.0) / 100.0);
    if (has (46)) set (lynxtune::vibratoDelay, at (46, 500.0) / 1500.0);
    if (has (50)) set (lynxtune::vibratoOnset, at (50, 500.0) / 1500.0);
    // round 166c: the created vibrato's Amplitude (X47, 0 … 30), Formant (X48, 0 … 100) and Variation (X49, 0 … 100),
    // and Transpose In Scale (X179 on / off) with Scale Transpose (X181, −7 … +7 steps)
    if (has (47)) set (lynxtune::vibratoAmplitude, at (47, 10.0) / 30.0);
    if (has (48)) set (lynxtune::vibratoFormant, at (48, 70.0) / 100.0);
    if (has (49)) set (lynxtune::vibratoVariation, at (49, 20.0) / 100.0);
    if (has (179) && ! classic) set (lynxtune::transposeInScale, at (179, 0.0) >= 0.5 ? 1.0 : 0.0);
    if (has (181)) set (lynxtune::scaleTranspose, lynxtune::scaleTransposeValue ((int) std::lround (std::clamp (at (181, 0.0), -7.0, 7.0))));
    if (const auto edits = autoTuneProNoteEdits (state, key->first, key->second))
        for (int k = 0; k < 12; ++k)
        {
            set (lynxtune::noteC + k, ((edits->second >> k) & 1u) != 0 ? 0.0 : 1.0);
            set (lynxtune::bypassC + k, ((edits->first >> k) & 1u) != 0 ? 1.0 : 0.0);
        }
    return n;
}

// Auto-Tune EFX+ (round 127; [DATA] her 6 states, 17 slots): an "<AUTOTUNEEFXPLUS>" element of <PARAM id="p…"
// value="…"/> in real units. pKey 0…11, pScale (0 Major, 1 Minor, 2 Chromatic — the same list as Auto-Tune Pro: every
// pScale 2 state has all twelve notes on, every pScale 1 state on key 11 holds B minor's notes), pNoteC … pNoteB = the
// notes that are targets (absolute pitch classes; one state adds F to B minor), pRetuneSpeed (ms, read with Auto-Tune
// Pro's law [model: not measured on EFX+]), pHumanize, pPitchShift (semitones), pFormant, pThroatLength (%),
// pInputType, pDryWetMix (%). Its multi-effect chain (Duet, Tube, Mutate, Vocode, filters) and Auto-Motion patterns
// have no LYNX Tune equivalent and are left out.
int translateAutoTuneEfx (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    const auto root = findBytes (state, "<AUTOTUNEEFXPLUS");
    if (root == std::string::npos || slot.kind != EffectKind::lynxTune)
        return 0;
    const std::string text (state.begin() + (std::ptrdiff_t) root, state.end());
    std::map<std::string, double> v;
    for (auto p = text.find ("<PARAM "); p != std::string::npos; p = text.find ("<PARAM ", p + 7))
    {
        const auto id = attributeIn (text, p, "id");
        float value = 0;
        if (! id.empty() && parseFloat (attributeIn (text, p, "value"), value) && std::isfinite (value))
            v[id] = value;
    }
    auto at = [&v] (const char* id, double fallback) {
        const auto it = v.find (id);
        return it != v.end() ? it->second : fallback;
    };
    if (v.find ("pKey") == v.end() || v.find ("pScale") == v.end())
        return 0;
    const int key = std::clamp ((int) std::lround (at ("pKey", 0.0)), 0, 11);
    static constexpr const char* notes[12] { "pNoteC", "pNoteC#", "pNoteD", "pNoteD#", "pNoteE", "pNoteF",
                                             "pNoteF#", "pNoteG", "pNoteG#", "pNoteA", "pNoteA#", "pNoteB" };
    std::uint16_t mask = 0;
    for (int k = 0; k < 12; ++k)
        if (at (notes[k], 1.0) >= 0.5)
            mask = (std::uint16_t) (mask | (1u << k));
    const int theirs = (int) std::lround (at ("pScale", 2.0));
    auto scale = theirs == 0 ? vocal::Scale::major : (theirs == 1 ? vocal::Scale::minor : vocal::Scale::chromatic);
    if (mask == 0)
        mask = 0xfff;
    if (vocal::scaleMask (scale, key) != mask)
        scale = vocal::Scale::custom; // the note switches carry the exact targets
    auto& p = slot.params;
    p[lynxtune::key] = (float) key / 11.0f;
    p[lynxtune::scale] = (float) (int) scale / (float) (vocal::scaleCount - 1);
    for (int k = 0; k < 12; ++k)
        p[(std::size_t) (lynxtune::noteC + k)] = ((mask >> k) & 1u) != 0 ? 1.0f : 0.0f;
    const auto retune = std::clamp (at ("pRetuneSpeed", 20.0), 0.0, 400.0);
    p[lynxtune::retune] = (float) std::sqrt (std::clamp (1.36 * retune - 3.0, 0.0, 400.0) / 400.0);
    p[lynxtune::humanize] = (float) std::clamp (at ("pHumanize", 0.0) / 100.0, 0.0, 1.0);
    p[lynxtune::flexTune] = 0.0f;
    p[lynxtune::transpose] = (float) ((std::clamp (std::round (at ("pPitchShift", 0.0)), -12.0, 12.0) + 12.0) / 24.0);
    const auto formant = at ("pFormant", 1.0) >= 0.5;
    p[lynxtune::formantPreserve] = formant ? 1.0f : 0.0f;
    p[lynxtune::formantShift] = 0.5f;
    p[lynxtune::throatLength] = lynxtune::throatValue (at ("pThroatLength", 100.0)); // round 166c: the engine's throat
    static constexpr int rangeOf[5] { 0, 1, 2, 3, 3 };
    p[lynxtune::range] = (float) rangeOf[std::clamp ((int) std::lround (at ("pInputType", 1.0)), 0, 4)] / (float) (lynxtune::rangeCount - 1);
    p[lynxtune::mix] = (float) std::clamp (at ("pDryWetMix", 100.0) / 100.0, 0.0, 1.0);
    return 24;
}

// Waves Tune Real-Time: the key and scale saved in its state (wavesTuneScale; [DATA] every one of the 11 distinct
// states of her corpus names its scale — "Natural Minor" / "Major" — with the root and the twelve note statuses);
// a scale LYNX Tune has no name for becomes its custom scale with the same notes. RealWorld 20 = Speed, 21 = Note
// Transition (ms). [EXEC] Black-box (official trial, REA round 120 §2): a held mistuned note is pulled to the target
// along a first-order curve whose time constant is the Note Transition time (50 % / 90 % at 12 ms + 0.69 / 2.3 x NT for
// NT = 21 … 250 ms), which is LYNX Tune's retune law at retune = NT; Speed barely changes anything on sung notes (only
// below ~5 ms, slightly less vibrato), so it is not translated; no humanize.
std::optional<double> wavesValue (const std::vector<double>& rw, std::size_t i);

int translateWavesTune (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    if (slot.kind != EffectKind::lynxTune)
        return 0;
    int n = 0;
    if (const auto sc = wavesTuneScale (state))
    {
        slot.params[lynxtune::key] = (float) sc->key / 11.0f;
        slot.params[lynxtune::scale] = (float) sc->scale / (float) (vocal::scaleCount - 1);
        for (int k = 0; k < 12; ++k)
            slot.params[(std::size_t) (lynxtune::noteC + k)] = sc->scale != (int) vocal::Scale::custom || ((sc->mask >> k) & 1u) != 0 ? 1.0f : 0.0f;
        n += 2;
    }
    const auto rw = wavesRealWorldParams (state);
    if (const auto transition = wavesValue (rw, 21); transition && *transition >= 0.0)
    {
        slot.params[lynxtune::retune] = (float) std::sqrt (std::clamp (*transition, 0.0, 400.0) / 400.0);
        slot.params[lynxtune::humanize] = 0.0f;
        n += 2;
    }
    // round 135w: Speed is shown as saved; it only acts (Link off) where the trial showed an effect (below ~5 ms)
    if (const auto speed = wavesValue (rw, 20); speed && *speed > 0.0)
    {
        slot.params[lynxtune::speed] = lynxtune::speedValue (*speed);
        slot.params[lynxtune::speedLink] = *speed < 5.0 ? 0.0f : 1.0f;
        ++n;
    }
    // RealWorld 22 = 100 in every state of her corpus, Correction % by its default [model]
    if (const auto amount = wavesValue (rw, 22); amount && *amount >= 1.0 && *amount <= 100.0)
        slot.params[lynxtune::correction] = (float) (*amount / 100.0);
    return n;
}

// Fresh Air: its two air amounts drive Aether's dynamic air model (round 124), which reproduces the official trial
// measured black-box (docs/analise/REA_rodada124.md §3): the same two parallel compressed high-passed bands, the same
// amount laws, so the amounts map one to one; Trim (the float at 2526 of the state, -12 … 0 dB) becomes the output.
// (Round 113 fitted a static shelf; the original is level-dependent: quiet highs get up to +12 dB, loud ones +3.)
int translateFreshAir (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    namespace a = mixsuite::aether;
    const auto air = freshAirAmounts (state);
    if (! air || slot.kind != EffectKind::aether)
        return 0;
    slot.params[a::dynamic] = 1.0f;
    slot.params[a::midAir] = air->first;
    slot.params[a::highAir] = air->second;
    slot.params[a::air] = 0.0f;
    slot.params[a::harmonics] = 0.0f;
    slot.params[a::mix] = 1.0f;
    float trimDb = 0.0f;
    std::memcpy (&trimDb, state.data() + 2526, 4);
    if (! std::isfinite (trimDb) || trimDb < -12.0f || trimDb > 0.0f)
        trimDb = 0.0f;
    slot.params[a::output] = (trimDb + 12.0f) / 24.0f;
    return 3;
}

// --- Waves RealWorld lists (round 113). [EXEC] Black-box measurement of the official trials (WaveShell 17.1, tools/
// fxmeasure, headless): each control was moved alone and the value of the list that followed it (and its text) was
// recorded; then the plugin at her projects' states and LYNX's substitute were run on the same sine stairs (1 and
// 6 kHz, -60 .. 0 dBFS) and vocal takes and the substitute fitted (docs/analise/REA_rodada113.md §3c).
// The value at `i`, or nothing when the list is shorter / holds "*"
std::optional<double> wavesValue (const std::vector<double>& rw, std::size_t i)
{
    if (i < rw.size() && std::isfinite (rw[i]))
        return rw[i];
    return std::nullopt;
}

float logNorm (double v, double lo, double hi) { return (float) std::clamp (std::log (v / lo) / std::log (hi / lo), 0.0, 1.0); }
float linNorm (double v, double lo, double hi) { return (float) std::clamp ((v - lo) / (hi - lo), 0.0, 1.0); }

// Renaissance Compressor (28 values): 0 release ms, 1 threshold dB, 2 gain dB, 8 attack ms, 9 ratio, 17 ARC (0) / manual,
// 26 mix %, 27 trim dB.
// Round 123: Compressor's "vintage program" voicing is the compressor measured black-box on the official trial at her
// state (core/VoiceMatch.h; REA_rodada123 §3), taking threshold, ratio, attack, release and gain as they are.
int translateWavesRComp (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    const auto rw = wavesRealWorldParams (state);
    const auto threshold = wavesValue (rw, 1), ratio = wavesValue (rw, 9);
    if (slot.kind != EffectKind::compressor || ! threshold || ! ratio)
        return 0;
    slot.params[0] = linNorm (*threshold, -60.0, 0.0);
    slot.params[1] = linNorm (*ratio, 1.0, 20.0);
    slot.params[2] = logNorm (wavesValue (rw, 8).value_or (0.5), 0.1, 100.0);
    slot.params[3] = logNorm (wavesValue (rw, 0).value_or (160.0), 10.0, 1000.0);
    slot.params[4] = linNorm (wavesValue (rw, 2).value_or (0.0), 0.0, 24.0);
    slot.params[5] = 0.0f;
    slot.params[6] = (float) std::clamp (wavesValue (rw, 26).value_or (100.0) / 100.0, 0.0, 1.0); // mix %
    slot.params[7] = linNorm (wavesValue (rw, 27).value_or (0.0), -18.0, 18.0);                  // trim dB
    slot.params[8] = 1.0f;                                                                         // vintage voicing
    // round 135w: ARC / Manual (17), Electro / Opto (22), Warm / Smooth (16) [EXEC round 113 list]; the output limiter
    // the reference always has
    slot.params[9] = wavesValue (rw, 17).value_or (0.0) >= 0.5 ? 1.0f : 0.0f;
    slot.params[10] = wavesValue (rw, 22).value_or (0.0) >= 0.5 ? 1.0f : 0.0f;
    slot.params[11] = wavesValue (rw, 16).value_or (0.0) >= 0.5 ? 1.0f : 0.0f;
    slot.params[12] = 1.0f;
    // round 150: the reference's own ranges (Compressor's "Ranges = RComp" and its RC parameters, REA_rodada150 §6):
    // ratio 0.5 … 50 (below 1 expands), gain −30 … +30, attack 0.5 … 500 ms, release 5 … 5000 ms, as they are
    const auto r = std::clamp (*ratio, 0.5, 50.0);
    slot.params[13] = 1.0f;
    slot.params[14] = (float) (r >= 1.0 ? 0.625 * (1.0 - std::log (r) / std::log (50.0)) : 0.625 + 0.375 * std::log (r) / std::log (0.5));
    slot.params[15] = linNorm (wavesValue (rw, 2).value_or (0.0), -30.0, 30.0);
    slot.params[16] = logNorm (wavesValue (rw, 8).value_or (16.0), 0.5, 500.0);
    slot.params[17] = logNorm (wavesValue (rw, 0).value_or (160.0), 5.0, 5000.0);
    return 6;
}

// DeEsser (15 values): 0 frequency Hz, 2 threshold dB, 10 monitor (1 = side chain), 13 side chain (1 = high pass,
// 2 = band pass), 14 audio (0 = split, 1 = wideband). Round 123: Hush's "classic split" voicing is the de-esser measured
// black-box on the official trial (core/VoiceMatch.h; REA_rodada123 §2), so the controls map one to one.
int translateWavesDeEsser (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    namespace h = mixsuite::hush;
    const auto rw = wavesRealWorldParams (state);
    const auto hz = wavesValue (rw, 0);
    if (slot.kind != EffectKind::hushDeEsser || ! hz)
        return 0;
    int n = 1;
    slot.params[h::voicing] = 1.0f;
    slot.params[h::frequency] = logNorm (*hz, 2000.0, 16000.0);
    slot.params[h::output] = 0.5f;
    if (const auto t = wavesValue (rw, 2))
        slot.params[h::threshold] = linNorm (*t, -80.0, 0.0), ++n;
    if (const auto m = wavesValue (rw, 14))
        slot.params[h::mode] = *m >= 0.5 ? 1.0f : 0.0f, ++n;
    if (const auto d = wavesValue (rw, 13))
        // round 165 [EXEC]: the 17.1 trial stores HighPass 1 / BandPass 0 (band-pass states were read as high-pass); 2 =
        // band-pass as before
        slot.params[h::detector] = (*d < 0.5 || *d >= 1.5) ? 1.0f : 0.0f, ++n;
    if (const auto l = wavesValue (rw, 10))
        slot.params[h::listen] = *l >= 0.5 ? 1.0f : 0.0f, ++n;
    return n;
}

// CLA-2A (208 values): 38 gain, 74 peak reduction, 112 comp (0) / limit, 153 Hi Freq, 206 trim dB. Round 123: Glow
// Leveler's "opto classic" voicing is the leveler measured black-box on the official trial (core/VoiceMatch.h;
// REA_rodada123 §4), where the dials read 0 .. 100 like the original's, so they map one to one.
int translateWavesCla2a (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    namespace g = mixsuite::glow;
    const auto rw = wavesRealWorldParams (state);
    const auto gain = wavesValue (rw, 38), reduction = wavesValue (rw, 74);
    if (slot.kind != EffectKind::glowLeveler || ! gain || ! reduction)
        return 0;
    slot.params[g::voicing] = 1.0f;
    slot.params[g::gain] = (float) std::clamp (*gain / 100.0, 0.0, 1.0);
    slot.params[g::peakReduction] = (float) std::clamp (*reduction / 100.0, 0.0, 1.0);
    slot.params[g::mode] = wavesValue (rw, 112).value_or (0.0) >= 0.5 ? 1.0f : 0.0f;
    slot.params[g::emphasis] = (float) std::clamp (wavesValue (rw, 153).value_or (50.0) / 100.0, 0.0, 1.0);
    slot.params[g::trim] = linNorm (wavesValue (rw, 206).value_or (0.0), -18.0, 18.0);
    // round 152 [EXEC] (REA_rodada152a §2, read back through the trial's own parameters): 204 Mix %, 207 Auto Makeup,
    // 140 Analog (0 = 50 Hz, 1 = 60 Hz, 2 = off)
    slot.params[g::mix] = (float) std::clamp (wavesValue (rw, 204).value_or (100.0) / 100.0, 0.0, 1.0);
    slot.params[g::autoMakeup] = wavesValue (rw, 207).value_or (0.0) >= 0.5 ? 1.0f : 0.0f;
    const auto analog = std::lround (wavesValue (rw, 140).value_or (2.0));
    slot.params[g::analog] = analog == 0 ? 0.5f : analog == 1 ? 1.0f : 0.0f;
    slot.params[g::optoLaw] = 1.0f;
    slot.params[g::releaseLaw] = 1.0f; // round 165w: the measured release
    // round 165x [EXEC] (REA_rodada165x §1): the trial's AUTO MAKEUP turns its own Gain knob (the saved Gain, index 38,
    // already holds the make-up: 40 → 53.79 after Auto Makeup on at 46 and the dial to 80; the host's Gain parameter
    // still reads 40), so the loaded dial is the make-up's reference (round 152 added the make-up again from 46)
    slot.params[g::makeupRef] = slot.params[g::peakReduction];
    // round 165x [EXEC]: 17 = VU DISPLAY (0 In, 1 GR, 2 Out; read back through the trial's own parameter)
    const auto vu = std::lround (wavesValue (rw, 17).value_or (1.0));
    slot.params[g::vuDisplay] = vu == 0 ? 0.5f : vu == 2 ? 1.0f : 0.0f;
    return 8;
}

// CLA-76 (106 values): 0 / 1 Input / Output as positive dB magnitudes (−0 … −∞ dB), 2 / 3 Attack / Release knobs
// 1 … 7, 59 Ratio (0 = 20, 1 = 12, 2 = 8, 3 = 4, 4 = all buttons), 103 Mix %. [EXEC] Read back through the trial's own
// parameter API with her state loaded (REA_rodada127 §3). Her only state was rendered on the official trial (mix 0 /
// 100 / 41.3 %: the mix is a linear dry / wet blend, the dry path is exact) and Crest fitted to its wet path: Bus
// style, threshold −43.7 dB, 7.1:1, 9.3 dB knee, 31.9 dB range (the FET's gain-reduction ceiling), attack 5.9 ms,
// release 97 ms, +31.6 dB (the all-buttons wet path's small-signal gain) — on her take the null went from −11.5 dB
// (2:1 Compressor) to −19.9 dB. Other settings move around that point by the manual's laws [model]: the input level
// moves the threshold and the wet gain dB for dB, the output the wet gain, attack 800 → 20 µs and release
// 1100 → 50 ms log-spaced over the knobs 1 … 7, the ratio buttons as labelled; the mix becomes the slot's dry / wet.
// Round 148 (the Waves trial in full, no demo mutes; REA_rodada148 §5): her state re-rendered on her three whole takes
// and Crest refitted on them (Nelder–Mead on the waveform null): threshold −55.4 dB, 2.8:1 (all buttons), knee 24.8 dB,
// range 31.5 dB, attack 1.56 ms, release 2.5 s (Crest's longest: the reference's all-buttons release is slow) — null
// go / kwe / lu −13.0 / −7.9 / −10.2 → −25.4 / −19.2 / −21.8 dB, 1/3-octave error 3.3–3.9 → 0.5–1.0 dB.
int translateWavesCla76 (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    namespace c = dyn::crest;
    const auto rw = wavesRealWorldParams (state);
    const auto in = wavesValue (rw, 0), out = wavesValue (rw, 1);
    if (slot.kind != EffectKind::crestCompressor || ! in || ! out)
        return 0;
    const auto at = [&] (std::size_t i, double fallback) { return wavesValue (rw, i).value_or (fallback); };
    const auto inDb = -std::clamp (*in, 0.0, 120.0), outDb = -std::clamp (*out, 0.0, 120.0);
    const auto attackKnob = std::clamp (at (2, 3.0), 1.0, 7.0), releaseKnob = std::clamp (at (3, 4.0), 1.0, 7.0);
    const auto attackLaw = [] (double k) { return 0.8 * std::pow (0.02 / 0.8, (k - 1.0) / 6.0); };
    const auto releaseLaw = [] (double k) { return 1100.0 * std::pow (50.0 / 1100.0, (k - 1.0) / 6.0); };
    static constexpr double ratios[5] { 20.0, 12.0, 8.0, 4.0, 2.8 };
    const int ratioIndex = std::clamp ((int) std::lround (at (59, 3.0)), 0, 4);
    auto& p = slot.params;
    p[c::style] = c::styleValue (c::Style::bus);
    p[c::threshold] = linNorm (-55.4 - (inDb + 8.7), -60.0, 0.0);
    p[c::ratio] = c::ratioValue (ratios[ratioIndex]);
    p[c::knee] = linNorm (24.8, 0.0, 72.0);
    p[c::range] = linNorm (31.5, 0.0, 60.0);
    p[c::attack] = c::attackValue (1.56 * attackLaw (attackKnob) / attackLaw (4.3));
    p[c::release] = c::releaseValue (std::min (2500.0, 2500.0 * releaseLaw (releaseKnob) / releaseLaw (4.31)));
    p[c::autoRelease] = 0.0f;
    p[c::autoGain] = 0.0f;
    p[c::mix] = 1.0f;
    p[c::output] = linNorm (31.5 + (inDb + 8.7) + (outDb + 16.9), -36.0, 36.0);
    slot.mix = (float) std::clamp (at (103, 100.0) / 100.0, 0.0, 1.0);
    return 11;
}

// SSL Native Vocalstrip 2 (round 145): its state is XML with named parameters in the original's own units
// (<SSL_PLUGIN_STATE … StateASelected="1"><A><PROCESSOR_STATE><PARAM id="compThreshold" value="-33.2"/> …); the A or B
// setting that plays maps one to one onto Ridge Strip's Vocal strip voicing, which reproduces the official trial
// measured black-box (docs/analise/REA_rodada145.md §2). Other SSL Native strips keep Ridge's defaults.
int translateSslVocalstrip2 (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    namespace r = mixsuite::ridge;
    namespace vs = voxstrip;
    const auto root = findBytes (state, "<SSL_PLUGIN_STATE");
    if (root == std::string::npos || slot.kind != EffectKind::ridgeStrip)
        return 0;
    const std::string text (state.begin() + (std::ptrdiff_t) root, state.end());
    const auto plugin = attributeIn (text, 0, "PluginName");
    const bool k4e = plugin.find ("4K E") != std::string::npos;
    if (plugin.find ("Vocalstrip 2") == std::string::npos && ! k4e)
        return 0;
    const bool useB = attributeIn (text, 0, "StateASelected") == "0";
    const auto block = text.find (useB ? "<B" : "<A");
    if (block == std::string::npos)
        return 0;
    const auto blockEnd = text.find (useB ? "</B>" : "</A>", block);
    std::map<std::string, double> v;
    for (auto q = text.find ("<PARAM ", block); q != std::string::npos && q < blockEnd; q = text.find ("<PARAM ", q + 7))
    {
        const auto id = attributeIn (text, q, "id");
        float value = 0;
        if (! id.empty() && parseFloat (attributeIn (text, q, "value"), value) && std::isfinite (value))
            v[id] = value;
    }
    if (v.size() < 10)
        return 0;
    auto at = [&v] (const char* id, double fallback) {
        const auto it = v.find (id);
        return it != v.end() ? it->second : fallback;
    };
    auto sw = [&at] (const char* id, bool fallback) { return at (id, fallback ? 1.0 : 0.0) >= 0.5 ? 1.0f : 0.0f; };
    auto& p = slot.params;
    if (k4e)
    {
        // round 148: SSL 4K E -> Ridge's 4K voicing, the controls in the original's units through Ridge's own laws
        // (EQ and filter frequencies: LF in Hz, the others in kHz; HighPassFreq 10 = out, LowPassFreq 35 = out; CompRatio
        // 40 = infinity; EqColour 0 / 1 / 2 = Brown / Black / Orange; SaturationIn / Amount = the mic pre and its gain).
        // Not read: pan, width, the external key, filters / EQ to the side chain, filters to input, high quality
        p[r::console4k] = 1.0f;
        p[r::voicing] = 0.0f;
        p[r::vocal] = 0.0f;
        p[r::input] = linNorm (at ("InputTrim", 0.0), -20.0, 20.0);
        const auto fader = at ("FaderLevel", 0.0);
        p[r::output] = linNorm (at ("OutputTrim", 0.0) + std::max (fader, -20.0), -20.0, 20.0);
        p[r::phase] = sw ("PhaseInvert", false);
        const auto hp = at ("HighPassFreq", 10.0), lp = at ("LowPassFreq", 35.0);
        p[r::hpf] = hp < 16.5 ? 0.0f : std::max (1e-4f, logNorm (hp, 16.0, 350.0));
        p[r::lpf] = lp > 15.95 ? 0.0f : std::max (1e-4f, logNorm (lp * 1000.0, 22000.0, 3000.0));
        auto gain = [] (double db) { return linNorm (db, -15.0, 15.0); };
        p[r::lfGain] = gain (at ("LowEqGain", 0.0));
        p[r::lfFreq] = logNorm (at ("LowEqFreq", 200.0), 30.0, 450.0);
        p[r::lfBell] = sw ("LowEqBell", false);
        p[r::lmfGain] = gain (at ("LowMidEqGain", 0.0));
        p[r::lmfFreq] = logNorm (at ("LowMidEqFreq", 1.0) * 1000.0, 200.0, 2500.0);
        p[r::lmfQ] = r::k4QValue (at ("LowMidEqQ", 1.5));
        p[r::hmfGain] = gain (at ("HighMidEqGain", 0.0));
        p[r::hmfFreq] = logNorm (at ("HighMidEqFreq", 3.0) * 1000.0, 600.0, 7000.0);
        p[r::hmfQ] = r::k4QValue (at ("HighMidEqQ", 1.5));
        p[r::hfGain] = gain (at ("HighEqGain", 0.0));
        p[r::hfFreq] = logNorm (at ("HighEqFreq", 8.0) * 1000.0, 1500.0, 16000.0);
        p[r::hfBell] = sw ("HighEqBell", false);
        p[r::eqOn] = sw ("EqIn", true);
        p[r::k4Colour] = r::k4ColourValue ((int) std::lround (at ("EqColour", 0.0)));
        const auto ratio = at ("CompRatio", 1.0);
        p[r::compRatio] = ratio <= 1.0005 ? 0.0f : (ratio >= 30.0 ? 1.0f : (float) std::sqrt (std::clamp ((ratio - 1.0) / 19.0, 0.0, 0.996)));
        p[r::compThreshold] = linNorm (at ("CompThreshold", 10.0), -20.0, 10.0);
        p[r::compRelease] = logNorm (at ("CompRelease", 0.4), 0.1, 4.0);
        p[r::compFastAttack] = sw ("CompFastAttack", false);
        p[r::compMix] = (float) std::clamp (at ("CompMix", 100.0) / 100.0, 0.0, 1.0);
        p[r::autoMakeup] = sw ("CompAutoMakeUp", true);
        p[r::k4MakeupOffset] = linNorm (at ("CompAutoMakeUpCompensation", 0.0), -12.0, 12.0);
        p[r::gateRange] = linNorm (at ("GateRange", 0.0), 0.0, 40.0);
        p[r::gateThreshold] = linNorm (at ("GateThreshold", -30.0), -30.0, 10.0);
        p[r::gateRelease] = logNorm (at ("GateRelease", 0.4), 0.1, 4.0);
        p[r::gateFastAttack] = sw ("GateFastAttack", false);
        p[r::expander] = sw ("GateExpander", false);
        p[r::dynOn] = sw ("DynamicsIn", true);
        p[r::order] = sw ("DynamicsPreEq", false);
        p[r::k4Pre] = sw ("SaturationIn", false);
        p[r::k4Mic] = linNorm (at ("SaturationAmount", 0.0), 0.0, 50.0);
        p[r::analog] = sw ("AnalogueVca", true);
        if (at ("Bypass", 0.0) >= 0.5)
            slot.enabled = false;
        return (int) v.size();
    }
    p[r::vocal] = 1.0f;
    p[r::vInput] = vs::gainValue (at ("inputGain", 0.0));
    p[r::vOutput] = vs::gainValue (at ("outputGain", 0.0));
    p[r::vOrder] = (float) std::clamp (at ("processOrder", 0.0), 0.0, 1.0);
    p[r::vDsOn] = sw ("dsOn", false);
    p[r::vDsFreq] = vs::dsFreqValue (at ("dsFreq", 7500.0));
    p[r::vDsThreshold] = vs::thresholdValue (at ("dsThreshold", -24.0));
    p[r::vDsSplit] = sw ("dsSplit", true);
    p[r::vDsLookahead] = sw ("dsLookAhead", false);
    p[r::vDsListen] = sw ("dsAudition", false);
    p[r::vDpOn] = sw ("dpOn", false);
    p[r::vDpFreq] = vs::dpFreqValue (at ("dpFreq", 120.0));
    p[r::vDpThreshold] = vs::thresholdValue (at ("dpThreshold", -24.0));
    p[r::vDpSplit] = sw ("dpSplit", true);
    p[r::vDpLookahead] = sw ("dpLookAhead", false);
    p[r::vDpListen] = sw ("dpAudition", false);
    p[r::vEqOn] = sw ("eqOn", false);
    p[r::vHpfOn] = sw ("hpfOn", true);
    p[r::vHpfFreq] = vs::hpfValue (at ("hpfFreq", 30.0));
    p[r::vNotchOn] = sw ("notchOn", true);
    p[r::vNotchFreq] = vs::notchValue (at ("notchFreq", 200.0));
    p[r::vNotchGain] = vs::notchGainValue (at ("notchGain", 0.0));
    p[r::vHighOn] = sw ("hsOn", true);
    p[r::vHighFreq] = vs::highValue (at ("hsFreq", 1000.0));
    p[r::vHighGain] = vs::highGainValue (at ("hsGain", 0.0));
    p[r::vCompOn] = sw ("compOn", false);
    p[r::vExpThreshold] = vs::expThresholdValue (at ("expThreshold", -96.0));
    p[r::vCompThreshold] = vs::thresholdValue (at ("compThreshold", 0.0));
    p[r::vRatio] = vs::ratioValue (at ("compRatio", 4.0));
    p[r::vAttack] = vs::attackValue (at ("compAttack", 0.1));
    p[r::vRelease] = vs::releaseValue (at ("compRelease", 100.0));
    p[r::vMakeup] = vs::makeupValue (at ("compMakeup", 0.0));
    p[r::vKnee] = sw ("compKnee", false);
    p[r::vDrive] = sw ("compDrive", false);
    if (at ("bypassAll", 0.0) >= 0.5)
        slot.enabled = false;
    return (int) v.size();
}

// SSL E-Channel (57 values; RW index: 0 comp threshold dB, 1 ratio, 2 fast (1 = on), 3 release s, 5 expander threshold,
// 6 range, 7 gate (1) / expander (0), 8 expander fast attack, 9 expander release, 11 dynamics bypass, 12 dynamics to
// channel out, 13 LF bell, 14/15 LF gain / Hz, 16/17/18 LMF gain / kHz / Q, 19/20/21 HMF, 22 HF bell, 23/24 HF gain / kHz,
// 25 EQ bypass, 26 EQ to dynamics side chain, 27 HP Hz (< 17 = out), 28 LP kHz (30 = out), 29 filter split (1 = on),
// 30 output dB, 31 analog, 39 phase, 40 input dB). Round 123: Ridge Strip's "E" voicing (core/VoiceMatch.h) is the
// console measured black-box on the official trial — every EQ band and filter from its impulse responses, the
// feedback compressor and expander from sine stairs, tone bursts, noise and her takes — so the controls map one to
// one in the console's own units (docs/analise/REA_rodada123.md). Round 147b: analog translated (off in every state of
// her corpus). Round 165: EQ to the dynamics side chain (Dyn S.C., RealWorld 26) translated.
int translateWavesSslChannel (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    namespace r = mixsuite::ridge;
    const auto rw = wavesRealWorldParams (state);
    if (slot.kind != EffectKind::ridgeStrip || rw.size() < 31)
        return 0;
    const auto at = [&] (std::size_t i, double fallback) { return wavesValue (rw, i).value_or (fallback); };
    const auto gain = [] (double db) { return linNorm (db, -15.0, 15.0); };
    auto& p = slot.params;
    p[r::voicing] = 1.0f;
    p[r::curve] = 0.0f;
    p[r::input] = linNorm (at (40, 0.0), -20.0, 20.0);
    p[r::output] = linNorm (at (30, 0.0), -20.0, 20.0);
    p[r::phase] = at (39, 0.0) > 0.5 ? 1.0f : 0.0f;
    const auto hp = at (27, 0.0);
    p[r::hpf] = hp >= 16.5 ? std::max (1e-4f, logNorm (std::min (hp, 350.0), 16.0, 350.0)) : 0.0f;
    const auto lpk = at (28, 30.0);
    // Ridge's low-pass law runs 22 kHz (dial 0) .. 3 kHz (dial 1), log
    p[r::lpf] = lpk < 25.0 ? std::max (1e-4f, (float) std::clamp (std::log (22000.0 / (lpk * 1000.0)) / std::log (22000.0 / 3000.0), 0.0, 1.0)) : 0.0f;
    p[r::lfGain] = gain (at (14, 0.0));
    p[r::lfFreq] = logNorm (at (15, 60.0), 30.0, 450.0);
    p[r::lfBell] = at (13, 0.0) > 0.5 ? 1.0f : 0.0f;
    p[r::lmfGain] = gain (at (16, 0.0));
    p[r::lmfFreq] = logNorm (at (17, 0.5) * 1000.0, 200.0, 2500.0);
    p[r::lmfQ] = (float) (1.0 - vmatch::echannel::dialFromQ (at (18, 2.5)));
    p[r::hmfGain] = gain (at (19, 0.0));
    p[r::hmfFreq] = logNorm (at (20, 3.5) * 1000.0, 600.0, 7000.0);
    p[r::hmfQ] = (float) (1.0 - vmatch::echannel::dialFromQ (at (21, 1.5)));
    p[r::hfGain] = gain (at (23, 0.0));
    p[r::hfFreq] = logNorm (at (24, 8.0) * 1000.0, 1500.0, 16000.0);
    p[r::hfBell] = at (22, 0.0) > 0.5 ? 1.0f : 0.0f;
    p[r::eqOn] = at (25, 0.0) > 0.5 ? 0.0f : 1.0f;
    const auto ratio = std::max (1.0, at (1, 1.0));
    // round 165 [EXEC]: the console's ∞ : 1 is stored as 100 (it played as 20 : 1 here, 3 … 5 dB too loud)
    p[r::compRatio] = ratio >= 99.0 ? 1.0f : (float) std::sqrt (std::clamp ((ratio - 1.0) / 19.0, 0.0, 0.998));
    p[r::compThreshold] = linNorm (at (0, 0.0), -20.0, 10.0);
    p[r::compRelease] = logNorm (std::clamp (at (3, 0.4), 0.1, 4.0), 0.1, 4.0);
    p[r::compFastAttack] = at (2, 2.0) == 1.0 ? 1.0f : 0.0f;
    p[r::gateRange] = linNorm (at (6, 0.0), 0.0, 40.0);
    p[r::gateThreshold] = linNorm (at (5, -30.0), -30.0, 10.0);
    p[r::gateRelease] = logNorm (std::clamp (at (9, 0.2), 0.1, 4.0), 0.1, 4.0);
    p[r::gateFastAttack] = at (8, 0.0) > 0.5 ? 1.0f : 0.0f;
    p[r::expander] = at (7, 0.0) > 0.5 ? 0.0f : 1.0f;
    p[r::dynOn] = at (11, 0.0) > 0.5 ? 0.0f : 1.0f;
    p[r::order] = at (12, 0.0) > 0.5 ? 0.0f : 1.0f; // Ridge: on = dynamics before the EQ
    p[r::filterSplit] = at (29, 1.0) == 1.0 ? 1.0f : 0.0f;
    p[r::filtersToSidechain] = 0.0f;
    p[r::compMix] = 1.0f;
    p[r::analog] = at (31, 0.0) > 0.5 ? 1.0f : 0.0f; // round 147b: ANALOG (its noise floor, measured on the trial)
    p[r::eqToSidechain] = at (26, 0.0) > 0.5 ? 1.0f : 0.0f; // round 165: Dyn S.C. ([EXEC] RealWorld 26 follows the switch)
    return 32;
}

// H-Delay (RealWorld list, round 119): 1 sync (0 BPM, 1 host, 2 ms), 2 BPM, 3 delay ms, 4 note (3 per length: triplet,
// straight, dotted from 1/64; 18 = 1 bar, 19 = 2 bars), 6 mix %, 7 feedback, 10 ping-pong (1) / stereo (0), 14 output dB,
// 15 low-pass Hz (20100 = off), 17 high-pass Hz (19 = off), 19 / 20 modulation depth % / rate Hz, 23 analog (0 = off).
// [EXEC] Measured on the official trial: ping-pong sends the mono sum to the left echo, the right side repeats it one
// time later at nearly the same level (-0.3 dB) and the loop gain per left-right pair is 0.93 x (feedback / 100)^2
// (feedback 30 / 60 / 90: -20.9 / -9.5 / -2.7 dB); stereo mode feeds each side back at 0.98 x feedback / 100 per
// repeat; the echoes are flat (+-0.5 dB, 30 Hz-20 kHz); the first echo is +1.1 dB over the mono sum with the analog
// modes and +2.5 dB without (stereo mode: -1.9 / -0.4 dB over its own side); the mix is an equal-power dry / wet.
int translateWavesHDelay (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    namespace e = echosuite::echoform;
    const auto rw = wavesRealWorldParams (state);
    const auto sync = wavesValue (rw, 1), fb = wavesValue (rw, 7);
    if (slot.kind != EffectKind::echoform || ! sync || ! fb)
        return 0;
    auto& p = slot.params;
    // round 135w: [EXEC] round 119's probe: 1 = PingPong, 2 = Stereo (2 used to read as ping-pong)
    const auto ppValue = wavesValue (rw, 10).value_or (1.0);
    const bool pingPong = ppValue >= 0.5 && ppValue < 1.5;
    const bool analog = wavesValue (rw, 23).value_or (2.0) >= 0.5;
    p[e::style] = e::styleValue (pingPong ? e::Style::pingPong : e::Style::single);
    p[e::pairs] = pingPong ? 1.0f : 0.0f;
    // round 129: phase L / R (RealWorld 8 / 9, round 119 §2 [EXEC]; 1 = inverted)
    p[e::phaseL] = wavesValue (rw, 8).value_or (0.0) >= 0.5 ? 1.0f : 0.0f;
    p[e::phaseR] = wavesValue (rw, 9).value_or (0.0) >= 0.5 ? 1.0f : 0.0f;
    // round 135w: the ping-pong's start side (RealWorld 45: 0 left, 1 right — round 119 probe [EXEC])
    p[e::startSide] = wavesValue (rw, 45).value_or (0.0) >= 0.5 ? 1.0f : 0.0f;
    const auto note = (int) std::lround (wavesValue (rw, 4).value_or (13.0));
    const auto ownTempo = wavesValue (rw, 2);
    // round 135w: H-Delay's own BPM (sync 0) keeps its note and tempo (was folded into milliseconds)
    const bool ownBpmSync = *sync < 0.5 && ownTempo && *ownTempo >= 30.0 && *ownTempo <= 300.0;
    p[e::tempoSource] = ownBpmSync ? 1.0f : 0.0f;
    if (ownBpmSync)
        p[e::ownBpm] = e::bpmValue (*ownTempo);
    if ((ownBpmSync || (*sync >= 0.5 && *sync < 1.5)) && note >= 0 && note <= 19)
    {
        // host tempo (or its own): the note follows that tempo
        const auto index = note >= 18 ? note - 12 : note / 3;  // 1/64 … 1/2, 1 bar, 2 bars
        const auto kind = note >= 18 ? 1 : (note % 3 == 0 ? 3 : (note % 3 == 1 ? 1 : 2)); // triplet, straight, dotted
        for (const auto side : { e::syncL, e::syncR })
            p[(std::size_t) side] = ((float) kind + 1.5f) / 4.0f;
        for (const auto side : { e::noteL, e::noteR })
            p[(std::size_t) side] = ((float) index + 0.5f) / 10.0f;
    }
    else
    {
        // its own BPM or milliseconds: the time it computed is in the list
        const auto ms = std::clamp (wavesValue (rw, 3).value_or (375.0), 1.0, 3500.0);
        for (const auto side : { e::syncL, e::syncR })
            p[(std::size_t) side] = 0.0f;
        for (const auto side : { e::timeL, e::timeR })
            p[(std::size_t) side] = e::timeValue (ms);
    }
    const auto f = std::max (0.0, *fb) / 100.0;
    // round 124: the hybrid engine reproduces the trial's loop exactly (REA_rodada124 §5) when the state uses no
    // filter, modulation or lo-fi (every state of her corpus); otherwise the round-119 approximation below
    const auto hp0 = wavesValue (rw, 17).value_or (19.0), lp0 = wavesValue (rw, 15).value_or (20100.0);
    if (hp0 <= 19.5 && lp0 >= 20000.0 && wavesValue (rw, 19).value_or (0.0) <= 0.0 && wavesValue (rw, 21).value_or (0.0) < 0.5)
    {
        p[e::hybrid] = e::hybridValue (analog ? 2 : 1);
        p[e::feedback] = (float) std::clamp (f / 2.0, 0.0, 1.0);
        p[e::mix] = (float) std::clamp (wavesValue (rw, 6).value_or (100.0) / 100.0, 0.0, 1.0);
        p[e::output] = linNorm (wavesValue (rw, 14).value_or (0.0), -18.0, 18.0);
        return 9;
    }
    const auto loop = pingPong ? 0.93 * f * f : 0.98 * f;
    p[e::feedback] = (float) std::clamp (loop / 2.0, 0.0, 1.0);
    p[e::mix] = (float) std::clamp (wavesValue (rw, 6).value_or (100.0) / 100.0, 0.0, 1.0);
    // the analog modes also compress loud input (1 kHz bursts: +3 dB at -34 dBFS, 0 at -10, -2.3 at -4): on her
    // three vocal takes the echoes of the reference are 2.5 dB under the small-signal gain, hence -1.4 instead of +1.1
    const auto level = pingPong ? (analog ? -1.4 : 2.5) : (analog ? -4.4 : -0.4);
    p[e::output] = linNorm (wavesValue (rw, 14).value_or (0.0) + level, -18.0, 18.0);
    p[e::mode] = e::modeValue (e::Mode::clean);
    p[e::age] = 0.0f;
    p[e::drive] = 0.0f;
    p[e::diffusion] = 0.0f;
    p[e::ducking] = 0.0f;
    p[e::width] = 1.0f;
    p[e::spread] = 0.5f;
    // round 137: LoFi (RealWorld 21, on = 1) → Echoform's own LoFi switch (the same in-loop low-pass, spec §4.2)
    p[e::lofi] = wavesValue (rw, 21).value_or (0.0) >= 0.5 ? 1.0f : 0.0f;
    const auto hp = wavesValue (rw, 17).value_or (19.0), lp = wavesValue (rw, 15).value_or (20100.0);
    p[e::lowCut] = hp > 19.5 ? linNorm (hp, 10.0, 2000.0) : 0.0f;
    // Echoform's high cut acts like a one-pole at about half its label below 80 % (REA_rodada101 §3.3)
    p[e::highCut] = lp < 20000.0 ? linNorm (2.0 * lp, 200.0, 20000.0) : 1.0f;
    const auto depth = wavesValue (rw, 19).value_or (0.0);
    p[e::modDepth] = (float) std::clamp (std::sqrt (std::max (0.0, depth) / 100.0), 0.0, 1.0);
    if (depth > 0.0)
        p[e::modRate] = (float) std::clamp (std::pow ((std::clamp (wavesValue (rw, 20).value_or (0.1), 0.01, 10.0) - 0.01) / 9.99, 1.0 / 2.3277), 0.0, 1.0);
    return 8;
}

// NS1 (RealWorld list, round 119): value 229 is the one knob, 0 … 100 %; Quell's Amount follows the same scale
// (fitted to the official trial, docs/analise/REA_rodada119.md §3).
int translateWavesNs1 (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    const auto rw = wavesRealWorldParams (state);
    const auto amount = wavesValue (rw, 229);
    if (slot.kind != EffectKind::quellDenoiser || ! amount)
        return 0;
    slot.params[voicetools::quell::amount] = (float) std::clamp (*amount / 100.0, 0.0, 1.0);
    slot.params[voicetools::quell::matched] = 1.0f; // round 124: the engine fitted to the trial (REA_rodada124 §6)
    return 2;
}

// Doubler (RealWorld list, round 119): 0 output dB, 1 direct on, 2 direct gain dB, 3 direct pan deg, 4 align,
// 9 / 10 / 11 low shelf on / gain dB / Hz, 12 / 13 / 14 high shelf Hz / gain dB / on, 15 feedback high-pass Hz,
// 16 / 17 voice 1 / 2 source (0 own side, 1 sum, 2 difference), 18 range (0 = 80 Hz), then a block of 10 per voice from
// 26: on, gain dB, pan deg, octave, detune cents, delay ms, feedback %, depth cents, rate. [EXEC] Gemini's laws are the
// measured ones (equal-power voice pan, stereo rotation of the direct signal, exact detune, octave down, LFO at
// rate / 2 Hz, feedback = %/100 per repeat, the aligned direct signal 6.7 ms late), so the values map one to one.
int translateWavesDoubler (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    namespace g = voicetools::gemini;
    const auto rw = wavesRealWorldParams (state);
    if (slot.kind != EffectKind::geminiDoubler || rw.size() < 45 || ! wavesValue (rw, 31))
        return 0;
    const auto at = [&] (std::size_t i, double fallback) { return wavesValue (rw, i).value_or (fallback); };
    const auto flag = [&] (std::size_t i, double fallback) { return at (i, fallback) >= 0.5 ? 1.0f : 0.0f; };
    auto& p = slot.params;
    p[g::output] = linNorm (at (0, 0.0), -24.0, 0.0);
    p[g::directOn] = flag (1, 1.0);
    p[g::directGain] = linNorm (at (2, 0.0), -48.0, 0.0);
    p[g::directPan] = linNorm (at (3, 0.0), -45.0, 45.0);
    p[g::align] = flag (4, 1.0);
    p[g::lowOn] = flag (9, 0.0);
    // round 165w: the measured shelves take the reference's whole range (−88 … +12 dB: the deep cuts too) one to one
    p[g::eqLaw] = 1.0f;
    p[g::lowGain] = g::shelfValue (at (10, 0.0));
    p[g::lowFreq] = g::freqValue (at (11, 100.0));
    p[g::highFreq] = g::freqValue (at (12, 5000.0));
    p[g::highGain] = g::shelfValue (at (13, 0.0));
    p[g::highOn] = flag (14, 0.0);
    p[g::feedbackCut] = g::freqValue (at (15, 16.0));
    p[g::range] = flag (18, 0.0);
    // round 135w: a four-voice doubler's voices 3 and 4 (blocks 46 / 56, the same layout as 26 / 36 [model]: a two-voice
    // state carries those blocks too, so they are read only when the state names the four-voice plug-in; their source
    // indices are not probed yet: own side)
    const bool four = wavesPluginName (state).find ("Doubler4") != std::string::npos;
    for (int v = 0; v < (four ? 4 : 2); ++v)
    {
        const auto b = (std::size_t) (26 + 10 * v);
        const auto param = [&] (g::VoiceParam q) -> float& { return p[(std::size_t) g::voiceParam (v, q)]; };
        param (g::vOn) = flag (b, 1.0);
        param (g::vGain) = linNorm (at (b + 1, 0.0), -48.0, 0.0);
        param (g::vPan) = linNorm (at (b + 2, v % 2 == 0 ? -45.0 : 45.0), -45.0, 45.0);
        param (g::vOctave) = flag (b + 3, 0.0);
        param (g::vDetune) = linNorm (at (b + 4, 0.0), -100.0, 100.0);
        param (g::vDelay) = linNorm (at (b + 5, 10.0), 0.0, 100.0);
        param (g::vFeedback) = linNorm (at (b + 6, 0.0), 0.0, 100.0);
        param (g::vDepth) = linNorm (at (b + 7, 0.0), -200.0, 200.0);
        param (g::vRate) = logNorm (std::clamp (at (b + 8, 1.0), 0.1, 400.0), 0.1, 400.0);
        param (g::vSource) = v >= 2 ? 1.0f / 6.0f : ((float) std::clamp ((int) std::lround (at (16 + (std::size_t) v, 0.0)), 0, 2) + 0.5f) / 3.0f;
    }
    return 33;
}

// S1 Imager (RealWorld list, round 124): 0 gain dB, 1 width (0 … 3, the plug-in keeps two decimals), 4 rotation deg
// (±45), 5 asymmetry deg (±90), 13 swap, 14 / 15 invert left / right, 16 M/S input. [EXEC] Black-box on the official
// trial (REA_rodada124_extras §4): a frequency-flat 2 × 2 matrix, gain · rotation · width · input. Width w below 1
// scales the side signal (mid 1, side w); above 1 the side / mid ratio is w with mid = √(0.625 + 0.375 / w²) (exact on
// 41 measured points); rotation φ is the plain rotation [cos −sin; sin cos]. Fruity Stereo Shaper (LYNX's exact engine)
// holds any 2 × 2 matrix, so the translation nulls the reference. Asymmetry, swap / invert and M/S input are not
// measured beyond their neutral settings (all neutral in her states): a state that uses them is not translated.
int translateWavesS1Imager (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    const auto rw = wavesRealWorldParams (state);
    const auto width = wavesValue (rw, 1);
    if (slot.kind != EffectKind::stereoShaper || ! width)
        return 0;
    const auto at = [&] (std::size_t i) { return wavesValue (rw, i).value_or (0.0); };
    if (std::abs (at (5)) > 1e-6 || at (13) >= 0.5 || at (14) >= 0.5 || at (15) >= 0.5 || at (16) >= 0.5)
        return 0;
    const auto w = std::clamp (std::round (*width * 100.0) / 100.0, 0.0, 3.0);
    const auto mid = w <= 1.0 ? 1.0 : std::sqrt (0.625 + 0.375 / (w * w));
    const auto side = w * mid;
    const auto a = 0.5 * (mid + side), b = 0.5 * (mid - side);
    const auto phi = std::clamp (at (4), -45.0, 45.0) * 3.14159265358979323846 / 180.0;
    const auto c = std::cos (phi), s = std::sin (phi);
    const auto g = std::pow (10.0, std::clamp (at (0), -24.0, 0.0) / 20.0);
    // rotation · width: [c −s; s c] · [a b; b a]
    const double m00 = g * (c * a - s * b), m01 = g * (c * b - s * a), m10 = g * (s * a + c * b), m11 = g * (s * b + c * a);
    // the Shaper's knob law: |gain| = (3^|x| − 1) / 2 with x = raw / 12800
    const auto raw = [] (double gain) {
        const auto x = std::log (2.0 * std::abs (gain) + 1.0) / std::log (3.0);
        return std::clamp ((int) std::lround ((gain < 0.0 ? -x : x) * 12800.0), -25600, 25600);
    };
    const auto unit = [] (int r) { return (float) (r + 25600) / 51200.0f; };
    auto& p = slot.params;
    p[0] = unit (raw (m01)); // R → L
    p[1] = unit (raw (m00)); // L
    p[2] = unit (raw (m11)); // R
    p[3] = unit (raw (m10)); // L → R
    p[4] = 0.5f;             // no delay
    p[5] = 0.5f;             // no phase
    p[6] = 0.0f;
    p[7] = 0.0f;
    return 4;
}

// --- Soundtoys (round 135): her states, read black-box in the official trials -------------------------------------
// A Soundtoys state is text ("WIDGET = …; PRESET =<encoded>;"); the payload is NOT decoded. Each state found in her
// projects was loaded into the official trial through the host API and every parameter read back
// (docs/analise/REA_rodada135s.md §1); the table maps the payload's FNV-1a hash to those settings in LYNX's units.
struct KnownState
{
    std::uint64_t hash;
    std::vector<std::pair<int, float>> values;
};
namespace vs = voiceshift;
namespace em = mixsuite::ember;
namespace ap = autopan;
namespace st135
{
#include "core/SoundtoysStates.inc"
}

std::optional<std::uint64_t> soundtoysPayloadHash (const std::vector<std::uint8_t>& state)
{
    static constexpr char key[] = "PRESET =";
    const auto* b = state.data();
    const auto n = state.size();
    for (std::size_t i = 0; i + sizeof (key) - 1 <= n; ++i)
        if (std::memcmp (b + i, key, sizeof (key) - 1) == 0)
        {
            auto start = i + sizeof (key) - 1, end = start;
            while (end < n && b[end] != ';') ++end;
            if (end >= n) return std::nullopt;
            while (start < end && std::isspace (b[start])) ++start;
            while (end > start && std::isspace (b[end - 1])) --end;
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (auto k = start; k < end; ++k) { h ^= b[k]; h *= 0x100000001b3ull; }
            return h;
        }
    return std::nullopt;
}

template <std::size_t N>
int translateKnown (const KnownState (&table)[N], EffectKind kind, const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    if (slot.kind != kind) return 0;
    const auto h = soundtoysPayloadHash (state);
    if (! h) return 0;
    for (const auto& k : table)
        if (k.hash == *h)
        {
            for (const auto& [p, v] : k.values)
                if (p >= 0 && p < (int) slot.params.size()) slot.params[(std::size_t) p] = v;
            return (int) k.values.size();
        }
    return 0;
}

[[maybe_unused]] int translateAlterBoy (const std::vector<std::uint8_t>& s, EffectSlot& slot) { return translateKnown (st135::alterBoyStates, EffectKind::voiceShift, s, slot); }
[[maybe_unused]] int translateDecapitator (const std::vector<std::uint8_t>& s, EffectSlot& slot) { return translateKnown (st135::decapitatorStates, EffectKind::ember, s, slot); }
[[maybe_unused]] int translatePanMan (const std::vector<std::uint8_t>& s, EffectSlot& slot) { return translateKnown (st135::panManStates, EffectKind::autoPan, s, slot); }
} // namespace

int knownSoundtoysStates() noexcept { return (int) (std::size (st135::alterBoyStates) + std::size (st135::decapitatorStates) + std::size (st135::panManStates)); }
std::optional<std::uint64_t> soundtoysStateHash (const std::vector<std::uint8_t>& state) { return soundtoysPayloadHash (state); }

namespace
{
// ---- Soundtoys (round 136): the encoded preset is not decoded; her saved states are recognised by its hash
namespace st136
{
#include "core/SoundtoysStates136.inc"
}
using namespace st136;

std::uint64_t presetHash (const std::vector<std::uint8_t>& state)
{
    const auto at = findBytes (state, "PRESET =");
    if (at == std::string::npos)
        return 0;
    std::size_t end = at + 8;
    while (end < state.size() && state[end] != '\r')
        ++end;
    if (end > at + 8 && state[end - 1] == ';')
        --end;
    std::uint64_t h = 0xcbf29ce484222325ull; // FNV-1a 64
    for (auto i = at + 8; i < end; ++i)
        h = (h ^ state[i]) * 0x100000001b3ull;
    return h;
}

int translateSoundtoysDecapitator (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    namespace e = mixsuite::ember;
    if (slot.kind != EffectKind::ember)
        return 0;
    const auto h = presetHash (state);
    for (const auto& st : decapitatorStates)
    {
        if (st.hash != h)
            continue;
        static constexpr int order[10] { e::classicStyle, e::classicDrive, e::classicPunish, e::classicLowCut, e::classicThump,
                                         e::classicTone, e::classicHighCut, e::classicSteep, e::classicMix, e::classicOutput };
        slot.params[(std::size_t) e::voicing] = 1.0f;
        for (int i = 0; i < 10; ++i)
            slot.params[(std::size_t) order[i]] = st.v[i];
        slot.mix = 1.0f;
        return 10;
    }
    return 0;
}

int translateSoundtoysPanMan (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    if (slot.kind != EffectKind::autoPan || slot.params.size() < (std::size_t) autopan::count + 12)
        return 0;
    const auto h = presetHash (state);
    for (const auto& st : panManStates)
    {
        if (st.hash != h)
            continue;
        for (int i = 0; i < 12; ++i)
            slot.params[(std::size_t) (autopan::count + i)] = st.v[i]; // round 136's Rhythm voicing params
        slot.mix = 1.0f;
        return 12;
    }
    return 0;
}

int translateSoundtoysAlterBoy (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    namespace t = lynxtune;
    if (slot.kind != EffectKind::lynxTune || slot.params.size() < (std::size_t) t::count)
        return 0;
    const auto h = presetHash (state);
    for (const auto& st : alterBoyStates)
    {
        if (st.hash != h)
            continue;
        const double pitch = -12.0 + 24.0 * st.v[0], formant = -12.0 + 24.0 * st.v[1];
        const int mode = std::clamp ((int) std::lround (st.v[4] * 2.0), 0, 2); // transpose, quantize, robot
        auto& p = slot.params;
        p[(std::size_t) t::scale] = 0.0f; // chromatic
        p[(std::size_t) t::retune] = 0.0f;
        p[(std::size_t) t::humanize] = 0.0f;
        p[(std::size_t) t::flexTune] = 0.0f;
        for (int k = 0; k < 12; ++k)
        {
            p[(std::size_t) (t::noteC + k)] = 1.0f;
            p[(std::size_t) (t::bypassC + k)] = mode == 0 ? 1.0f : 0.0f; // Transpose: no correction
        }
        p[(std::size_t) t::transpose] = (float) ((std::clamp (std::lround (pitch), -12L, 12L) + 12) / 24.0);
        const bool wide = std::abs (formant) > 7.0;
        p[(std::size_t) t::wideFormant] = wide ? 1.0f : 0.0f;
        p[(std::size_t) t::formantShift] = (float) std::clamp (formant / (wide ? 24.0 : 14.0) + 0.5, 0.0, 1.0);
        p[(std::size_t) t::formantPreserve] = st.v[5] >= 0.5f ? 0.0f : 1.0f; // formant link: the formants follow the pitch
        p[(std::size_t) t::driveStage] = 1.0f;
        p[(std::size_t) t::drive] = st.v[2];
        p[(std::size_t) t::mix] = st.v[3];
        p[(std::size_t) t::outputTrim] = 0.5f;
        slot.mix = 1.0f;
        return 8;
    }
    return 0;
}

int translateSoundtoysEchoBoy (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    namespace e = echosuite::echoform;
    if (slot.kind != EffectKind::echoform || slot.params.size() < (std::size_t) e::paramCount)
        return 0;
    const auto h = presetHash (state);
    for (const auto& st : echoBoyStates)
    {
        if (st.hash != h)
            continue;
        auto& p = slot.params;
        p[(std::size_t) e::tapeVoicing] = e::tapeVoicingValue (st.voicing);
        p[(std::size_t) e::syncL] = p[(std::size_t) e::syncR] = 0.625f; // a note at the project tempo
        p[(std::size_t) e::noteL] = p[(std::size_t) e::noteR] = st.note;
        p[(std::size_t) e::feedback] = st.loopGain / 2.0f;              // feedbackGain = 2 v
        p[(std::size_t) e::mix] = st.mix;
        p[(std::size_t) e::output] = 0.5f;
        slot.mix = 1.0f;
        return 6;
    }
    return 0;
}

RawTranslator rawTranslatorNamed (std::string_view name)
{
    if (name == "soundtoys-echoboy")
        return translateSoundtoysEchoBoy;
    if (name == "soundtoys-littlealterboy")
        return translateSoundtoysAlterBoy;
    if (name == "soundtoys-decapitator")
        return translateSoundtoysDecapitator;
    if (name == "soundtoys-panman")
        return translateSoundtoysPanMan;    if (name == "waves-s1-imager")
        return translateWavesS1Imager;
    if (name == "waves-h-delay")
        return translateWavesHDelay;
    if (name == "waves-ns1")
        return translateWavesNs1;
    if (name == "waves-doubler")
        return translateWavesDoubler;
    if (name == "waves-ssl-channel")
        return translateWavesSslChannel;
    if (name == "ssl-vocalstrip-2")
        return translateSslVocalstrip2;
    if (name == "auto-tune-pro")
        return translateAutoTunePro;
    if (name == "waves-tune")
        return translateWavesTune;
    if (name == "fresh-air")
        return translateFreshAir;
    if (name == "waves-rcomp")
        return translateWavesRComp;
    if (name == "waves-deesser")
        return translateWavesDeEsser;
    if (name == "waves-cla-2a")
        return translateWavesCla2a;
    if (name == "waves-cla-76")
        return translateWavesCla76;
    return nullptr;
}

// the value of attribute `name` in the element starting at `from` ("" when absent)
std::string attributeIn (const std::string& text, std::size_t from, const std::string& name)
{
    const auto end = text.find ('>', from);
    const auto key = " " + name + "=\"";
    const auto at = text.find (key, from);
    if (at == std::string::npos || (end != std::string::npos && at > end))
        return {};
    const auto start = at + key.size();
    const auto close = text.find ('"', start);
    return close == std::string::npos ? std::string() : text.substr (start, close - start);
}
} // namespace

std::optional<std::pair<int, int>> autoTuneProKey (const std::vector<std::uint8_t>& state)
{
    const auto at = findBytes (state, "<Custom ");
    if (at == std::string::npos || findBytes (state, "<SessionChunkData") == std::string::npos)
        return std::nullopt;
    const std::string text (state.begin() + (std::ptrdiff_t) at, state.begin() + (std::ptrdiff_t) std::min (state.size(), at + 4096));
    float key = -1, scale = -1;
    if (! parseFloat (attributeIn (text, 0, "ScaleGivenKey"), key) || ! parseFloat (attributeIn (text, 0, "ScaleGivenScale"), scale))
        return std::nullopt;
    if (key < 0 || key > 11 || key != std::floor (key))
        return std::nullopt;
    // [EXEC] the scale list's first entries (the official trial's own texts, REA round 120 §5): 0 = Major, 1 = Minor,
    // 2 = Chromatic — as read from her takes in round 113 §2. The other scales (tunings) are not translated.
    // Round 138 (REA_rodada138 §3.1): a state that carries <ModernScales> (saved by a newer Auto-Tune Pro) stores the
    // index of the MODERN scale list instead (0 Chromatic, 1 Major, 2 Minor, 3 Harmonic Minor, 4 Jazz Melodic Minor,
    // 5 Dorian, 6 Phrygian, 7 Lydian, 8 Mixolydian, 9 Locrian, 10 Major Pentatonic, 11 Minor Pentatonic, 12 Blues,
    // 13 Whole Tone, 14 Diminished): [EXEC] her 42 states with "2" load into the official trial as Minor, the one with
    // "1" as Major (the old reading took them for Chromatic / Minor)
    vocal::Scale lynxScale;
    if (findBytes (state, "<ModernScales") != std::string::npos)
    {
        using S = vocal::Scale;
        static constexpr S modern[15] { S::chromatic, S::major, S::minor, S::harmonicMinor, S::melodicMinor, S::dorian, S::phrygian,
                                        S::lydian, S::mixolydian, S::locrian, S::majorPentatonic, S::minorPentatonic, S::blues,
                                        S::wholeTone, S::diminished };
        if (scale < 0.0f || scale > 14.0f || scale != std::floor (scale))
            return std::nullopt;
        lynxScale = modern[(int) scale];
    }
    else if (scale == 0.0f)
        lynxScale = vocal::Scale::major;
    else if (scale == 1.0f)
        lynxScale = vocal::Scale::minor;
    else if (scale == 2.0f)
        lynxScale = vocal::Scale::chromatic;
    else
        return std::nullopt; // the other scales: not confirmed
    return std::pair<int, int> { (int) key, (int) lynxScale };
}

std::optional<std::pair<std::uint16_t, std::uint16_t>> autoTuneProNoteEdits (const std::vector<std::uint8_t>& state, int key, int lynxScale)
{
    // [DATA] <Scales><Scale ScaleIndex="n" Length="7|12"><Bypass Values="0100000"/><Remove Values="…"/>: one digit per
    // scale degree (major / minor: 7 degrees above the root; chromatic: 12 semitones above the root)
    // round 138: a <ModernScales> state keeps one 12-digit row per modern scale (semitones above the root; Remove = the
    // notes that are not targets, the scale's own non-members included)
    if (const auto ms = findBytes (state, "<ModernScales"); ms != std::string::npos)
    {
        using S = vocal::Scale;
        static constexpr S modern[15] { S::chromatic, S::major, S::minor, S::harmonicMinor, S::melodicMinor, S::dorian, S::phrygian,
                                        S::lydian, S::mixolydian, S::locrian, S::majorPentatonic, S::minorPentatonic, S::blues,
                                        S::wholeTone, S::diminished };
        int mi = -1;
        for (int k = 0; k < 15; ++k)
            if ((int) modern[k] == lynxScale)
                mi = k;
        const auto at = mi < 0 ? std::string::npos : findBytes (state, "<Scale ScaleIndex=\"" + std::to_string (mi) + "\"", ms);
        if (at == std::string::npos)
            return std::nullopt;
        const std::string text (state.begin() + (std::ptrdiff_t) at, state.begin() + (std::ptrdiff_t) std::min (state.size(), at + 600));
        auto mask = [&] (const char* element) -> std::optional<std::uint16_t> {
            const auto e = text.find (element);
            if (e == std::string::npos)
                return std::nullopt;
            const auto v = attributeIn (text, e, "Values");
            if (v.size() != 12u)
                return std::nullopt;
            std::uint16_t m = 0;
            for (int d = 0; d < 12; ++d)
                if (v[(std::size_t) d] == '1')
                    m = (std::uint16_t) (m | (1u << ((key + d) % 12)));
            return m;
        };
        const auto bypass = mask ("<Bypass "), remove = mask ("<Remove ");
        if (! bypass || ! remove)
            return std::nullopt;
        return std::pair<std::uint16_t, std::uint16_t> { *bypass, *remove };
    }
    const int index = lynxScale == (int) vocal::Scale::major ? 0 : lynxScale == (int) vocal::Scale::minor ? 1 : lynxScale == (int) vocal::Scale::chromatic ? 2 : -1;
    if (index < 0)
        return std::nullopt;
    const auto at = findBytes (state, "<Scale ScaleIndex=\"" + std::to_string (index) + "\"");
    if (at == std::string::npos)
        return std::nullopt;
    const std::string text (state.begin() + (std::ptrdiff_t) at, state.begin() + (std::ptrdiff_t) std::min (state.size(), at + 600));
    static constexpr int major[7] { 0, 2, 4, 5, 7, 9, 11 }, minor[7] { 0, 2, 3, 5, 7, 8, 10 };
    auto mask = [&] (const char* element) -> std::optional<std::uint16_t> {
        const auto e = text.find (element);
        if (e == std::string::npos)
            return std::nullopt;
        const auto v = attributeIn (text, e, "Values");
        const auto n = index == 2 ? 12u : 7u;
        if (v.size() != n)
            return std::nullopt;
        std::uint16_t m = 0;
        for (std::size_t d = 0; d < n; ++d)
            if (v[d] == '1')
                m = (std::uint16_t) (m | (1u << ((key + (index == 2 ? (int) d : index == 0 ? major[d] : minor[d])) % 12)));
        return m;
    };
    const auto bypass = mask ("<Bypass "), remove = mask ("<Remove ");
    if (! bypass || ! remove)
        return std::nullopt;
    return std::pair<std::uint16_t, std::uint16_t> { *bypass, *remove };
}

std::map<int, double> autoTuneProParams (const std::vector<std::uint8_t>& state)
{
    std::map<int, double> out;
    const auto at = findBytes (state, "ParamsData=\"");
    if (at == std::string::npos || findBytes (state, "<SessionChunkData") == std::string::npos)
        return out;
    const auto start = at + 12;
    const auto end = findBytes (state, "\"", start);
    if (end == std::string::npos)
        return out;
    // JUCE MemoryBlock::toBase64Encoding: "<size>.<chars>", 6 bits per char, least significant bit first
    const std::string text (state.begin() + (std::ptrdiff_t) start, state.begin() + (std::ptrdiff_t) end);
    const auto dot = text.find ('.');
    if (dot == std::string::npos || dot == 0 || dot > 9)
        return out;
    std::size_t size = 0;
    for (std::size_t i = 0; i < dot; ++i)
    {
        if (text[i] < '0' || text[i] > '9')
            return out;
        size = size * 10 + (std::size_t) (text[i] - '0');
    }
    if (size > (1u << 20))
        return out;
    static constexpr const char* alphabet = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+";
    std::string raw (size, '\0');
    std::size_t bit = 0;
    for (std::size_t i = dot + 1; i < text.size() && bit < size * 8; ++i)
    {
        const auto* c = std::strchr (alphabet, text[i]);
        if (c == nullptr || text[i] == '\0')
            return {};
        const auto v = (unsigned) (c - alphabet);
        for (int k = 0; k < 6 && bit < size * 8; ++k, ++bit)
            if ((v >> k) & 1u)
                raw[bit >> 3] = (char) (raw[bit >> 3] | (1 << (bit & 7)));
    }
    // <Parameter X<n>="<value>"/> entries
    for (auto p = raw.find (" X"); p != std::string::npos; p = raw.find (" X", p + 2))
    {
        std::size_t q = p + 2;
        int n = 0, digits = 0;
        while (q < raw.size() && raw[q] >= '0' && raw[q] <= '9' && digits < 4)
            n = n * 10 + (raw[q++] - '0'), ++digits;
        if (digits == 0 || q + 1 >= raw.size() || raw[q] != '=' || raw[q + 1] != '"')
            continue;
        const auto close = raw.find ('"', q + 2);
        if (close == std::string::npos)
            break;
        float v = 0;
        if (parseFloat (raw.substr (q + 2, close - q - 2), v))
            out[n] = (double) v;
    }
    return out;
}

std::optional<PitchCorrectorScale> wavesTuneScale (const std::vector<std::uint8_t>& state)
{
    const auto start = findBytes (state, "<PresetChunkXMLTree");
    if (start == std::string::npos)
        return std::nullopt;
    const std::string xml (state.begin() + (std::ptrdiff_t) start, state.end());
    std::string setup = "SETUP_A";
    if (const auto a = xml.find ("<ActiveSetup>"); a != std::string::npos)
        if (const auto b = xml.find ("</ActiveSetup>", a); b != std::string::npos)
            setup = trim (std::string_view (xml).substr (a + 13, b - a - 13));
    const auto data = xml.find ("<PresetData Setup=\"" + setup + "\"");
    if (data == std::string::npos)
        return std::nullopt;
    const auto end = std::min (xml.find ("</PresetData>", data), xml.size());
    const auto scaleAt = xml.find ("<Scale ", data);
    const auto segAt = xml.find ("<NoteSegmentation>", data);
    const auto segEnd = xml.find ("</NoteSegmentation>", data);
    const auto rootAt = xml.find ("<Root>", data);
    if (scaleAt >= end || segAt >= end || segEnd >= end || rootAt >= end)
        return std::nullopt;
    int root = -1;
    try
    {
        root = std::stoi (xml.substr (rootAt + 6, 4));
    }
    catch (...)
    {
        return std::nullopt;
    }
    if (root < 0 || root > 11)
        return std::nullopt;
    // the twelve notes above the root, in order (NoteCents 0, 100 … 1100): "x" = excluded, anything else = a target
    std::uint16_t rel = 0;
    int count = 0;
    for (auto p = xml.find ("<NoteStatus>", segAt); p < segEnd && count < 12; p = xml.find ("<NoteStatus>", p + 1), ++count)
    {
        const auto q = xml.find ("</NoteStatus>", p);
        if (q == std::string::npos || q > segEnd)
            return std::nullopt;
        if (trim (std::string_view (xml).substr (p + 12, q - p - 12)) != "x")
            rel = (std::uint16_t) (rel | (1u << count));
    }
    if (count != 12 || rel == 0)
        return std::nullopt;
    PitchCorrectorScale out;
    out.key = root;
    out.mask = (std::uint16_t) (((rel << root) | (rel >> (12 - root))) & 0xfff);
    out.scale = (int) vocal::Scale::custom;
    for (int s = 0; s < vocal::scaleCount; ++s)
        if (s != (int) vocal::Scale::custom && vocal::scaleMask ((vocal::Scale) s, root) == out.mask)
        {
            out.scale = s;
            break;
        }
    return out;
}

std::optional<PitchCorrectorScale> pitchCorrectorScale (const std::vector<std::uint8_t>& state)
{
    if (const auto k = autoTuneProKey (state))
        return PitchCorrectorScale { k->first, k->second, vocal::scaleMask ((vocal::Scale) k->second, k->first) };
    if (findBytes (state, "<AUTOTUNEEFXPLUS") != std::string::npos) // Auto-Tune EFX+ (round 127)
    {
        EffectSlot tune = makeEffectSlot (EffectKind::lynxTune);
        if (translateAutoTuneEfx (state, tune) == 0)
            return std::nullopt;
        std::uint16_t mask = 0;
        for (int k = 0; k < 12; ++k)
            if (tune.params[(std::size_t) (lynxtune::noteC + k)] >= 0.5f)
                mask = (std::uint16_t) (mask | (1u << k));
        return PitchCorrectorScale { lynxtune::keyIndex (tune.params[lynxtune::key]), lynxtune::scaleIndex (tune.params[lynxtune::scale]), mask };
    }
    return wavesTuneScale (state);
}

std::optional<std::pair<float, float>> freshAirAmounts (const std::vector<std::uint8_t>& state)
{
    // [DATA] the component state of every Fresh Air slot surveyed (round 113): 2530 bytes, u32 1, u32 size − 8, then
    // two floats 0..1 at 1290 / 1294; [EXEC] the official trial saves its "Mid Air" / "High Air" parameters there
    if (state.size() != 2530)
        return std::nullopt;
    auto u32 = [&state] (std::size_t at) {
        std::uint32_t v = 0;
        std::memcpy (&v, state.data() + at, 4);
        return v;
    };
    auto f32 = [&state] (std::size_t at) {
        float v = 0;
        std::memcpy (&v, state.data() + at, 4);
        return v;
    };
    if (u32 (0) != 1 || u32 (4) != (std::uint32_t) state.size() - 8)
        return std::nullopt;
    const auto mid = f32 (1290), high = f32 (1294);
    if (! std::isfinite (mid) || ! std::isfinite (high) || mid < 0.0f || mid > 1.0f || high < 0.0f || high > 1.0f)
        return std::nullopt;
    return std::pair<float, float> { mid, high };
}


PluginIdentity pluginIdentity (const ForeignPlugin& plugin)
{
    PluginIdentity id;
    id.format = plugin.format;
    id.name = plugin.name;
    id.path = plugin.path;
    id.vst2Id = plugin.vst2Id;
    id.classId = plugin.classId;
    id.hasClassId = plugin.hasClassId;
    id.clapId = plugin.clapId;
    if (plugin.format == ForeignPlugin::Format::vst3)
    {
        std::vector<std::uint8_t> component, controller;
        if (splitFlVst3State (plugin.state, component, controller))
            id.state = std::move (component);
    }
    else if (plugin.format == ForeignPlugin::Format::clap)
    {
        if (plugin.state.size() > 4) // u32 0 + the plugin's own state (REA_rodada22 §2.5)
            id.state.assign (plugin.state.begin() + 4, plugin.state.end());
    }
    else
        id.state = plugin.state;
    return id;
}

std::vector<SubstituteEntry> parseSubstituteTable (std::string_view text, int* errors)
{
    std::vector<SubstituteEntry> entries;
    int bad = 0;
    std::optional<SubstituteEntry> current;
    std::string lynxName;
    std::vector<std::string> presetItems, mapItems;
    bool entryBad = false;

    auto finish = [&] {
        if (! current)
            return;
        const auto kind = effectKindNamed (lynxName);
        if (! kind || entryBad || current->key.empty())
        {
            ++bad;
            current.reset();
            return;
        }
        current->effect = *kind;
        for (const auto& item : presetItems)
        {
            const auto eq = item.rfind ('=');
            float v = 0;
            const auto index = eq == std::string::npos ? -1 : paramNamed (*kind, item.substr (0, eq));
            if (index < 0 || ! parseFloat (trim (item.substr (eq + 1)), v))
            {
                ++bad;
                continue;
            }
            current->preset.push_back ({ index, std::clamp (v, 0.0f, 1.0f) });
        }
        for (const auto& item : mapItems)
        {
            SubstituteParamMap m;
            if (parseMap (item, *kind, m))
                current->params.push_back (std::move (m));
            else
                ++bad;
        }
        if (current->product.empty())
            current->product = current->key;
        entries.push_back (std::move (*current));
        current.reset();
    };

    std::size_t start = 0;
    while (start <= text.size())
    {
        auto end = text.find ('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        const auto line = trim (text.substr (start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            finish();
            current = SubstituteEntry {};
            current->key = trim (std::string_view (line).substr (1, line.size() - 2));
            lynxName.clear();
            presetItems.clear();
            mapItems.clear();
            entryBad = false;
            continue;
        }
        const auto eq = line.find ('=');
        if (! current || eq == std::string::npos)
        {
            ++bad;
            continue;
        }
        const auto key = lowerAscii (trim (std::string_view (line).substr (0, eq)));
        const auto value = trim (std::string_view (line).substr (eq + 1));
        auto& e = *current;
        if (key == "product")
            e.product = value;
        else if (key == "vendor")
            e.vendor = value;
        else if (key == "role")
        {
            if (lowerAscii (value) == "instrument")
                e.instrument = true;
            else if (lowerAscii (value) != "effect")
                entryBad = true;
        }
        else if (key == "lynx")
            lynxName = value;
        else if (key == "vst2")
        {
            for (const auto& item : splitList (value, ';'))
            {
                std::uint32_t id = 0;
                if (parseVst2Id (item, id))
                    e.vst2Ids.push_back (id);
                else
                    ++bad;
            }
        }
        else if (key == "vst3")
        {
            for (const auto& item : splitList (value, ';'))
            {
                std::array<std::uint8_t, 16> id {};
                if (parseHex16 (item, id))
                    e.vst3ClassIds.push_back (id);
                else
                    ++bad;
            }
        }
        else if (key == "clap")
            for (const auto& item : splitList (value, ';'))
                e.clapIds.push_back (item);
        else if (key == "names")
            for (const auto& item : splitList (value, ';'))
            {
                if (auto k = matchKey (item); ! k.empty())
                    e.names.push_back (std::move (k));
            }
        else if (key == "waves")
            for (const auto& item : splitList (value, ';'))
            {
                if (auto k = matchKey (item); ! k.empty())
                    e.wavesNames.push_back (std::move (k));
            }
        else if (key == "preset")
            for (auto& item : splitList (value, ';'))
                presetItems.push_back (std::move (item));
        else if (key == "mix")
        {
            float v = 1;
            if (parseFloat (value, v))
                e.mix = std::clamp (v, 0.0f, 1.0f);
            else
                ++bad;
        }
        else if (key == "map")
            mapItems.push_back (value);
        else if (key == "translate")
        {
            if (fabfilter::translatorNamed (value) != nullptr || rawTranslatorNamed (value) != nullptr)
                e.translator = value;
            else
                ++bad;
        }
        else if (key == "vst2params")
        {
            float v = 0;
            if (parseFloat (value, v) && v >= 1 && v <= 100000 && v == std::floor (v))
                e.vst2Params = (int) v;
            else
                ++bad;
        }
        else
            ++bad;
    }
    finish();
    if (errors != nullptr)
        *errors = bad;
    return entries;
}

const std::vector<SubstituteEntry>& pluginSubstitutes()
{
    return registry();
}

int registerPluginSubstitutes (std::string_view tableText)
{
    auto added = parseSubstituteTable (tableText);
    auto& entries = registry();
    for (auto& entry : added)
    {
        entries.erase (std::remove_if (entries.begin(), entries.end(), [&] (const SubstituteEntry& e) { return e.key == entry.key; }), entries.end());
        // later registrations win: they go first, so findPluginSubstitute meets them before the built-in ones
        entries.insert (entries.begin(), std::move (entry));
    }
    return (int) added.size();
}

const SubstituteEntry* findPluginSubstitute (const PluginIdentity& plugin, bool instrument)
{
    const auto& entries = registry();
    auto candidates = [&] (auto&& matches) -> const SubstituteEntry* {
        for (const auto& e : entries)
            if (e.instrument == instrument && matches (e))
                return &e;
        return nullptr;
    };
    // 1) format identifiers
    if (plugin.vst2Id != 0 && (plugin.format == ForeignPlugin::Format::vst2 || plugin.format == ForeignPlugin::Format::none))
        if (auto* e = candidates ([&] (const SubstituteEntry& x) { return std::find (x.vst2Ids.begin(), x.vst2Ids.end(), plugin.vst2Id) != x.vst2Ids.end(); }))
            return e;
    if (plugin.hasClassId)
        if (auto* e = candidates ([&] (const SubstituteEntry& x) { return std::find (x.vst3ClassIds.begin(), x.vst3ClassIds.end(), plugin.classId) != x.vst3ClassIds.end(); }))
            return e;
    if (! plugin.clapId.empty())
        if (auto* e = candidates ([&] (const SubstituteEntry& x) { return std::find (x.clapIds.begin(), x.clapIds.end(), plugin.clapId) != x.clapIds.end(); }))
            return e;
    // 2) a WaveShell sub-plugin, by the name its own state carries
    const auto waves = matchKey (wavesPluginName (plugin.state));
    if (! waves.empty())
        if (auto* e = candidates ([&] (const SubstituteEntry& x) { return std::find (x.wavesNames.begin(), x.wavesNames.end(), waves) != x.wavesNames.end(); }))
            return e;
    // 3) names (never the shell's file name: it holds many plugins)
    std::vector<std::string> keys;
    const auto nameKey = matchKey (plugin.name);
    if (nameKey.rfind ("waveshell", 0) == 0)
        return nullptr;
    if (! nameKey.empty())
        keys.push_back (nameKey);
    {
        auto file = plugin.path;
        std::replace (file.begin(), file.end(), '\\', '/');
        if (const auto slash = file.rfind ('/'); slash != std::string::npos)
            file = file.substr (slash + 1);
        if (const auto dot = file.rfind ('.'); dot != std::string::npos)
            file = file.substr (0, dot);
        if (auto k = matchKey (file); ! k.empty() && k.rfind ("waveshell", 0) != 0)
            keys.push_back (std::move (k));
    }
    for (const auto& key : keys)
        if (auto* e = candidates ([&] (const SubstituteEntry& x) {
                return std::find (x.names.begin(), x.names.end(), key) != x.names.end()
                       || (! waves.empty() && std::find (x.wavesNames.begin(), x.wavesNames.end(), key) != x.wavesNames.end());
            }))
            return e;
    return nullptr;
}

EffectSlot makeSubstituteSlot (const SubstituteEntry& entry, const PluginIdentity* original, int* translated)
{
    auto slot = makeEffectSlot (entry.effect);
    for (const auto& [index, value] : entry.preset)
        if (index >= 0 && index < maxEffectParams)
            slot.params[(std::size_t) index] = value;
    slot.mix = entry.mix;

    int count = 0;
    if (original != nullptr && ! original->state.empty() && ! entry.params.empty())
    {
        std::optional<FlVst2State> vst2;
        std::vector<std::pair<std::string, std::string>> attributes;
        std::vector<double> waves;
        bool attributesRead = false, wavesRead = false;
        for (const auto& m : entry.params)
        {
            std::optional<double> value;
            switch (m.source)
            {
                case SubstituteParamMap::Source::vst2:
                {
                    if (! vst2)
                    {
                        FlVst2State s;
                        vst2 = parseFlVst2State (original->state, s) ? s : FlVst2State {}; // a chunk / unreadable: no params
                    }
                    if (vst2->params.empty())
                        break;
                    // all programs saved: the current one's block (needs the plugin's parameter count)
                    std::size_t base = 0, perProgram = vst2->params.size();
                    if (vst2->allPrograms())
                    {
                        const auto n = (std::size_t) entry.vst2Params;
                        if (n > 0 && vst2->params.size() % n == 0 && (std::size_t) vst2->currentProgram < vst2->params.size() / n)
                        {
                            perProgram = n;
                            base = (std::size_t) std::max (0, vst2->currentProgram) * n;
                        }
                        else if (n > 0 && vst2->params.size() >= n && vst2->currentProgram == 0)
                            perProgram = n;
                        else
                            break; // which block is current is not known
                    }
                    if ((std::size_t) m.index < perProgram)
                        value = (double) vst2->params[base + (std::size_t) m.index];
                    break;
                }
                case SubstituteParamMap::Source::xml:
                {
                    if (! attributesRead)
                    {
                        attributes = juceXmlStateAttributes (original->state);
                        attributesRead = true;
                    }
                    for (const auto& [name, text] : attributes)
                        if (name == m.attribute)
                        {
                            float f = 0;
                            if (parseFloat (text, f))
                                value = (double) f;
                            break;
                        }
                    break;
                }
                case SubstituteParamMap::Source::waves:
                {
                    if (! wavesRead)
                    {
                        waves = wavesRealWorldParams (original->state);
                        wavesRead = true;
                    }
                    if ((std::size_t) m.index < waves.size() && std::isfinite (waves[(std::size_t) m.index]))
                        value = waves[(std::size_t) m.index];
                    break;
                }
            }
            if (! value)
                continue;
            const auto v = mapValue (m, *value);
            if (m.toSlotMix)
                slot.mix = v;
            else if (m.target >= 0 && m.target < maxEffectParams)
                slot.params[(std::size_t) m.target] = v;
            ++count;
        }
    }
    if (original != nullptr && ! original->state.empty() && ! entry.translator.empty())
        if (const auto translate = rawTranslatorNamed (entry.translator))
            count += translate (original->state, slot);
    if (original != nullptr && ! original->state.empty() && ! entry.translator.empty())
        if (const auto translate = fabfilter::translatorNamed (entry.translator))
            if (const auto state = fabfilter::parseState (original->state))
                count += translate (*state, slot);
    // round 135: the header shows the original's preset name (LYNX engines only: an FL effect's preset box lists FL's
    // own presets)
    if (original != nullptr && ! original->state.empty() && flPluginsForEffect (slot.kind).empty())
        slot.presetName = originalPresetName (original->state);
    if (translated != nullptr)
        *translated = count;
    return slot;
}

std::string originalPresetName (const std::vector<std::uint8_t>& state)
{
    if (const auto chunk = findBytes (state, "<PresetChunkXMLTree"); chunk != std::string::npos)
    {
        const auto a = findBytes (state, "<Preset Name=\"", chunk);
        if (a == std::string::npos)
            return {};
        const auto start = a + 14;
        const auto b = findBytes (state, "\"", start);
        if (b == std::string::npos || b - start > 200)
            return {};
        return xmlUnescape (trim (std::string (state.begin() + (std::ptrdiff_t) start, state.begin() + (std::ptrdiff_t) b)));
    }
    for (const auto& [name, value] : juceXmlStateAttributes (state))
        if (name == "presetName")
            return trim (value);
    return {};
}

bool substituteEffectSlot (EffectSlot& slot, const PluginIdentity& identity)
{
    return substituteEffectSlot (slot, identity, SubstituteContext {});
}

bool substituteEffectSlot (EffectSlot& slot, const PluginIdentity& identity, const SubstituteContext& context)
{
    if (slot.kind != EffectKind::plugin)
        return false;
    const auto* entry = findPluginSubstitute (identity, false);
    if (entry == nullptr)
        return false;
    int translated = 0;
    auto replacement = makeSubstituteSlot (*entry, &identity, &translated);
    // a pitch corrector whose own key is unknown: the project's key (its other pitch correctors), with the entry's
    // gentle retune; no key anywhere: the entry's chromatic preset
    if (replacement.kind == EffectKind::lynxTune && context.knowsKey() && ! pitchCorrectorScale (identity.state))
    {
        replacement.params[lynxtune::key] = (float) context.key / 11.0f;
        replacement.params[lynxtune::scale] = (float) context.scale / (float) (vocal::scaleCount - 1);
    }
    replacement.enabled = slot.enabled;
    replacement.mix = std::clamp (slot.mix * replacement.mix, 0.0f, 1.0f); // nested dry/wet crossfades multiply
    replacement.sidechain = slot.sidechain;
    replacement.sidechain2 = slot.sidechain2;
    replacement.substituteFor = slot.plugin;
    if (replacement.substituteFor.name.empty())
        replacement.substituteFor.name = identity.name;
    slot = std::move (replacement);
    return true;
}

SubstituteContext substituteContextOf (const Song& song)
{
    std::vector<std::pair<std::pair<int, int>, int>> votes; // (key, scale) -> slots
    auto look = [&votes] (const ForeignPlugin& plugin) {
        if (plugin.format == ForeignPlugin::Format::none)
            return;
        const auto sc = pitchCorrectorScale (pluginIdentity (plugin).state);
        if (sc && sc->scale != (int) vocal::Scale::custom)
        {
            const std::pair<int, int> k { sc->key, sc->scale };
            auto it = std::find_if (votes.begin(), votes.end(), [&] (const auto& v) { return v.first == k; });
            if (it == votes.end())
                votes.push_back ({ k, 1 });
            else
                ++it->second;
        }
    };
    for (const auto& track : song.mixer)
        for (const auto& fx : track.effects)
        {
            look (fx.plugin.foreign);
            look (fx.substituteFor.foreign);
        }
    SubstituteContext context;
    int best = 0;
    for (const auto& [k, n] : votes)
        if (n > best)
        {
            best = n;
            context.key = k.first;
            context.scale = k.second;
        }
    return context;
}

bool restoreOriginalPlugin (EffectSlot& slot)
{
    if (! slot.isSubstitute())
        return false;
    EffectSlot original;
    original.kind = EffectKind::plugin;
    original.enabled = slot.enabled;
    original.mix = 1.0f; // the substitute's own dry/wet doesn't belong to the plugin; FL's slot mix is not kept
    original.sidechain = slot.sidechain;
    original.sidechain2 = slot.sidechain2;
    original.plugin = std::move (slot.substituteFor);
    slot = std::move (original);
    return true;
}

std::string substituteOriginalName (const EffectSlot& slot)
{
    if (! slot.isSubstitute())
        return {};
    if (! slot.substituteFor.name.empty())
        return slot.substituteFor.name;
    if (! slot.substituteFor.foreign.name.empty())
        return slot.substituteFor.foreign.name;
    return slot.substituteFor.id;
}

std::vector<std::pair<std::string, std::string>> juceXmlStateAttributes (const std::vector<std::uint8_t>& state)
{
    std::vector<std::pair<std::string, std::string>> attributes;
    auto start = findBytes (state, "VC2!");
    start = findBytes (state, "<?xml", start == std::string::npos ? 0 : start);
    if (start == std::string::npos)
        return attributes;
    // the first element after the declaration
    std::size_t i = start + 5;
    while (i < state.size() && state[i] != '>')
        ++i;
    while (i < state.size() && state[i] != '<')
        ++i;
    if (i >= state.size())
        return attributes;
    std::size_t end = i;
    char quote = 0;
    for (; end < state.size(); ++end)
    {
        const auto c = (char) state[end];
        if (quote != 0)
        {
            if (c == quote)
                quote = 0;
        }
        else if (c == '"' || c == '\'')
            quote = c;
        else if (c == '>')
            break;
        else if (c == 0)
            return attributes;
    }
    const std::string tag (state.begin() + (std::ptrdiff_t) i + 1, state.begin() + (std::ptrdiff_t) end);
    // skip the element name, then name="value" pairs
    std::size_t p = 0;
    while (p < tag.size() && ! std::isspace ((unsigned char) tag[p]) && tag[p] != '/')
        ++p;
    while (p < tag.size())
    {
        while (p < tag.size() && (std::isspace ((unsigned char) tag[p]) || tag[p] == '/'))
            ++p;
        const auto nameStart = p;
        while (p < tag.size() && tag[p] != '=' && ! std::isspace ((unsigned char) tag[p]))
            ++p;
        const auto name = tag.substr (nameStart, p - nameStart);
        while (p < tag.size() && std::isspace ((unsigned char) tag[p]))
            ++p;
        if (p >= tag.size() || tag[p] != '=')
            break;
        ++p;
        while (p < tag.size() && std::isspace ((unsigned char) tag[p]))
            ++p;
        if (p >= tag.size() || (tag[p] != '"' && tag[p] != '\''))
            break;
        const auto q = tag[p++];
        const auto close = tag.find (q, p);
        if (close == std::string::npos)
            break;
        attributes.push_back ({ name, xmlUnescape (tag.substr (p, close - p)) });
        p = close + 1;
    }
    return attributes;
}

std::string wavesPluginName (const std::vector<std::uint8_t>& state)
{
    const auto chunk = findBytes (state, "<PresetChunkXMLTree");
    if (chunk == std::string::npos)
        return {};
    const auto a = findBytes (state, "<PluginName>", chunk);
    if (a == std::string::npos)
        return {};
    const auto b = findBytes (state, "</PluginName>", a);
    if (b == std::string::npos)
        return {};
    return xmlUnescape (trim (std::string (state.begin() + (std::ptrdiff_t) a + 12, state.begin() + (std::ptrdiff_t) b)));
}

std::vector<double> wavesRealWorldParams (const std::vector<std::uint8_t>& state)
{
    std::vector<double> values;
    const auto text = wavesParametersText (state);
    std::size_t p = 0;
    while (p < text.size())
    {
        while (p < text.size() && std::isspace ((unsigned char) text[p]))
            ++p;
        const auto start = p;
        while (p < text.size() && ! std::isspace ((unsigned char) text[p]))
            ++p;
        if (p == start)
            break;
        const auto token = text.substr (start, p - start);
        if (token == "*")
        {
            values.push_back (std::numeric_limits<double>::quiet_NaN());
            continue;
        }
        try
        {
            std::size_t used = 0;
            const auto v = std::stod (token, &used);
            values.push_back (used == token.size() ? v : std::numeric_limits<double>::quiet_NaN());
        }
        catch (...)
        {
            values.push_back (std::numeric_limits<double>::quiet_NaN());
        }
    }
    return values;
}

std::string foreignToText (const ForeignPlugin& plugin)
{
    std::vector<std::uint8_t> out;
    putU32 (out, foreignTextMagic);
    putU32 (out, (std::uint32_t) plugin.format);
    putU32 (out, plugin.vst2Id);
    out.insert (out.end(), plugin.classId.begin(), plugin.classId.end());
    putU32 (out, plugin.hasClassId ? 1u : 0u);
    putString (out, plugin.clapId);
    putString (out, plugin.name);
    putString (out, plugin.path);
    putBytes (out, plugin.state.data(), plugin.state.size());
    putU32 (out, (std::uint32_t) plugin.midiInPort);
    putU32 (out, (std::uint32_t) plugin.midiOutPort);
    return base64Encode (out.data(), out.size());
}

bool foreignFromText (const std::string& text, ForeignPlugin& out)
{
    std::vector<std::uint8_t> data;
    if (! base64Decode (text, data))
        return false;
    BinReader r { data };
    if (r.u32() != foreignTextMagic)
        return false;
    ForeignPlugin p;
    const auto format = r.u32();
    if (format > (std::uint32_t) ForeignPlugin::Format::other)
        return false;
    p.format = (ForeignPlugin::Format) format;
    p.vst2Id = r.u32();
    if (r.pos + 16 > data.size())
        return false;
    std::copy (data.begin() + (std::ptrdiff_t) r.pos, data.begin() + (std::ptrdiff_t) r.pos + 16, p.classId.begin());
    r.pos += 16;
    p.hasClassId = r.u32() != 0;
    p.clapId = r.str();
    p.name = r.str();
    p.path = r.str();
    p.state = r.bytes();
    p.midiInPort = (int) r.u32();
    p.midiOutPort = (int) r.u32();
    if (! r.ok)
        return false;
    out = std::move (p);
    return true;
}

std::string base64Encode (const std::uint8_t* data, std::size_t size)
{
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve ((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3)
    {
        const std::uint32_t b0 = data[i], b1 = i + 1 < size ? data[i + 1] : 0, b2 = i + 2 < size ? data[i + 2] : 0;
        const auto v = (b0 << 16) | (b1 << 8) | b2;
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += i + 1 < size ? table[(v >> 6) & 63] : '=';
        out += i + 2 < size ? table[v & 63] : '=';
    }
    return out;
}

bool base64Decode (const std::string& text, std::vector<std::uint8_t>& out)
{
    out.clear();
    std::uint32_t buffer = 0;
    int bits = 0, padding = 0;
    for (const auto c : text)
    {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') { ++padding; continue; }
        else if (std::isspace ((unsigned char) c)) continue;
        else return false;
        if (padding > 0)
            return false; // data after padding
        buffer = (buffer << 6) | (std::uint32_t) v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back ((std::uint8_t) (buffer >> bits));
        }
    }
    return padding <= 2;
}
std::uint64_t soundtoysPresetHash (const std::vector<std::uint8_t>& state) { return presetHash (state); }

} // namespace lynx
