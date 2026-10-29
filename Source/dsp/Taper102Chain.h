#pragma once
// Taper 102 chain (plan phase B5): record electronics -> medium (B4b) -> reproduce-head losses and head contour -> reproduce electronics,
// one instance for up to two independent channels.
//
//   in (dBFS, 0 VU = -18 dBFS peak) -> Input dB -> record EQ (B1 fit, HF trim) -> clip (A6, 25.8 dB over 0 VU) -> AtrMedium
//      -> playback loss A(f) -> head contour -> reproduce EQ (B1 fit, flux referred) -> level calibration -> Output dB
//
// Everything between the oversampler's up and down filters runs at 4x (44.1/48 kHz hosts), 2x (88.2/96 kHz) or 1x (176.4 kHz and above), i.e.
// at about 176-192 kHz, so the electronics fits (poles near 20-50 kHz) and the medium share one rate. The medium itself runs with its own
// oversampling set to 1 at that internal rate (same filters and curves as B4b's recommended 4x/2x setting).
//
// Units: AtrMedium takes 1.0 = peak of an operating-level sine (the reference fluxivity of its tape: 370 nWb/m for 456, 320 for ATR Master);
// "0 VU" of this chain is the selected Operating level, so the medium's record level is 20 log10(selected / reference) dB.
// Threading follows AtrMedium: prepare() is not real-time safe, setControls() is for the message thread, process() / reset() do not
// allocate or lock. A change of Speed, EQ or Tape swaps the electronics through a three-slot mailbox and cross-fades over 20 ms (both
// chains run; the new chain starts from the old chain's state); Input, Output, Operating level and Bias move through smoothed gains and
// the medium's own hand-off.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "AtrHalfBand.h"
#include "AtrMedium.h"
#include "Taper102Align.h"
#include "Taper102Dsp.h"
#include "Taper102Fits.h"
#include "Taper102Tables.h"

