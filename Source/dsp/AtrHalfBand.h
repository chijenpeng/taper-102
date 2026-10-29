#pragma once
// Dependency-free 2x / 4x oversampler for the ATR-102 runtime medium: cascaded polyphase-allpass IIR half-band stages.
// The design is a port of juce::dsp::FilterDesign::designIIRLowpassHalfBandPolyphaseAllpassMethod and the stage parameters
// of juce::dsp::Oversampling (filterHalfBandPolyphaseIIR, maximum quality) for the first two stages, which
// taper::detail::HalfBandOversampler16 also uses; unlike that class it has no JUCE dependency and supports 1x, 2x and 4x.
// Not real-time safe: prepare(). Everything else is allocation-free.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace taper::atr
{
class HalfBandCascade
{
public:
    static constexpr int maxChannels = 2;
    static constexpr int maxStages = 2;
    static constexpr int maxAllpass = 16;
    static constexpr double kStopbandDb = -100.0;

    // stages = 0 (1x), 1 (2x) or 2 (4x). Output of up() has (m << stages) samples per channel.
    void prepare(int stages, int channels, int maxBlock)
    {
        stages_ = std::clamp(stages, 0, maxStages);
        nch_ = std::clamp(channels, 1, maxChannels);
        maxBlock_ = maxBlock;
        for (int n = 0; n < maxStages; ++n)
        {
            const double twUp = 0.10 * (n == 0 ? 0.5 : 1.0), twDown = 0.12 * (n == 0 ? 0.5 : 1.0);
            // Stop-band depths: JUCE's maximum-quality values are -90/-80 dB (up) and -75/-65 dB (down); the down filters limit the alias
            // floor of a saturating curve, so this cascade uses a common depth (kStopbandDb) for all of them.
            const double dbUp = kStopbandDb, dbDown = kStopbandDb;
            design(twUp, dbUp, stage_[n].up);
            design(twDown, dbDown, stage_[n].down);
            for (int c = 0; c < maxChannels; ++c) stage_[n].buf[c].assign(static_cast<std::size_t>(maxBlock) << (n + 1), 0.0);
        }
        reset();
    }

    void reset() noexcept
    {
        for (auto& st : stage_)
        {
            for (auto& ch : st.up.v) for (auto& x : ch) x = 0.0;
            for (auto& ch : st.down.v) for (auto& x : ch) x = 0.0;
            for (auto& x : st.delay) x = 0.0;
        }
    }

    int stages() const noexcept { return stages_; }
    int factor() const noexcept { return 1 << stages_; }

    // in: one channel, m <= maxBlock samples. Returns the oversampled block (valid until the next up() of that channel).
    double* up(int ch, const double* in, int m) noexcept
    {
        const double* src = in;
        double* dst = nullptr;
        for (int n = 0; n < stages_; ++n)
        {
            dst = stage_[n].buf[ch].data();
            upStage(stage_[n].up, ch, src, dst, m);
            src = dst; m *= 2;
        }
        return dst ? dst : const_cast<double*>(in);
    }

    // Consumes the block returned by up() of the same channel (modified in place by the caller) and writes m samples.
    void down(int ch, double* out, int m) noexcept
    {
        int len = m << (stages_ - 1);
        for (int n = stages_ - 1; n >= 0; --n)
        {
            double* dst = n == 0 ? out : stage_[n - 1].buf[ch].data();
            downStage(stage_[n], ch, stage_[n].buf[ch].data(), dst, len);
            len /= 2;
        }
    }

    // Flush denormals out of the recursions; call once per block.
    void snap() noexcept
    {
        for (auto& st : stage_)
            for (int c = 0; c < nch_; ++c)
                for (int n = 0; n < maxAllpass; ++n)
                {
                    if (std::abs(st.up.v[c][n]) < 1e-30) st.up.v[c][n] = 0.0;
                    if (std::abs(st.down.v[c][n]) < 1e-30) st.down.v[c][n] = 0.0;
                }
    }

private:
    struct Chain
    {
        int direct = 0, total = 0;
        double a[maxAllpass] {};
        double v[maxChannels][maxAllpass] {};
    };
    struct Stage { Chain up, down; double delay[maxChannels] {}; std::vector<double> buf[maxChannels]; };

    static void design(double tw, double db, Chain& c)
    {
        const double pi = 3.14159265358979323846;
        const double wt = 2.0 * pi * tw, ds = std::pow(10.0, db / 20.0);
        const double k = std::pow(std::tan((pi - wt) / 4.0), 2.0), kp = std::sqrt(1.0 - k * k);
        const double e = (1.0 - std::sqrt(kp)) / (1.0 + std::sqrt(kp)) * 0.5;
        const double q = e + 2.0 * std::pow(e, 5.0) + 15.0 * std::pow(e, 9.0) + 150.0 * std::pow(e, 13.0);
        double k1 = ds * ds / (1.0 - ds * ds);
        int n = static_cast<int>(std::lround(std::ceil(std::log(k1 * k1 / 16.0) / std::log(q))));
        if (n % 2 == 0) ++n;
        if (n == 1) n = 3;
        const int N = (n - 1) / 2;
        double ai[maxAllpass] {};
        for (int i = 1; i <= N && i <= maxAllpass; ++i)
        {
            double num = 0.0, delta = 1.0;
            for (int m = 0; std::abs(delta) > 1e-100; ++m)
            {
                delta = ((m & 1) ? -1.0 : 1.0) * std::pow(q, m * (m + 1)) * std::sin((2 * m + 1) * pi * i / static_cast<double>(n));
                num += delta;
            }
            num *= 2.0 * std::pow(q, 0.25);
            double den = 0.0;
            delta = 1.0;
            for (int m = 1; std::abs(delta) > 1e-100; ++m)
            {
                delta = ((m & 1) ? -1.0 : 1.0) * std::pow(q, m * m) * std::cos(m * 2.0 * pi * i / static_cast<double>(n));
                den += delta;
            }
            den = 1.0 + 2.0 * den;
            const double wi = num / den;
            const double api = std::sqrt((1.0 - wi * wi * k) * (1.0 - wi * wi / k)) / (1.0 + wi * wi);
            ai[i - 1] = (1.0 - api) / (1.0 + api);
        }
        // direct path: even-indexed sections (0, 2, ...); delayed path: odd-indexed sections after the pure delay
        int w = 0;
        for (int i = 0; i < N; i += 2) c.a[w++] = ai[i];
        c.direct = w;
        for (int i = 1; i < N; i += 2) c.a[w++] = ai[i];
        c.total = w;
    }

    static void upStage(Chain& c, int ch, const double* src, double* dst, int m) noexcept
    {
        for (int i = 0; i < m; ++i)
        {
            const double in = src[i];
            double o = in;
            for (int n = 0; n < c.direct; ++n) { const double al = c.a[n]; const double y = al * o + c.v[ch][n]; c.v[ch][n] = o - al * y; o = y; }
            dst[2 * i] = o;
            o = in;
            for (int n = c.direct; n < c.total; ++n) { const double al = c.a[n]; const double y = al * o + c.v[ch][n]; c.v[ch][n] = o - al * y; o = y; }
            dst[2 * i + 1] = o;
        }
    }

    static void downStage(Stage& st, int ch, const double* src, double* dst, int outLen) noexcept
    {
        Chain& c = st.down;
        for (int i = 0; i < outLen; ++i)
        {
            double o = src[2 * i];
            for (int n = 0; n < c.direct; ++n) { const double al = c.a[n]; const double y = al * o + c.v[ch][n]; c.v[ch][n] = o - al * y; o = y; }
            const double d = o;
            o = src[2 * i + 1];
            for (int n = c.direct; n < c.total; ++n) { const double al = c.a[n]; const double y = al * o + c.v[ch][n]; c.v[ch][n] = o - al * y; o = y; }
            dst[i] = (st.delay[ch] + d) * 0.5;
            st.delay[ch] = o;
        }
    }

    Stage stage_[maxStages];
    int stages_ = 0, nch_ = 1, maxBlock_ = 0;
};
}
