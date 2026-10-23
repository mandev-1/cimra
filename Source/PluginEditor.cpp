#include "PluginEditor.h"

namespace
{
const juce::Colour background (0xff16181d), panel (0xff1f2229), floorColour (0xff262a33),
                   gridColour (0xff323743), textColour (0xffd8dbe2), dimText (0xff8a90a0), accent (0xfff2cc8f);
}

//==============================================================================
RoomView::RoomView (CimraProcessor& p) : processor (p)
{
    setMouseCursor (juce::MouseCursor::CrosshairCursor);
}

juce::Rectangle<float> RoomView::roomArea (const cimra::RoomSettings& room) const
{
    auto bounds = getLocalBounds().toFloat().reduced (28.0f, 34.0f).withTrimmedBottom (20.0f);
    auto aspect = room.width / room.depth;
    if (bounds.getWidth() / bounds.getHeight() > aspect)
        return bounds.withSizeKeepingCentre (bounds.getHeight() * aspect, bounds.getHeight());
    return bounds.withSizeKeepingCentre (bounds.getWidth(), bounds.getWidth() / aspect);
}

// The front wall (y = 0, where the mic usually stands) is drawn at the bottom.
juce::Point<float> RoomView::toScreen (const cimra::RoomSettings& room, float x, float y) const
{
    auto area = roomArea (room);
    return { area.getX() + x / room.width * area.getWidth(), area.getBottom() - y / room.depth * area.getHeight() };
}

void RoomView::paint (juce::Graphics& g)
{
    const auto room = processor.currentRoom();
    const auto area = roomArea (room);
    const auto pxPerMetre = area.getWidth() / room.width;

    // Floor and 1 m grid (2 m in big rooms).
    g.setColour (floorColour);
    g.fillRoundedRectangle (area, 3.0f);
    g.setColour (gridColour);
    const auto step = room.width > 16.0f || room.depth > 16.0f ? 2.0f : 1.0f;
    for (float x = step; x < room.width; x += step)
        g.drawVerticalLine (juce::roundToInt (area.getX() + x * pxPerMetre), area.getY(), area.getBottom());
    for (float y = step; y < room.depth; y += step)
        g.drawHorizontalLine (juce::roundToInt (area.getBottom() - y * pxPerMetre), area.getX(), area.getRight());
    g.setColour (dimText);
    g.drawRoundedRectangle (area, 3.0f, 1.5f);

    // Critical distance: inside it the instrument is heard mostly direct, outside mostly room.
    const auto mic = toScreen (room, room.width * 0.5f, room.micY);
    const auto rc = room.criticalDistance() * pxPerMetre;
    {
        juce::Path circle;
        circle.addEllipse (mic.x - rc, mic.y - rc, rc * 2.0f, rc * 2.0f);
        juce::Path dashed;
        const float dashes[] = { 4.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, circle, dashes, 2);
        g.saveState();
        g.reduceClipRegion (area.toNearestInt());
        g.setColour (accent.withAlpha (0.35f));
        g.fillPath (dashed);
        g.restoreState();
    }

    // Mic pair: two capsules splayed outwards, facing into the room.
    g.setColour (textColour);
    for (int m = 0; m < 2; ++m)
    {
        auto axis = room.micAxis (m);
        auto base = mic.translated ((m == 0 ? -4.0f : 4.0f), 0.0f);
        g.drawLine (base.x, base.y, base.x + axis.x * 14.0f, base.y - axis.y * 14.0f, 2.0f);
    }
    g.fillEllipse (mic.x - 4.0f, mic.y - 4.0f, 8.0f, 8.0f);

    // Other instruments in the same room.
    const auto group = processor.linkGroup();
    g.setFont (juce::FontOptions (12.0f));
    for (auto& [otherId, other] : cimra::SharedRoom::get().instancesInGroup (group))
    {
        if (otherId == processor.instanceId())
            continue;
        auto pos = toScreen (room, other.x * room.width, other.y * room.depth);
        auto halfWidth = 0.5f * other.sourceWidth * pxPerMetre;
        g.setColour (other.colour.withAlpha (0.55f));
        if (halfWidth > 1.0f)
            g.drawLine (pos.x - halfWidth, pos.y, pos.x + halfWidth, pos.y, 3.0f);
        g.drawEllipse (pos.x - 6.0f, pos.y - 6.0f, 12.0f, 12.0f, 2.0f);
        g.drawText (other.name, juce::Rectangle<float> (pos.x - 60.0f, pos.y + 8.0f, 120.0f, 14.0f), juce::Justification::centred);
    }

    // This instrument.
    const auto self = processor.info();
    const auto src = processor.sourcePosition (room);
    const auto pos = toScreen (room, src.x, src.y);
    const auto halfWidth = 0.5f * self.sourceWidth * pxPerMetre;
    g.setColour (self.colour.withAlpha (0.25f));
    g.fillEllipse (pos.x - 14.0f, pos.y - 14.0f, 28.0f, 28.0f);
    g.setColour (self.colour);
    if (halfWidth > 1.0f)
        g.drawLine (pos.x - halfWidth, pos.y, pos.x + halfWidth, pos.y, 4.0f);
    g.fillEllipse (pos.x - 7.0f, pos.y - 7.0f, 14.0f, 14.0f);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText (self.name, juce::Rectangle<float> (pos.x - 70.0f, pos.y - 28.0f, 140.0f, 15.0f), juce::Justification::centred);

    // Readout.
    const auto distance = room.directDistance (src);
    const auto direct = room.directGain (src);
    const auto drRatio = juce::Decibels::gainToDecibels (direct / room.diffuseGain());
    g.setColour (dimText);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (juce::String (room.width, 1) + " x " + juce::String (room.depth, 1) + " x " + juce::String (room.height, 1) + " m   RT60 "
                    + juce::String (room.rt60(), 2) + " s   " + juce::String (distance, 1) + " m from mic   direct/room "
                    + juce::String (drRatio, 1) + " dB",
                getLocalBounds().removeFromBottom (26), juce::Justification::centred);
    g.drawText (group < 0 ? "Unlinked: this track has its own room" : "Drag to place this instrument. Dashed circle = critical distance",
                getLocalBounds().removeFromTop (30), juce::Justification::centred);
}

