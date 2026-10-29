#pragma once
// ATR-102 runtime medium: a cheap reproduction of the spatial record model for the
// Taper 102 controls: Tape {456, ATR Master}, Speed {15, 30 ips}, Bias (overbias in dB over the manual alignment point) and record level.
//
//   in --> gain (record level) --> [oversample] --> Phi(u; tape, speed, bias) --> record-zone loss filter --> [decimate] --> out
//
// * Phi is the odd, memoryless remanence curve: the long-wavelength flux of a constant drive u, tabulated offline from the spatial record model on a uniform
//   grid of u (u = 1 is the operating level, grid step kUStep) at kNumBias bias nodes; blended across bias with a cubic and read with a
//   cubic (Catmull-Rom) interpolation. Output is normalised to the fundamental of an operating-level sine at the aligned bias, i.e.
//   an operating-level sine with the aligned bias comes out at about 1.0 amplitude (-14.0 dB / -14.86 dB re saturation flux for 456 / ATR Master).
// * Even harmonics: the real record field is not exactly odd (residual head magnetisation, bias-waveform asymmetry, record-amp offset). This is
//   modelled as a constant field offset delta (tables::kEvenOffsetKaPerM, kA/m of deep-gap field, chosen within the manual's second-harmonic limit)
//   and the curve is read as Phi(x + d) - Phi(d), d = delta / drive-at-operating-level of the set: zero in gives zero out (no DC step), the odd part
//   is unchanged to O(d^2) and the even part is d * Phi'(x) minus its mean. Constant over bias; the mean (DC) of the output is not removed here
//   (the reproduce chain has no DC path: the head's EMF is the derivative of flux).
// * The record-zone loss filter is a minimum-phase 3-pole/1-zero IIR (AtrModelMath.h) whose parameters depend on bias; it contains only the
//   loss of the recording process relative to long wavelengths. It does NOT contain spacing, thickness or gap losses of the reproduce head
//   (the chain adds those from its analytic A(f)).
// * Input convention: a sample of amplitude 1.0 is the peak of an operating-level sine; recordLevelDb scales the drive in dB.
//
// Threading: prepare() is not real-time safe. setControls() is meant for the message thread (it designs the curve blend and the filters, no
// allocation) and hands the result to the audio thread through a lock-free triple buffer; process() / reset() are allocation- and lock-free.
// A change of controls is picked up at the start of the next process() call and cross-faded over kCrossfadeSeconds (both curves and both
// filter chains run during the fade). A second change arriving during a fade restarts the fade from the state being left.
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "AtrHalfBand.h"
#include "AtrMediumTables.h"
#include "AtrModelMath.h"

namespace taper::atr
{
enum class Tape : int { Ampex456 = 0, AtrMaster = 1 };
enum class Speed : int { Ips15 = 0, Ips30 = 1 };

struct Controls
{
    Tape tape = Tape::Ampex456;
    Speed speed = Speed::Ips15;
    double overbiasDb = 0.0;     // dB over the manual alignment bias; table nodes cover -6..+8, the product range is about -4..+6
    double recordLevelDb = 0.0;  // dB re operating level
};

class AtrMedium
{
public:
    static constexpr int kMaxChannels = 2;
    static constexpr double kCrossfadeSeconds = 0.02;
    static constexpr double kGainSmoothSeconds = 0.005;

    // Oversampling that keeps the internal rate near or above 176 kHz: 4x below 66 kHz, 2x below 130 kHz, else 1x (to keep aliasing low).
    static int recommendedOversampling(double sampleRate) noexcept { return sampleRate < 66000.0 ? 4 : (sampleRate < 130000.0 ? 2 : 1); }

    // oversampling: 1, 2 or 4. Not real-time safe.
    void prepare(double sampleRate, int maxBlock, int channels, int oversampling = 4)
    {
        fs_ = sampleRate;
        maxBlock_ = maxBlock;
        nch_ = std::min(std::max(channels, 1), kMaxChannels);
        os_ = oversampling >= 4 ? 4 : (oversampling >= 2 ? 2 : 1);
        hb_.prepare(os_ == 4 ? 2 : (os_ == 2 ? 1 : 0), nch_, maxBlock);
        for (auto& v : work_) v.assign(static_cast<std::size_t>(maxBlock), 0.0);
        xfLen_ = std::max(1, static_cast<int>(kCrossfadeSeconds * fs_ * os_));
        gainCoef_ = 1.0 - std::exp(-1.0 / (kGainSmoothSeconds * fs_));
        measureLatency();
        setControls(controls_);
        reset();
    }