namespace taper::t102
{
using atr::Speed;
using atr::Tape;

enum class OperatingLevel : int { Nwb185 = 0, Nwb250 = 1, Nwb320 = 2, Nwb370 = 3 };

struct Controls
{
    Speed speed = Speed::Ips15;
    Eq eq = Eq::Nab15;                       // Nab15 / Iec15 at 15 ips, Aes30 at 30 ips (forced to match the speed)
    Tape tape = Tape::Ampex456;
    OperatingLevel level = OperatingLevel::Nwb370;
    double overbiasDb = 0.0;                 // dB over the aligned point, -4..+6 (clamped to the medium's -6..+8)
    double inputDb = 0.0, outputDb = 0.0;    // -24..+24
};

inline constexpr double kZeroVuDbfs = -18.0;      // 0 VU = -18 dBFS (peak of a sine)
inline constexpr double kRecordClipDbOverVu = 25.8;  // B1: A6 clips 25.8 dB above the operating level (manual: "26 dB")
inline constexpr double kRepro_HfPoleHz = 150e3;     // Approximation: makes the flux-referred reproduce chain proper (-0.08 dB at 20 kHz)
inline constexpr double kR15PotK = 20.0 / 33.0;      // Derived: R15/R18 20 k loaded by R23 33 k, g(w) = w / (1 + w (1 - w) kR15PotK); matches B1's 0.5 fit to 0.0002

// Weight of the pot-1.0 branch in H(w) = H0 + g (H1 - H0) (B1's pot 0.5 fit gives g = 0.4343, the loaded-pot law 0.4342).
inline double potWeight(double w) noexcept { w = std::min(std::max(w, 0.0), 1.0); return w / (1.0 + w * (1.0 - w) * kR15PotK); }

inline Eq eqFor(Speed s, Eq e) noexcept { return s == Speed::Ips30 ? Eq::Aes30 : (e == Eq::Aes30 ? Eq::Nab15 : e); }
inline double speedMps(Speed s) noexcept { return (s == Speed::Ips30 ? 30.0 : 15.0) * 0.0254; }
inline double referenceNwbm(Tape t) noexcept { return t == Tape::Ampex456 ? 370.0 : 320.0; }  // medium's 1.0 (B3/B4a level mapping)

// ---- analog prototypes -------------------------------------------------------------------------------------------------------------
inline int fitSet(Eq e) noexcept { return e == Eq::Aes30 ? 0 : (e == Eq::Iec15 ? 1 : 2); }
inline RootList fitRoots(const fits::Fit& f) noexcept
{
    RootList r; r.K = f.K;
    for (int i = 0; i < f.nz; ++i) r.addZero({ f.z[i].re, f.z[i].im });
    for (int i = 0; i < f.np; ++i) r.addPole({ f.p[i].re, f.p[i].im });
    return r;
}
inline const fits::Fit& recordFit(Eq e, int pot /*0 or 2: pot 0.0 / 1.0*/) noexcept { return fits::kFits[3 + fitSet(e) * 3 + pot]; }

// Flux-referred reproduce chain: B1's EMF -> line fit times s (EMF = jw flux) with one HF pole, unity gain at 1 kHz, positive at 1 kHz.
// Applies the refit factors (zeros then poles; 0 = unchanged); a root's whole complex value is scaled, which keeps conjugate pairs conjugate.
inline void applyScale(RootList& r, const RootScale& sc) noexcept
{
    for (int i = 0; i < r.nz; ++i) if (sc.s[i] != 0.0) r.z[i] *= sc.s[i];
    for (int i = 0; i < r.np; ++i) if (sc.s[r.nz + i] != 0.0) r.p[i] *= sc.s[r.nz + i];
}
// Unscaled analog prototype of the flux-referred reproduce chain.
inline RootList reproRootsAnalog(Eq e) noexcept
{
    RootList r = fitRoots(fits::kFits[fitSet(e)]);
    r.addZero({ 0.0, 0.0 });
    r.addPole({ -kRepro_HfPoleHz, 0.0 });
    r.K *= 2.0 * kPi * kRepro_HfPoleHz;
    const cplx h = analogResponse(r, 1000.0);
    r.K *= (h.real() < 0 ? -1.0 : 1.0) / std::abs(h);
    return r;
}
inline RootList reproRoots(Eq e, const Tables& t) noexcept { RootList r = reproRootsAnalog(e); applyScale(r, t.scaleRepro[static_cast<int>(e)]); return r; }
inline RootList recordRoots(Eq e, int branch, const Tables& t) noexcept { RootList r = fitRoots(recordFit(e, branch * 2)); applyScale(r, t.scaleRecord[static_cast<int>(e)][branch]); return r; }

inline RootList lossRoots(const LossFit& f) noexcept
{
    const double sc = 1.0;
    RootList r; double k = 1.0;
    for (int i = 0; i < f.nz; ++i) { r.addZero({ -f.fz[i] * sc, 0.0 }); k /= f.fz[i] * sc; }
    for (int i = 0; i < f.np; ++i) { r.addPole({ -f.fp[i] * sc, 0.0 }); k *= f.fp[i] * sc; }
    r.K = k * std::pow(2.0 * kPi, f.np - f.nz);
    return r;
}

inline void quadRoots(double f0, double q, cplx* out) noexcept  // roots (Hz) of s^2 + (w0/Q) s + w0^2
{
    const double a = -0.5 / q, d = a * a - 1.0;
    if (d < 0.0) { out[0] = f0 * cplx(a, std::sqrt(-d)); out[1] = std::conj(out[0]); }
    else { out[0] = f0 * (a + std::sqrt(d)); out[1] = f0 * (a - std::sqrt(d)); }
}
inline RootList bumpRoots(const BumpFit& b, Speed s) noexcept
{
    RootList r; r.K = 1.0;
    if (!b.enabled || b.ns == 0) { r.K = 1.0; return r; }
    const double sc = speedMps(s) / speedMps(Speed::Ips30);
    cplx num = 1.0, den = 1.0;
    for (int i = 0; i < b.ns; ++i)
    {
        cplx q[2];
        quadRoots(b.fz[i] * sc, b.qz[i], q); r.addZero(q[0]); r.addZero(q[1]); num *= q[0] * q[1];
        quadRoots(b.fp[i] * sc, b.qp[i], q); r.addPole(q[0]); r.addPole(q[1]); den *= q[0] * q[1];
    }
    r.K = b.kDc * (den / num).real();   // equal counts: the 2 pi factors cancel
    return r;
}

// Physical contour response the bump sections approximate: C(f) = 1 + sum d_i sin(kL_i) / (k L_i), k = 2 pi f / v.
inline double contourResponse(const BumpFit& b, double f, double v) noexcept
{
    const double k = 2.0 * kPi * f / v;
    double c = 1.0;
    for (int i = 0; i < 3; ++i) { const double x = k * b.lengthM[i]; c += b.d[i] * (x < 1e-9 ? 1.0 : std::sin(x) / x); }
    return c;
}

// ---- the chain ---------------------------------------------------------------------------------------------------------------------
class Taper102Chain
{
public:
    static constexpr int kMaxChannels = 2;
    static constexpr double kCrossfadeSeconds = 0.02, kGainSmoothSeconds = 0.005;