void RoomView::setParam (const char* paramId, float value)
{
    if (auto* p = processor.params.getParameter (paramId))
        p->setValueNotifyingHost (p->convertTo0to1 (value));
}

void RoomView::moveTo (juce::Point<float> screenPos)
{
    const auto room = processor.currentRoom();
    const auto area = roomArea (room);
    const auto nx = juce::jlimit (0.0f, 1.0f, (screenPos.x - area.getX()) / area.getWidth());
    const auto ny = juce::jlimit (0.0f, 1.0f, (area.getBottom() - screenPos.y) / area.getHeight());

    if (drag == Drag::mic)
    {
        setParam ("micY", ny);
    }
    else if (drag == Drag::source)
    {
        setParam ("posX", nx);
        setParam ("posY", ny);
    }
    repaint();
}

void RoomView::mouseDown (const juce::MouseEvent& e)
{
    const auto room = processor.currentRoom();
    const auto mic = toScreen (room, room.width * 0.5f, room.micY);
    const auto src = processor.sourcePosition (room);
    const auto srcPos = toScreen (room, src.x, src.y);
    const auto nearMic = e.position.getDistanceFrom (mic) < 14.0f && e.position.getDistanceFrom (srcPos) > 10.0f;

    drag = nearMic ? Drag::mic : Drag::source;
    for (auto* paramId : nearMic ? std::initializer_list<const char*> { "micY" } : std::initializer_list<const char*> { "posX", "posY" })
        processor.params.getParameter (paramId)->beginChangeGesture();
    moveTo (e.position);
}

void RoomView::mouseDrag (const juce::MouseEvent& e)
{
    moveTo (e.position);
}

void RoomView::mouseUp (const juce::MouseEvent&)
{
    for (auto* paramId : drag == Drag::mic ? std::initializer_list<const char*> { "micY" } : std::initializer_list<const char*> { "posX", "posY" })
        processor.params.getParameter (paramId)->endChangeGesture();
    drag = Drag::none;
}

//==============================================================================
Knob::Knob (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, const juce::String& text)
    : attachment (state, paramId, slider)
{
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 72, 16);
    label.setText (text, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (juce::FontOptions (12.0f));
    label.setColour (juce::Label::textColourId, dimText);
    addAndMakeVisible (slider);
    addAndMakeVisible (label);
}

void Knob::resized()
{
    auto b = getLocalBounds();
    label.setBounds (b.removeFromTop (16));
    slider.setBounds (b);
}

