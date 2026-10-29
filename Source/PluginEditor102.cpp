#include "PluginEditor102.h"
#include "PluginProcessor102.h"
#include "ValueText.h"
#include <BinaryData.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <vector>

namespace
{
struct EditorFonts
{
    EditorFonts()
        : inter(juce::Typeface::createSystemTypefaceFor(BinaryData::InterSemiBold_ttf,
                                                       BinaryData::InterSemiBold_ttfSize)),
          mono(juce::Typeface::createSystemTypefaceFor(BinaryData::JetBrainsMonoMedium_ttf,
                                                      BinaryData::JetBrainsMonoMedium_ttfSize)) {}
    juce::Typeface::Ptr inter, mono;
};

// Taper 102 palette: "meter-panel black" (chosen for the plug-in, colour language of the ATR-102 panel: near-black panel, warm pale lettering,
// amber meter fill); values chosen for the plug-in, not measured, no calibrated-meter claim.
namespace Theme
{
juce::Colour background() { return juce::Colour(0xff161715); }
juce::Colour field()      { return juce::Colour(0xff3b3d39); }
juce::Colour label()      { return juce::Colour(0xffb0b2ab); }
juce::Colour pale()       { return juce::Colour(0xffece6d2); }
juce::Colour control()    { return juce::Colour(0xffd9dbd5); }  // knob discs
juce::Colour accent()     { return juce::Colour(0xffe0b66d); }
juce::Colour clipping()   { return juce::Colour(0xffff6a4d); }
juce::Colour text()       { return juce::Colour(0xffffffff); }

juce::Font font(const juce::Typeface::Ptr& face, float size, float tracking)
{
    return juce::Font(juce::FontOptions(face).withHeight(size)).withExtraKerningFactor(tracking);
}
juce::Font labelFont() { return font(juce::SharedResourcePointer<EditorFonts>()->inter, 11.0f, .09f); }
juce::Font bodyFont() { return font(juce::SharedResourcePointer<EditorFonts>()->inter, 12.0f, .01f); }
juce::Font titleFont() { return font(juce::SharedResourcePointer<EditorFonts>()->inter, 17.0f, .18f); }
juce::Font valueFont(float size = 12.0f) { return font(juce::SharedResourcePointer<EditorFonts>()->mono, size, 0.0f); }
int textWidth(const juce::Font& f, const juce::String& text)
{
    return static_cast<int>(std::ceil(juce::GlyphArrangement::getStringWidth(f, text)));
}
}

constexpr int labelHeight = 16, gap = 8, diameter = 64;
constexpr int labelGap = 18, optionGap = 10, rowHeight = 18;
constexpr const char* defaultHelpTitle = "TAPER 102";
constexpr const char* defaultHelp =
    "Derived from the Ampex ATR-100 series manual's circuits; tape behaviour calibrated to datasheets and published measurements, "
    "not to a measured machine. Hover over a control to see what it does; double-click a knob for its default, or its value to type one.";

void helpFor(juce::Component& component, const juce::String& title, const juce::String& text)
{
    component.getProperties().set("helpTitle", title.toUpperCase());
    component.getProperties().set("help", text);
}
// Help copy is UTF-8 (the micro sign), so literals are decoded as UTF-8, not Latin-1.
void helpFor(juce::Component& component, const juce::String& title, const char* text)
{
    helpFor(component, title, juce::String::fromUTF8(text));
}
using Format = std::function<juce::String(double)>;
juce::String levelText(double value)
{
    const auto clean = std::abs(value) < .00005 ? 0.0 : value;
    return (clean > 0.0 ? "+" : "") + juce::String(clean, 1) + " dB";
}
// Typed values may carry the unit: "-3 dB", "+2".
bool parseTyped(const juce::String& text, double& value) { return taper::ui::parseDb(text.trim().toStdString(), value); }

class FlatLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider(juce::Graphics& graphics, int x, int y, int width, int height,
                          float position, float start, float end, juce::Slider& slider) override
    {
        const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();
        const auto centre = bounds.getCentre();
        const float radius = std::min(bounds.getWidth(), bounds.getHeight()) * .5f - 1.0f;
        const float angle = start + position * (end - start);
        graphics.setColour(slider.isMouseOverOrDragging() ? Theme::control().brighter(.1f) : Theme::control());
        graphics.fillEllipse(juce::Rectangle<float>(2 * radius, 2 * radius).withCentre(centre));
        const float dotRadius = std::max(2.5f, .11f * radius);
        graphics.setColour(Theme::background());
        graphics.fillEllipse(juce::Rectangle<float>(2 * dotRadius, 2 * dotRadius).withCentre(
            centre.getPointOnCircumference(.72f * radius, angle)));
    }
    void drawCornerResizer(juce::Graphics& graphics, int width, int height,
                           bool hovered, bool dragging) override
    {
        graphics.setColour(Theme::label().withAlpha(hovered || dragging ? .8f : .3f));
        const float size = static_cast<float>(std::min(width, height));
        for (float part : { .3f, .6f, .9f })
            graphics.drawLine(width - size * part, height - 2.0f,
                              width - 2.0f, height - size * part, 1.0f);
    }
};

