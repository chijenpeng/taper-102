#include "PluginEditor102.h"
#include "PluginProcessor102.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
bool number(const char* text, double& value)
{
    char* end = nullptr;
    value = std::strtod(text, &end);
    return end != text && *end == '\0' && std::isfinite(value);
}
bool set(juce::AudioProcessorValueTreeState& state, const char* id, float native)
{
    auto* parameter = state.getParameter(id);
    if (parameter == nullptr) return false;
    parameter->setValueNotifyingHost(parameter->convertTo0to1(native));
    return true;
}
juce::Component* find(juce::Component& root, const juce::String& id)
{
    if (root.getComponentID() == id) return &root;
    for (auto* child : root.getChildren()) if (auto* found = find(*child, id)) return found;
    return nullptr;
}
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
juce::MouseEvent event(juce::Component& component, juce::Point<float> position, int clicks = 2)
{
    return { juce::Desktop::getInstance().getMainMouseSource(), position, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 0, 0, 0, 0, 0,
        &component, &component, juce::Time::getCurrentTime(), position, juce::Time::getCurrentTime(), clicks, false };
}
void editorTests(Taper102AudioProcessor& processor, Taper102AudioProcessorEditor& editor)
{
    const auto native = [&](const char* id) { return processor.parameters.getRawParameterValue(id)->load(); };
    struct Parts { juce::Component* knob; juce::Slider* slider; juce::TextEditor* text; };
    const auto parts = [&](const char* id)
    {
        Parts result { find(editor, id), nullptr, nullptr };
        require(result.knob != nullptr, "real control exists");
        for (auto* child : result.knob->getChildren())
        {
            if (auto* slider = dynamic_cast<juce::Slider*>(child)) result.slider = slider;
            if (auto* text = dynamic_cast<juce::TextEditor*>(child)) result.text = text;
        }
        require(result.slider && result.text, "real rotary and native inline editor");
        return result;
    };
    const auto type = [&](Parts& p, const char* text)
    {
        p.knob->mouseDoubleClick(event(*p.knob, { 20.f, static_cast<float>(p.knob->getHeight() - 2) }));
        require(p.text->isVisible(), "value doubleclick opens inline editor");
        p.text->setText(juce::String::fromUTF8(text), false);
        p.text->onReturnKey();
    };
    struct Case { const char* id; const char* typed; float expect; };
    for (const Case& c : { Case { "input_db", "-3.5 dB", -3.5f }, Case { "output_db", "+2", 2.0f } })
    {
        auto p = parts(c.id);
        type(p, c.typed);
        require(std::abs(native(c.id) - c.expect) < 1e-4f, "typed value applies in native units");
        const float kept = native(c.id);
        for (const char* bad : { "NaN", "2 garbage", "1e999" }) { type(p, bad); require(native(c.id) == kept, "malformed text ignored"); }
        p.knob->mouseDoubleClick(event(*p.knob, { 20.f, static_cast<float>(p.knob->getHeight() - 2) }));
        p.text->setText("0.9999", false);
        p.text->onEscapeKey();
        require(native(c.id) == kept, "Escape cancels");
        p.slider->mouseDoubleClick(event(*p.slider, { 32, 32 }));
        auto* parameter = processor.parameters.getParameter(c.id);
        require(parameter->getValue() == parameter->getDefaultValue(), "disc double-click returns default");
    }
    {
        auto p = parts("input_db");
        type(p, "99"); require(std::abs(native("input_db") - 24) < 1e-3f, "typed value clamps to range");
        type(p, "-99"); require(std::abs(native("input_db") + 24) < 1e-3f, "typed value clamps low");
        set(processor.parameters, "input_db", 0);
    }
    for (const char* id : { "setup", "link", "ref", "bypass", "input_db", "output_db", "meters" })
    {
        auto* component = find(editor, id);
        require(component && component->getProperties().contains("help") && component->getProperties().contains("helpTitle"),
                "every real control/meter has persistent hover metadata");
        editor.showHelpForParameter(id);
    }
    for (const char* removed : { "speed", "eq", "tape", "level", "bias" }) require(find(editor, removed) == nullptr, "removed controls absent");
    auto* setup = find(editor, "setup");
    require(setup->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)) && native("setup") == 1, "Setup option selects 456 30 AES");
    require(setup->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)) && native("setup") == 2, "Setup option selects ATR 15 IEC");
    require(setup->keyPressed(juce::KeyPress(juce::KeyPress::leftKey)) && native("setup") == 1, "Setup option steps back");
    set(processor.parameters, "setup", 0);
    require(find(editor, "link")->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)) && native("link") == 1, "Link option on");
    set(processor.parameters, "link", 0);
    auto* bypass = find(editor, "bypass");
    require(bypass->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)) && native("bypass") == 1, "Bypass option on");
    set(processor.parameters, "bypass", 0);
    editor.showHelpForParameter({});
    std::cout << "Typed values, ranges, escape, defaults, option keys and help-metadata checks passed\n";
}
}

