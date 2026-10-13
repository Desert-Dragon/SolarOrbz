// Talus-angle thermal erosion (Musgrave, Kolb & Mace, "The synthesis and rendering of eroded
// fractal terrains," SIGGRAPH 1989) - material slides from a cell to any neighbor whose slope
// exceeds the talus (repose) angle, redistributed proportional to how far each neighbor is past
// threshold, conserving total mass per step. Independent reimplementation of this long-public
// algorithm, not derived from any commercial tool.
#pragma once

#include "../TerrainNode.h"

struct FThermalErosionConfig
{
    int32_t Iterations = 20;
    float TalusAngleDegrees = 33.0f; // repose angle - matches the UE plugin's own Erosion Layer default
    float TalusErosionRate = 0.5f;   // fraction of the excess-above-threshold moved per iteration
    float CellSizeMeters = 100.0f;   // converts index-space height difference to a slope angle (see HeightField.h's planar-grid simplification)
};

class ThermalErosionNode : public ITerrainNode
{
public:
    FThermalErosionConfig Config;

    const char* TypeName() const override { return "ThermalErosion"; }
    void Apply(FHeightField& Field, const FPipelineContext& Ctx) override;
};