class Knob final : public juce::Component
{
public:
    Knob(juce::AudioProcessorValueTreeState& state, const char* id, const char* title,
         const char* help, Format format, int discDiameter = diameter)
        : discDiameter_(discDiameter), parameter_(*state.getParameter(id)), undoManager_(state.undoManager), title_(title), format_(std::move(format))
    {
        setComponentID(id);
        setName(title);
        helpFor(*this, title, help);
        const float half = juce::degreesToRadians(135.0f);
        const float full = juce::MathConstants<float>::twoPi;
        slider_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider_.setRotaryParameters(full - half, full + half, true);
        slider_.setVelocityModeParameters(.15, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
        slider_.setDoubleClickReturnValue(true, parameter_.convertFrom0to1(parameter_.getDefaultValue()));
        slider_.textFromValueFunction = format_;
        slider_.valueFromTextFunction = [this](const juce::String& text)
        {
            double value = slider_.getValue();
            parseTyped(text, value);
            return value;
        };
        attachment_ = std::make_unique<juce::SliderParameterAttachment>(parameter_, slider_, state.undoManager);
        slider_.onValueChange = [this] { repaint(); };
        addAndMakeVisible(slider_);

        edit_.setFont(Theme::valueFont());
        edit_.setJustification(juce::Justification::centred);
        edit_.setIndents(0, 0);
        edit_.setColour(juce::TextEditor::backgroundColourId, Theme::field());
        edit_.setColour(juce::TextEditor::textColourId, Theme::pale());
        edit_.setColour(juce::TextEditor::highlightColourId, Theme::accent().withAlpha(.5f));
        edit_.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        edit_.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        edit_.onReturnKey = [this] { finishEditing(true); };
        edit_.onEscapeKey = [this] { finishEditing(false); };
        edit_.onFocusLost = [this] { finishEditing(true); };
        addChildComponent(edit_);
    }
    void paint(juce::Graphics& graphics) override
    {
        auto area = getLocalBounds();
        graphics.setColour(Theme::label());
        graphics.setFont(Theme::labelFont());
        graphics.drawText(title_, area.removeFromTop(labelHeight), juce::Justification::centred, false);
        graphics.setFont(Theme::valueFont());
        graphics.drawText(format_(slider_.getValue()), area.removeFromBottom(labelHeight),
                          juce::Justification::centred, false);
    }
    void resized() override
    {
        auto area = getLocalBounds();
        area.removeFromTop(labelHeight + gap);
        slider_.setBounds(area.removeFromTop(discDiameter_ + 2).withSizeKeepingCentre(discDiameter_ + 2, discDiameter_ + 2));
        edit_.setBounds(getLocalBounds().removeFromBottom(labelHeight));
    }
    int preferredHeight() const noexcept { return 2 * (labelHeight + gap) + discDiameter_; }
    void mouseDoubleClick(const juce::MouseEvent& event) override
    {
        if (event.y < getHeight() - labelHeight - 2) return;
        edit_.setText(format_(slider_.getValue()), false);
        edit_.setVisible(true);
        edit_.selectAll();
        edit_.grabKeyboardFocus();
    }
private:
    void finishEditing(bool apply)
    {
        if (!edit_.isVisible()) return;
        const auto text = edit_.getText();
        edit_.setVisible(false);
        double value = slider_.getValue();
        if (apply && parseTyped(text, value))
        {
            const auto& range = parameter_.getNormalisableRange();
            const double limited = juce::jlimit(static_cast<double>(range.start),
                                                static_cast<double>(range.end), value);
            const float native = range.snapToLegalValue(static_cast<float>(limited));
            const float normalised = parameter_.convertTo0to1(native);
            if (!juce::exactlyEqual(parameter_.getValue(), normalised))
            {
                if (undoManager_ != nullptr) undoManager_->beginNewTransaction();
                parameter_.beginChangeGesture();
                parameter_.setValueNotifyingHost(normalised);
                parameter_.endChangeGesture();
            }
        }
        repaint();
    }
    int discDiameter_;
    juce::RangedAudioParameter& parameter_;
    juce::UndoManager* undoManager_;
    juce::String title_;
    Format format_;
    juce::Slider slider_;
    juce::TextEditor edit_;
    std::unique_ptr<juce::SliderParameterAttachment> attachment_;
};

// Plain-text option row. A row may be governed by another control (the EQ row by Speed): `shown` maps the parameter's index to the option
// drawn as selected and `enabled` says which options can be chosen. Unavailable options stay visible, desaturated (ui-design 6): the
// forced/selected one at 60 %, the others at 30 %.
class OptionRow final : public juce::Component
{
public:
    struct Policy
    {
        std::function<bool()> forced;                // true while the row is governed
        std::function<int()> forcedIndex;            // option shown as selected while forced
        std::function<bool(int, bool)> available;    // (index, forced) -> can be chosen
    };
    OptionRow(juce::AudioProcessorValueTreeState& state, const char* id, const char* title,
              juce::StringArray choices, const char* help, Policy policy = {})
        : title_(title), choices_(std::move(choices)), policy_(std::move(policy)),
          attachment_(*state.getParameter(id), [this](float value)
          {
              selected_ = juce::jlimit(0, choices_.size() - 1, juce::roundToInt(value));
              repaint();
          }, state.undoManager)
    {
        setComponentID(id);
        setName(title);
        setWantsKeyboardFocus(true);
        helpFor(*this, title, help);
        rowHelp_ = juce::String::fromUTF8(help);
        attachment_.sendInitialUpdate();
    }
    // Per-option hover help: hovering option i shows "<ROW> · <option>" with texts[i]; the row label and gaps show the row's own help.
    void setOptionHelp(std::vector<juce::String> texts) { optionHelp_ = std::move(texts); }
    void showOptionHelp(int index)
    {
        const bool own = index >= 0 && static_cast<std::size_t>(index) < optionHelp_.size();
        getProperties().set("helpTitle", own ? (title_ + juce::String::fromUTF8(" \xC2\xB7 ") + choices_[index]).toUpperCase() : title_.toUpperCase());
        getProperties().set("help", own ? optionHelp_[static_cast<std::size_t>(index)] : rowHelp_);
    }
    void setLabelColumn(int width) { labelColumn_ = width; }
    int titleWidth() const { return std::max(labelColumn_, Theme::textWidth(Theme::labelFont(), title_) + labelGap); }
    int ownTitleWidth() const { return Theme::textWidth(Theme::labelFont(), title_) + labelGap; }
    int preferredWidth() const
    {
        const auto ranges = optionRanges();
        return ranges.empty() ? titleWidth() : ranges.back().getEnd();
    }
    void refresh() { const bool now = isForced(); if (now != lastForced_) { lastForced_ = now; hovered_ = -1; repaint(); } }
    void paint(juce::Graphics& graphics) override
    {
        graphics.setFont(Theme::labelFont());
        graphics.setColour(Theme::label());
        graphics.drawText(title_, 0, 0, titleWidth(), getHeight(), juce::Justification::centredLeft, false);
        const auto ranges = optionRanges();
        const bool forced = isForced();
        const int shown = forced ? policy_.forcedIndex() : selected_;
        for (int index = 0; index < choices_.size(); ++index)
        {
            const auto range = ranges[static_cast<std::size_t>(index)];
            const bool usable = isAvailable(index, forced);
            juce::Colour colour = index == hovered_ && usable ? Theme::text()
                : (index == shown ? Theme::pale() : Theme::label().withAlpha(.7f));
            if (!usable) colour = index == shown ? Theme::pale().withAlpha(.6f) : Theme::label().withAlpha(.3f);
            else if (forced && index == shown) colour = Theme::pale().withAlpha(.6f);
            graphics.setColour(colour);
            graphics.drawText(choices_[index], range.getStart(), 0, range.getLength() + 2, getHeight(),
                              juce::Justification::centredLeft, false);
        }
    }
    void mouseDown(const juce::MouseEvent& event) override
    {
        const int index = optionAt(event.x);
        if (index >= 0 && isAvailable(index, isForced())) attachment_.setValueAsCompleteGesture(static_cast<float>(index));
    }
    void mouseMove(const juce::MouseEvent& event) override { hovered_ = optionAt(event.x); showOptionHelp(hovered_); repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hovered_ = -1; showOptionHelp(-1); repaint(); }
    bool keyPressed(const juce::KeyPress& key) override
    {
        const int direction = key == juce::KeyPress::leftKey ? -1 : (key == juce::KeyPress::rightKey ? 1 : 0);
        if (direction == 0) return false;
        const bool forced = isForced();
        if (forced) return true; // governed: nothing to choose, key handled
        int next = selected_ + direction;
        while (next >= 0 && next < choices_.size() && !isAvailable(next, forced)) next += direction;
        if (next >= 0 && next < choices_.size()) attachment_.setValueAsCompleteGesture(static_cast<float>(next));
        return true;
    }
private:
    bool isForced() const { return policy_.forced && policy_.forced(); }
    bool isAvailable(int index, bool forced) const { return !policy_.available || policy_.available(index, forced); }
    std::vector<juce::Range<int>> optionRanges() const
    {
        std::vector<juce::Range<int>> ranges;
        int position = titleWidth();
        for (const auto& choice : choices_)
        {
            const int width = Theme::textWidth(Theme::labelFont(), choice);
            ranges.emplace_back(position, position + width);
            position += width + optionGap;
        }
        return ranges;
    }
    int optionAt(int position) const
    {
        const auto ranges = optionRanges();
        for (std::size_t index = 0; index < ranges.size(); ++index)
            if (ranges[index].expanded(optionGap / 2).contains(position)) return static_cast<int>(index);
        return -1;
    }
    juce::String title_;
    int labelColumn_ = 0;
    juce::StringArray choices_;
    Policy policy_;
    int selected_ = 0, hovered_ = -1;
    juce::String rowHelp_;
    std::vector<juce::String> optionHelp_;
    bool lastForced_ = false;
    juce::ParameterAttachment attachment_;
};

class PeakMeters final : public juce::Component
{
public:
    explicit PeakMeters(Taper102AudioProcessor& processor) : processor_(processor)
    {
        setComponentID("meters");
        helpFor(*this, "Input / output peaks",
                "Sample peak per channel in dBFS: IN before Input, OUT after Output. The tick marks the 0 VU setting. The number is the louder channel, red above 0 dBFS. Range -60..+6 dBFS chosen for Taper 102.");
    }
    void update()
    {
        channels_ = juce::jlimit(1, 2, processor_.getPreparedChannelCount());
        for (int side = 0; side < 2; ++side)
            for (int channel = 0; channel < 2; ++channel)
            {
                const float amplitude = side == 0 ? processor_.getInputPeak(channel) : processor_.getOutputPeak(channel);
                const float measured = std::isfinite(amplitude)
                    ? juce::Decibels::gainToDecibels(std::max(0.0f, amplitude), -100.0f) : -100.0f;
                levels_[side][channel] = std::max(measured, levels_[side][channel] - .8f);
            }
        repaint();
    }
    void paint(juce::Graphics& graphics) override
    {
        const int columnWidth = getWidth() / 2;
        for (int side = 0; side < 2; ++side)
        {
            auto column = juce::Rectangle<int>(side * columnWidth, 0, columnWidth, getHeight());
            graphics.setFont(Theme::labelFont());
            graphics.setColour(Theme::label());
            graphics.drawText(side == 0 ? "IN" : "OUT", column.withHeight(16), juce::Justification::centred, false);
            const int firstX = column.getCentreX() - (channels_ == 2 ? 13 : 4);
            for (int channel = 0; channel < channels_; ++channel)
            {
                const auto track = juce::Rectangle<float>(static_cast<float>(firstX + channel * 18), 21.0f, 8.0f, 46.0f);
                graphics.setColour(Theme::label().withAlpha(.25f));
                graphics.fillRect(track);
                const float proportion = juce::jlimit(0.0f, 1.0f, (levels_[side][channel] + 60.0f) / 66.0f);
                graphics.setColour(levels_[side][channel] > 0.0f ? Theme::clipping() : Theme::accent());
                graphics.fillRect(track.withTop(track.getBottom() - track.getHeight() * proportion));
                // 0 VU = -18 dBFS
                const float vu = track.getBottom() - track.getHeight() * (static_cast<float>(processor_.getZeroVuDbfs()) + 60.0f) / 66.0f;
                graphics.setColour(Theme::text().withAlpha(.8f));
                graphics.fillRect(track.getX() - 2.0f, vu - .5f, track.getWidth() + 4.0f, 1.0f);
                graphics.setFont(Theme::valueFont(11.0f));
                graphics.setColour(Theme::label());
                graphics.drawText(channels_ == 1 ? "M" : (channel == 0 ? "L" : "R"),
                                  firstX + channel * 18 - 3, 68, 14, 14, juce::Justification::centred, false);
            }
            const float highest = channels_ == 1 ? levels_[side][0] : std::max(levels_[side][0], levels_[side][1]);
            graphics.setFont(Theme::valueFont());
            graphics.setColour(highest > 0.0f ? Theme::clipping() : Theme::pale());
            graphics.drawText(highest <= -99.9f ? "-inf" : juce::String(highest, 1),
                              column.withY(82).withHeight(16), juce::Justification::centred, false);
            graphics.setFont(Theme::valueFont(11.0f));
            graphics.setColour(Theme::label());
            graphics.drawText("dBFS", column.withY(98).withHeight(14), juce::Justification::centred, false);
        }
    }
private:
    Taper102AudioProcessor& processor_;
    int channels_ = 2;
    float levels_[2][2] { { -100, -100 }, { -100, -100 } };
};
}

