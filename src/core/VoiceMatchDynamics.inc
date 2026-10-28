// E-type console dynamics laws (round 123), fitted black-box on the official SSL E-Channel trial (feedback
// compressor model; per setting: sine stairs, 1 kHz bursts, pink noise and her take; tmp/vm123/py/sslcfg3.py, gendyn.py).
// Grid: console threshold x (1 - 1/ratio); per node: threshold offset (dB), knee (dB), slope, fast attack (ms),
// release factor (x release knob, at 0.35 s) and the console's automatic make-up (dB, measured on the stairs).
// Round 165 [EXEC]: the ∞ : 1 column (u = 1) refitted on the trial's stairs (thresholds +10 … −20, fast attack): the old fit
// compressed like ~2.5 : 1 above the threshold (2 … 5 dB too loud); now 0.15 … 0.5 dB rms (offset, knee, slope, make-up)
constexpr double compThr[8] = { 10, 5, 0, -1.3, -5, -10, -15, -20 };
constexpr double compU[8] = { 0.166667, 0.333333, 0.444444, 0.5, 0.666667, 0.75, 0.875, 1 };
constexpr double compGrid[384] = { -16.4329, 2.24688, 0.0222197, 1.35149, 0.149149, 2.37267e-07, -18.0403, 2.21003, 0.106763, 1.40502, 0.130786, -2.55531e-05, -19.5449, 2.01588, 0.297107, 1.60967, 0.129974, 2.37267e-07, -20.3805, 1.88595, 0.452373, 1.77539, 0.13077, -2.55531e-05, -22.297, 1.67775, 1.15534, 2.53819, 0.134143, -2.55531e-05, -22.8436, 1.67554, 1.68521, 3.06044, 0.135908, -2.55531e-05, -23.4703, 1.55119, 3.99344, 5.14932, 0.14048, -2.55531e-05, -26.0645, 0.506822, 11.1798, 3.47661, 0.136803, 0, -15.3004, 2.37954, 0.0266502, 1.36963, 0.131008, 0.149521, -15.9005, 2.81872, 0.134008, 1.38613, 0.118383, 0.326156, -17.3496, 2.73351, 0.372782, 1.61395, 0.118797, 0.463132, -18.357, 2.56852, 0.558166, 1.80523, 0.120862, 0.538724, -21.0222, 2.05014, 1.30629, 2.64813, 0.128823, 0.750115, -21.8984, 1.86441, 1.83047, 3.25241, 0.132378, 0.82805, -23.0535, 1.48157, 3.87702, 5.54576, 0.140805, 1.00584, -25.6159, 0.360664, 12.9696, 3.71992, 0.13463, 1.2471, -12.5598, 3.32698, 0.0352423, 1.27486, 0.111394, 0.246369, -13.1107, 3.79454, 0.168706, 1.35455, 0.10488, 0.549748, -15.437, 3.41886, 0.430145, 1.63251, 0.106963, 0.794683, -17.0157, 3.02581, 0.611423, 1.85473, 0.109938, 0.933598, -20.349, 2.09028, 1.34398, 2.73878, 0.123933, 1.3364, -21.2447, 1.85343, 1.89066, 3.39228, 0.130131, 1.49004, -22.1564, 1.55424, 4.21804, 6.06878, 0.141665, 1.8481, -25.373, 0.234754, 11.1915, 3.93205, 0.133717, 2.30292, -11.5312, 3.76166, 0.0383367, 1.2633, 0.107275, 0.271194, -12.572, 3.97696, 0.174776, 1.357, 0.101454, 0.606828, -15.1247, 3.52504, 0.437611, 1.64856, 0.103742, 0.879158, -16.7002, 3.11986, 0.622449, 1.86136, 0.10769, 1.03415, -20.1274, 2.12315, 1.36088, 2.76166, 0.123016, 1.48566, -21.0189, 1.88173, 1.92076, 3.435, 0.129711, 1.65866, -21.8877, 1.60007, 4.33178, 6.223, 0.141877, 2.06305, -25.2807, 0.207989, 11.2525, 4.01057, 0.133364, 2.59987, -9.09739, 4.70544, 0.0455849, 1.23977, 0.0954781, 0.349064, -11.0103, 4.54283, 0.192054, 1.37048, 0.0919938, 0.783561, -14.0332, 3.88894, 0.464556, 1.66091, 0.0967676, 1.13814, -15.8742, 3.34972, 0.648845, 1.88216, 0.101924, 1.34073, -19.4016, 2.28716, 1.42306, 2.84461, 0.120164, 1.93419, -20.2399, 2.06963, 2.03958, 3.59566, 0.128407, 2.16277, -21.2441, 1.67348, 4.51607, 6.5481, 0.144298, 2.69899, -24.8679, 0.2, 12.2304, 4.22708, 0.13353, 3.4356, -7.09401, 5.3358, 0.0511387, 1.23035, 0.0829187, 0.492764, -8.85908, 5.31327, 0.214546, 1.37057, 0.0823974, 1.09952, -12.4984, 4.42982, 0.500868, 1.68803, 0.0891982, 1.58939, -14.4448, 3.86483, 0.698553, 1.93244, 0.0952017, 1.86722, -18.484, 2.51565, 1.49432, 2.99339, 0.115885, 2.67282, -19.449, 2.17065, 2.13281, 3.81773, 0.126214, 2.9801, -20.545, 1.665, 4.66344, 6.98471, 0.145652, 3.69623, -24.0058, 0.2, 13.0215, 4.51598, 0.1327, 4.65885, -5.26519, 5.88282, 0.0558554, 1.25178, 0.0741205, 0.74771, -6.67827, 6.10984, 0.236219, 1.38325, 0.0753916, 1.63088, -10.8791, 5.0433, 0.537559, 1.74093, 0.0832009, 2.31576, -13.3088, 4.2457, 0.732027, 2.02398, 0.0888754, 2.69372, -17.838, 2.49859, 1.52295, 3.13529, 0.113235, 3.75068, -18.8917, 2.03648, 2.16249, 4.00105, 0.126057, 4.14035, -19.9559, 1.53962, 4.71775, 7.34891, 0.147789, 5.02932, -23.1542, 0.2, 13.4622, 4.75542, 0.133778, 6.25711, -2.71453, 6.75594, 0.0625043, 1.21375, 0.0668748, 1.39429, -4.44049, 6.9382, 0.257793, 1.41059, 0.0694958, 2.84047, -9.79665, 5.33222, 0.557852, 1.83671, 0.0775121, 3.83831, -12.6893, 4.17373, 0.741327, 2.12317, 0.0845324, 4.34965, -17.4608, 1.93886, 1.51579, 3.20679, 0.112047, 5.65682, -18.3524, 1.68369, 2.17944, 4.07097, 0.126748, 6.10205, -19.2656, 1.43999, 4.79388, 7.45886, 0.149089, 7.07981, -22.2829, 0.216531, 14.2631, 4.8431, 0.135223, 8.5656 };
constexpr double relKnob[7] = { 0.1, 0.2, 0.35, 0.6, 1.2, 2, 4 };
constexpr double relFracFast[7] = { 0, 0.914342, 0.896424, 0.808953, 0.66682, 0.560619, 0.418667 };
constexpr double relTau1Fast[7] = { 117.207, 28.8146, 35.3817, 38.955, 39.807, 39.3687, 35.8225 };
constexpr double relSlowFast[7] = { 21.7157, 130.549, 345.537, 554.889, 895.607, 1205.7, 1572.54 };
constexpr double relFracSlow[7] = { 0.806063, 0.829173, 0.757915, 0.710803, 0.627265, 0.551749, 0.492526 };
constexpr double relTau1Slow[7] = { 43.2801, 57.754, 66.5323, 73.0382, 72.5349, 61.1902, 46.9343 };
constexpr double relSlowSlow[7] = { 236.966, 198.175, 223.307, 383.07, 803.984, 1255.01, 2129.01 };
constexpr double attSlowMs[7] = { 149.238, 132.59, 128.615, 127.158, 131.235, 141.618, 170.628 };
constexpr double refAttackFast = 3.81773, refReleaseFactor = 0.126214;

