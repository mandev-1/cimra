#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "RoomDSP.h"
#include "SharedRoom.h"

class CimraProcessor : public juce::AudioProcessor,
                       private juce::Timer
{
public:
    CimraProcessor();
    ~CimraProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 6.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;
    void updateTrackProperties (const TrackProperties& properties) override;

    // Current room and source position, read from the parameters (message or audio thread).
    cimra::RoomSettings currentRoom() const;
    cimra::Vec3 sourcePosition (const cimra::RoomSettings& room, float xOffset = 0.0f) const;
    int linkGroup() const;   // -1 when unlinked
    int instanceId() const { return id; }
    cimra::InstanceInfo info() const;

    juce::AudioProcessorValueTreeState params;

private:
    void timerCallback() override;
    std::vector<float> readSharedParams() const;
    void applySharedParams (const std::vector<float>& values);

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    cimra::SourceEngine engineL, engineR;
    cimra::LateReverb reverb;
    juce::AudioBuffer<float> dryCopy, dirBuf, erBuf, tailBuf, monoBuf;
    float compGain = 1.0f, roomGain = 1.0f, mixGain = 1.0f, outGain = 1.0f, smoothing = 0.0f;
    double sampleRate = 48000.0;

    int id = 0;
    juce::uint64 seenGroupVersion = 0;
    std::vector<float> lastSharedValues;
    int lastGroup = -2;
    std::atomic<bool> stateLoaded { false };

    juce::CriticalSection trackLock;
    juce::String trackName;
    juce::Colour trackColour;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CimraProcessor)
};
