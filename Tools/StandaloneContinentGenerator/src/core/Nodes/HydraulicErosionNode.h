// Particle-based ("droplet") hydraulic erosion - the standard published technique this whole
// genre of terrain tool uses for realistic river/valley carving and sediment deposition (Hans
// Theobald Beyer, "Implementation of a method for hydraulic erosion", Bachelor's thesis, TU Munich,
// 2015; widely reimplemented in open tutorials since, e.g. Sebastian Lague's "Procedural Terrain
// Erosion"). This is an independent reimplementation of that public algorithm against this
// project's own FHeightField/FRandomStream - not reverse-engineered from any commercial tool's
// code.
//
// Each of NumDroplets starts at a random cell and flows downhill for up to MaxDropletLifetime
// steps, picking up sediment on steep descents and depositing it where the slope shallows or
// flattens out, carving realistic-looking drainage networks out of whatever base terrain (Continent
// + Noise) is already in the field. Erosion/deposition both happen in INDEX space (see HeightField.h's
// header comment on the planar-grid simplification) - X wraps around the sphere, a droplet that
// wanders past a pole (Y out of range) simply stops.
#pragma once

#include "../TerrainNode.h"
#include <cstdint>

struct FHydraulicErosionConfig
{
    // NumDroplets/ErosionStrengthMeters: unlike the textbook version of this algorithm (which runs
    // over a HEIGHT FIELD NORMALIZED TO 0-1 and applies a small separate "erosion strength" constant),
    // this field's heights are already real meters, so SedimentCapacity/ErodeAmount below are already
    // meters-scale on their own - ErosionStrengthMeters then scales that again. Verified via CLI
    // (comparing Ocean/Beach/Biome % against an unmodified bake, on both a thin ~200m RadialSeeds
    // default and a 4000m-mountain PlateTectonics bake): the old defaults (50000 droplets * 2.0)
    // compounded into a near-total wipeout of land on BOTH terrain types (thin land has no margin to
    // lose; the thick case still pushed Ocean from ~70% to ~95%). 2000 droplets * 1.0 strength leaves
    // a visible-but-minor change (a percent or two of Ocean%, new Beach cells from redeposited
    // sediment) on both instead.
    int32_t NumDroplets = 2000;
    int32_t MaxDropletLifetime = 30;
    float Inertia = 0.05f;              // 0 = always follow steepest descent; 1 = never turn
    float SedimentCapacityFactor = 4.0f;
    float MinSlope = 0.01f;             // floor under slope-driven capacity, so flat ground can still carry a little sediment
    float ErosionRate = 0.3f;
    float DepositionRate = 0.3f;
    float EvaporationRate = 0.01f;
    float Gravity = 4.0f;
    float InitialWaterVolume = 1.0f;
    float InitialSpeed = 1.0f;
    float ErosionStrengthMeters = 1.0f; // scales abstract sediment units into real meters of height change
    int32_t SeedOffset = 1000;          // offset from Ctx.Seed so erosion's own RNG stream doesn't alias Continent/Noise's
};

class HydraulicErosionNode : public ITerrainNode
{
public:
    FHydraulicErosionConfig Config;

    const char* TypeName() const override { return "HydraulicErosion"; }
    void Apply(FHeightField& Field, const FPipelineContext& Ctx) override;
};