struct CompLaw
{
    double thresholdOffset, knee, slope, attackFastMs, attackSlowMs, releaseMs, release1Ms, release1Fraction, detectorAttackMs, detectorReleaseMs, makeup;
};

double knobInterp (const double* xs, const double* ys, int n, double x) noexcept
{
    if (x <= xs[0]) return ys[0];
    for (int i = 0; i < n - 1; ++i)
        if (x <= xs[i + 1])
            return ys[i] + (ys[i + 1] - ys[i]) * (x - xs[i]) / (xs[i + 1] - xs[i]);
    return ys[n - 1];
}

CompLaw compLaw (const Settings& s) noexcept
{
    CompLaw c {};
    const double r = std::max (1.0, s.compRatio);
    const double u = r > 1e6 ? 1.0 : 1.0 - 1.0 / r;
    const double thr = std::clamp (roundTo (s.compThreshold, 0.1), -20.0, 10.0);
    // bilinear interpolation on the grid (thresholds descend, u ascends); below ratio 1.2 the slope and make-up
    // fade to 0 at 1:1 (the console's make-up follows the compression it is set up for)
    int ti = 0;
    constexpr int nT = (int) std::size (compThr), nU = (int) std::size (compU);
    while (ti < nT - 2 && thr < compThr[ti + 1]) ++ti;
    const double tt = std::clamp ((compThr[ti] - thr) / (compThr[ti] - compThr[ti + 1]), 0.0, 1.0);
    double v[6];
    const double uu = std::max (u, compU[0]);
    int ui = 0;
    while (ui < nU - 2 && uu > compU[ui + 1]) ++ui;
    const double tu = std::clamp ((uu - compU[ui]) / (compU[ui + 1] - compU[ui]), 0.0, 1.0);
    for (int k = 0; k < 6; ++k)
    {
        auto at = [&] (int a, int b) { return compGrid[(a * nU + b) * 6 + k]; };
        v[k] = (1 - tt) * ((1 - tu) * at (ti, ui) + tu * at (ti, ui + 1)) + tt * ((1 - tu) * at (ti + 1, ui) + tu * at (ti + 1, ui + 1));
    }
    const double fade = u < compU[0] ? u / compU[0] : 1.0;
    c.thresholdOffset = v[0];
    c.knee = std::max (0.2, v[1]);
    c.slope = v[2] * fade;
    c.makeup = v[5] * fade;
    c.attackFastMs = v[3];
    const double rel = std::clamp (s.compRelease, 0.1, 4.0);
    const double relScale = v[4] / refReleaseFactor;
    // two-part release (measured): a fraction releases with a short time constant, the rest with the knob's
    c.releaseMs = (s.compFast ? knobInterp (relKnob, relSlowFast, 7, rel) : knobInterp (relKnob, relSlowSlow, 7, rel)) * relScale;
    c.release1Ms = (s.compFast ? knobInterp (relKnob, relTau1Fast, 7, rel) : knobInterp (relKnob, relTau1Slow, 7, rel)) * relScale;
    c.release1Fraction = s.compFast ? knobInterp (relKnob, relFracFast, 7, rel) : knobInterp (relKnob, relFracSlow, 7, rel);
    c.attackSlowMs = knobInterp (relKnob, attSlowMs, 7, rel) * v[3] / refAttackFast;
    c.detectorAttackMs = 0.021;
    c.detectorReleaseMs = 0.064;
    return c;
}

// expander / gate (fitted black-box: 24 threshold x range settings and 12 release / attack settings on stairs, bursts
// and her take; dynfit "sslx"): feed-forward, peak detector with a 51 ms release, the expander a 1 : 2.5 soft-knee
// (0.3 dB) slope below the threshold, the gate a switch with 3.9 dB of hysteresis; opening 0.25 ms (fast) / 1.06 ms,
// closing with 0.43 x the release knob; the console's threshold is 21.8 dB above dBFS peak
struct GateLaw
{
    double range, threshold, attackMs, releaseMs, detectorReleaseMs, slope, knee, hysteresis;
    bool gate;
};

GateLaw gateLaw (const Settings& s) noexcept
{
    GateLaw g {};
    g.range = std::clamp (s.expRange, 0.0, 40.0);
    g.threshold = std::clamp (s.expThreshold, -30.0, 10.0) - 21.763;
    g.attackMs = s.expFast ? 0.252 : 1.061;
    g.releaseMs = std::clamp (s.expRelease, 0.1, 4.0) * 1000.0 * 0.428;
    g.detectorReleaseMs = 50.92;
    g.slope = 1.508;
    g.knee = 0.297;
    g.hysteresis = 3.859;
    g.gate = s.gate;
    return g;
}
