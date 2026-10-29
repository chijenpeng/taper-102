#include "PluginProcessor102.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <thread>
#include <vector>

// Allocation counter for the audio thread only (the control pusher thread may allocate).
namespace allocation { thread_local bool enabled = false; thread_local std::size_t count = 0; }
void* operator new(std::size_t size){ if(allocation::enabled) ++allocation::count; if(auto* p=std::malloc(size?size:1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t size){ return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace
{
using taper::t102::Eq;
using taper::t102::OperatingLevel;
int checks = 0;
void require(bool condition, const char* text)
{
    ++checks;
    if (std::getenv("T102_TRACE")) std::cerr << "check " << checks << ": " << text << '\n';
    if (!condition) { std::cerr << "FAIL " << text << '\n'; std::exit(1); }
}
void set(Taper102AudioProcessor& p, const char* id, float native)
{
    auto* parameter = p.parameters.getParameter(id);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(native));
}
float value(Taper102AudioProcessor& p, const char* id) { return p.parameters.getRawParameterValue(id)->load(); }
juce::MemoryBlock encode(juce::ValueTree state)
{
    juce::MemoryBlock block;
    auto xml = state.createXml();
    juce::AudioProcessor::copyXmlToBinary(*xml, block);
    return block;
}
float tone(int n, double amp = .1) { return static_cast<float>(amp * std::sin(.0731 * n) + .5 * amp * std::sin(.31 * n)); }

void configure(Taper102AudioProcessor& p, int channels, double rate, int block)
{
    const auto set = channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    p.setPlayConfigDetails(channels, channels, rate, block);
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add(set);
    layout.outputBuses.add(set);
    require(p.setBusesLayout(layout), "layout accepted");
    p.setNonRealtime(false);
}
// Renders `total` samples of the test tone in blocks; returns channel 0 (and channel 1 into `right` if stereo).
std::vector<float> render(Taper102AudioProcessor& p, int channels, int total, int block, std::vector<float>* right = nullptr,
                          bool bypassedCall = false, double rightAmp = .07)
{
    std::vector<float> out;
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> audio(channels, block);
    for (int offset = 0; offset < total; offset += block)
    {
        const int n = std::min(block, total - offset);
        audio.setSize(channels, n, false, false, true);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < n; ++i) audio.setSample(c, i, tone(offset + i, c == 0 ? .1 : rightAmp));
        if (bypassedCall) p.processBlockBypassed(audio, midi); else p.processBlock(audio, midi);
        for (int i = 0; i < n; ++i)
        {
            out.push_back(audio.getSample(0, i));
            if (channels == 2 && right) right->push_back(audio.getSample(1, i));
        }
    }
    return out;
}
bool allFinite(const std::vector<float>& v) { for (float x : v) if (!std::isfinite(x)) return false; return true; }
double rms(const std::vector<float>& v, std::size_t from)
{
    double s = 0; for (std::size_t i = from; i < v.size(); ++i) s += double(v[i]) * v[i];
    return std::sqrt(s / double(v.size() - from));
}
}

int main()
{
    std::cout << std::unitbuf;
    const juce::ScopedJuceInitialiser_GUI gui;
    const char* ids[] { "setup", "input_db", "output_db", "bypass", "link", "ref" };
    const auto near = [](float x, float y) { return std::abs(x - y) < 1e-3f; };
    {
        Taper102AudioProcessor p;
        require(p.getParameters().size() == Taper102AudioProcessor::numParameters && Taper102AudioProcessor::numParameters == 6, "six parameters");
        require(p.getName() == "Taper 102", "name");
        for (const char* removed : { "speed", "eq", "tape", "level", "bias" }) require(p.parameters.getParameter(removed) == nullptr, "removed parameters absent");
        const auto s = p.currentControls();
        require(s.chain.speed == taper::atr::Speed::Ips15 && s.chain.eq == Eq::Nab15 && s.chain.tape == taper::atr::Tape::Ampex456
                && s.chain.level == OperatingLevel::Nwb370 && s.chain.overbiasDb == 0.0 && s.chain.inputDb == 0.0
                && s.chain.outputDb == 0.0 && !s.bypass && s.refDbfs == -12.0, "chain receives the default controls");
        require(value(p, "setup") == 0 && near(value(p, "input_db"), 0) && near(value(p, "output_db"), 0) && value(p, "bypass") == 0
                && value(p, "link") == 0 && value(p, "ref") == 2 && p.getZeroVuDbfs() == -12.0, "defaults (456 15 NAB, 0 VU = -12 dBFS)");
        const auto native = [&](const char* id, float norm) { return p.parameters.getParameter(id)->convertFrom0to1(norm); };
        require(near(native("input_db", 0), -24) && near(native("input_db", 1), 24) && near(native("output_db", 0), -24) && near(native("output_db", 1), 24), "range ends");

        // every setup maps to the right chain controls
        struct Row { int setup; taper::atr::Speed speed; Eq eq; taper::atr::Tape tape; OperatingLevel level; };
        for (const Row& r : { Row { 0, taper::atr::Speed::Ips15, Eq::Nab15, taper::atr::Tape::Ampex456, OperatingLevel::Nwb370 },
                              Row { 1, taper::atr::Speed::Ips30, Eq::Aes30, taper::atr::Tape::Ampex456, OperatingLevel::Nwb370 },
                              Row { 2, taper::atr::Speed::Ips15, Eq::Iec15, taper::atr::Tape::AtrMaster, OperatingLevel::Nwb320 },
                              Row { 3, taper::atr::Speed::Ips30, Eq::Aes30, taper::atr::Tape::AtrMaster, OperatingLevel::Nwb320 } })
        {
            set(p, "setup", static_cast<float>(r.setup));
            const auto c = p.currentControls().chain;
            require(c.speed == r.speed && c.eq == r.eq && c.tape == r.tape && c.level == r.level && c.overbiasDb == 0.0, "setup maps to speed, EQ, tape, aligned level and bias");
        }
        set(p, "setup", 0);

        // State round trip at native precision.
        set(p, "setup", 3); set(p, "input_db", 7.12f); set(p, "output_db", -4.76f); set(p, "bypass", 1); set(p, "link", 1); set(p, "ref", 3);
        float saved[6];
        for (int i = 0; i < 6; ++i) saved[i] = value(p, ids[i]);
        juce::MemoryBlock block; p.getStateInformation(block);
        Taper102AudioProcessor q;
        q.setStateInformation(block.getData(), static_cast<int>(block.getSize()));
        for (int i = 0; i < 6; ++i) require(value(q, ids[i]) == saved[i], "state round trip");
        auto xml = juce::AudioProcessor::getXmlFromBinary(block.getData(), static_cast<int>(block.getSize()));
        require(xml->getIntAttribute("schemaVersion") == 2, "schema version 2 written");

        // Other schemas, unknown ids, NaN, duplicates and junk bytes: ignored, state unchanged.
        auto state = juce::ValueTree::fromXml(*xml);
        for (int variant = 0; variant < 7; ++variant)
        {
            auto bad = state.createCopy();
            if (variant == 0) bad.setProperty("schemaVersion", 3, nullptr);
            if (variant == 1) bad.removeProperty("schemaVersion", nullptr);
            if (variant == 2) bad.getChild(0).setProperty("value", "nan", nullptr);
            if (variant == 3) bad.getChild(0).setProperty("id", "drive_db", nullptr);
            if (variant == 4) bad.setProperty("schemaVersion", 99, nullptr);
            if (variant == 5) bad.getChild(1).setProperty("id", bad.getChild(0).getProperty("id"), nullptr);
            if (variant == 6) bad.getChild(1).setProperty("id", "bias", nullptr);   // an old id inside a schema-2 state
            Taper102AudioProcessor fresh;
            auto enc = encode(bad);
            fresh.setStateInformation(enc.getData(), static_cast<int>(enc.getSize()));
            require(value(fresh, "setup") == 0 && near(value(fresh, "input_db"), 0) && value(fresh, "bypass") == 0 && value(fresh, "ref") == 2, "invalid state keeps defaults");
        }
        const char junk[] = "not a state";
        Taper102AudioProcessor fresh;
        fresh.setStateInformation(junk, sizeof junk);
        fresh.setStateInformation(nullptr, 0);
        require(value(fresh, "setup") == 0 && value(fresh, "ref") == 2, "junk state ignored");

        // Schema-1 migration: (tape, speed) -> setup; input/output/link/ref/bypass kept; level and bias ignored.
        const auto old = [&](float speed, float eq, float tape, float level, float bias, bool withLinkRef)
        {
            juce::ValueTree t("Taper102State");
            t.setProperty("schemaVersion", 1, nullptr);
            const auto add = [&](const char* id, float v) { juce::ValueTree c("PARAM"); c.setProperty("id", id, nullptr); c.setProperty("value", v, nullptr); t.appendChild(c, nullptr); };
            add("speed", speed); add("eq", eq); add("tape", tape); add("level", level); add("bias", bias);
            add("input_db", 6.5f); add("output_db", -2.5f); add("bypass", 1);
            if (withLinkRef) { add("link", 1); add("ref", 3); }
            return encode(t);
        };
        struct Mig { float speed, eq, tape; int expect; };
        for (const Mig& m : { Mig { 0, 0, 0, 0 }, Mig { 1, 2, 0, 1 }, Mig { 0, 1, 1, 2 }, Mig { 1, 2, 1, 3 }, Mig { 0, 1, 0, 0 } /* 456 15 IEC becomes NAB */ })
        {
            Taper102AudioProcessor mp;
            set(mp, "link", 0);
            auto enc = old(m.speed, m.eq, m.tape, 1, 4.0f, true);
            mp.setStateInformation(enc.getData(), static_cast<int>(enc.getSize()));
            require(value(mp, "setup") == static_cast<float>(m.expect), "schema 1 maps (tape, speed) to the setup");
            require(near(value(mp, "input_db"), 6.5f) && near(value(mp, "output_db"), -2.5f) && value(mp, "bypass") == 1 && value(mp, "link") == 1 && value(mp, "ref") == 3, "schema 1 keeps input, output, bypass, link and 0 VU");
            const auto c = mp.currentControls().chain;
            require(c.overbiasDb == 0.0 && c.level == (m.tape == 1 ? OperatingLevel::Nwb320 : OperatingLevel::Nwb370), "old level and bias are ignored");
        }
        {   // a schema-1 state from before Link / 0 VU existed: link off, 0 VU -18
            Taper102AudioProcessor mp; set(mp, "link", 1);
            auto enc = old(0, 0, 0, 3, 0, false);
            mp.setStateInformation(enc.getData(), static_cast<int>(enc.getSize()));
            require(value(mp, "link") == 0 && value(mp, "ref") == 0 && mp.getZeroVuDbfs() == -18.0, "old state without link / 0 VU loads with Link off and -18 dBFS");
        }
        {   // a schema-1 state with an unknown id is invalid
            auto t = juce::ValueTree::fromXml(*juce::AudioProcessor::getXmlFromBinary(old(0, 0, 0, 3, 0, true).getData(), static_cast<int>(old(0, 0, 0, 3, 0, true).getSize())));
            t.getChild(0).setProperty("id", "drive_db", nullptr);
            Taper102AudioProcessor mp; auto enc = encode(t);
            mp.setStateInformation(enc.getData(), static_cast<int>(enc.getSize()));
            require(value(mp, "setup") == 0 && value(mp, "ref") == 2 && near(value(mp, "input_db"), 0), "invalid schema-1 state is ignored");
        }
    }

    // Latency at the common rates (reported = the chain's oversampler latency, rounded).
    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        Taper102AudioProcessor p; configure(p, 2, rate, 256); p.prepareToPlay(rate, 256);
        const int latency = p.getLatencySamples();
        require(!p.hasProcessingError() && (rate > 150000.0 ? latency == 0 : latency > 0 && latency < 12), "latency reported, prepared");
        std::cout << "latency " << rate << " Hz: " << latency << " samples\n";
    }

    // Mono and stereo, silence, extremes: finite output; mono and stereo-with-other-right agree on the left channel.
    for (int channels : { 1, 2 })
    {
        Taper102AudioProcessor p; configure(p, channels, 48000, 128); p.prepareToPlay(48000, 128);
        juce::AudioBuffer<float> silence(channels, 128); juce::MidiBuffer midi;
        for (int b = 0; b < 40; ++b) { silence.clear(); p.processBlock(silence, midi);
            for (int c = 0; c < channels; ++c) for (int i = 0; i < 128; ++i) require(std::isfinite(silence.getSample(c, i)), "silence in finite out"); }
        require(p.getPreparedChannelCount() == channels, "channel count");
        for (int setup = 0; setup < 4; ++setup)
        {
            set(p, "setup", static_cast<float>(setup)); set(p, "input_db", 24); set(p, "output_db", 24); set(p, "ref", 3);
            p.pushControlsNow();
            std::vector<float> r;
            const auto out = render(p, channels, 12000, 128, &r);
            require(allFinite(out) && allFinite(r), "extreme controls finite");
            set(p, "input_db", -24); set(p, "output_db", -24); set(p, "ref", 0);
            p.pushControlsNow();
            r.clear();
            require(allFinite(render(p, channels, 6000, 128, &r)) && allFinite(r), "other extremes finite");
        }
        require(!p.hasProcessingError(), "no processing error");
    }
    {   // hostile input values stay finite
        Taper102AudioProcessor p; configure(p, 2, 48000, 64); p.prepareToPlay(48000, 64);
        juce::AudioBuffer<float> audio(2, 64); juce::MidiBuffer midi;
        const float bad[] { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 1e30f, -1e30f, 1e-40f };
        for (int b = 0; b < 20; ++b)
        {
            for (int c = 0; c < 2; ++c) for (int i = 0; i < 64; ++i) audio.setSample(c, i, bad[(i + b + c) % 5]);
            p.processBlock(audio, midi);
            for (int c = 0; c < 2; ++c) for (int i = 0; i < 64; ++i) require(std::isfinite(audio.getSample(c, i)), "NaN/Inf/huge input gives finite output");
        }
    }
    {   // mono equals the left channel of a stereo run whose right channel differs (independent channels)
        std::vector<float> mono, left, right;
        { Taper102AudioProcessor p; configure(p, 1, 48000, 128); p.setNonRealtime(true); p.prepareToPlay(48000, 128); mono = render(p, 1, 9600, 128); }
        { Taper102AudioProcessor p; configure(p, 2, 48000, 128); p.setNonRealtime(true); p.prepareToPlay(48000, 128); left = render(p, 2, 9600, 128, &right, false, .9); }
        double worst = 0;
        for (std::size_t i = 0; i < mono.size(); ++i) worst = std::max(worst, double(std::abs(mono[i] - left[i])));
        require(worst == 0.0, "stereo channels are independent (mono equals left)");
    }
    {   // Unsupported layout rejected.
        Taper102AudioProcessor p;
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add(juce::AudioChannelSet::stereo()); layout.outputBuses.add(juce::AudioChannelSet::mono());
        require(!p.setBusesLayout(layout), "mismatched layout rejected");
    }

    // Block-size invariance through the processor (non-realtime), per setup and rate.
    for (double rate : { 44100.0, 96000.0 })
        for (int setup : { 0, 3 })
        {
            std::vector<float> reference, referenceR;
            for (int block : { 64, 512, 100 })
            {
                Taper102AudioProcessor p; configure(p, 2, rate, block); p.setNonRealtime(true);
                set(p, "input_db", 6); set(p, "setup", static_cast<float>(setup));
                p.prepareToPlay(rate, block);
                std::vector<float> right;
                const auto outL = render(p, 2, 19200, block, &right);
                if (reference.empty()) { reference = outL; referenceR = right; }
                else
                {
                    double worst = 0;
                    for (std::size_t i = 0; i < reference.size(); ++i)
                        worst = std::max({ worst, double(std::abs(reference[i] - outL[i])), double(std::abs(referenceR[i] - right[i])) });
                    require(worst < 1e-6, "block-size invariance");
                }
            }
        }

    // Output -6 dB lowers the level by 6 dB; 0 VU in returns 0 VU out in every setup (and every 0 VU setting).
    {
        const auto level = [&](int setup, int ref, double dbfs, float outDb = 0.0f)
        {
            Taper102AudioProcessor p; configure(p, 2, 48000, 256); p.setNonRealtime(true);
            set(p, "setup", static_cast<float>(setup)); set(p, "ref", static_cast<float>(ref)); set(p, "output_db", outDb);
            p.prepareToPlay(48000, 256);
            juce::MidiBuffer midi; juce::AudioBuffer<float> audio(2, 256);
            const double amp = std::pow(10.0, dbfs / 20.0);
            std::vector<float> o;
            for (int offset = 0; offset < 24000; offset += 256)
            {
                for (int c = 0; c < 2; ++c) for (int i = 0; i < 256; ++i) audio.setSample(c, i, static_cast<float>(amp * std::sin(2 * 3.14159265358979 * 997.0 * (offset + i) / 48000.0)));
                p.processBlock(audio, midi);
                for (int i = 0; i < 256; ++i) o.push_back(audio.getSample(0, i));
            }
            return rms(o, 12000) / (amp / std::sqrt(2.0));
        };
        for (int setup = 0; setup < 4; ++setup)
            for (int ref = 0; ref < 4; ++ref)
            {
                require(std::abs(20 * std::log10(level(setup, ref, -18.0 + 3.0 * ref))) < 0.02, "0 VU in returns 0 VU out (every setup, every 0 VU setting)");
                require(std::abs(20 * std::log10(level(setup, ref, -50.0))) < 0.05, "small-signal level is unchanged");
            }
        require(std::abs(20 * std::log10(level(0, 2, -12.0, -6.0) / level(0, 2, -12.0)) + 6.0) < 0.01, "Output -6 dB lowers the level by 6 dB");

        // 0 VU switching mid-stream is click-free
        Taper102AudioProcessor p; configure(p, 2, 48000, 128); p.setNonRealtime(true); p.prepareToPlay(48000, 128);
        juce::MidiBuffer midi; juce::AudioBuffer<float> audio(2, 128);
        std::vector<float> o; int n = 0;
        for (int b = 0; b < 300; ++b)
        {
            if (b == 100) set(p, "ref", 0);
            if (b == 200) set(p, "ref", 3);
            for (int i = 0; i < 128; ++i, ++n) { const float v = static_cast<float>(0.05 * std::sin(2 * 3.14159265358979 * 997.0 * n / 48000.0)); audio.setSample(0, i, v); audio.setSample(1, i, v); }
            p.processBlock(audio, midi);
            for (int i = 0; i < 128; ++i) o.push_back(audio.getSample(0, i));
        }
        double step = 0; for (std::size_t i = 1; i < o.size(); ++i) step = std::max(step, double(std::abs(o[i] - o[i - 1])));
        require(allFinite(o) && step < 0.04, "0 VU switching is click-free");
        // setup switching mid-stream is finite and click-free
        Taper102AudioProcessor s; configure(s, 2, 48000, 128); s.setNonRealtime(true); s.prepareToPlay(48000, 128);
        std::vector<float> o2; n = 0;
        for (int b = 0; b < 300; ++b)
        {
            if (b == 100) set(s, "setup", 1);
            if (b == 200) set(s, "setup", 2);
            for (int i = 0; i < 128; ++i, ++n) { const float v = static_cast<float>(0.05 * std::sin(2 * 3.14159265358979 * 997.0 * n / 48000.0)); audio.setSample(0, i, v); audio.setSample(1, i, v); }
            s.processBlock(audio, midi);
            for (int i = 0; i < 128; ++i) o2.push_back(audio.getSample(0, i));
        }
        double step2 = 0; for (std::size_t i = 1; i < o2.size(); ++i) step2 = std::max(step2, double(std::abs(o2[i] - o2[i - 1])));
        require(allFinite(o2) && step2 < 0.04, "setup switching is click-free");
    }

    // Link: with Link on, a user edit of Input by d dB moves the Output parameter by -d dB (clamped); the DSP has no hidden compensation.
    {
        Taper102AudioProcessor p;
        set(p, "input_db", 5); set(p, "output_db", 0);
        require(near(value(p, "output_db"), 0), "link off: Input does not move Output");
        set(p, "input_db", 0);
        set(p, "link", 1);
        require(near(value(p, "input_db"), 0) && near(value(p, "output_db"), 0), "toggling Link moves nothing");
        set(p, "input_db", 3);
        require(near(value(p, "output_db"), -3), "Input 0 -> +3 moves Output 0 -> -3");
        set(p, "input_db", -2);
        require(near(value(p, "output_db"), 2), "Input +3 -> -2 moves Output -3 -> +2");
        set(p, "output_db", -10);
        require(near(value(p, "input_db"), -2), "Output edits do not move Input");
        set(p, "input_db", 20);
        require(near(value(p, "output_db"), -24) && near(value(p, "input_db"), 20), "Output clamps at -24, Input keeps moving");
        set(p, "input_db", 21);
        require(near(value(p, "output_db"), -24), "Output stays at its end");
        set(p, "output_db", 22); set(p, "input_db", 17);
        require(near(value(p, "output_db"), 24), "Output clamps at +24");
        set(p, "link", 0); set(p, "input_db", 0);
        require(near(value(p, "output_db"), 24), "Link turned off: no coupling");
        Taper102AudioProcessor src; set(src, "input_db", 7); set(src, "output_db", 3); set(src, "link", 1);
        juce::MemoryBlock blk; src.getStateInformation(blk);
        Taper102AudioProcessor dst; set(dst, "link", 1); set(dst, "input_db", 1); set(dst, "output_db", 1);
        dst.setStateInformation(blk.getData(), static_cast<int>(blk.getSize()));
        require(near(value(dst, "input_db"), 7) && near(value(dst, "output_db"), 3) && value(dst, "link") == 1, "state restore does not couple");
        set(dst, "input_db", 8);
        require(near(value(dst, "output_db"), 2), "coupling resumes after a restore");
        Taper102AudioProcessor q; set(q, "link", 1);
        std::thread([&] { q.parameters.getParameter("input_db")->setValueNotifyingHost(q.parameters.getParameter("input_db")->convertTo0to1(4.0f)); }).join();
        require(near(value(q, "output_db"), 0), "off-thread Input change is not applied on that thread");
        q.applyPendingLink();
        require(near(value(q, "output_db"), -4), "deferred Input change moves Output on the message thread");
    }

    // Bypass: latency-aligned dry through the parameter and through processBlockBypassed; toggling mid-stream stays finite and click-free.
    for (int mode = 0; mode < 2; ++mode)
    {
        Taper102AudioProcessor p; configure(p, 2, 48000, 256); p.setNonRealtime(true);
        if (mode == 0) set(p, "bypass", 1);
        p.prepareToPlay(48000, 256);
        const int L = p.getLatencySamples();
        const auto out = render(p, 2, 12000, 256, nullptr, mode == 1);
        double worst = 0;
        for (int i = 4800 + L; i < 12000; ++i) worst = std::max(worst, double(std::abs(out[static_cast<std::size_t>(i)] - tone(i - L))));
        require(worst < 1e-6, mode == 0 ? "bypass parameter is latency-aligned dry" : "host bypass is latency-aligned dry");
    }
    {
        Taper102AudioProcessor p; configure(p, 2, 48000, 256); p.setNonRealtime(true); p.prepareToPlay(48000, 256);
        render(p, 2, 4800, 256);
        set(p, "bypass", 1);
        auto b = render(p, 2, 4800, 256);
        double step = 0;
        for (std::size_t i = 1; i < b.size(); ++i) step = std::max(step, double(std::abs(b[i] - b[i - 1])));
        require(allFinite(b) && step < 0.1, "bypass crossfade is click-free");
    }

    // Realtime path: control changes reach the chain through the background pusher; audio thread allocation-free.
    {
        Taper102AudioProcessor p; configure(p, 2, 48000, 128); p.prepareToPlay(48000, 128);
        juce::AudioBuffer<float> audio(2, 128); juce::MidiBuffer midi;
        const auto run = [&](int blocks, bool count)
        {
            std::vector<float> o;
            for (int b = 0; b < blocks; ++b)
            {
                for (int c = 0; c < 2; ++c) for (int i = 0; i < 128; ++i) audio.setSample(c, i, tone(b * 128 + i));
                const auto before = allocation::count;
                allocation::enabled = count; p.processBlock(audio, midi); allocation::enabled = false;
                if (count) require(before == allocation::count, "processBlock allocation-free (realtime, with control changes)");
                for (int i = 0; i < 128; ++i) o.push_back(audio.getSample(0, i));
            }
            return o;
        };
        const auto a = run(200, true);
        set(p, "input_db", 12);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const auto b = run(200, true);
        set(p, "setup", 3); set(p, "ref", 3); set(p, "bypass", 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto c = run(200, true);
        require(allFinite(a) && allFinite(b) && allFinite(c), "realtime output finite");
        require(std::abs(rms(b, 12800) / rms(a, 12800) - 1) > .05, "realtime control change reaches the chain");
        require(!p.hasProcessingError(), "realtime no error");
    }

    // CPU in a host-like run (reported, not asserted).
    for (double rate : { 48000.0, 96000.0 })
    {
        Taper102AudioProcessor p; configure(p, 2, rate, 512); p.prepareToPlay(rate, 512);
        juce::AudioBuffer<float> audio(2, 512); juce::MidiBuffer midi;
        const int blocks = static_cast<int>(10.0 * rate / 512);
        double best = 1e9;
        for (int rep = 0; rep < 3; ++rep)
        {
            const auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < blocks; ++b)
            {
                for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i) audio.setSample(c, i, tone(b * 512 + i, .25));
                p.processBlock(audio, midi);
            }
            best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
        std::cout << "CPU " << rate << " Hz stereo, 512 block: " << 100.0 * best / (blocks * 512.0 / rate) << " % of one core (includes the test-tone generation)\n";
    }
    std::cout << checks << " Taper 102 processor checks passed\n";
}