    // Message thread. Designs the blended table and the filters for the new controls and publishes them.
    void setControls(const Controls& c) noexcept
    {
        controls_ = c;
        build(c, slots_[writeIdx_]);
        const int old = mailbox_.exchange(writeIdx_ | kDirty, std::memory_order_acq_rel);
        writeIdx_ = old & 3;
        gainTarget_.store(std::pow(10.0, std::clamp(c.recordLevelDb, -60.0, 60.0) / 20.0), std::memory_order_relaxed);
    }

    // Audio thread (or stopped): clears all state and adopts the latest controls without a fade.
    void reset() noexcept
    {
        hb_.reset();
        if (mailbox_.load(std::memory_order_acquire) & kDirty) adopt();
        old_ = cur_;
        for (int c = 0; c < kMaxChannels; ++c)
        {
            std::memset(&state_[c], 0, sizeof(state_[c]));
            gain_[c] = gainTarget_.load(std::memory_order_relaxed);
            xfPos_[c] = xfLen_;
        }
    }

    // In place, io[c] points at n samples of channel c (n may be any size; internally chunked to maxBlock).
    void process(double* const* io, int channels, int n) noexcept
    {
        const int nch = std::min(channels, nch_);
        const bool fresh = pollMailbox();
        if (fresh) for (int c = 0; c < kMaxChannels; ++c) { xfPos_[c] = 0; state_[c].chain[1] = state_[c].chain[0]; std::memset(&state_[c].chain[0], 0, sizeof(state_[c].chain[0])); }
        const double gT = gainTarget_.load(std::memory_order_relaxed);
        for (int c = 0; c < nch; ++c)
        {
            double* x = io[c];
            for (int done = 0; done < n;)
            {
                const int m = std::min(maxBlock_, n - done);
                double* in = work_[c].data();
                double g = gain_[c];
                for (int i = 0; i < m; ++i)
                {
                    g += (gT - g) * gainCoef_;
                    const double v = x[done + i];
                    in[i] = std::abs(v) < 1e30 ? v * g : 0.0;
                }
                gain_[c] = g;
                double* y = hb_.up(c, in, m);
                runCurve(c, y, m * os_);
                if (os_ == 1) std::memcpy(x + done, y, static_cast<std::size_t>(m) * sizeof(double)); else hb_.down(c, x + done, m);
                done += m;
            }
        }
        hb_.snap();
        for (int c = 0; c < nch; ++c) flush(state_[c]);
    }

    // Group delay of the oversampler pair at low frequencies, in input samples (the curve and the loss filter are not included).
    double latencySamples() const noexcept { return latency_; }
    int oversampling() const noexcept { return os_; }
    const Controls& controls() const noexcept { return controls_; }
    static const tables::SetData& setData(Tape t, Speed s) noexcept { return tables::kSets[static_cast<int>(t) * 2 + static_cast<int>(s)]; }

    // Pure evaluation helpers for the harness: the memoryless curve and the analog record-zone filter at given controls.
    double curveAt(double u) const noexcept { return evalCurve(cur_, u); }   // including the even-harmonic offset
    double oddCurveAt(double u) const noexcept { return evalOdd(cur_, u); }  // the odd table alone, Phi(u)
    void filterParams(double* p /*[4]*/) const noexcept { for (int i = 0; i < 4; ++i) p[i] = cur_.fp[i]; }

private:
    static constexpr int kDirty = 4;
    static constexpr int kPad = 1;
    static constexpr int kTable = tables::kNumU + 3;

    struct Snapshot
    {
        double table[kTable];  // table[kPad + j] = Phi at node j; table[0] = odd mirror, tail = linear continuation
        double tailSlope;      // per grid step beyond the last node
        double offU;           // even-harmonic field offset in units of u (see header comment)
        double offPhi;         // Phi(offU), subtracted so that zero in gives zero out
        double fp[4];          // analog filter parameters (for inspection)
        Biquad bq[2];
    };
    struct Chain { double z[2][2]; };
    struct State { Chain chain[2]; };  // [0] = current, [1] = outgoing during a cross-fade

