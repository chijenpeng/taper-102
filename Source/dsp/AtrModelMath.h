#pragma once
// Pure math shared by the ATR-102 runtime medium, its table generator and the validation harness (no tables, no state).
#include <algorithm>
#include <cmath>
#include <complex>

namespace taper::atr
{
inline constexpr double kPi = 3.14159265358979323846;

// Cubic Hermite (Catmull-Rom) value between p1 and p2 at fraction f, with p0 and p3 the outer neighbours.
inline double catmullRom(double p0, double p1, double p2, double p3, double f) noexcept
{
    return p1 + 0.5 * f * (p2 - p0 + f * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + f * (3.0 * (p1 - p2) + p3 - p0)));
}

// Reproduce-side losses of B4a's playback model for a uniformly magnetised coating (spacing, thickness, gap), used by the harness
// to put the record-zone filter and the memoryless curve on the same footing as R2's playback; B5 will use Taper's own A(f).
struct PlaybackGeometry { double spacing = 0.25e-6, thickness = 12.7e-6, gap = 3.05e-6; };
inline double playbackLoss(double f, double ipsSpeedMps, const PlaybackGeometry& g = {}) noexcept
{
    if (f <= 0.0) return 1.0;
    const double k = 2.0 * kPi * f / ipsSpeedMps, x = 0.5 * k * g.gap;
    return std::exp(-k * g.spacing) * (-std::expm1(-k * g.thickness)) / (k * g.thickness) * (x < 1e-9 ? 1.0 : std::sin(x) / x);
}

// ---- record-zone loss filter -----------------------------------------------------------------------------------------------------
// Analog prototype (minimum phase, unity gain at DC):
//   H(s) = (1 + rho s/wp) / (1 + s/wp)  *  w0^2 / (s^2 + (w0/Q) s + w0^2)
// p = { rho, fp [Hz], f0 [Hz], Q }: a first-order shelf (zero at fp/rho, rho in [0, 1], HF asymptote rho) times a 2-pole low-pass.
inline std::complex<double> recordFilterResponse(const double* p, double f) noexcept
{
    using C = std::complex<double>;
    const C j(0.0, 1.0), s = j * f;
    const C first = (1.0 + p[0] * s / p[1]) / (1.0 + s / p[1]);
    const C biq = 1.0 / (1.0 + s / (p[3] * p[2]) + (s / p[2]) * (s / p[2]));
    return first * biq;
}

struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; // y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2 (transposed direct form II in the processor)
};

// Bilinear transform of each section with the substitution constant matched at the section's own corner frequency, so that corners land
// where the analog prototype puts them for any sample rate; fs is the rate the filter runs at.
inline void designRecordFilter(const double* p, double fs, Biquad* out /* [2] */) noexcept
{
    const double T = 1.0 / fs;
    { // first-order shelf section, corner at the pole
        const double wp = 2.0 * kPi * p[1], K = wp / std::tan(std::min(wp * T * 0.5, 1.5));
        const double bz = K * p[0] / wp, bp = K / wp, a0 = 1.0 + bp;
        out[0] = { (1.0 + bz) / a0, (1.0 - bz) / a0, 0.0, (1.0 - bp) / a0, 0.0 };
    }
    { // low-pass biquad, corner f0
        const double w0 = 2.0 * kPi * p[2], K = w0 / std::tan(std::min(w0 * T * 0.5, 1.5)), Q = p[3];
        const double k2 = K * K / (w0 * w0), kq = K / (w0 * Q);
        const double a0 = k2 + kq + 1.0;
        out[1] = { 1.0 / a0, 2.0 / a0, 1.0 / a0, 2.0 * (1.0 - k2) / a0, (k2 - kq + 1.0) / a0 };
    }
}