    // oversampling 0 = AtrMedium::recommendedOversampling(fs); else 1, 2 or 4. Not real-time safe.
    void prepare(double sampleRate, int maxBlock, int channels, int oversampling = 0)
    {
        fs_ = sampleRate; maxBlock_ = maxBlock; nch_ = std::min(std::max(channels, 1), kMaxChannels);
        os_ = oversampling == 0 ? atr::AtrMedium::recommendedOversampling(fs_) : (oversampling >= 4 ? 4 : (oversampling >= 2 ? 2 : 1));
        fsi_ = fs_ * os_;
        hb_.prepare(os_ == 4 ? 2 : (os_ == 2 ? 1 : 0), nch_, maxBlock);
        medium_.prepare(fsi_, maxBlock * os_, nch_, 1);
        xfLen_ = std::max(1, static_cast<int>(kCrossfadeSeconds * fsi_));
        gainCoef_ = 1.0 - std::exp(-1.0 / (kGainSmoothSeconds * fs_));
        for (auto& v : buf_) v.assign(static_cast<std::size_t>(maxBlock), 0.0);
        measureLatency();
        setControls(controls_);
        reset();
    }

    void setTables(const Tables& t) { tables_ = t; setControls(controls_); }   // generator / harness only (message thread)
    const Tables& tables() const noexcept { return tables_; }

    void setControls(const Controls& in) noexcept
    {
        Controls c = in;
        c.eq = eqFor(c.speed, c.eq);
        c.overbiasDb = std::min(std::max(c.overbiasDb, -6.0), 8.0);
        c.inputDb = std::min(std::max(c.inputDb, -24.0), 24.0); c.outputDb = std::min(std::max(c.outputDb, -24.0), 24.0);
        controls_ = c;
        buildElectro(c, slots_[writeIdx_]);
        const int old = mailbox_.exchange(writeIdx_ | kDirty, std::memory_order_acq_rel);
        writeIdx_ = old & 3;
        atr::Controls m;
        m.tape = c.tape; m.speed = c.speed; m.overbiasDb = c.overbiasDb;
        m.recordLevelDb = 20.0 * std::log10(kOperatingLevelsNwbm[static_cast<int>(c.level)] / referenceNwbm(c.tape));
        medium_.setControls(m);
        inTarget_.store(std::pow(10.0, (c.inputDb - kZeroVuDbfs) / 20.0), std::memory_order_relaxed);
        outTarget_.store(std::pow(10.0, (c.outputDb + kZeroVuDbfs + (std::isnan(makeupOverride_) ? tables_.makeupDb[static_cast<int>(c.tape)][static_cast<int>(c.eq)][static_cast<int>(c.level)] : makeupOverride_)) / 20.0), std::memory_order_relaxed);
    }
    const Controls& controls() const noexcept { return controls_; }

    void reset() noexcept
    {
        hb_.reset(); medium_.reset();
        if (mailbox_.load(std::memory_order_acquire) & kDirty) adopt();
        old_ = cur_;
        for (int c = 0; c < kMaxChannels; ++c)
        {
            st_[c] = State {}; inGain_[c] = inTarget_.load(std::memory_order_relaxed); outGain_[c] = outTarget_.load(std::memory_order_relaxed);
            xfPos_[c] = xfLen_;
        }
    }

    // In place; io[c] = n samples of channel c, full scale 1.0 (n is chunked to maxBlock).
    void process(double* const* io, int channels, int n) noexcept
    {
        const int nch = std::min(channels, nch_);
        if (pollMailbox()) for (int c = 0; c < kMaxChannels; ++c) { xfPos_[c] = 0; st_[c].prev = st_[c].cur; }
        const double gi = inTarget_.load(std::memory_order_relaxed), go = outTarget_.load(std::memory_order_relaxed);
        for (int done = 0; done < n;)
        {
            const int m = std::min(maxBlock_, n - done);
            double* up[kMaxChannels] {};
            for (int c = 0; c < nch; ++c)
            {
                double* x = buf_[c].data();
                double g = inGain_[c];
                for (int i = 0; i < m; ++i) { g += (gi - g) * gainCoef_; const double v = io[c][done + i]; x[i] = std::abs(v) < 1e30 ? v * g : 0.0; }
                inGain_[c] = g;
                up[c] = hb_.up(c, x, m);
                runPre(c, up[c], m * os_);
            }
            medium_.process(up, nch, m * os_);
            for (int c = 0; c < nch; ++c)
            {
                runPost(c, up[c], m * os_);
                if (os_ == 1) std::copy(up[c], up[c] + m, io[c] + done); else hb_.down(c, io[c] + done, m);
                double g = outGain_[c];
                for (int i = 0; i < m; ++i) { g += (go - g) * gainCoef_; io[c][done + i] *= g; }
                outGain_[c] = g;
            }
            done += m;
        }
        hb_.snap();
        for (int c = 0; c < nch; ++c) { flushAll(st_[c].cur); flushAll(st_[c].prev); }
    }