struct Taper102AudioProcessorEditor::Impl final : juce::Component
{
    explicit Impl(Taper102AudioProcessor& owner)
        : processor(owner),
          input(owner.parameters,"input_db","INPUT","Level into the record stage. At 0 dB a tone at the 0 VU level records at the operating level; each dB more drives the tape harder. 456 15 NAB: 0.19% THD at 0 VU (0.05% of it second harmonic), 3% at +9.5 dB. Link ties Output to it.",levelText),
          output(owner.parameters,"output_db","OUTPUT","Level after the reproduce stage. At 0 dB, 0 VU in leaves at 0 VU in every setup. With Link on, Output follows Input.",levelText),
          setup(owner.parameters,"setup","SETUP",{"456 15 NAB","456 30 AES","ATR 15 IEC","ATR 30 AES"},"Picks tape, speed, EQ standard and the tape's aligned operating level (456 at 370 nWb/m, ATR Master at 320 nWb/m), as in the manual. Hover an option for what it does to the sound."),
          link(owner.parameters,"link","LINK",{"OFF","ON"},"Moves Output opposite to Input, dB for dB, so driving the tape harder keeps the level roughly the same (456, +12 dB: 1.5 dB quieter at 0 VU). Output stops at its ends. Automating Input moves Output too."),
          ref(owner.parameters,"ref","0 VU DBFS",{"-18","-15","-12","-9"},"Which digital level the machine treats as 0 VU. -18 drives the tape hardest; -9 is the cleanest: 456 at 15 ips reaches 3% distortion at -8.5 dBFS with -18 and at -2.5 dBFS with -12."),
          path(owner.parameters,"bypass","BYPASS",{"OFF","ON"},"Plays the input instead of the tape, delayed by the reported latency so switching never shifts timing; a short crossfade avoids clicks."),
          meters(owner)
    {
        setup.setOptionHelp({
            juce::String::fromUTF8("Most tape colour: about +1.4 dB bump near 40 Hz, top slightly rolled (-0.7 dB at 20 kHz) and the most saturation: 0.19% THD at 0 VU, 3% at +9.5 dB over 0 VU."),
            juce::String::fromUTF8("Cleaner and more open: 0.11% THD at 0 VU, a smaller bump moved up to about 85 Hz with tighter lows below it, flat to 20 kHz."),
            juce::String::fromUTF8("Saturates like 456 at 15 ips (0.175% THD at 0 VU) but with a smaller low bump (+0.6 dB), so the low end is tighter."),
            juce::String::fromUTF8("Cleanest and flattest setup: 0.13% THD at 0 VU.") });
        for (auto* component : std::array<juce::Component*,7>{&input,&output,&setup,&ref,&link,&path,&meters})
            addAndMakeVisible(component);
        showHelpFor(nullptr);
    }
    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(Theme::background());
        graphics.setFont(Theme::titleFont());
        graphics.setColour(Theme::pale());
        graphics.drawSingleLineText("TAPER 102", 24, 33);
        auto area = helpArea;
        graphics.setFont(Theme::labelFont());
        graphics.setColour(Theme::pale());
        graphics.drawText(helpTitle, area.removeFromTop(16), juce::Justification::topLeft, false);
        graphics.setFont(Theme::bodyFont());
        graphics.setColour(Theme::label());
        graphics.drawFittedText(helpText, area, juce::Justification::topLeft, 3, 1.0f);
    }
    void resized() override
    {
        // Two lanes of option groups (24 px between groups), centred over the knob group (bank of three 64 px knobs in 104 px cells plus the
        // meter column); the window width follows the widest of that group and the help text.
        constexpr int margin = 24, cell = 104, bankWidth = 2 * cell, rowsTop = 54, rowGap = 2, groupGap = 24, meterWidth = 132;
        const int groupWidth = bankWidth + 24 + meterWidth;
        const int groupLeft = (getWidth() - groupWidth) / 2, centre = groupLeft + groupWidth / 2;
        const std::array<std::array<OptionRow*, 3>, 2> lanes { { { &setup, nullptr, nullptr }, { &ref, &link, &path } } };
        int y = rowsTop;
        for (const auto& lane : lanes)
        {
            int width = 0;
            for (auto* row : lane) if (row != nullptr) width += row->preferredWidth() + (width > 0 ? groupGap : 0);
            int x = centre - width / 2;
            for (auto* row : lane)
                if (row != nullptr) { row->setBounds(x, y, row->preferredWidth(), rowHeight); x += row->preferredWidth() + groupGap; }
            y += rowHeight + rowGap;
        }
        const int firstRow = y - rowGap + 4;
        const std::array<Knob*, 2> knobs { &input, &output };
        for (std::size_t i = 0; i < knobs.size(); ++i)
            knobs[i]->setBounds(groupLeft + static_cast<int>(i) * cell, firstRow, cell, knobs[i]->preferredHeight());
        meters.setBounds(groupLeft + bankWidth + 24, firstRow + (input.preferredHeight() - 112) / 2, meterWidth, 112);
        helpArea = { margin, getHeight() - 64, getWidth() - 2 * margin, 54 };
    }
    void showHelpFor(juce::Component* component)
    {
        juce::String title = defaultHelpTitle, text = defaultHelp;
        if (processor.hasProcessingError())
        {
            title = "PROCESSING FAULT";
            text = "The plug-in could not prepare the tape model for this sample rate or channel layout; audio passes through unprocessed. Hover a control to see its help.";
        }
        for (auto* current = component; current != nullptr && current != this; current = current->getParentComponent())
        {
            if (!current->getProperties().contains("help")) continue;
            title = current->getProperties()["helpTitle"].toString();
            text = current->getProperties()["help"].toString();
            break;
        }
        if (helpTitle != title || helpText != text)
        {
            helpTitle = title; helpText = text;
            repaint(helpArea);
        }
    }
    void showHelpForParameter(const juce::String& idAndOption)   // "setup" or "setup:2" (option's own help, for snapshots)
    {
        const auto id = idAndOption.upToFirstOccurrenceOf(":", false, false);
        const int option = idAndOption.contains(":") ? idAndOption.fromFirstOccurrenceOf(":", false, false).getIntValue() : -1;
        for (auto* child : getChildren())
            if (child->getComponentID() == id)
            {
                auto* row = dynamic_cast<OptionRow*>(child);
                if (row != nullptr) row->showOptionHelp(option);
                showHelpFor(child);
                if (row != nullptr) row->showOptionHelp(-1);
                return;
            }
        showHelpFor(nullptr);
    }
    Taper102AudioProcessor& processor;
    juce::SharedResourcePointer<EditorFonts> fonts;
    FlatLookAndFeel lookAndFeel;
    Knob input, output;
    OptionRow setup, link, ref, path;
    PeakMeters meters;
    juce::String helpTitle, helpText;
    juce::Rectangle<int> helpArea;
};

