#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <memory>

class Taper102AudioProcessor;

class Taper102AudioProcessorEditor final : public juce::AudioProcessorEditor,
                                           private juce::Timer
{
public:
    static constexpr int designWidth = 560, designHeight = 288;
    explicit Taper102AudioProcessorEditor(Taper102AudioProcessor&);
    ~Taper102AudioProcessorEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

    // Offline snapshot helpers: read actual processor measurements, never seed them.
    void updateMeters();
    void showHelpForParameter(const juce::String& id);

private:
    void timerCallback() override;
    struct Impl;
    Taper102AudioProcessor& processor_;
    std::unique_ptr<Impl> impl_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Taper102AudioProcessorEditor)
};
