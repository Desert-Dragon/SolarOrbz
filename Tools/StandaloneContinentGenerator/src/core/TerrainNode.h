// A small, ordered pipeline of terrain nodes operating on one shared FHeightField - deliberately
// NOT a full arbitrary-topology visual node graph (multiple input/output ports, branching,
// reconvergent paths). That's a real UI project of its own; this matches the UE plugin's own
// actual model instead, which the Terrain Graph Editor is itself careful to describe as just a
// transient VIEW over (SolarOrbzTerrainGraphEditor.md): a linear ordered stack where each layer
// reads the accumulated result of everything above it and adds to or transforms it. "Richer
// layering model" here means more node TYPES and a documented way for one node (Combine) to blend
// in a second procedurally-generated field, not a rewrite into a dataflow graph.
#pragma once

#include "HeightField.h"
#include <cstdint>
#include <memory>
#include <vector>

// Shared context every node's Apply() can read - the seed is intentionally singular/global (not
// per-node) so re-running the whole pipeline with a different Seed reshuffles everything
// consistently, matching how Config.Seed already works for Continent/Noise elsewhere.
struct FPipelineContext
{
    int32_t Seed = 0;

    // The actual planet radius, meters - every OTHER height value in this pipeline (Continent's
    // OceanFloorDepthMeters, Noise's AmplitudeMeters, etc.) is already a real, absolute meters value,
    // independent of radius; this field exists purely so a node's internal noise-sampling FREQUENCY
    // (cycles per trip around the unit sphere its direction vectors live on) can be converted to/from
    // a real-world WAVELENGTH, the same way it needs to be convertible in the UE plugin's own
    // USolarOrbzFractalNoiseTerrainLayerBase::TargetQuadEdgeMeters work (see that class's
    // WavelengthMeters field for the full derivation this mirrors: wavelength = RadiusMeters /
    // Frequency). Set by the caller (GUI from Viz.PreviewRadiusMeters, CLI from --radius-meters)
    // before Bake() - defaults to Earth's own mean radius so a node reading this before any caller
    // sets it still gets a sane answer rather than 0.
    double RadiusMeters = 6378137.0;
};

class ITerrainNode
{
public:
    virtual ~ITerrainNode() = default;
    virtual const char* TypeName() const = 0;

    // Mutates Field in place: additive nodes (Continent, Noise) add their own contribution onto
    // whatever is already there; transform nodes (HeightAdjust, erosion) reshape what's there.
    virtual void Apply(FHeightField& Field, const FPipelineContext& Ctx) = 0;
};

// Ordered list of nodes sharing one grid resolution - the whole pipeline's "Bake".
class TerrainPipeline
{
public:
    int32_t GridWidth = 256;
    int32_t GridHeight = 128;
    FPipelineContext Context;
    std::vector<std::unique_ptr<ITerrainNode>> Nodes;

    FHeightField Bake() const
    {
        FHeightField Field(GridWidth, GridHeight, 0.0f);
        for (const std::unique_ptr<ITerrainNode>& Node : Nodes)
        {
            Node->Apply(Field, Context);
        }
        return Field;
    }
};