// designRecordFilter() matches each section at its own corner, which fails for corners near or above fs / 4 (the loss filters of the 30 ips
// sets have f0 up to 100 kHz) and leaves up to 2 dB of error at 20 kHz at 96-192 kHz. This refinement keeps the same structure but moves the
// four analog parameters (rho, ln fp, ln f0, ln Q) with a small Nelder-Mead search until the DIGITAL magnitude matches the analog prototype at 28
// log-spaced frequencies between 20 Hz and 20 kHz. Fixed-size, allocation-free, about 100-300 us; call it from setControls(), not from the audio thread.
inline void designRecordFilterFitted(const double* p, double fs, Biquad* out /* [2] */) noexcept
{
    static constexpr int nf = 28;
    static constexpr int n = 4;
    std::complex<double> z1[nf]; double tgt[nf];
    for (int i = 0; i < nf; ++i)
    {
        const double f = 20.0 * std::pow(1000.0, i / (nf - 1.0));
        z1[i] = std::polar(1.0, -2.0 * kPi * f / fs);
        tgt[i] = 20.0 * std::log10(std::abs(recordFilterResponse(p, f)));
    }
    auto make = [&](const double* x, Biquad* b)
    {
        const double q[4] = { std::min(std::max(x[0], 0.0), 1.0), std::exp(std::min(std::max(x[1], 6.0), 12.0)), std::exp(std::min(std::max(x[2], 7.0), 13.0)), std::exp(std::min(std::max(x[3], -1.6), 1.6)) };
        designRecordFilter(q, fs, b);
    };
    auto cost = [&](const double* x)
    {
        Biquad b[2]; make(x, b); double e = 0.0;
        for (int i = 0; i < nf; ++i)
        {
            const std::complex<double> zz = z1[i] * z1[i];
            const std::complex<double> h = (b[0].b0 + b[0].b1 * z1[i]) / (1.0 + b[0].a1 * z1[i]) * (b[1].b0 + b[1].b1 * z1[i] + b[1].b2 * zz) / (1.0 + b[1].a1 * z1[i] + b[1].a2 * zz);
            const double d = 20.0 * std::log10(std::abs(h)) - tgt[i];
            e += d * d;
        }
        return e;
    };
    double v[n + 1][n], fv[n + 1];
    const double x0[n] = { p[0], std::log(p[1]), std::log(p[2]), std::log(p[3]) }, step[n] = { 0.08, 0.06, 0.06, 0.06 };
    for (int i = 0; i <= n; ++i) { for (int d = 0; d < n; ++d) v[i][d] = x0[d] + (i == d + 1 ? step[d] : 0.0); fv[i] = cost(v[i]); }
    for (int it = 0; it < 220; ++it)
    {
        int lo = 0, hi = 0, nx = 0;
        for (int i = 1; i <= n; ++i) { if (fv[i] < fv[lo]) lo = i; if (fv[i] > fv[hi]) hi = i; }
        for (int i = 0; i <= n; ++i) if (i != hi && (nx == hi || fv[i] > fv[nx])) nx = i;
        if (fv[hi] - fv[lo] < 1e-9) break;
        double c[n] = {}; for (int i = 0; i <= n; ++i) if (i != hi) for (int d = 0; d < n; ++d) c[d] += v[i][d] / n;
        auto at = [&](double t, double* r) { for (int d = 0; d < n; ++d) r[d] = c[d] + t * (v[hi][d] - c[d]); };
        double xr[n], xe[n], xc[n]; at(-1.0, xr); const double fr = cost(xr);
        if (fr < fv[lo]) { at(-2.0, xe); const double fe = cost(xe); const double* s = fe < fr ? xe : xr; for (int d = 0; d < n; ++d) v[hi][d] = s[d]; fv[hi] = std::min(fe, fr); }
        else if (fr < fv[nx]) { for (int d = 0; d < n; ++d) v[hi][d] = xr[d]; fv[hi] = fr; }
        else
        {
            at(fr < fv[hi] ? -0.5 : 0.5, xc); const double fc = cost(xc);
            if (fc < std::min(fr, fv[hi])) { for (int d = 0; d < n; ++d) v[hi][d] = xc[d]; fv[hi] = fc; }
            else for (int i = 0; i <= n; ++i) if (i != lo) { for (int d = 0; d < n; ++d) v[i][d] = v[lo][d] + 0.5 * (v[i][d] - v[lo][d]); fv[i] = cost(v[i]); }
        }
    }
    int best = 0; for (int i = 1; i <= n; ++i) if (fv[i] < fv[best]) best = i;
    make(v[best], out);
}

inline std::complex<double> biquadResponse(const Biquad& b, double f, double fs) noexcept
{
    const std::complex<double> z1 = std::polar(1.0, -2.0 * kPi * f / fs), z2 = z1 * z1;
    return (b.b0 + b.b1 * z1 + b.b2 * z2) / (1.0 + b.a1 * z1 + b.a2 * z2);
}
}