    static void flush(State& s) noexcept
    {
        for (auto& ch : s.chain) for (auto& sec : ch.z) for (double& v : sec) if (std::abs(v) < 1e-30) v = 0.0;
    }

    // Weights of the nodes for a bias in dB (cubic Catmull-Rom across the nodes, linear extrapolation of the missing outer neighbour).
    static void biasWeights(double db, double* w /*[kNumBias]*/, int& seg, double& f) noexcept
    {
        constexpr int n = tables::kNumBias;
        const double step = tables::kBiasNodesDb[1] - tables::kBiasNodesDb[0];
        double t = (db - tables::kBiasNodesDb[0]) / step;
        t = std::min(std::max(t, 0.0), static_cast<double>(n - 1));
        seg = std::min(static_cast<int>(t), n - 2);
        f = t - seg;
        for (int i = 0; i < n; ++i) w[i] = 0.0;
        const double w0 = -0.5 * f * (1.0 - f) * (1.0 - f), w1 = 1.0 - 2.5 * f * f + 1.5 * f * f * f;
        const double w2 = 0.5 * f + 2.0 * f * f - 1.5 * f * f * f, w3 = 0.5 * f * f * (f - 1.0);
        if (seg > 0) w[seg - 1] += w0; else { w[0] += 2.0 * w0; w[1] -= w0; }
        w[seg] += w1; w[seg + 1] += w2;
        if (seg + 2 < n) w[seg + 2] += w3; else { w[n - 1] += 2.0 * w3; w[n - 2] -= w3; }
    }

    void build(const Controls& c, Snapshot& s) const noexcept
    {
        const tables::SetData& d = setData(c.tape, c.speed);
        constexpr int n = tables::kNumBias, nu = tables::kNumU;
        double w[n]; int seg; double f;
        biasWeights(c.overbiasDb, w, seg, f);
        for (int j = 0; j < nu; ++j)
        {
            double v = 0.0;
            for (int b = 0; b < n; ++b) v += w[b] * static_cast<double>(d.phi[b][j]);
            s.table[kPad + j] = v;
        }
        s.table[0] = -s.table[kPad + 1];
        s.tailSlope = s.table[kPad + nu - 1] - s.table[kPad + nu - 2];
        s.table[kPad + nu] = s.table[kPad + nu - 1] + s.tailSlope;
        s.table[kPad + nu + 1] = s.table[kPad + nu] + s.tailSlope;
        s.offU = tables::kEvenOffsetKaPerM / d.driveAtOperatingKaPerM;
        s.offPhi = evalOdd(s, s.offU);
        // filter parameters: rho and Q linear, frequencies in the log domain, same blend as the table
        double p[4] = { 0, 0, 0, 0 };
        for (int b = 0; b < n; ++b)
        {
            p[0] += w[b] * d.filter[b][0];
            p[1] += w[b] * std::log(d.filter[b][1]);
            p[2] += w[b] * std::log(d.filter[b][2]);
            p[3] += w[b] * d.filter[b][3];
        }
        s.fp[0] = std::min(std::max(p[0], 0.0), 1.0); s.fp[1] = std::exp(p[1]); s.fp[2] = std::exp(p[2]); s.fp[3] = std::max(p[3], 0.2);
        designRecordFilterFitted(s.fp, fs_ * os_, s.bq);
    }

    static double evalCurve(const Snapshot& s, double u) noexcept { return evalOdd(s, u + s.offU) - s.offPhi; }

    static double evalOdd(const Snapshot& s, double u) noexcept
    {
        const double a = std::abs(u) * (1.0 / tables::kUStep);
        double y;
        if (a >= static_cast<double>(tables::kNumU - 1))
            y = s.table[kPad + tables::kNumU - 1] + s.tailSlope * (a - static_cast<double>(tables::kNumU - 1));
        else
        {
            const int i = static_cast<int>(a);
            const double f = a - i;
            const double* p = &s.table[i];  // p[0] = node i-1
            y = catmullRom(p[0], p[1], p[2], p[3], f);
        }
        return u < 0.0 ? -y : y;
    }

