// Offline checks of the room model: timing, decay time, tail level and determinism.

#include "RoomDSP.h"
#include <cstdio>
#include <random>

using namespace cimra;

static int failures = 0;

static void check (bool ok, const char* what, double got, double expected)
{
    std::printf ("%s %-48s got %9.4f  expected %9.4f\n", ok ? "PASS" : "FAIL", what, got, expected);
    failures += ok ? 0 : 1;
}

static constexpr double sr = 48000.0;

static size_t peakIndex (const std::vector<float>& v)
{
    size_t best = 0;
    for (size_t i = 0; i < v.size(); ++i)
        if (std::abs (v[i]) > std::abs (v[best]))
            best = i;
    return best;
}

// Schroeder backward integration, T20 fit extrapolated to 60 dB.
static double measureRt60 (const std::vector<float>& ir)
{
    std::vector<double> edc (ir.size());
    double sum = 0;
    for (size_t i = ir.size(); i-- > 0;)
        edc[i] = (sum += (double) ir[i] * ir[i]);
    auto db = [&] (size_t i) { return 10.0 * std::log10 (edc[i] / edc[0]); };
    size_t i5 = 0, i25 = 0;
    while (i5 < ir.size() && db (i5) > -5.0) ++i5;
    i25 = i5;
    while (i25 < ir.size() && db (i25) > -25.0) ++i25;
    return 3.0 * (double) (i25 - i5) / sr;
}

int main()
{
    RoomSettings room;   // 8 x 10 x 3.5 m, walls 0.5
    Vec3 src { 4.0f, 6.0f, 1.2f };
    const int n = (int) (sr * 4);

    // Direct sound arrives after distance / c.
    {
        SourceEngine engine;
        engine.prepare (sr, 0.6f);
        engine.setTargets (room, src, 0.0f);
        std::vector<float> in ((size_t) n, 0.0f), dl ((size_t) n), dr ((size_t) n), el ((size_t) n), er ((size_t) n);
        in[0] = 1.0f;
        engine.process (in.data(), dl.data(), dr.data(), el.data(), er.data(), n);
        auto expected = room.directDistance (src) / kSpeedOfSound * sr;
        check (std::abs ((double) peakIndex (dl) - expected) < 3.0, "direct path delay (samples)", (double) peakIndex (dl), expected);

        double erEnergy = 0;
        for (auto x : el) erEnergy += (double) x * x;
        check (erEnergy > 0.0 && std::isfinite (erEnergy), "early reflections present and finite", erEnergy, erEnergy);
    }

    // Removing the direct delay puts the direct sound at time zero.
    {
        SourceEngine engine;
        engine.prepare (sr, 0.6f);
        auto offset = room.directDistance (src) / kSpeedOfSound * (float) sr;
        engine.setTargets (room, src, offset);
        std::vector<float> in ((size_t) n, 0.0f), dl ((size_t) n), dr ((size_t) n), el ((size_t) n), er ((size_t) n);
        in[0] = 1.0f;
        engine.process (in.data(), dl.data(), dr.data(), el.data(), er.data(), n);
        check (peakIndex (dl) < 3, "time-aligned direct path (samples)", (double) peakIndex (dl), 0.0);
    }

    // Decay time of the tail follows Sabine for several rooms.
    for (float walls : { 0.2f, 0.5f, 0.85f })
    {
        auto r = room;
        r.walls = walls;
        LateReverb reverb;
        reverb.prepare (sr);
        reverb.setParams (r, 1.0f, 0.0f);
        std::vector<float> in ((size_t) n, 0.0f), l ((size_t) n), rr ((size_t) n);
        in[0] = 1.0f;
        reverb.process (in.data(), l.data(), rr.data(), n);
        // RT60 is quoted at mid frequencies; highs are meant to die faster.
        float lp = 0, a = onePoleCoeff (1000.0f, sr);
        for (auto& x : l) x = (lp += a * (x - lp));
        auto measured = measureRt60 (l);
        char label[64];
        std::snprintf (label, sizeof label, "RT60 (s), walls %.2f", walls);
        check (std::abs (measured / r.rt60() - 1.0) < 0.15, label, measured, r.rt60());
    }

    // Tail level for steady noise matches the requested input gain (within 1 dB).
    {
        LateReverb reverb;
        reverb.prepare (sr);
        reverb.setParams (room, 0.5f, 0.0f);
        std::mt19937 rng (1);
        std::normal_distribution<float> noise;
        std::vector<float> in ((size_t) n), l ((size_t) n), r ((size_t) n);
        for (auto& x : in) x = noise (rng);
        reverb.process (in.data(), l.data(), r.data(), n);
        double e = 0;
        for (size_t i = (size_t) n / 2; i < (size_t) n; ++i) e += (double) l[i] * l[i];
        auto rms = std::sqrt (e / (n / 2));
        check (std::abs (20.0 * std::log10 (rms / 0.5)) < 1.0, "tail RMS for unit noise", rms, 0.5);
    }

    // Two instances fed the same signal produce identical tails (so summing tracks = one shared room).
    {
        LateReverb a, b;
        a.prepare (sr);
        b.prepare (sr);
        a.setParams (room, 1.0f, 100.0f);
        b.setParams (room, 1.0f, 100.0f);
        std::vector<float> in ((size_t) 4800, 0.0f), al (4800), ar (4800), bl (4800), br (4800);
        in[0] = 1.0f;
        a.process (in.data(), al.data(), ar.data(), 4800);
        b.process (in.data(), bl.data(), br.data(), 4800);
        double diff = 0;
        for (size_t i = 0; i < al.size(); ++i) diff += std::abs (al[i] - bl[i]);
        check (diff == 0.0, "instances produce identical tails", diff, 0.0);
    }

    // Further away = less direct sound, same diffuse field.
    {
        auto nearGain = room.directGain ({ 4.0f, 3.0f, 1.2f });
        auto farGain = room.directGain ({ 4.0f, 9.0f, 1.2f });
        check (farGain < nearGain * 0.4f, "direct gain falls with distance", farGain / nearGain, 0.2);
    }

    std::printf (failures == 0 ? "\nAll tests passed\n" : "\n%d test(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
