#include "PluginProcessor102.h"
#include "PluginEditor102.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>

namespace
{
using taper::t102::Eq;
using taper::t102::OperatingLevel;

// Order matters: values_[] and the schema-2 state check use this table.
constexpr const char* ids[] { "setup", "input_db", "output_db", "bypass", "link", "ref" };
enum Slot : std::size_t { kSetup = 0, kInput, kOutput, kBypass, kLink, kRef };
// Schema 1 (speed / EQ / tape / operating level / bias as separate parameters) is migrated by setupFromOld().
constexpr const char* oldIds[] { "speed", "eq", "tape", "level", "bias", "input_db", "output_db", "bypass", "link", "ref" };

bool nativeValue(const juce::var& var, float& value)
{
    const auto text = var.toString().trim();
    const auto utf8 = text.toRawUTF8();
    char* end = nullptr;
    const double parsed = std::strtod(utf8, &end);
    if (utf8 == end || end == nullptr || *end != 0 || !std::isfinite(parsed)
        || std::abs(parsed) > std::numeric_limits<float>::max()) return false;
    value = static_cast<float>(parsed);
    return std::isfinite(value);
}
// The chain is calibrated to 0 VU = -18 dBFS; a higher 0 VU setting attenuates before the chain by (-18 - ref) dB and restores after it.
float refGainFor(double refDbfs) noexcept { return static_cast<float>(std::pow(10.0, (-18.0 - refDbfs) / 20.0)); }
bool same(const Taper102AudioProcessor::Snapshot& a, const Taper102AudioProcessor::Snapshot& b) noexcept
{
    using juce::exactlyEqual;
    return a.chain.speed == b.chain.speed && a.chain.eq == b.chain.eq && a.chain.tape == b.chain.tape && a.chain.level == b.chain.level
        && exactlyEqual(a.chain.overbiasDb, b.chain.overbiasDb) && exactlyEqual(a.chain.inputDb, b.chain.inputDb)
        && exactlyEqual(a.chain.outputDb, b.chain.outputDb) && a.bypass == b.bypass
        && exactlyEqual(a.refDbfs, b.refDbfs);
}
}

struct Taper102AudioProcessor::ControlPusher final : juce::Thread
{
    explicit ControlPusher(Taper102AudioProcessor& o) : juce::Thread("Taper 102 controls"), owner(o) {}
    void run() override
    {
        while (!threadShouldExit())
        {
            owner.pushControlsNow();
            wait(10);
        }
    }
    Taper102AudioProcessor& owner;
};

juce::AudioProcessorValueTreeState::ParameterLayout Taper102AudioProcessor::createLayout()
{
    using P = juce::ParameterID;
    using A = juce::AudioParameterFloatAttributes;
    constexpr int v = 2;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<juce::AudioParameterChoice>(P { "setup", v }, "Setup",
        juce::StringArray { "456 15 ips NAB", "456 30 ips AES", "ATR Master 15 ips IEC", "ATR Master 30 ips AES" }, 0));
    layout.add(std::make_unique<juce::AudioParameterFloat>(P { "input_db", v }, "Input",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.01f), 0.0f, A().withLabel("dB").withMeta(true))); // Link: an Input edit moves Output
    layout.add(std::make_unique<juce::AudioParameterFloat>(P { "output_db", v }, "Output",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.01f), 0.0f, A().withLabel("dB")));
    layout.add(std::make_unique<juce::AudioParameterBool>(P { "bypass", v }, "Bypass", false));
    layout.add(std::make_unique<juce::AudioParameterBool>(P { "link", v }, "Link", false));
    layout.add(std::make_unique<juce::AudioParameterChoice>(P { "ref", v }, "0 VU", juce::StringArray { "-18 dBFS", "-15 dBFS", "-12 dBFS", "-9 dBFS" }, 2));
    return layout;
}

struct Taper102AudioProcessor::GestureWatcher final : juce::AudioProcessorParameter::Listener
{
    explicit GestureWatcher(Taper102AudioProcessor& o) : owner(o) {}
    void parameterValueChanged(int, float) override {}
    void parameterGestureChanged(int, bool starting) override { owner.inputGesture(starting); }
    Taper102AudioProcessor& owner;
};