    static double section(const Biquad& b, double* z, double x) noexcept  // transposed direct form II
    {
        const double y = b.b0 * x + z[0];
        z[0] = b.b1 * x - b.a1 * y + z[1];
        z[1] = b.b2 * x - b.a2 * y;
        return y;
    }

    void runCurve(int c, double* y, int len) noexcept
    {
        State& st = state_[c];
        int i = 0;
        if (xfPos_[c] < xfLen_)
        {
            const int stop = std::min(len, xfLen_ - xfPos_[c]);
            const double inv = 1.0 / static_cast<double>(xfLen_);
            for (; i < stop; ++i)
            {
                const double x = y[i];
                const double a = section(cur_.bq[1], st.chain[0].z[1], section(cur_.bq[0], st.chain[0].z[0], evalCurve(cur_, x)));
                const double b = section(old_.bq[1], st.chain[1].z[1], section(old_.bq[0], st.chain[1].z[0], evalCurve(old_, x)));
                const double w = static_cast<double>(xfPos_[c] + i + 1) * inv;
                y[i] = b + w * (a - b);
            }
            xfPos_[c] += stop;
        }
        for (; i < len; ++i)
            y[i] = section(cur_.bq[1], st.chain[0].z[1], section(cur_.bq[0], st.chain[0].z[0], evalCurve(cur_, y[i])));
    }

    void adopt() noexcept
    {
        const int old = mailbox_.exchange(readIdx_, std::memory_order_acq_rel);
        readIdx_ = old & 3;
        cur_ = slots_[readIdx_];
    }
    bool pollMailbox() noexcept
    {
        if (!(mailbox_.load(std::memory_order_acquire) & kDirty)) return false;
        old_ = cur_;
        adopt();
        return true;
    }

    void measureLatency()
    {
        // low-frequency phase delay of up -> down (identity in between), from a sine of 512 samples per period over 4 whole periods,
        // fed in chunks of at most maxBlock so the result does not depend on maxBlock
        if (os_ == 1) { latency_ = 0.0; return; }
        constexpr int kPeriod = 512, kTotal = 4 * kPeriod;
        const int chunk = std::max(1, std::min(maxBlock_, kPeriod));
        double* buf = work_[0].data();
        hb_.reset();
        double sc = 0.0, ss = 0.0;
        const double w = 2.0 * kPi / kPeriod;
        std::vector<double> o(static_cast<std::size_t>(chunk));
        for (int pos = 0; pos < kTotal; pos += chunk)
        {
            const int n = std::min(chunk, kTotal - pos);
            for (int i = 0; i < n; ++i) buf[i] = std::sin(w * (pos + i));
            hb_.up(0, buf, n);
            hb_.down(0, o.data(), n);
            for (int i = 0; i < n; ++i) { const int t = pos + i; if (t >= 2 * kPeriod) { ss += o[static_cast<std::size_t>(i)] * std::sin(w * t); sc += o[static_cast<std::size_t>(i)] * std::cos(w * t); } }
        }
        latency_ = -std::atan2(sc, ss) / w;
        hb_.reset();
    }

    static_assert(std::atomic<double>::is_always_lock_free && std::atomic<int>::is_always_lock_free, "the control hand-off must be lock-free");
    double fs_ = 48000.0, gainCoef_ = 0.01, latency_ = 0.0;
    int maxBlock_ = 0, nch_ = 1, os_ = 4, xfLen_ = 1;
    Controls controls_;
    HalfBandCascade hb_;
    std::vector<double> work_[kMaxChannels];
    Snapshot slots_[3] {};
    Snapshot cur_ {}, old_ {};
    std::atomic<int> mailbox_ { 1 };  // slot 1 parked; writer owns 0, reader owns 2
    int writeIdx_ = 0, readIdx_ = 2;
    std::atomic<double> gainTarget_ { 1.0 };
    State state_[kMaxChannels] {};
    double gain_[kMaxChannels] { 1.0, 1.0 };
    int xfPos_[kMaxChannels] { 0, 0 };
};
}
