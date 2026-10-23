#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
juce::NormalisableRange<float> metres (float lo, float hi, float centre)
{
    juce::NormalisableRange<float> r (lo, hi, 0.1f);
    r.setSkewForCentre (centre);
    return r;
}

auto withUnit (const char* unit) { return juce::AudioParameterFloatAttributes().withLabel (unit); }

float load (const juce::AudioProcessorValueTreeState& p, const char* paramId)
{
    return p.getRawParameterValue (paramId)->load();
}

const juce::Colour palette[] = { juce::Colour (0xffe07a5f), juce::Colour (0xff81b29a), juce::Colour (0xfff2cc8f),
                                 juce::Colour (0xff6d9dc5), juce::Colour (0xffc08cd8), juce::Colour (0xff5fc9c4) };
} // namespace

//==============================================================================
CimraProcessor::CimraProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (*this, nullptr, "Cimra", createLayout())
{
    id = cimra::SharedRoom::get().registerInstance();
    lastSharedValues = readSharedParams();
    startTimerHz (15);
}

CimraProcessor::~CimraProcessor()
{
    stopTimer();
    cimra::SharedRoom::get().unregisterInstance (id);
}

juce::AudioProcessorValueTreeState::ParameterLayout CimraProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    // Shared room, mirrored across every instance in the same link group.
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "roomWidth", 1 }, "Room Width", metres (3.0f, 30.0f, 9.0f), 8.0f, withUnit ("m")),
                std::make_unique<AudioParameterFloat> (ParameterID { "roomDepth", 1 }, "Room Depth", metres (3.0f, 40.0f, 11.0f), 10.0f, withUnit ("m")),
                std::make_unique<AudioParameterFloat> (ParameterID { "roomHeight", 1 }, "Room Height", metres (2.4f, 15.0f, 4.5f), 3.5f, withUnit ("m")),
                std::make_unique<AudioParameterFloat> (ParameterID { "walls", 1 }, "Walls (Dead-Live)", NormalisableRange<float> (0.0f, 1.0f), 0.5f,
                                                       AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 100)) + " %"; })),
                std::make_unique<AudioParameterFloat> (ParameterID { "decay", 1 }, "Decay Scale", NormalisableRange<float> (0.5f, 2.0f, 0.01f, 0.63f), 1.0f, withUnit ("x")),
                std::make_unique<AudioParameterFloat> (ParameterID { "tail", 1 }, "Tail Level", NormalisableRange<float> (-24.0f, 6.0f, 0.1f), -3.0f, withUnit ("dB")),
                std::make_unique<AudioParameterFloat> (ParameterID { "micY", 1 }, "Mic Position", NormalisableRange<float> (0.0f, 1.0f), 0.15f,
                                                       AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 100)) + " % deep"; })));

    // Per instrument.
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "posX", 1 }, "Position X", NormalisableRange<float> (0.0f, 1.0f), 0.5f),
                std::make_unique<AudioParameterFloat> (ParameterID { "posY", 1 }, "Position Y", NormalisableRange<float> (0.0f, 1.0f), 0.45f),
                std::make_unique<AudioParameterFloat> (ParameterID { "srcWidth", 1 }, "Source Width", NormalisableRange<float> (0.0f, 4.0f, 0.01f), 0.0f, withUnit ("m")),
                std::make_unique<AudioParameterFloat> (ParameterID { "roomLevel", 1 }, "Room Level", NormalisableRange<float> (-36.0f, 6.0f, 0.1f), 0.0f, withUnit ("dB")),
                std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 100.0f, withUnit ("%")),
                std::make_unique<AudioParameterFloat> (ParameterID { "output", 1 }, "Output", NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f, withUnit ("dB")),
                std::make_unique<AudioParameterBool> (ParameterID { "levelComp", 1 }, "Keep Level", true),
                std::make_unique<AudioParameterBool> (ParameterID { "trueTiming", 1 }, "Distance Delay", false),
                std::make_unique<AudioParameterChoice> (ParameterID { "link", 1 }, "Room Link", StringArray { "Off", "Room A", "Room B", "Room C", "Room D" }, 1));

    return layout;
}

//==============================================================================
cimra::RoomSettings CimraProcessor::currentRoom() const
{
    cimra::RoomSettings room;
    room.width = load (params, "roomWidth");
    room.depth = load (params, "roomDepth");
    room.height = load (params, "roomHeight");
    room.walls = load (params, "walls");
    room.decayScale = load (params, "decay");
    room.micY = juce::jlimit (0.3f, room.depth - 0.3f, load (params, "micY") * room.depth);
    room.micHeight = std::min (1.6f, room.height - 0.3f);
    return room;
}

