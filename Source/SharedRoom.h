// Process-wide registry that lets every Cimra instance in the DAW share one room.
//
// DAWs load each plugin binary once per process, so this static object is visible to every
// instance on every track. Instances in the same link group mirror each other's room parameters
// and publish their position so each editor can draw the whole band.
//
// All access happens on the message thread (timers and editors), the mutex is a safety net for
// hosts that call updateTrackProperties from elsewhere.

#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <array>
#include <map>
#include <mutex>

namespace cimra
{

constexpr int kNumLinkGroups = 4;

// Parameters that describe the room itself and are kept identical within a link group.
inline const juce::StringArray& sharedParamIds()
{
    static const juce::StringArray ids { "roomWidth", "roomDepth", "roomHeight", "walls", "decay", "tail", "micY" };
    return ids;
}

struct InstanceInfo
{
    juce::String name;
    juce::Colour colour;
    float x = 0.5f, y = 0.5f, sourceWidth = 0.0f;
    int group = -1;
};

class SharedRoom
{
public:
    static SharedRoom& get()
    {
        static SharedRoom instance;
        return instance;
    }

    struct GroupState
    {
        std::vector<float> values;
        juce::uint64 version = 0;
    };

    int registerInstance()
    {
        std::lock_guard<std::mutex> lock (mutex);
        auto id = ++lastId;
        instances[id] = {};
        return id;
    }

    void unregisterInstance (int id)
    {
        std::lock_guard<std::mutex> lock (mutex);
        instances.erase (id);
    }

    void publish (int id, const InstanceInfo& info)
    {
        std::lock_guard<std::mutex> lock (mutex);
        instances[id] = info;
    }

    std::map<int, InstanceInfo> instancesInGroup (int group)
    {
        std::lock_guard<std::mutex> lock (mutex);
        std::map<int, InstanceInfo> result;
        for (auto& [id, info] : instances)
            if (group >= 0 && info.group == group)
                result[id] = info;
        return result;
    }

    GroupState getGroup (int group)
    {
        std::lock_guard<std::mutex> lock (mutex);
        return groups[(size_t) group];
    }

    juce::uint64 setGroup (int group, const std::vector<float>& values)
    {
        std::lock_guard<std::mutex> lock (mutex);
        auto& g = groups[(size_t) group];
        g.values = values;
        return ++g.version;
    }

private:
    SharedRoom() = default;

    std::mutex mutex;
    int lastId = 0;
    std::map<int, InstanceInfo> instances;
    std::array<GroupState, kNumLinkGroups> groups;
};

} // namespace cimra