int main(int argc, char** argv)
{
    std::vector<const char*> args;
    bool speed30 = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--setup2") == 0) speed30 = true;
        else args.push_back(argv[i]);
    }
    if (args.empty() || args.size() > 4)
    {
        std::cerr << "Usage: taper102_snapshot output.png [editorScale pixelScale helpId] [--setup2]\n";
        return 2;
    }
    double editorScale = 1, pixelScale = 1;
    if ((args.size() > 1 && !number(args[1], editorScale)) || (args.size() > 2 && !number(args[2], pixelScale))
        || editorScale < .75 || editorScale > 2 || pixelScale <= 0 || pixelScale > 4)
    {
        std::cerr << "Invalid snapshot scale\n";
        return 2;
    }
    const juce::ScopedJuceInitialiser_GUI gui;
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(args[0]));
    if (!file.hasFileExtension("png")) { std::cerr << "Output must be a PNG\n"; return 2; }

    Taper102AudioProcessor processor;
    { Taper102AudioProcessorEditor checked(processor); editorTests(processor, checked); }
    if (speed30) set(processor.parameters, "setup", 1);
    processor.setPlayConfigDetails(2, 2, 48000.0, 512);
    processor.prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer midi;
    constexpr int frames = 24000;
    constexpr double frequency = 1000.0;
    const float amplitude = juce::Decibels::decibelsToGain(-18.0f);
    for (int offset = 0; offset < frames; offset += 512)
    {
        const int count = std::min(512, frames - offset);
        audio.setSize(2, count, false, false, true);
        for (int sample = 0; sample < count; ++sample)
        {
            const float value = amplitude * static_cast<float>(std::sin(
                juce::MathConstants<double>::twoPi * frequency * (offset + sample) / 48000.0));
            audio.setSample(0, sample, value);
            audio.setSample(1, sample, .7f * value);
        }
        processor.processBlock(audio, midi);
    }
    if (processor.hasProcessingError()) { std::cerr << "Processor reports an error\n"; return 1; }

    Taper102AudioProcessorEditor editor(processor);
    editor.setSize(juce::roundToInt(Taper102AudioProcessorEditor::designWidth * editorScale),
                   juce::roundToInt(Taper102AudioProcessorEditor::designHeight * editorScale));
    editor.updateMeters();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    if (args.size() > 3) editor.showHelpForParameter(juce::String::fromUTF8(args[3]));
    const auto image = editor.createComponentSnapshot(editor.getLocalBounds(), true, static_cast<float>(pixelScale));
    if (!file.getParentDirectory().createDirectory()) { std::cerr << "Cannot create output directory\n"; return 1; }
    juce::FileOutputStream stream(file);
    if (!stream.openedOk() || !stream.setPosition(0) || !stream.truncate())
    {
        std::cerr << "Cannot open snapshot output\n";
        return 1;
    }
    if (!juce::PNGImageFormat().writeImageToStream(image, stream)) return 1;
    std::cout << "Wrote " << file.getFullPathName() << " " << image.getWidth() << 'x' << image.getHeight()
              << "; meters from actual processed -18dBFS 1kHz tone, PDC " << processor.getLatencySamples() << '\n';
    processor.releaseResources();
}