cimra::Vec3 CimraProcessor::sourcePosition (const cimra::RoomSettings& room, float xOffset) const
{
    return { juce::jlimit (0.2f, room.width - 0.2f, load (params, "posX") * room.width + xOffset),
             juce::jlimit (0.2f, room.depth - 0.2f, load (params, "posY") * room.depth),
             std::min (1.2f, room.height - 0.3f) };
}

int CimraProcessor::linkGroup() const
{
    return juce::roundToInt (load (params, "link")) - 1;
}

cimra::InstanceInfo CimraProcessor::info() const
{
    cimra::InstanceInfo i;
    {
        const juce::ScopedLock sl (trackLock);
        i.name = trackName.isNotEmpty() ? trackName : "Cimra " + juce::String (id);
        i.colour = trackColour.isTransparent() ? palette[(size_t) (id - 1) % std::size (palette)] : trackColour;
    }
    i.x = load (params, "posX");
    i.y = load (params, "posY");
    i.sourceWidth = load (params, "srcWidth");
    i.group = linkGroup();
    return i;
}

void CimraProcessor::updateTrackProperties (const TrackProperties& properties)
{
    const juce::ScopedLock sl (trackLock);
    trackName = properties.name;
    trackColour = properties.colour;
}

//==============================================================================
std::vector<float> CimraProcessor::readSharedParams() const
{
    std::vector<float> values;
    for (auto& paramId : cimra::sharedParamIds())
        values.push_back (params.getRawParameterValue (paramId)->load());
    return values;
}

void CimraProcessor::applySharedParams (const std::vector<float>& values)
{
    auto& ids = cimra::sharedParamIds();
    for (int i = 0; i < ids.size() && i < (int) values.size(); ++i)
        if (auto* p = params.getParameter (ids[i]))
            p->setValueNotifyingHost (p->convertTo0to1 (values[(size_t) i]));
}

void CimraProcessor::timerCallback()
{
    auto& shared = cimra::SharedRoom::get();
    auto group = linkGroup();

    if (stateLoaded.exchange (false))
    {
        // Treat the loaded room as already seen; if the group exists the instance joins it.
        lastSharedValues = readSharedParams();
        seenGroupVersion = 0;
    }

    if (group != lastGroup)
    {
        lastGroup = group;
        seenGroupVersion = 0;   // adopt the room of the newly chosen group
    }

    if (group >= 0)
    {
        auto values = readSharedParams();
        auto state = shared.getGroup (group);

        auto changedLocally = false;
        for (size_t i = 0; i < values.size(); ++i)
            changedLocally |= std::abs (values[i] - lastSharedValues[i]) > 1.0e-4f * std::max (1.0f, std::abs (values[i]));

        if (state.version == 0 || changedLocally)
        {
            seenGroupVersion = shared.setGroup (group, values);
        }
        else if (state.version != seenGroupVersion)
        {
            applySharedParams (state.values);
            seenGroupVersion = state.version;
        }

        lastSharedValues = readSharedParams();
    }

    shared.publish (id, info());
}

//==============================================================================
bool CimraProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    auto in = layouts.getMainInputChannelSet();
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

void CimraProcessor::prepareToPlay (double newSampleRate, int samplesPerBlock)
{
    sampleRate = newSampleRate;
    engineL.prepare (sampleRate, 0.6f);
    engineR.prepare (sampleRate, 0.6f);
    reverb.prepare (sampleRate);

    for (auto* b : { &dryCopy, &dirBuf, &erBuf, &tailBuf })
        b->setSize (2, samplesPerBlock);
    monoBuf.setSize (3, samplesPerBlock);

    smoothing = cimra::onePoleCoeff (20.0f, sampleRate);
    compGain = roomGain = mixGain = outGain = -1.0f;   // snap to target on the first block
}

void CimraProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto n = buffer.getNumSamples();
    const auto numIn = getTotalNumInputChannels();
    if (n == 0 || numIn == 0)
        return;

    for (auto* b : { &dryCopy, &dirBuf, &erBuf, &tailBuf })
        b->setSize (2, n, false, false, true);
    monoBuf.setSize (3, n, false, false, true);

    // Gather input. Stereo material splits into two half-level sources, mono stays one source;
    // both paths give the same level for a centred signal.
    const auto* inL = buffer.getReadPointer (0);
    const auto* inR = buffer.getReadPointer (numIn > 1 ? 1 : 0);
    dryCopy.copyFrom (0, 0, inL, n);
    dryCopy.copyFrom (1, 0, inR, n);

    auto* mono = monoBuf.getWritePointer (0);
    auto* halfL = monoBuf.getWritePointer (1);
    auto* halfR = monoBuf.getWritePointer (2);
    for (int i = 0; i < n; ++i)
    {
        halfL[i] = 0.5f * inL[i];
        halfR[i] = 0.5f * inR[i];
        mono[i] = halfL[i] + halfR[i];
    }

    // Geometry.
    const auto room = currentRoom();
    const auto centre = sourcePosition (room);
    const auto width = load (params, "srcWidth");
    const auto stereoSource = numIn > 1 && width > 0.01f;
    const auto directSamples = room.directDistance (centre) / cimra::kSpeedOfSound * (float) sampleRate;
    const auto offset = load (params, "trueTiming") > 0.5f ? 0.0f : directSamples;

    dirBuf.clear();
    erBuf.clear();
    tailBuf.clear();

    if (stereoSource)
    {
        engineL.setTargets (room, sourcePosition (room, -0.5f * width), offset);
        engineR.setTargets (room, sourcePosition (room, 0.5f * width), offset);
        engineL.process (halfL, dirBuf.getWritePointer (0), dirBuf.getWritePointer (1), erBuf.getWritePointer (0), erBuf.getWritePointer (1), n);
        engineR.process (halfR, dirBuf.getWritePointer (0), dirBuf.getWritePointer (1), erBuf.getWritePointer (0), erBuf.getWritePointer (1), n);
    }
    else
    {
        engineL.setTargets (room, centre, offset);
        engineL.process (mono, dirBuf.getWritePointer (0), dirBuf.getWritePointer (1), erBuf.getWritePointer (0), erBuf.getWritePointer (1), n);
    }

    // Diffuse tail starts roughly two mean free paths after the direct sound.
    const auto tailLevel = juce::Decibels::decibelsToGain (load (params, "tail"));
    const auto predelay = directSamples + 2.0f * room.meanFreePath() / cimra::kSpeedOfSound * (float) sampleRate - offset;
    reverb.setParams (room, room.diffuseGain() * tailLevel, predelay);
    reverb.process (mono, tailBuf.getWritePointer (0), tailBuf.getWritePointer (1), n);

    // Gains.
    const auto roomTarget = load (params, "roomLevel") <= -35.9f ? 0.0f : juce::Decibels::decibelsToGain (load (params, "roomLevel"));
    auto compTarget = 1.0f;
    if (load (params, "levelComp") > 0.5f)
    {
        // Hold perceived loudness constant: moving back changes the direct/room balance, not the fader level.
        auto direct = room.directGain (centre);
        auto diffuse = room.diffuseGain() * tailLevel * roomTarget;
        compTarget = std::min (16.0f, 1.0f / std::sqrt (direct * direct + diffuse * diffuse));
    }
    const auto mixTarget = load (params, "mix") * 0.01f;
    const auto outTarget = juce::Decibels::decibelsToGain (load (params, "output"));

    if (compGain < 0.0f)
    {
        compGain = compTarget;
        roomGain = roomTarget;
        mixGain = mixTarget;
        outGain = outTarget;
    }

    for (int ch = 0; ch < buffer.getNumChannels() && ch < 2; ++ch)
    {
        auto* out = buffer.getWritePointer (ch);
        const auto* dry = dryCopy.getReadPointer (ch);
        const auto* dir = dirBuf.getReadPointer (ch);
        const auto* er = erBuf.getReadPointer (ch);
        const auto* tail = tailBuf.getReadPointer (ch);
        auto c = compGain, r = roomGain, m = mixGain, o = outGain;

        for (int i = 0; i < n; ++i)
        {
            c += (compTarget - c) * smoothing;
            r += (roomTarget - r) * smoothing;
            m += (mixTarget - m) * smoothing;
            o += (outTarget - o) * smoothing;
            auto wet = c * (dir[i] + r * (er[i] + tail[i]));
            out[i] = o * (dry[i] + m * (wet - dry[i]));
        }

        if (ch == 1 || buffer.getNumChannels() == 1)
        {
            compGain = c;
            roomGain = r;
            mixGain = m;
            outGain = o;
        }
    }
}

//==============================================================================
void CimraProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = params.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void CimraProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (params.state.getType()))
            params.replaceState (juce::ValueTree::fromXml (*xml));

    stateLoaded = true;   // handled on the message thread in timerCallback
}

juce::AudioProcessorEditor* CimraProcessor::createEditor()
{
    return new CimraEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CimraProcessor();
}
