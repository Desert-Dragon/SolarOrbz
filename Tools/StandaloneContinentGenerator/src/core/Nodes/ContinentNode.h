// Wraps the existing ContinentLayer (all 3 algorithms + Plate Tectonics) as a pipeline node -
// typically the first node, since it's additive onto a Field that starts at 0.
#pragma once

#include "../TerrainNode.h"
#include "../ContinentLayer.h"
#include "../LatLongGrid.h"

class ContinentNode : public ITerrainNode
{
public:
    FContinentConfig Config;

    // Exposed so the GUI's "Run Simulation" button can drive the same persistent plate state the
    // standalone tool already had before the pipeline existed.
    ContinentLayer Layer;

    const char* TypeName() const override { return "Continent"; }

    void Apply(FHeightField& Field, const FPipelineContext& Ctx) override
    {
        Layer.Config = Config;
        Layer.Config.Seed = Ctx.Seed;
        Layer.Bake();

        const FLatLongGrid Grid(Field.Width, Field.Height);
        Grid.ForEachCell([&](int32_t Idx, const Vec3& Dir, double, double)
        {
            Field.HeightMeters[Idx] += Layer.GetRawHeightMeters(Dir);
        });
    }
};
