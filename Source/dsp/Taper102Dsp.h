#pragma once
// Taper 102 chain (phase B5): small, dependency-free DSP helpers. Analog pole/zero lists (B1's fits, the reproduce-loss fit, the head-bump
// fit) become cascades of digital biquads by the bilinear transform with every root pre-warped so that its corner stays where the analog
// prototype puts it; the cascade gain is matched to the analog magnitude at a reference frequency (and its polarity to the analog phase).
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>

#include "AtrModelMath.h"

namespace taper::t102
{
using cplx = std::complex<double>;
using atr::Biquad;
inline constexpr double kPi = atr::kPi;

// H(s) = K prod(s - 2 pi z) / prod(s - 2 pi p), z and p in Hz (s in rad/s), at most 8 roots each. Conjugate pairs are listed explicitly.
struct RootList
{
    double K = 1.0;
    int nz = 0, np = 0;
    cplx z[8], p[8];
    void addZero(cplx r) { z[nz++] = r; }
    void addPole(cplx r) { p[np++] = r; }
};

inline cplx analogResponse(const RootList& r, double hz) noexcept
{
    const cplx s(0.0, 2.0 * kPi * hz);
    cplx n = r.K, d = 1.0;
    for (int i = 0; i < r.nz; ++i) n *= s - 2.0 * kPi * r.z[i];
    for (int i = 0; i < r.np; ++i) d *= s - 2.0 * kPi * r.p[i];
    return n / d;
}

inline constexpr int kMaxSections = 6;

struct Sos
{
    int n = 0;
    Biquad b[kMaxSections];
    cplx response(double hz, double fs) const noexcept
    {
        cplx h = 1.0;
        for (int i = 0; i < n; ++i) h *= atr::biquadResponse(b[i], hz, fs);
        return h;
    }
};

struct SosState { double z[kMaxSections][2] {}; };

// Transposed direct form II. The state is flushed of denormals by the caller (flush()).
inline double runSos(const Sos& s, SosState& st, double x) noexcept
{
    for (int i = 0; i < s.n; ++i)
    {
        const Biquad& q = s.b[i];
        const double y = q.b0 * x + st.z[i][0];
        st.z[i][0] = q.b1 * x - q.a1 * y + st.z[i][1];
        st.z[i][1] = q.b2 * x - q.a2 * y;
        x = y;
    }
    return x;
}
inline void flush(SosState& st) noexcept
{
    for (auto& s : st.z) for (double& v : s) if (std::abs(v) < 1e-30) v = 0.0;
}

namespace detail
{
// Bilinear image of an analog root (Hz) at sample rate fs, radially pre-warped so that |root| maps onto itself.
inline cplx digitalRoot(cplx hz, double fs, bool prewarp) noexcept
{
    cplx r = 2.0 * kPi * hz;
    const double m = std::abs(r), T = 1.0 / fs;
    if (prewarp && m > 0.0) r *= std::tan(std::min(0.5 * m * T, 1.5)) / (0.5 * m * T);
    return (1.0 + 0.5 * T * r) / (1.0 - 0.5 * T * r);
}

// Sort roots into conjugate pairs and single reals; returns digital roots grouped in sections of at most two, lowest corner first.
struct Group { int n = 0; cplx r[2]; double mag = 0; };
inline int groupRoots(const cplx* hz, int count, double fs, bool prewarp, Group* out) noexcept
{
    bool used[8] = {};
    int ng = 0;
    for (int i = 0; i < count; ++i)
    {
        if (used[i] || std::abs(hz[i].imag()) <= 1e-9 * std::max(1.0, std::abs(hz[i]))) continue;
        for (int j = i + 1; j < count; ++j)
            if (!used[j] && std::abs(hz[j] - std::conj(hz[i])) < 1e-6 * std::abs(hz[i]) + 1e-12)
            {
                used[i] = used[j] = true;
                Group g; g.n = 2; g.r[0] = digitalRoot(hz[i], fs, prewarp); g.r[1] = digitalRoot(hz[j], fs, prewarp); g.mag = std::abs(hz[i]);
                out[ng++] = g;
                break;
            }
    }
    // reals, ascending magnitude, paired two by two
    int idx[8], nr = 0;
    for (int i = 0; i < count; ++i) if (!used[i]) idx[nr++] = i;
    std::sort(idx, idx + nr, [&](int a, int b) { return std::abs(hz[a]) < std::abs(hz[b]); });
    for (int k = 0; k < nr; k += 2)
    {
        Group g; g.n = k + 1 < nr ? 2 : 1;
        g.r[0] = digitalRoot(hz[idx[k]], fs, prewarp); g.mag = std::abs(hz[idx[k]]);
        if (g.n == 2) g.r[1] = digitalRoot(hz[idx[k + 1]], fs, prewarp);
        out[ng++] = g;
    }
    std::sort(out, out + ng, [](const Group& a, const Group& b) { return a.mag < b.mag; });
    return ng;
}
}

// Digital cascade for H(s). Roots in excess on either side are completed by roots at z = -1 (s = infinity) for zeros; the number of
// poles must be at least the number of zeros. The gain is matched to the analog response at refHz (magnitude and polarity).
inline Sos designSos(const RootList& r, double fs, double refHz = 1000.0, bool prewarp = true) noexcept
{
    detail::Group zg[8], pg[8];
    cplx zp[8], pp[8];
    for (int i = 0; i < r.np; ++i) pp[i] = r.p[i];
    int nzTot = r.nz;
    for (int i = 0; i < r.nz; ++i) zp[i] = r.z[i];
    const int nzg = detail::groupRoots(zp, nzTot, fs, prewarp, zg), npg = detail::groupRoots(pp, r.np, fs, prewarp, pg);
    const int nExtra = std::max(0, r.np - r.nz);  // zeros at z = -1
    Sos s;
    s.n = std::max(nzg, npg);
    int extraLeft = nExtra;
    for (int i = 0; i < s.n; ++i)
    {
        cplx zr[2]; int nz = 0;
        if (i < nzg) { nz = zg[i].n; zr[0] = zg[i].r[0]; zr[1] = zg[i].r[1]; }
        while (nz < 2 && extraLeft > 0) { zr[nz++] = -1.0; --extraLeft; }
        cplx pr[2]; int np = 0;
        if (i < npg) { np = pg[i].n; pr[0] = pg[i].r[0]; pr[1] = pg[i].r[1]; }
        Biquad q;
        q.b0 = 1.0;
        q.b1 = nz >= 1 ? -(nz == 2 ? (zr[0] + zr[1]).real() : zr[0].real()) : 0.0;
        q.b2 = nz == 2 ? (zr[0] * zr[1]).real() : 0.0;
        q.a1 = np >= 1 ? -(np == 2 ? (pr[0] + pr[1]).real() : pr[0].real()) : 0.0;
        q.a2 = np == 2 ? (pr[0] * pr[1]).real() : 0.0;
        s.b[i] = q;
    }
    const cplx ha = analogResponse(r, refHz), hd = s.response(refHz, fs);
    double g = std::abs(ha) / std::max(std::abs(hd), 1e-300);
    if ((ha / hd).real() < 0.0) g = -g;
    s.b[0].b0 *= g; s.b[0].b1 *= g; s.b[0].b2 *= g;
    return s;
}

inline double dB(double x) noexcept { return 20.0 * std::log10(std::max(x, 1e-300)); }
}