Taper102AudioProcessorEditor::Taper102AudioProcessorEditor(Taper102AudioProcessor& owner)
    : AudioProcessorEditor(owner), processor_(owner), impl_(std::make_unique<Impl>(owner))
{
    setLookAndFeel(&impl_->lookAndFeel);
    addAndMakeVisible(*impl_);
    setResizable(true, true);
    setResizeLimits(designWidth * 3 / 4, designHeight * 3 / 4, designWidth * 2, designHeight * 2);
    getConstrainer()->setFixedAspectRatio(static_cast<double>(designWidth) / designHeight);
    const auto saved = static_cast<double>(owner.parameters.state.getProperty("editorScale", 1.0));
    const auto scale = std::isfinite(saved) ? juce::jlimit(.75, 2.0, saved) : 1.0;
    setSize(juce::roundToInt(designWidth * scale), juce::roundToInt(designHeight * scale));
    startTimerHz(30);
}
Taper102AudioProcessorEditor::~Taper102AudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel(nullptr);
}
void Taper102AudioProcessorEditor::paint(juce::Graphics& graphics) { graphics.fillAll(Theme::background()); }
void Taper102AudioProcessorEditor::resized()
{
    const float scale = static_cast<float>(getWidth()) / designWidth;
    impl_->setTransform(juce::AffineTransform::scale(scale));
    impl_->setBounds(0, 0, designWidth, designHeight);
    if (isShowing() && getPeer() != nullptr)
        processor_.parameters.state.setProperty("editorScale", static_cast<double>(scale), nullptr);
}
void Taper102AudioProcessorEditor::updateMeters() { impl_->meters.update(); }
void Taper102AudioProcessorEditor::showHelpForParameter(const juce::String& id) { impl_->showHelpForParameter(id); }
void Taper102AudioProcessorEditor::timerCallback()
{
    updateMeters();
    auto* under = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
    impl_->showHelpFor(under != nullptr && impl_->isParentOf(under) ? under : nullptr);
}
