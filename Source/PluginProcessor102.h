#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Taper102Chain.h"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

// Taper 102: the Ampex ATR-102-derived record -> tape -> reproduce chain (Taper102Chain.h) as an AU / VST3 effect.
//
// Control path. Taper102Chain::setControls designs the electronics (filter cascades) and writes into a lock-free mailbox, so it runs on a
// small background thread (ControlPusher, ~100 Hz) that pushes a snapshot whenever a parameter changed; the chain picks it up at the next
// block start with its own cross-fades and smoothing. In non-realtime rendering (isNonRealtime()) processBlock pushes the snapshot
// synchronously first, so offline bounces apply every automation change at the block that carries it. Bypass (parameter or host) is not part
// of the chain: it is a latency-aligned dry crossfade in this class.
class Taper102AudioProcessor final : public juce::AudioProcessor,
                                      private juce::AudioProcessorValueTreeState::Listener,
                                      private juce::Timer
{
public:
    static constexpr int stateSchemaVersion = 2;
    static constexpr int numParameters = 6;

    struct Snapshot
    {
        taper::t102::Controls chain;   // eq is the effective one (AES at 30 ips)
        bool bypass = false;
        double refDbfs = -12.0;      // digital level that counts as 0 VU (the chain itself is calibrated to -18)
    };

    Taper102AudioProcessor();
    ~Taper102AudioProcessor() override;
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Taper 102"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
    juce::AudioProcessorParameter* getBypassParameter() const override;

    float getInputPeak(int channel) const noexcept;
    // The digital level (dBFS) the "0 VU" parameter currently selects: -18, -15, -12 or -9.
    double getZeroVuDbfs() const noexcept;
    float getOutputPeak(int channel) const noexcept;
    int getPreparedChannelCount() const noexcept { return preparedChannels_.load(std::memory_order_relaxed); }
    bool hasProcessingError() const noexcept { return processingError_.load(std::memory_order_relaxed); }
    // Control values as the chain receives them (snapped to each control's step, effective EQ).
    Snapshot currentControls() const noexcept { return readControls(); }
    // Reads the parameters now and pushes them to the chain (non-realtime callers and tests).
    void pushControlsNow();
    juce::AudioProcessorValueTreeState parameters;

    // Link: while the "link" parameter is on, a change of Input by d dB moves the Output parameter by -d dB (clamped to its range). The
    // Output parameter itself is a normal gain; there is no hidden compensation in the DSP. User edits arrive on the message thread and are
    // applied at once; a change delivered on another thread (host automation on the audio thread) is accumulated and applied by a timer.
    // Restoring a state never couples. Automating Input with Link on therefore also moves Output.
    void applyPendingLink();

private:
    void parameterChanged(const juce::String& id, float newValue) override;
    void timerCallback() override { applyPendingLink(); }
    void applyLinkDelta(double deltaDb);
    std::atomic<float> lastInput_ { 0.0f }, pendingDelta_ { 0.0f };
    std::atomic<bool> restoring_ { false };
    bool applyingLink_ = false;

    struct ControlPusher;
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void process(juce::AudioBuffer<float>&, bool hostBypassed) noexcept;
    Snapshot readControls() const noexcept;
    void clearPeaks() noexcept;
    void startPusher();
    void stopPusher();

    taper::t102::Taper102Chain chain_;
    std::array<std::atomic<float>*, numParameters> values_ {};
    std::array<std::atomic<float>, 2> inputPeaks_ {}, outputPeaks_ {};
    std::atomic<int> preparedChannels_ { 0 };
    std::atomic<bool> processingError_ { false }, hostBypassed_ { false };
    std::mutex pushMutex_; // serialises pushes (background thread vs non-realtime audio thread); never taken in realtime processing
    std::unique_ptr<ControlPusher> pusher_;
    Snapshot lastPushed_;
    bool havePushed_ = false;
    double rate_ = 48000;
    bool prepared_ = false;

    // audio-thread state (allocated in prepareToPlay)
    int maxBlock_ = 0, delay_ = 0, ringSize_ = 0, ringPos_ = 0;
    float bypassMix_ = 0.0f, bypassStep_ = 0.01f, refGain_ = 1.0f, refCoef_ = 0.01f;
    std::atomic<float> refTarget_ { 1.0f };
    std::vector<double> work_[2], ring_[2];
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Taper102AudioProcessor)
};