    double latencySamples() const noexcept { return latency_; }   // oversampler pair, low-frequency phase delay (electronics group delay not included)
    int oversampling() const noexcept { return os_; }
    double internalRate() const noexcept { return fsi_; }
    atr::AtrMedium& medium() noexcept { return medium_; }

    // Response helpers for the harness (analytic on the digital filters of the current snapshot, small signal, medium excluded).
    cplx recordResponse(double hz) const noexcept { return cur_.rec[0].response(hz, fsi_) * cur_.mixA + cur_.rec[1].response(hz, fsi_) * cur_.mixB; }
    cplx postResponse(double hz) const noexcept { return cur_.loss.response(hz, fsi_) * cur_.bump.response(hz, fsi_) * cur_.repro.response(hz, fsi_); }
    cplx lossResponse(double hz) const noexcept { return cur_.loss.response(hz, fsi_); }
    cplx bumpResponse(double hz) const noexcept { return cur_.bump.response(hz, fsi_); }
    cplx reproResponse(double hz) const noexcept { return cur_.repro.response(hz, fsi_); }
    double clipLevel() const noexcept { return cur_.clip; }
    // Alignment overrides: record trim position (0..1) and makeup gain in dB replace the table values (generator only; call before process).
    void overrideTrim(double pot) { trimOverride_ = pot; setControls(controls_); }
    void overrideMakeupDb(double db) { makeupOverride_ = db; setControls(controls_); }
    void setBumpEnabled(bool on) { tables_.bump.enabled = on; setControls(controls_); }

private:
    static constexpr int kDirty = 4;
    struct Electro
    {
        Sos rec[2]; double mixA = 1.0, mixB = 0.0; double clip = 1e9;
        Sos loss, bump, repro;
    };
    struct ChanState { SosState rec[2], loss, bump, repro; };
    struct State { ChanState cur, prev; };
    static void flushAll(ChanState& s) noexcept { flush(s.rec[0]); flush(s.rec[1]); flush(s.loss); flush(s.bump); flush(s.repro); }

    void buildElectro(const Controls& c, Electro& e) const noexcept
    {
        const int t = static_cast<int>(c.tape), q = static_cast<int>(c.eq);
        const double pot = trimOverride_ >= 0.0 ? trimOverride_ : tables_.trimPot[t][q];
        const double g = potWeight(pot);
        e.rec[0] = designSos(recordRoots(c.eq, 0, tables_), fsi_, 1000.0, false);
        e.rec[1] = designSos(recordRoots(c.eq, 1, tables_), fsi_, 1000.0, false);
        // REC GAIN: normalise the mixed head current to unity at 1 kHz (polarity positive), as the technician sets the flux
        const cplx h = e.rec[0].response(1000.0, fsi_) * (1.0 - g) + e.rec[1].response(1000.0, fsi_) * g;
        const double n = (h.real() < 0 ? -1.0 : 1.0) / std::abs(h);
        e.mixA = (1.0 - g) * n; e.mixB = g * n;
        e.clip = std::pow(10.0, kRecordClipDbOverVu / 20.0);
        e.loss = designSos(lossRoots(tables_.loss[static_cast<int>(c.speed)]), fsi_);
        e.bump = designSos(bumpRoots(tables_.bump, c.speed), fsi_, 1000.0);
        e.repro = designSos(reproRoots(c.eq, tables_), fsi_, 1000.0, false);
    }

