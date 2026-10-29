#pragma once
// Taper 102 chain: the data that the alignment generator (t102_align.cpp) produces and Taper102Chain.h consumes.
#include <array>

namespace taper::t102
{
enum class Eq : int { Nab15 = 0, Iec15 = 1, Aes30 = 2 };
inline constexpr double kOperatingLevelsNwbm[4] = { 185.0, 250.0, 320.0, 370.0 };

// Reproduce-head playback loss (spacing 0.25 um, thickness 12.7 um, gap 3.05 um, uniform depth: the medium's geometry, A(f)) as a
// minimum-phase real pole/zero set, fitted per speed to the DIGITAL response at 192 kHz (bilinear with pre-warped roots). The roots are corner
// frequencies in Hz (the root is at -f); DC gain 1.
struct LossFit
{
    int nz = 0, np = 0;
    double fz[4] {}, fp[4] {};
};

// Head contour ("bump") response, a cascade of analog biquads (zero f0/Q, pole f0/Q) with DC gain kDc, frequencies for 30 ips (scaled by
// speed for 15 ips). enabled = false gives unity (used while the generator aligns and measures the bump target).
struct BumpFit
{
    bool enabled = false;
    int ns = 0;
    double kDc = 1.0;
    double fz[5] {}, qz[5] {}, fp[5] {}, qp[5] {};
    // the physical model the sections approximate (kept for documentation and the validation harness): C(f) = 1 + sum_i d_i sin(k L_i) / (k L_i), k = 2 pi f / v
    double d[3] {}, lengthM[3] {};
};

// Per-root frequency scale factors (0 = 1.0) that make a plain bilinear transform of the analog circuit fit match its complex response at 192 kHz
// (the zeros then the poles of the chain's RootList, a conjugate pair sharing one factor); found by t102_align step 0.
struct RootScale { double s[10] {}; };

struct Tables
{
    RootScale scaleRepro[3];       // [Eq]
    RootScale scaleRecord[3][2];   // [Eq][pot 0.0 / pot 1.0 branch]
    double trimPot[2][3] {};       // record HF trim as the equivalent R15 / R18 position (0..1), [tape][Eq]
    double makeupDb[2][3][4] {};   // reproduce level calibration, [tape][Eq][operating level]
    LossFit loss[2];   // [speed]
    BumpFit bump;
};
}
