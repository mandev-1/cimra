// Cimra room model: pure C++ DSP with no JUCE dependency, so it can be unit-tested on its own.
//
// Signal model for one instrument standing in a shoebox room, picked up by an ORTF stereo pair:
//   - direct sound + early reflections from an image-source model (up to 3rd order)
//   - a late diffuse tail from an 8-line feedback delay network whose RT60 comes from Sabine's formula
//
// Every plugin instance that shares a room computes an identical tail, so summing the tracks
// gives the same result as one reverb fed by the whole band.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace cimra
{

constexpr float kSpeedOfSound = 343.0f;
constexpr float kPi = 3.14159265358979f;
constexpr int kMaxOrder = 3;

inline float onePoleCoeff (float cutoffHz, double sampleRate)
{
    return 1.0f - std::exp (-2.0f * kPi * cutoffHz / (float) sampleRate);
}

struct Vec3
{
    float x = 0, y = 0, z = 0;
    Vec3 operator- (const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    float length() const { return std::sqrt (x * x + y * y + z * z); }
    float dot (const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
};

//==============================================================================
struct RoomSettings
{
    float width = 8.0f, depth = 10.0f, height = 3.5f;   // metres
    float walls = 0.5f;          // 0 = dead / absorptive, 1 = live / reflective
    float decayScale = 1.0f;     // multiplier on the physically computed RT60
    float micY = 1.5f;           // metres from the front wall; the pair sits centred in x and faces +y
    float micHeight = 1.6f;
    float micSpacing = 0.17f;    // ORTF
    float micAngle = 0.96f;      // radians each side of the axis (55 degrees)

    float absorption() const     { return 0.6f + (0.04f - 0.6f) * walls; }
    float reflection() const     { return std::sqrt (1.0f - absorption()); }
    float volume() const         { return width * depth * height; }
    float surface() const        { return 2.0f * (width * depth + width * height + depth * height); }
    float meanFreePath() const   { return 4.0f * volume() / surface(); }

    float rt60() const
    {
        return std::clamp (0.161f * volume() / (surface() * absorption()) * decayScale, 0.1f, 10.0f);
    }

    // Distance at which direct and diffuse energy are equal (omni source and receiver).
    float criticalDistance() const  { return 0.057f * std::sqrt (volume() / rt60()); }

    // Amplitude of the diffuse field relative to the direct sound at 1 m, seen through a cardioid.
    float diffuseGain() const       { return 0.577f / criticalDistance(); }

    float erCutoff (int order) const
    {
        return (2500.0f + (12000.0f - 2500.0f) * walls) * std::pow (0.7f, (float) (order - 1));
    }

    Vec3 micPosition (int mic) const
    {
        return { width * 0.5f + (mic == 0 ? -0.5f : 0.5f) * micSpacing, micY, micHeight };
    }

    Vec3 micAxis (int mic) const
    {
        return { (mic == 0 ? -1.0f : 1.0f) * std::sin (micAngle), std::cos (micAngle), 0.0f };
    }

    // Distance from a source to the nearer capsule and the mean cardioid-weighted direct gain.
    float directDistance (Vec3 src) const
    {
        return std::min ((src - micPosition (0)).length(), (src - micPosition (1)).length());
    }

    float directGain (Vec3 src) const
    {
        float sum = 0;
        for (int m = 0; m < 2; ++m)
        {
            auto v = src - micPosition (m);
            auto r = std::max (v.length(), 0.3f);
            sum += (0.5f + 0.5f * v.dot (micAxis (m)) / r) / r;
        }
        return 0.5f * sum;
    }
};

//==============================================================================
// Direct sound and early reflections for one point source, rendered to an ORTF pair.
class SourceEngine
{
public:
    void prepare (double newSampleRate, float maxDelaySeconds)
    {
        sampleRate = newSampleRate;
        int size = 1;
        while (size < (int) (maxDelaySeconds * sampleRate) + 4)
            size <<= 1;
        buffer.assign ((size_t) size, 0.0f);
        mask = size - 1;
        maxDelay = (float) (size - 4);
        smoothing = onePoleCoeff (8.0f, sampleRate);   // ~20 ms glide when the source is moved

        lattice.clear();
        for (int nx = -kMaxOrder; nx <= kMaxOrder; ++nx)
            for (int ny = -kMaxOrder; ny <= kMaxOrder; ++ny)
                for (int nz = -kMaxOrder; nz <= kMaxOrder; ++nz)
                    if (std::abs (nx) + std::abs (ny) + std::abs (nz) <= kMaxOrder)
                        lattice.push_back ({ nx, ny, nz });

        for (auto& t : taps)
            t.assign (lattice.size(), {});

        reset();
    }

    void reset()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writePos = 0;
        for (auto& s : filterState)
            s.fill (0.0f);
        initialised = false;
    }

    // offsetSamples is subtracted from every path, which lets the caller remove the common
    // direct-path delay so the instrument stays in time with the rest of the mix.
    void setTargets (const RoomSettings& room, Vec3 src, float offsetSamples)
    {
        const float beta = room.reflection();

        for (int m = 0; m < 2; ++m)
        {
            auto mic = room.micPosition (m);
            auto axis = room.micAxis (m);

            for (size_t i = 0; i < lattice.size(); ++i)
            {
                const auto& n = lattice[i];
                Vec3 image { imageCoord (n[0], src.x, room.width),
                             imageCoord (n[1], src.y, room.depth),
                             imageCoord (n[2], src.z, room.height) };
                auto v = image - mic;
                auto r = std::max (v.length(), 0.3f);
                auto cardioid = 0.5f + 0.5f * v.dot (axis) / r;
                int order = std::abs (n[0]) + std::abs (n[1]) + std::abs (n[2]);

                auto& tap = taps[(size_t) m][i];
                tap.order = order;
                tap.targetGain = std::pow (beta, (float) order) * cardioid / r;
                tap.targetDelay = std::clamp (r / kSpeedOfSound * (float) sampleRate - offsetSamples, 0.0f, maxDelay);

                if (! initialised)
                {
                    tap.gain = tap.targetGain;
                    tap.delay = tap.targetDelay;
                }
            }
        }

        // Air absorption on the direct path, wall absorption getting darker with each bounce.
        auto distance = room.directDistance (src);
        filterCoeff[0] = onePoleCoeff (std::clamp (20000.0f * std::exp (-distance / 40.0f), 2000.0f, 20000.0f), sampleRate);
        for (int o = 1; o <= kMaxOrder; ++o)
            filterCoeff[(size_t) o] = onePoleCoeff (room.erCutoff (o), sampleRate);

        initialised = true;
    }

    // Adds direct sound to dirL/dirR and early reflections to erL/erR.
    void process (const float* in, float* dirL, float* dirR, float* erL, float* erR, int numSamples)
    {
        float* dir[2] = { dirL, dirR };
        float* er[2] = { erL, erR };

        for (int s = 0; s < numSamples; ++s)
        {
            buffer[(size_t) writePos] = in[s];

            for (int m = 0; m < 2; ++m)
            {
                std::array<float, kMaxOrder + 1> bus {};

                for (auto& tap : taps[(size_t) m])
                {
                    tap.delay += (tap.targetDelay - tap.delay) * smoothing;
                    tap.gain += (tap.targetGain - tap.gain) * smoothing;
                    bus[(size_t) tap.order] += tap.gain * read (tap.delay);
                }

                auto& state = filterState[(size_t) m];
                for (int o = 0; o <= kMaxOrder; ++o)
                    state[(size_t) o] += filterCoeff[(size_t) o] * (bus[(size_t) o] - state[(size_t) o]);

                dir[m][s] += state[0];
                er[m][s] += state[1] + state[2] + state[3];
            }

            writePos = (writePos + 1) & mask;
        }
    }

private:
    struct Tap { float delay = 0, targetDelay = 0, gain = 0, targetGain = 0; int order = 0; };

    // Position of the n-th image of coordinate p in a room of length L along that axis.
    static float imageCoord (int n, float p, float L)
    {
        return (float) n * L + ((n & 1) ? L - p : p);
    }

    float read (float delay) const
    {
        auto whole = (int) delay;
        auto frac = delay - (float) whole;
        auto i0 = writePos - whole;
        return buffer[(size_t) (i0 & mask)] * (1.0f - frac) + buffer[(size_t) ((i0 - 1) & mask)] * frac;
    }

    double sampleRate = 48000.0;
    std::vector<float> buffer;
    int mask = 0, writePos = 0;
    float maxDelay = 0, smoothing = 0;
    bool initialised = false;

    std::vector<std::array<int, 3>> lattice;
    std::array<std::vector<Tap>, 2> taps;
    std::array<float, kMaxOrder + 1> filterCoeff {};
    std::array<std::array<float, kMaxOrder + 1>, 2> filterState {};
};

//==============================================================================
// Diffuse late reverberation: input diffusion followed by an 8-line Hadamard FDN.
// Fully deterministic, so every instance sharing a room produces the same tail.
class LateReverb
{
public:
    static constexpr int numLines = 8;

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        auto maxLen = (size_t) (0.2 * sampleRate * maxSizeScale) + 4;
        for (auto& line : lines)
            line.buffer.assign (nextPow2 (maxLen), 0.0f);

        for (size_t i = 0; i < diffusers.size(); ++i)
        {
            auto len = (size_t) (diffuserMs[i] * 0.001 * sampleRate);
            diffusers[i].buffer.assign (len, 0.0f);
        }

        predelay.assign (nextPow2 ((size_t) (0.5 * sampleRate) + 4), 0.0f);
        crossover = onePoleCoeff (3000.0f, sampleRate);
        smoothing = onePoleCoeff (4.0f, sampleRate);
        reset();
    }

    void reset()
    {
        for (auto& line : lines)
        {
            std::fill (line.buffer.begin(), line.buffer.end(), 0.0f);
            line.writePos = 0;
            line.lp = 0;
        }
        for (auto& d : diffusers)
        {
            std::fill (d.buffer.begin(), d.buffer.end(), 0.0f);
            d.pos = 0;
        }
        std::fill (predelay.begin(), predelay.end(), 0.0f);
        predelayPos = 0;
        initialised = false;
    }

    void setParams (const RoomSettings& room, float inputGain, float predelaySamples)
    {
        auto sizeScale = std::clamp (std::cbrt (room.volume() / 280.0f), 0.3f, maxSizeScale);
        auto rt60 = room.rt60();
        float gSum = 0;

        for (size_t i = 0; i < lines.size(); ++i)
        {
            auto& line = lines[i];
            // Whole-sample lengths at rest: interpolated reads would dull the tail on every pass.
            line.targetLength = std::round (std::min (lineMs[i] * 0.001f * sizeScale * (float) sampleRate,
                                                      (float) line.buffer.size() - 4.0f));
            line.feedback = std::pow (10.0f, -3.0f * line.targetLength / (rt60 * (float) sampleRate));
            gSum += line.feedback;
            if (! initialised)
                line.length = line.targetLength;
        }

        // Highs die faster than lows; deader walls make the difference bigger.
        auto hfRatio = 0.35f + 0.45f * room.walls;
        for (auto& line : lines)
            line.feedbackHigh = std::pow (line.feedback, 1.0f / hfRatio);

        // Steady-state energy of a line is input / (1 - g^2) in each band; the crossover passes
        // about a fifth of white noise energy into the low band.
        auto g = gSum / (float) numLines;
        auto gHigh = std::pow (g, 1.0f / hfRatio);
        auto energy = lowBandShare / (1.0f - g * g) + (1.0f - lowBandShare) / (1.0f - gHigh * gHigh);
        targetInputGain = inputGain / std::sqrt (energy);
        targetPredelay = std::clamp (predelaySamples, 0.0f, (float) predelay.size() - 4.0f);

        if (! initialised)
        {
            currentInputGain = targetInputGain;
            currentPredelay = targetPredelay;
        }
        initialised = true;
    }

    // Adds the stereo tail to outL/outR.
    void process (const float* in, float* outL, float* outR, int numSamples)
    {
        auto pdMask = (int) predelay.size() - 1;

        for (int s = 0; s < numSamples; ++s)
        {
            currentInputGain += (targetInputGain - currentInputGain) * smoothing;
            currentPredelay += (targetPredelay - currentPredelay) * smoothing;

            predelay[(size_t) predelayPos] = in[s];
            auto x = readInterp (predelay, predelayPos, pdMask, currentPredelay);
            predelayPos = (predelayPos + 1) & pdMask;

            for (auto& d : diffusers)
            {
                auto delayed = d.buffer[d.pos];
                auto v = x + diffusion * delayed;
                d.buffer[d.pos] = v;
                x = delayed - diffusion * v;
                d.pos = (d.pos + 1) % d.buffer.size();
            }

            std::array<float, numLines> raw {}, out {};
            for (size_t i = 0; i < lines.size(); ++i)
            {
                auto& line = lines[i];
                line.length += (line.targetLength - line.length) * smoothing;
                if (std::abs (line.targetLength - line.length) < 0.01f)
                    line.length = line.targetLength;
                auto lineMask = (int) line.buffer.size() - 1;
                raw[i] = readInterp (line.buffer, line.writePos, lineMask, line.length);
                line.lp += crossover * (raw[i] - line.lp);
                out[i] = line.feedback * line.lp + line.feedbackHigh * (raw[i] - line.lp);
            }

            outL[s] += 0.5f * (raw[0] - raw[2] + raw[4] - raw[6]);
            outR[s] += 0.5f * (raw[1] - raw[3] + raw[5] - raw[7]);

            hadamard (out);
            auto inGain = currentInputGain * x;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                auto& line = lines[i];
                line.buffer[(size_t) line.writePos] = out[i] + inGain * inputSigns[i];
                line.writePos = (line.writePos + 1) & ((int) line.buffer.size() - 1);
            }
        }
    }

private:
    struct Line { std::vector<float> buffer; int writePos = 0; float length = 0, targetLength = 0, feedback = 0, feedbackHigh = 0, lp = 0; };
    struct Diffuser { std::vector<float> buffer; size_t pos = 0; };

    static size_t nextPow2 (size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }

    // Reads `delay` samples behind the most recently written sample at writePos.
    static float readInterp (const std::vector<float>& buf, int writePos, int mask, float delay)
    {
        auto whole = (int) delay;
        auto frac = delay - (float) whole;
        auto i0 = writePos - whole;
        return buf[(size_t) (i0 & mask)] * (1.0f - frac) + buf[(size_t) ((i0 - 1) & mask)] * frac;
    }

    static void hadamard (std::array<float, numLines>& v)
    {
        for (size_t h = 1; h < numLines; h <<= 1)
            for (size_t i = 0; i < numLines; i += h * 2)
                for (size_t j = i; j < i + h; ++j)
                {
                    auto a = v[j], b = v[j + h];
                    v[j] = a + b;
                    v[j + h] = a - b;
                }
        for (auto& x : v)
            x *= 0.35355339f;   // 1 / sqrt(8)
    }

    static constexpr float maxSizeScale = 2.5f;
    static constexpr float diffusion = 0.6f;
    static constexpr float lowBandShare = 0.2f;
    static constexpr std::array<float, numLines> lineMs { 29.7f, 37.1f, 41.1f, 43.7f, 53.0f, 59.9f, 67.7f, 71.3f };
    static constexpr std::array<float, numLines> inputSigns { 1, -1, 1, 1, -1, 1, -1, -1 };
    static constexpr std::array<float, 4> diffuserMs { 4.77f, 3.59f, 12.73f, 9.31f };

    double sampleRate = 48000.0;
    std::array<Line, numLines> lines;
    std::array<Diffuser, 4> diffusers;
    std::vector<float> predelay;
    int predelayPos = 0;
    float targetInputGain = 0, currentInputGain = 0, targetPredelay = 0, currentPredelay = 0;
    float crossover = 0.3f, smoothing = 0;
    bool initialised = false;
};

} // namespace cimra
