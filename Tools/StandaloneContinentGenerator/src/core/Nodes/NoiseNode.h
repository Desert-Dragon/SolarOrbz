// Additive fractal-noise layer - the standalone tool's equivalent of the UE plugin's Noise Layer
// (USolarOrbzFractalNoiseTerrainLayerBase family), scope-cut to the single Perlin/Ridged/Billow/
// Value/Voronoi basis + fBm accumulation already ported in NoiseBasis.h (no Warp, no per-octave
// masks - see this tool's README for the full list of layers intentionally left out of v1).
#pragma once

#include "../TerrainNode.h"
#include "../NoiseBasis.h"
#include "../LatLongGrid.h"
#include <algorithm>

struct FNoiseNodeConfig
{
    ENoiseType Type = ENoiseType::Perlin;
    int32_t Octaves = 5;
    float Frequency = 2.0f;
    float Persistence = 0.5f;
    float Lacunarity = 2.0f;
    float AmplitudeMeters = 500.0f;
    int32_t SeedOffset = 0; // added to Ctx.Seed so stacking several NoiseNodes doesn't look identical

    // Alternative to Frequency, authored directly in real-world meters - this field's own wavelength
    // IS the actual ground distance between successive peaks/valleys of this layer's base octave,
    // not a dimensionless "cycles around the sphere" count. Frequency is "roughly cycles per trip
    // around the sphere" (SamplePos = Dir * Frequency, and Dir is a unit vector), so its real-world
    // wavelength is RadiusMeters / Frequency - reaching a 3m wavelength on an Earth-scale planet
    // needs Frequency ~2,126,000, not a number anyone would reach by sliding a 0.5-20 range control.
    // Exists specifically so ground-scale detail (the walking-around-on-the-surface case the
    // DetailNoiseNode below is built for) is something a designer can dial in directly, mirroring the
    // UE plugin's USolarOrbzFractalNoiseTerrainLayerBase::WavelengthMeters fix exactly (same
    // derivation, same reasoning - see that field's own header comment).
    bool bWavelengthInMeters = false;
    float WavelengthMeters = 4.0f;
};

class NoiseNode : public ITerrainNode
{
public:
    FNoiseNodeConfig Config;

    const char* TypeName() const override { return "Noise"; }

    void Apply(FHeightField& Field, const FPipelineContext& Ctx) override
    {
        const FLatLongGrid Grid(Field.Width, Field.Height);
        Grid.ForEachCell([&](int32_t Idx, const Vec3& Dir, double, double)
        {
            Field.HeightMeters[Idx] += EvaluateAt(Dir, Ctx);
        });
    }

    // Continuous (not grid-rasterized) height contribution from this one layer at an arbitrary
    // direction - what Apply() above calls per grid cell, but also directly usable by a caller that
    // needs this layer's real shape at a resolution finer than any whole-planet bake grid could
    // practically reach (GroundPatchMesh.h's ground-level preview is the reason this got pulled out
    // into its own method: a 3m-wavelength layer baked onto even this tool's largest bake grid, 512
    // columns around a planet-sized circumference, is many kilometers between samples - nowhere near
    // Nyquist for a 3m feature, so the baked grid can only ever alias it into meaningless per-cell
    // jitter, never its actual shape. Evaluating it continuously, right at the handful of points a
    // small ground patch actually needs, is the only way that shape becomes visible at all).
    float EvaluateAt(const Vec3& Dir, const FPipelineContext& Ctx) const
    {
        const int32_t EffectiveSeed = Ctx.Seed + Config.SeedOffset;
        const Vec3 SeedOffsetVec = NoiseBasis::ComputeSeedOffset(EffectiveSeed);
        const float EffectiveFrequency = Config.bWavelengthInMeters
            ? (float)(Ctx.RadiusMeters / std::max((double)Config.WavelengthMeters, 1.0e-4))
            : Config.Frequency;
        const Vec3 SamplePos = Dir * EffectiveFrequency + SeedOffsetVec;
        // Generalized fBm (same basis every octave, matching the UE plugin's
        // ComputeNormalizedNoiseUncompensated) - NOT the Perlin-only FractalNoise3D, which exists
        // for the coastline-wobble callers that intentionally always want Perlin regardless of a
        // layer's own NoiseType. Using that one here left Ridged/Billow/Value/Voronoi layers stuck
        // at a single raw octave no matter what Octaves was set to - a single octave of Ridged
        // noise (sharp creases, no multi-scale detail to break them up) is exactly what rendered as
        // a blocky, grid-aliased lattice instead of layered mountain ridges.
        const float N = NoiseBasis::FractalNoiseGeneric(Config.Type, SamplePos, Config.Octaves, Config.Persistence, Config.Lacunarity, EffectiveSeed);
        return N * Config.AmplitudeMeters;
    }
};
