// Classic opto leveler laws (round 123), fitted black-box on the official CLA-2A trial at her state (gain 48.96, peak
// reduction 62.6, compress, Hi Freq 50) — stairs, bursts, pink noise and her takes, raw and at their place in her chain
// (after the SSL / NS1 / RComp / DeEsser trials) — and swept dials (tools/fxmeasure/py/vm123, dynfit "cla5").
// Included by VoiceMatch.cpp.
constexpr double y0Db = -33.33189, ytDb = -34.16184, gamma = 2.147392; // C = ((level - Yt) / Y0)^gamma above Yt (peak, dBFS)
constexpr double attackMs = 12.43423, attackBeta = 0.3359726;         // log-domain attack, faster as C grows: tau / (1 + C)^beta
constexpr double releaseA = 6.083167, releaseN = 1.560733;            // dC/dt = -a C^n
constexpr double slowShare = 0.6198693, attackSlowMs = 27033.11, releaseA2 = 16.65784, releaseN2 = 3.937137;
constexpr double h2 = 0.01176161, h3 = 0.02052627;                        // after the make-up
constexpr double tiltC0 = 0.4374144, tiltC1 = 0.4175737, tiltHz = 6850.834; // H = g (1 + s/wz) / (1 + s g'/wz)
constexpr double scHz = 8560.034, scQ = 0.3806853, scDb = 8.317298;      // side-chain emphasis (peaking)
constexpr double memGain = 2.321769, memUpMs = 162.5647, memDownMs = 1.0;
constexpr double limitGamma = 12.0, limitShiftDb = 0.88;               // Limit: a much steeper cell law [model: only her
                                                                        // peak reduction measured]
// round 152 [EXEC] (REA_rodada152a §2): Limit fitted on the trial's up / down 1 kHz stairs at peak reduction 46 / 62.6 / 80
// (rms 0.34 / 0.52 / 0.50 dB, round 123's Limit 0.37 / 1.00 / 1.32): the cell's exponent, the level shift, the share of
// the input added to the side chain (so loud passages can be turned down below the threshold: the trial's
// over-compression at 62.6) and the cell's deepest reduction (the trial stops at 30.4 dB)
constexpr double limitGamma152 = 10.6297, limitShiftDb152 = 0.8281, limitFeedForward = 0.015, limitFloorDb = 30.44;
// AUTO MAKEUP [EXEC]: the trial's gain change below the threshold with Auto Makeup on, per peak reduction dial (dB,
// relative to the dial's default 46, where it was switched on; a −40 dBFS tone, fresh instances)
constexpr double amKnob[] = { 0, 10, 20, 30, 40, 46, 50, 55, 60, 62.6, 65, 70, 75, 80, 85, 90, 95, 100 };
constexpr double amGain[] = { -1.48, -1.32, -1.17, -1.01, -0.86, 0.0, 2.85, 3.55, 6.07, 6.78, 7.42, 8.73, 10.05, 11.37, 12.69, 14.01, 15.33, 16.65 };
// the unit's idle response at 48 kHz (3rd order: a 4 Hz high-pass and the ~0.7 dB dip around 12 kHz), measured
constexpr double idleB[4] = { 0.94821, -1.13885, 0.19868, -0.00757 }, idleA[4] = { 1.0, -1.21822, 0.20333, 0.0158 };
constexpr double idleMakeupDb = 16.014902 - 15.98; // the idle filter's mid-band loss, folded into the make-up

// gain dial -> level below the threshold (dB, 1 kHz), measured at every point listed
constexpr double gainKnob[] = { 0, 10, 20, 30, 40, 45, 48.56, 48.96, 49.6, 49.84, 50, 50.16, 55, 60, 70, 80, 90, 100 };
constexpr double gainLevel[] = { -51.36, -34.95, -18.05, -2.55, 8.45, 12.65, 15.64, 15.98, 16.51, 16.72, 16.85, 16.98, 20.75, 24.66, 29.67, 33.68, 35.19, 38.70 };
// peak reduction dial -> 1 kHz input level giving 3 dB of reduction (dB) at Hi Freq 50; below 45 the curve leaves
// 0 dBFS (no reduction up to full scale: extrapolated at the measured slope)
constexpr double prKnob[] = { 35, 40, 45, 45.72, 50, 55, 60.16, 60.48, 60.6, 61.16, 61.28, 62.6, 63.08, 70, 80, 90, 100 };
constexpr double prLevel[] = { 12.4, 2.6, -7.19, -8.59, -16.97, -20.05, -23.33, -23.53, -23.62, -23.96, -24.03, -24.78, -25.07, -29.37, -34.05, -37.27, -40.47 };
constexpr double herLevel = -24.78;
// Hi Freq dial -> shift of the same level (dB), measured at peak reduction 62.6 and 80 (the same shifts)
constexpr double hfKnob[] = { 0, 25, 50, 75, 100 };
constexpr double hfShift[] = { 6.15, 3.07, 0.0, -2.77, -5.63 };

double table (const double* xs, const double* ys, int n, double x) noexcept
{
    if (x <= xs[0]) return ys[0] + (ys[1] - ys[0]) * (x - xs[0]) / (xs[1] - xs[0]);
    for (int i = 0; i < n - 1; ++i)
        if (x <= xs[i + 1])
            return ys[i] + (ys[i + 1] - ys[i]) * (x - xs[i]) / (xs[i + 1] - xs[i]);
    return ys[n - 1];
}
} // namespace

double autoMakeupDb (double peakReduction) noexcept { return table (amKnob, amGain, (int) std::size (amKnob), std::clamp (peakReduction, 0.0, 100.0)); }
double makeupDb (double gainKnob_) noexcept { return table (gainKnob, gainLevel, (int) std::size (gainKnob), std::clamp (gainKnob_, 0.0, 100.0)); }
double gainForMakeupDb (double db) noexcept { return std::clamp (table (gainLevel, gainKnob, (int) std::size (gainKnob), db), 0.0, 100.0); }
double sensitivityDb (double peakReduction, double hiFreq) noexcept
{
    const double pr = table (prKnob, prLevel, (int) std::size (prKnob), std::clamp (peakReduction, 0.0, 100.0)) - herLevel;
    return pr + table (hfKnob, hfShift, (int) std::size (hfKnob), std::clamp (hiFreq, 0.0, 100.0));
}
namespace
{
