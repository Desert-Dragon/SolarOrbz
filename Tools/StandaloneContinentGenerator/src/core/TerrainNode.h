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
