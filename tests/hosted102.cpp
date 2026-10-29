// Taper 102 hosted wrapper test: loads the BUILT plug-in through JUCE's plug-in hosting (VST3, and AudioUnit when the component is
// registered) and checks Link through the real wrappers, the way a host sees it.
//
//   taper102_hosted_test <path/to/Taper 102.vst3> [<path/to/Taper 102.component>]
//
// The AU path needs the component installed in ~/Library/Audio/Plug-Ins/Components (ditto the fresh build there first); it is skipped
// when no second argument is given. Host-visible values are read from the hosted parameters, which the wrapper updates only for edits it
// reports to the host (performEdit / AUEvent): exactly what a DAW receives.
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <iostream>
#include <thread>

namespace
{
int failures = 0;
void require(bool ok, const juce::String& what)
{
    std::cout << (ok ? "PASS " : "FAIL ") << what << "\n";
    if (!ok) ++failures;
}

juce::AudioProcessorParameter* find(juce::AudioProcessor& p, const juce::String& name)
{
    for (auto* parameter : p.getParameters())
        if (parameter->getName(64) == name) return parameter;
    return nullptr;
}
float dbOf(juce::AudioProcessorParameter* p) { return p->getValue() * 48.0f - 24.0f; }
void setDb(juce::AudioProcessorParameter* p, float db) { p->setValueNotifyingHost((db + 24.0f) / 48.0f); }
void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

// A thread that keeps calling processBlock, as a host's audio thread does; optional job runs on that thread first.
struct AudioThread final : juce::Thread
{
    explicit AudioThread(juce::AudioPluginInstance& p) : juce::Thread("audio"), plugin(p) {}
    void run() override
    {
        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer midi;
        while (!threadShouldExit())
        {
            if (auto job = pending.exchange(nullptr)) { (*job)(); delete job; }
            buffer.clear();
            plugin.processBlock(buffer, midi);
            juce::Thread::sleep(5);
        }
    }
    void post(std::function<void()> f) { pending = new std::function<void()>(std::move(f)); }
    juce::AudioPluginInstance& plugin;
    std::atomic<std::function<void()>*> pending { nullptr };
};

[[maybe_unused]] juce::Slider* nthSlider(juce::Component& c, int& index)
{
    for (auto* child : c.getChildren())
    {
        if (auto* s = dynamic_cast<juce::Slider*>(child)) { if (index-- == 0) return s; }
        if (auto* s = nthSlider(*child, index)) return s;
    }
    return nullptr;
}

void run(juce::AudioPluginFormatManager& formats, const juce::String& path, const juce::String& label)
{
    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* format : formats.getFormats()) format->findAllTypesForFile(found, path);
    if (found.isEmpty()) { require(false, label + ": plug-in found"); return; }
    juce::String error;
    std::unique_ptr<juce::AudioPluginInstance> plugin(formats.createPluginInstance(*found[0], 48000.0, 512, error));
    if (plugin == nullptr) { require(false, label + ": instantiate " + error); return; }
    plugin->setPlayConfigDetails(2, 2, 48000.0, 512);
    plugin->prepareToPlay(48000.0, 512);
    auto *input = find(*plugin, "Input"), *output = find(*plugin, "Output"), *link = find(*plugin, "Link");
    require(input != nullptr && output != nullptr && link != nullptr, label + ": parameters found");
    if (!input || !output || !link) return;

    AudioThread audio(*plugin);
    audio.startThread();
    pump(100);
    const auto reset = [&](bool linkOn)
    {
        link->setValueNotifyingHost(0.0f); pump(150);
        setDb(input, 0.0f); setDb(output, 0.0f); pump(150);
        if (linkOn) { link->setValueNotifyingHost(1.0f); pump(150); }
    };

    reset(true);
    require(std::abs(dbOf(output)) < 0.05f, label + ": toggling Link moves nothing");

    // (a1) host sets Input on the message thread (automation lane edit, generic UI, remote control).
    setDb(input, 12.0f); pump(300);
    require(std::abs(dbOf(output) + 12.0f) < 0.05f, label + ": host message-thread Input +12 -> host sees Output -12 (got " + juce::String(dbOf(output), 2) + ")");

    // (a2) host sets Input from its audio thread (sample-accurate automation playback).
    reset(true);
    audio.post([&] { setDb(input, 6.0f); });
    pump(400);
    require(std::abs(dbOf(output) + 6.0f) < 0.05f, label + ": host audio-thread Input +6 -> host sees Output -6 (got " + juce::String(dbOf(output), 2) + ")");

    // second move: delta is relative to the last applied value, not to a stale one
    setDb(input, 2.0f); pump(300);
    require(std::abs(dbOf(output) + 2.0f) < 0.05f, label + ": second host move +6 -> +2 gives Output -2 (got " + juce::String(dbOf(output), 2) + ")");

    // The plug-in's own editor is a native view inside a hosted instance and cannot be driven from here; its drags are covered in-process
    // by taper102_processor_tests (gesture-open Input edits couple synchronously, Output keeps one gesture).

    audio.stopThread(2000);
    plugin->releaseResources();
}
} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc < 2) { std::cerr << "usage: taper102_hosted_test <Taper 102.vst3> [<Taper 102.component>]\n"; return 2; }
    juce::AudioPluginFormatManager formats;
    formats.addFormat(new juce::VST3PluginFormat());
   #if JUCE_MAC
    formats.addFormat(new juce::AudioUnitPluginFormat());
   #endif
    run(formats, argv[1], "VST3");
    if (argc > 2) run(formats, argv[2], "AU");
    std::cout << (failures == 0 ? "ALL PASS\n" : "FAILURES\n");
    return failures == 0 ? 0 : 1;
}