    void runPre(int c, double* y, int len) noexcept
    {
        ChanState& cs = st_[c].cur; ChanState& ps = st_[c].prev;
        const double clip = cur_.clip;
        int i = 0;
        if (xfPos_[c] < xfLen_)
        {
            const int stop = std::min(len, xfLen_ - xfPos_[c]);
            const double inv = 1.0 / static_cast<double>(xfLen_);
            for (; i < stop; ++i)
            {
                const double x = y[i];
                const double a = cur_.mixA * runSos(cur_.rec[0], cs.rec[0], x) + cur_.mixB * runSos(cur_.rec[1], cs.rec[1], x);
                const double b = old_.mixA * runSos(old_.rec[0], ps.rec[0], x) + old_.mixB * runSos(old_.rec[1], ps.rec[1], x);
                const double w = static_cast<double>(xfPos_[c] + i + 1) * inv;
                y[i] = std::min(std::max(b + w * (a - b), -clip), clip);
            }
        }
        for (; i < len; ++i)
        {
            const double x = y[i];
            const double a = cur_.mixA * runSos(cur_.rec[0], cs.rec[0], x) + cur_.mixB * runSos(cur_.rec[1], cs.rec[1], x);
            y[i] = std::min(std::max(a, -clip), clip);
        }
        // the cross-fade counter advances once per block in runPost (after both stages have used it)
    }
    void runPost(int c, double* y, int len) noexcept
    {
        ChanState& cs = st_[c].cur; ChanState& ps = st_[c].prev;
        int i = 0;
        if (xfPos_[c] < xfLen_)
        {
            const int stop = std::min(len, xfLen_ - xfPos_[c]);
            const double inv = 1.0 / static_cast<double>(xfLen_);
            for (; i < stop; ++i)
            {
                const double x = y[i];
                const double a = runSos(cur_.repro, cs.repro, runSos(cur_.bump, cs.bump, runSos(cur_.loss, cs.loss, x)));
                const double b = runSos(old_.repro, ps.repro, runSos(old_.bump, ps.bump, runSos(old_.loss, ps.loss, x)));
                const double w = static_cast<double>(xfPos_[c] + i + 1) * inv;
                y[i] = b + w * (a - b);
            }
            xfPos_[c] += stop;
        }
        for (; i < len; ++i) y[i] = runSos(cur_.repro, cs.repro, runSos(cur_.bump, cs.bump, runSos(cur_.loss, cs.loss, y[i])));
    }

    void adopt() noexcept { const int old = mailbox_.exchange(readIdx_, std::memory_order_acq_rel); readIdx_ = old & 3; cur_ = slots_[readIdx_]; }
    bool pollMailbox() noexcept
    {
        if (!(mailbox_.load(std::memory_order_acquire) & kDirty)) return false;
        old_ = cur_; adopt(); return true;
    }

    // Low-frequency phase delay of the oversampler pair, probed with a 512-sample-period sine over 4 whole periods (the last 2 are
    // demodulated). The probe is fed in chunks of at most maxBlock, so the result is independent of maxBlock (the cascade is block invariant).
    void measureLatency()
    {
        if (os_ == 1) { latency_ = 0.0; return; }
        constexpr int kPeriod = 512, kTotal = 4 * kPeriod;
        const int chunk = std::max(1, std::min(maxBlock_, kPeriod));
        double* b = buf_[0].data();
        hb_.reset();
        double sc = 0.0, ss = 0.0;
        const double w = 2.0 * kPi / kPeriod;
        std::vector<double> o(static_cast<std::size_t>(chunk));
        for (int pos = 0; pos < kTotal; pos += chunk)
        {
            const int n = std::min(chunk, kTotal - pos);
            for (int i = 0; i < n; ++i) b[i] = std::sin(w * (pos + i));
            hb_.up(0, b, n);
            hb_.down(0, o.data(), n);
            for (int i = 0; i < n; ++i) { const int t = pos + i; if (t >= 2 * kPeriod) { ss += o[static_cast<std::size_t>(i)] * std::sin(w * t); sc += o[static_cast<std::size_t>(i)] * std::cos(w * t); } }
        }
        latency_ = -std::atan2(sc, ss) / w;
        hb_.reset();
    }

    static_assert(std::atomic<double>::is_always_lock_free && std::atomic<int>::is_always_lock_free, "the control hand-off must be lock-free");
    double fs_ = 48000.0, fsi_ = 192000.0, gainCoef_ = 0.01, latency_ = 0.0;
    int maxBlock_ = 0, nch_ = 1, os_ = 4, xfLen_ = 1;
    Controls controls_;
    Tables tables_ = kAlign;
    double trimOverride_ = -1.0, makeupOverride_ = std::nan("");
    atr::HalfBandCascade hb_;
    atr::AtrMedium medium_;
    std::vector<double> buf_[kMaxChannels];
    Electro slots_[3] {}, cur_ {}, old_ {};
    std::atomic<int> mailbox_ { 1 };
    int writeIdx_ = 0, readIdx_ = 2;
    std::atomic<double> inTarget_ { 1.0 }, outTarget_ { 1.0 };
    State st_[kMaxChannels] {};
    double inGain_[kMaxChannels] { 1.0, 1.0 }, outGain_[kMaxChannels] { 1.0, 1.0 };
    int xfPos_[kMaxChannels] { 0, 0 };
};
}
