// Additive fractal-noise layer - the standalone tool's equivalent of the UE plugin's Noise Layer
// (USolarOrbzFractalNoiseTerrainLayerBase family), scope-cut to the single Perlin/Ridged/Billow/
// Value/Voronoi basis + fBm accumulation already ported in NoiseBasis.h (no Warp, no per-octave
// masks - see this tool's README for the full list of layers intentionally left out of v1).
#pragma once

#include "../TerrainNode.h"
#include "../NoiseBasis.h"
#include "../LatLongGrid.h"

struct FNoiseNodeConfig
{
    ENoiseType Type = ENoiseType::Perlin;
    int32_t Octaves = 5;
    float Frequency = 2.0f;
    float Persistence = 0.5f;
    float Lacunarity = 2.0f;
    float AmplitudeMeters = 500.0f;
    int32_t SeedOffset = 0; // added to Ctx.Seed so stacking several NoiseNodes doesn't look identical
};

class NoiseNode : public ITerrainNode
{
public:
    FNoiseNodeConfig Config;

    const char* TypeName() const override { return "Noise"; }

    void Apply(FHeightField& Field, const FPipelineContext& Ctx) override
    {
        const int32_t EffectiveSeed = Ctx.Seed + Config.SeedOffset;
        const Vec3 SeedOffsetVec = NoiseBasis::ComputeSeedOffset(EffectiveSeed);
        const FLatLongGrid Grid(Field.Width, Field.Height);
        Grid.ForEachCell([&](int32_t Idx, const Vec3& Dir, double, double)
        {
            const Vec3 SamplePos = Dir * Config.Frequency + SeedOffsetVec;
            float N;
            if (Config.Octaves > 1 && Config.Type == ENoiseType::Perlin)
            {
                N = NoiseBasis::FractalNoise3D(SamplePos, Config.Octaves, Config.Persistence, Config.Lacunarity, EffectiveSeed);
            }
            else
            {
                N = NoiseBasis::SampleBasis(Config.Type, SamplePos, EffectiveSeed);
            }
            Field.HeightMeters[Idx] += N * Config.AmplitudeMeters;
        });
    }
};