//==============================================================================
CimraEditor::CimraEditor (CimraProcessor& p)
    : AudioProcessorEditor (p), processor (p), roomView (p),
      roomWidth (p.params, "roomWidth", "Width"), roomDepth (p.params, "roomDepth", "Depth"),
      roomHeight (p.params, "roomHeight", "Height"), walls (p.params, "walls", "Dead / Live"),
      decay (p.params, "decay", "Decay"), tail (p.params, "tail", "Tail"), micY (p.params, "micY", "Mic"),
      srcWidth (p.params, "srcWidth", "Source width"), roomLevel (p.params, "roomLevel", "Room level"),
      mix (p.params, "mix", "Mix"), output (p.params, "output", "Output"),
      keepLevelAttachment (p.params, "levelComp", keepLevel),
      distanceDelayAttachment (p.params, "trueTiming", distanceDelay)
{
    lookAndFeel.setColourScheme (juce::LookAndFeel_V4::getMidnightColourScheme());
    lookAndFeel.setColour (juce::Slider::rotarySliderFillColourId, accent);
    lookAndFeel.setColour (juce::Slider::thumbColourId, accent);
    lookAndFeel.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    lookAndFeel.setColour (juce::ResizableWindow::backgroundColourId, background);
    setLookAndFeel (&lookAndFeel);

    link.addItemList (p.params.getParameter ("link")->getAllValueStrings(), 1);
    linkAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (p.params, "link", link);
    linkLabel.setText ("Room link", juce::dontSendNotification);
    linkLabel.setColour (juce::Label::textColourId, dimText);
    link.setTooltip ("Tracks on the same room share size, walls, decay and mic position");
    keepLevel.setTooltip ("Hold loudness constant as the instrument moves, so only depth changes");
    distanceDelay.setTooltip ("Delay the instrument by its real distance to the mic (off keeps it in time)");

    addAndMakeVisible (roomView);
    for (auto* c : std::initializer_list<juce::Component*> { &roomWidth, &roomDepth, &roomHeight, &walls, &decay, &tail, &micY,
                                                             &srcWidth, &roomLevel, &mix, &output, &keepLevel, &distanceDelay, &link, &linkLabel })
        addAndMakeVisible (c);

    setSize (920, 560);
    startTimerHz (30);
}

CimraEditor::~CimraEditor()
{
    setLookAndFeel (nullptr);
}

void CimraEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);
    g.setColour (panel);
    g.fillRect (getLocalBounds().removeFromRight (380));

    g.setColour (textColour);
    g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    g.drawText ("CIMRA", 16, 10, 200, 26, juce::Justification::centredLeft);

    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.setColour (accent);
    const auto group = processor.linkGroup();
    g.drawText (group < 0 ? "ROOM (this track only)" : "SHARED ROOM (all tracks on " + link.getText() + ")", sharedHeader, juce::Justification::centredLeft);
    g.drawText ("THIS INSTRUMENT", instrumentHeader, juce::Justification::centredLeft);
}

void CimraEditor::resized()
{
    auto b = getLocalBounds();
    auto right = b.removeFromRight (380).reduced (14, 12);
    roomView.setBounds (b.withTrimmedTop (30));

    auto row = [] (juce::Rectangle<int>& area, std::initializer_list<juce::Component*> items)
    {
        auto r = area.removeFromTop (96);
        area.removeFromTop (8);
        auto w = r.getWidth() / 4;
        for (auto* c : items)
            c->setBounds (r.removeFromLeft (w).reduced (3, 0));
    };

    sharedHeader = right.removeFromTop (22);
    row (right, { &roomWidth, &roomDepth, &roomHeight, &micY });
    row (right, { &walls, &decay, &tail });
    right.removeFromTop (14);

    instrumentHeader = right.removeFromTop (22);
    row (right, { &srcWidth, &roomLevel, &mix, &output });
    right.removeFromTop (12);

    auto toggles = right.removeFromTop (26);
    keepLevel.setBounds (toggles.removeFromLeft (toggles.getWidth() / 2));
    distanceDelay.setBounds (toggles);
    right.removeFromTop (10);
    auto linkRow = right.removeFromTop (26);
    linkLabel.setBounds (linkRow.removeFromLeft (80));
    link.setBounds (linkRow.removeFromLeft (170));
}

void CimraEditor::timerCallback()
{
    roomView.repaint();
    repaint (sharedHeader);
}