Taper102AudioProcessor::Taper102AudioProcessor()
    : juce::AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "Taper102State", createLayout())
{
    for (std::size_t index = 0; index < values_.size(); ++index)
        values_[index] = parameters.getRawParameterValue(ids[index]);
    lastInput_.store(values_[kInput]->load());
    parameters.addParameterListener("input_db", this);
    gestureWatcher_ = std::make_unique<GestureWatcher>(*this);
    parameters.getParameter("input_db")->addListener(gestureWatcher_.get());
    startTimer(50);
}
Taper102AudioProcessor::~Taper102AudioProcessor()
{
    stopTimer();
    cancelPendingUpdate();
    parameters.getParameter("input_db")->removeListener(gestureWatcher_.get());
    parameters.removeParameterListener("input_db", this);
    stopPusher();
}

void Taper102AudioProcessor::inputGesture(bool starting)
{
    inputGestureOpen_.store(starting);
    if (starting) return;
    applyPendingLink();
    if (outputGestureOpen_)
    {
        outputGestureOpen_ = false;
        if (auto* output = parameters.getParameter("output_db")) output->endChangeGesture();
    }
}

void Taper102AudioProcessor::parameterChanged(const juce::String&, float newValue)
{
    const float previous = lastInput_.exchange(newValue);
    if (restoring_.load() || applyingLink_ || values_[kLink]->load(std::memory_order_relaxed) < 0.5f) return;
    const float delta = newValue - previous;
    if (delta == 0.0f) return;
    if (inputGestureOpen_.load())
    {   // a user edit in this plug-in's own UI: couple inside the edit
        applyLinkDelta(delta);
        return;
    }
    float old = pendingDelta_.load();
    while (!pendingDelta_.compare_exchange_weak(old, old + delta)) {}
    if (juce::Thread::getCurrentThreadId() != audioThread_.load(std::memory_order_relaxed))
        triggerAsyncUpdate(); // message loop, outside the delivering call; the 50 ms timer is the fallback for the audio thread
}
void Taper102AudioProcessor::applyPendingLink()
{
    const float delta = pendingDelta_.exchange(0.0f);
    if (delta != 0.0f && !restoring_.load()) applyLinkDelta(delta);
}
void Taper102AudioProcessor::applyLinkDelta(double deltaDb)
{
    auto* output = parameters.getParameter("output_db");
    if (output == nullptr) return;
    const auto& range = parameters.getParameterRange("output_db");
    const double current = static_cast<double>(output->convertFrom0to1(output->getValue()));
    const float target = static_cast<float>(juce::jlimit(static_cast<double>(range.start), static_cast<double>(range.end), current - deltaDb));
    const bool inGesture = inputGestureOpen_.load();
    applyingLink_ = true;
    if (inGesture) { if (!outputGestureOpen_) { output->beginChangeGesture(); outputGestureOpen_ = true; } }
    else output->beginChangeGesture();
    output->setValueNotifyingHost(output->convertTo0to1(target));
    if (!inGesture) output->endChangeGesture();
    applyingLink_ = false;
}

bool Taper102AudioProcessor::isBusesLayoutSupported(const BusesLayout& layout) const
{
    const auto input = layout.getMainInputChannelSet(), output = layout.getMainOutputChannelSet();
    return input == output && (input == juce::AudioChannelSet::mono() || input == juce::AudioChannelSet::stereo());
}

Taper102AudioProcessor::Snapshot Taper102AudioProcessor::readControls() const noexcept
{
    const auto get = [this](std::size_t i) { return static_cast<double>(values_[i]->load(std::memory_order_relaxed)); };
    // Parameters travel as normalised floats; snapping to each control's step in double gives the chain exact decimals.
    const auto snapped = [&](std::size_t i, double lo, double hi, double step)
    {
        return juce::jlimit(lo, hi, std::round(get(i) / step) * step);
    };
    Snapshot s;
    auto& c = s.chain;
    // Each setup fixes the speed, the EQ standard, the tape, the tape's aligned operating level and the aligned bias (overbias 0).
    const int setup = juce::jlimit(0, 3, juce::roundToInt(get(kSetup)));
    c.speed = (setup & 1) != 0 ? taper::atr::Speed::Ips30 : taper::atr::Speed::Ips15;
    c.eq = setup == 0 ? Eq::Nab15 : (setup == 2 ? Eq::Iec15 : Eq::Aes30);
    c.tape = setup >= 2 ? taper::atr::Tape::AtrMaster : taper::atr::Tape::Ampex456;
    c.level = setup >= 2 ? OperatingLevel::Nwb320 : OperatingLevel::Nwb370;
    c.overbiasDb = 0.0;
    c.inputDb = snapped(kInput, -24.0, 24.0, 0.01);
    c.outputDb = snapped(kOutput, -24.0, 24.0, 0.01);
    s.refDbfs = -18.0 + 3.0 * juce::jlimit(0, 3, juce::roundToInt(get(kRef)));
    s.bypass = get(kBypass) >= 0.5 || hostBypassed_.load(std::memory_order_relaxed);
    return s;
}

