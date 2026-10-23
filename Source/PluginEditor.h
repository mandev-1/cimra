#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

// Top-down view of the shared room: the mic pair, every linked instrument, and this one (draggable).
class RoomView : public juce::Component
{
public:
    explicit RoomView (CimraProcessor&);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> roomArea (const cimra::RoomSettings&) const;
    juce::Point<float> toScreen (const cimra::RoomSettings&, float xMetres, float yMetres) const;
    void moveTo (juce::Point<float> screenPos);
    void setParam (const char* paramId, float value);

    CimraProcessor& processor;
    enum class Drag { none, source, mic } drag = Drag::none;
};

class Knob : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const juce::String& label);
    void resized() override;

private:
    juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label label;
    juce::AudioProcessorValueTreeState::SliderAttachment attachment;
};

class CimraEditor : public juce::AudioProcessorEditor,
                    private juce::Timer
{
public:
    explicit CimraEditor (CimraProcessor&);
    ~CimraEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    CimraProcessor& processor;
    juce::LookAndFeel_V4 lookAndFeel;
    RoomView roomView;

    Knob roomWidth, roomDepth, roomHeight, walls, decay, tail, micY;
    Knob srcWidth, roomLevel, mix, output;

    juce::ToggleButton keepLevel { "Keep level" }, distanceDelay { "Distance delay" };
    juce::ComboBox link;
    juce::Label linkLabel;
    juce::AudioProcessorValueTreeState::ButtonAttachment keepLevelAttachment, distanceDelayAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> linkAttachment;   // created once the items exist

    juce::Rectangle<int> sharedHeader, instrumentHeader;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CimraEditor)
};
