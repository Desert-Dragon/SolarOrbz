// Blends a secondary, independently-generated noise field into the running result - the one place
// this pipeline lets two generators combine with something other than plain addition. A true
// multi-port node graph would let any upstream node feed this as its second input; this scope-cut
// version owns its own secondary FNoiseNodeConfig instead, which covers the common case (blend in
// a second noise layer with a different blend mode) without the UI/data-model work of arbitrary
// wiring - see TerrainNode.h's header comment for why that's out of scope for this pass.
#pragma once

#include "../TerrainNode.h"
#include "../LatLongGrid.h"
#include "NoiseNode.h"
#include <algorithm>

enum class ECombineBlendMode : uint8_t
{
    Add,
    Subtract,
    Multiply, // multiplies the RUNNING field by (1 + secondary*Weight) - a relief-modulation effect
    Max,
    Min,
    Average,
};

struct FCombineConfig
{
    ECombineBlendMode Mode = ECombineBlendMode::Add;
    float Weight = 1.0f;
    FNoiseNodeConfig Secondary;
};

class CombineNode : public ITerrainNode
{
public:
    FCombineConfig Config;

    const char* TypeName() const override { return "Combine"; }

    void Apply(FHeightField& Field, const FPipelineContext& Ctx) override
    {
        const int32_t EffectiveSeed = Ctx.Seed + Config.Secondary.SeedOffset;
        const Vec3 SeedOffsetVec = NoiseBasis::ComputeSeedOffset(EffectiveSeed);
        const FLatLongGrid Grid(Field.Width, Field.Height);

        Grid.ForEachCell([&](int32_t Idx, const Vec3& Dir, double, double)
        {
            const Vec3 SamplePos = Dir * Config.Secondary.Frequency + SeedOffsetVec;
            float Secondary;
            if (Config.Secondary.Octaves > 1 && Config.Secondary.Type == ENoiseType::Perlin)
            {
                Secondary = NoiseBasis::FractalNoise3D(SamplePos, Config.Secondary.Octaves, Config.Secondary.Persistence, Config.Secondary.Lacunarity, EffectiveSeed);
            }
            else
            {
                Secondary = NoiseBasis::SampleBasis(Config.Secondary.Type, SamplePos, EffectiveSeed);
            }
            const float SecondaryMeters = Secondary * Config.Secondary.AmplitudeMeters * Config.Weight;

            float& H = Field.HeightMeters[Idx];
            switch (Config.Mode)
            {
            case ECombineBlendMode::Add: H += SecondaryMeters; break;
            case ECombineBlendMode::Subtract: H -= SecondaryMeters; break;
            case ECombineBlendMode::Multiply: H *= (1.0f + Secondary * Config.Weight); break;
            case ECombineBlendMode::Max: H = std::max(H, SecondaryMeters); break;
            case ECombineBlendMode::Min: H = std::min(H, SecondaryMeters); break;
            case ECombineBlendMode::Average: H = (H + SecondaryMeters) * 0.5f; break;
            }
        });
    }
};