void Taper102AudioProcessor::pushControlsNow()
{
    const std::lock_guard<std::mutex> lock(pushMutex_);
    if (!prepared_) return;
    const auto s = readControls();
    if (havePushed_ && same(s, lastPushed_)) return;
    chain_.setControls(s.chain);
    refTarget_.store(refGainFor(s.refDbfs), std::memory_order_relaxed);
    lastPushed_ = s;
    havePushed_ = true;
}

void Taper102AudioProcessor::startPusher()
{
    pusher_ = std::make_unique<ControlPusher>(*this);
    pusher_->startThread();
}
void Taper102AudioProcessor::stopPusher()
{
    if (pusher_ != nullptr) { pusher_->stopThread(2000); pusher_.reset(); }
}

void Taper102AudioProcessor::prepareToPlay(double rate, int maxBlock)
{
    stopPusher();
    {
        const std::lock_guard<std::mutex> lock(pushMutex_);
        prepared_ = false;
        rate_ = std::isfinite(rate) && rate > 0 ? rate : 48000.0;
        maxBlock_ = std::max(1, maxBlock);
        const int channels = juce::jlimit(1, 2, getTotalNumInputChannels());
        chain_.prepare(rate_, maxBlock_, channels, 0);
        const auto s = readControls();
        chain_.setControls(s.chain);
        refTarget_.store(refGainFor(s.refDbfs), std::memory_order_relaxed);
        refGain_ = refTarget_.load(std::memory_order_relaxed);
        refCoef_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * rate_)));
        chain_.reset(); // jump smoothing and cross-fades to the current controls
        lastPushed_ = s;
        havePushed_ = true;
        delay_ = std::max(0, static_cast<int>(std::lround(chain_.latencySamples())));
        ringSize_ = delay_ + 1;
        ringPos_ = 0;
        for (int c = 0; c < 2; ++c)
        {
            work_[c].assign(static_cast<std::size_t>(maxBlock_), 0.0);
            ring_[c].assign(static_cast<std::size_t>(ringSize_), 0.0);
        }
        bypassMix_ = s.bypass ? 1.0f : 0.0f;
        bypassStep_ = static_cast<float>(1.0 / std::max(1.0, 0.01 * rate_)); // 10 ms
        prepared_ = true;
        preparedChannels_.store(channels, std::memory_order_relaxed);
        processingError_.store(false, std::memory_order_relaxed);
        setLatencySamples(delay_);
    }
    clearPeaks();
    startPusher();
}
void Taper102AudioProcessor::clearPeaks() noexcept
{
    for (auto& meter : inputPeaks_) meter.store(0, std::memory_order_relaxed);
    for (auto& meter : outputPeaks_) meter.store(0, std::memory_order_relaxed);
}
void Taper102AudioProcessor::releaseResources() { stopPusher(); }
void Taper102AudioProcessor::reset()
{
    const std::lock_guard<std::mutex> lock(pushMutex_);
    if (prepared_)
    {
        chain_.reset();
        for (auto& r : ring_) std::fill(r.begin(), r.end(), 0.0);
        ringPos_ = 0;
        bypassMix_ = lastPushed_.bypass ? 1.0f : 0.0f;
        refGain_ = refTarget_.load(std::memory_order_relaxed);
    }
    clearPeaks();
}

