// Transforms whatever height is already accumulated - "adjust the base layer" - rather than adding
// a new one. Order of operations matches the obvious authoring intent: scale the shape first, bias
// it (shift sea level up/down), reshape the result with a sign-preserving power curve (push
// mid-range values toward flat plains or toward sharp peaks/trenches without moving 0 or the
// extremes), then hard-clamp as a safety rail.
#pragma once

#include "../TerrainNode.h"
#include <cmath>

struct FHeightAdjustConfig
{
    float ScaleMultiplier = 1.0f;
    float OffsetMeters = 0.0f;
    float PowerCurve = 1.0f; // 1.0 = no change; >1 flattens mid-range (plateaus+sharp peaks); <1 roughens mid-range
    float CurveReferenceMeters = 4000.0f; // the |height| the power curve normalizes against before re-scaling back
    float ClampMinMeters = -20000.0f;
    float ClampMaxMeters = 20000.0f;
};

class HeightAdjustNode : public ITerrainNode
{
public:
    FHeightAdjustConfig Config;

    const char* TypeName() const override { return "HeightAdjust"; }

    void Apply(FHeightField& Field, const FPipelineContext&) override
    {
        const float Ref = std::max(Config.CurveReferenceMeters, 1.0f);
        for (float& H : Field.HeightMeters)
        {
            float V = H * Config.ScaleMultiplier + Config.OffsetMeters;
            if (Config.PowerCurve != 1.0f)
            {
                const float Sign = V < 0.0f ? -1.0f : 1.0f;
                const float Normalized = std::min(std::fabs(V) / Ref, 1.0f);
                V = Sign * std::pow(Normalized, Config.PowerCurve) * Ref;
            }
            H = MathUtil::Clamp(V, Config.ClampMinMeters, Config.ClampMaxMeters);
        }
    }
};