void Taper102AudioProcessor::process(juce::AudioBuffer<float>& buffer, bool bypassed) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    const int samples = buffer.getNumSamples();
    const int channels = buffer.getNumChannels();
    if (samples == 0) return;
    hostBypassed_.store(bypassed, std::memory_order_relaxed);
    const float decay = static_cast<float>(std::exp(-samples / (rate_ * .3)));
    const auto peakOf = [&](int channel)
    {
        const float* data = buffer.getReadPointer(channel);
        float peak = 0;
        for (int i = 0; i < samples; ++i) { const float a = std::abs(data[i]); if (std::isfinite(a) && a > peak) peak = a; }
        return peak;
    };
    std::array<float, 2> in {}, out {};
    for (int c = 0; c < std::min(channels, 2); ++c) in[static_cast<std::size_t>(c)] = peakOf(c);

    if (prepared_ && channels <= preparedChannels_.load(std::memory_order_relaxed))
    {
        // Offline bounces apply control changes synchronously (deterministic); realtime relies on the background pusher so the audio
        // thread never designs filters or waits on a lock.
        if (isNonRealtime()) pushControlsNow();
        const float target = (bypassed || values_[kBypass]->load(std::memory_order_relaxed) >= 0.5f) ? 1.0f : 0.0f;
        double* ptrs[2] { work_[0].data(), work_[1].data() };
        const int ringSize = ringSize_;
        for (int done = 0; done < samples;)
        {
            const int m = std::min(maxBlock_, samples - done);
            for (int c = 0; c < channels; ++c)
            {
                const float* src = buffer.getReadPointer(c) + done;
                double* w = work_[static_cast<std::size_t>(c)].data();
                for (int i = 0; i < m; ++i) w[i] = static_cast<double>(src[i]);
            }
            // dry path: the input delayed by the reported latency (sanitised), kept in the ring until the mix below
            std::vector<double>* ring = ring_;
            int pos = ringPos_;
            // wet = chain on a copy; dry = delayed input (the chain works in place on work_, so the dry samples are saved in the ring first)
            for (int c = 0; c < channels; ++c)
            {
                double* r = ring[c].data();
                const double* w = work_[static_cast<std::size_t>(c)].data();
                float* dst = buffer.getWritePointer(c) + done;
                int p = pos;
                for (int i = 0; i < m; ++i)
                {
                    const double x = std::isfinite(w[i]) ? w[i] : 0.0;
                    r[p] = x;
                    int q = p - delay_; if (q < 0) q += ringSize;
                    dst[i] = static_cast<float>(r[q]); // dry, delayed (temporarily stored in the output buffer)
                    if (++p == ringSize) p = 0;
                }
            }
            ringPos_ = (pos + m) % ringSize;
            // 0 VU reference: pre-gain before the chain, the inverse after it (one smoothed gain, per sample)
            const float refTarget = refTarget_.load(std::memory_order_relaxed);
            float ref = refGain_;
            for (int c = 0; c < channels; ++c)
            {
                double* w = work_[static_cast<std::size_t>(c)].data();
                float r = refGain_;
                for (int i = 0; i < m; ++i) { r += (refTarget - r) * refCoef_; w[i] *= static_cast<double>(r); }
                ref = r;
            }
            chain_.process(ptrs, channels, m);
            {
                float r = refGain_;
                for (int i = 0; i < m; ++i)
                {
                    r += (refTarget - r) * refCoef_;
                    const double inv = 1.0 / static_cast<double>(r);
                    for (int c = 0; c < channels; ++c) work_[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] *= inv;
                }
            }
            refGain_ = ref;
            float mix = bypassMix_;
            float* dsts[2] { buffer.getWritePointer(0) + done, channels > 1 ? buffer.getWritePointer(1) + done : nullptr };
            for (int i = 0; i < m; ++i)
            {
                if (mix < target) mix = std::min(target, mix + bypassStep_);
                else if (mix > target) mix = std::max(target, mix - bypassStep_);
                for (int c = 0; c < channels; ++c)
                {
                    const double wet = work_[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)];
                    const double y = wet + (static_cast<double>(dsts[c][i]) - wet) * static_cast<double>(mix);
                    dsts[c][i] = std::isfinite(y) ? static_cast<float>(y) : 0.0f;
                }
            }
            bypassMix_ = mix;
            done += m;
        }
        for (int c = 0; c < std::min(channels, 2); ++c) out[static_cast<std::size_t>(c)] = peakOf(c);
    }
    else
    {
        // Unprepared or unsupported: pass finite dry audio and report an error.
        processingError_.store(true, std::memory_order_relaxed);
        for (int c = 0; c < channels; ++c)
        {
            float* data = buffer.getWritePointer(c);
            for (int i = 0; i < samples; ++i) if (!std::isfinite(data[i])) data[i] = 0;
        }
        for (int c = 0; c < std::min(channels, 2); ++c) out[static_cast<std::size_t>(c)] = peakOf(c);
    }
    for (std::size_t c = 0; c < 2; ++c)
    {
        inputPeaks_[c].store(std::max(in[c], inputPeaks_[c].load(std::memory_order_relaxed) * decay), std::memory_order_relaxed);
        outputPeaks_[c].store(std::max(out[c], outputPeaks_[c].load(std::memory_order_relaxed) * decay), std::memory_order_relaxed);
    }
}
void Taper102AudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { audioThread_.store(juce::Thread::getCurrentThreadId(), std::memory_order_relaxed); process(buffer, false); }
void Taper102AudioProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { audioThread_.store(juce::Thread::getCurrentThreadId(), std::memory_order_relaxed); process(buffer, true); }

float Taper102AudioProcessor::getInputPeak(int channel) const noexcept
{
    return channel >= 0 && channel < 2 ? inputPeaks_[static_cast<std::size_t>(channel)].load(std::memory_order_relaxed) : 0;
}
float Taper102AudioProcessor::getOutputPeak(int channel) const noexcept
{
    return channel >= 0 && channel < 2 ? outputPeaks_[static_cast<std::size_t>(channel)].load(std::memory_order_relaxed) : 0;
}
double Taper102AudioProcessor::getZeroVuDbfs() const noexcept
{
    return -18.0 + 3.0 * juce::jlimit(0, 3, juce::roundToInt(values_[kRef]->load(std::memory_order_relaxed)));
}
juce::AudioProcessorParameter* Taper102AudioProcessor::getBypassParameter() const
{
    return parameters.getParameter("bypass");
}

void Taper102AudioProcessor::getStateInformation(juce::MemoryBlock& block)
{
    auto state = parameters.copyState();
    state.setProperty("schemaVersion", stateSchemaVersion, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, block);
}
// Setup from the old (tape, speed): 456/15 -> 456 15 NAB, 456/30 -> 456 30 AES, ATR/15 -> ATR 15 IEC, ATR/30 -> ATR 30 AES.
// (An old 456 / 15 ips / IEC state becomes 456 15 NAB.)
static int setupFromOld(float tape, float speed) noexcept
{
    const bool atr = juce::roundToInt(tape) >= 1, fast = juce::roundToInt(speed) >= 1;
    return (atr ? 2 : 0) + (fast ? 1 : 0);
}

// Other schemas and malformed states are ignored; defaults (or the current values) stay. Schema 1 is migrated.
void Taper102AudioProcessor::setStateInformation(const void* data, int bytes)
{
    const auto xml = getXmlFromBinary(data, bytes);
    if (!xml || !xml->hasTagName("Taper102State")) return;
    auto state = juce::ValueTree::fromXml(*xml);
    float version = 0;
    if (!nativeValue(state.getProperty("schemaVersion", 0), version)) return;
    const bool old = juce::exactlyEqual(version, 1.0f);
    if (!old && !juce::exactlyEqual(version, static_cast<float>(stateSchemaVersion))) return;
    const char* const* names = old ? oldIds : ids;
    const std::size_t count = old ? std::size(oldIds) : std::size(ids);
    std::array<float, 10> values {};
    std::array<bool, 10> seen {};
    for (const auto& child : state)
    {
        if (!child.hasType("PARAM")) continue;
        const auto id = child.getProperty("id").toString();
        std::size_t index = 0;
        while (index < count && id != names[index]) ++index;
        if (index == count) return;
        float value = 0;
        if (seen[index] || !nativeValue(child.getProperty("value"), value)) return;
        seen[index] = true;
        values[index] = value;
    }
    if (old)
    {   // old slots: 0 speed, 1 eq, 2 tape, 3 level, 4 bias, 5 input, 6 output, 7 bypass, 8 link, 9 ref (level and bias are dropped;
        // a state written before Link or the 0 VU setting existed keeps Link off and 0 VU = -18 dBFS)
        const float setup = static_cast<float>(setupFromOld(seen[2] ? values[2] : 0.0f, seen[0] ? values[0] : 0.0f));
        const std::array<float, 6> migrated { setup, values[5], values[6], values[7], values[8], values[9] };
        const std::array<bool, 6> have { true, seen[5], seen[6], seen[7], seen[8], seen[9] };
        const float defaults[6] { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        juce::ValueTree migratedState(state.getType());
        for (std::size_t i = 0; i < 6; ++i)
        {
            juce::ValueTree child("PARAM");
            child.setProperty("id", ids[i], nullptr);
            child.setProperty("value", have[i] ? migrated[i] : defaults[i], nullptr);
            migratedState.appendChild(child, nullptr);
        }
        state = migratedState;
    }
    restoring_.store(true);
    pendingDelta_.store(0.0f);
    parameters.replaceState(state);
    lastInput_.store(values_[kInput]->load());
    restoring_.store(false);
}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new Taper102AudioProcessor(); }

juce::AudioProcessorEditor* Taper102AudioProcessor::createEditor() { return new Taper102AudioProcessorEditor(*this); }
