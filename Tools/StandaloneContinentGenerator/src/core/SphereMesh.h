// Builds a displayable triangle mesh from any direction->height-in-meters sampler (a ContinentLayer,
// a baked TerrainPipeline's FHeightField, whatever) walked over a FLatLongGrid - a deliberate v1
// scope cut: this is NOT a port of the UE plugin's icosphere subdivision/UV-seam/pentagon-pole-
// handling (FSolarOrbzIcoSphereGenerator). A plain lat/long UV-sphere is far simpler to build and
// is good enough for a fast-iteration preview tool; it pinches at the poles (every vertex at Y=0 or
// Y=Height-1 collapses to the same point) same as any naive UV-sphere. Used only by the GUI
// frontend - the CLI samples its sources directly for its PNG output, no mesh involved.
#pragma once

#include "Vec3.h"
#include "LatLongGrid.h"
#include <vector>
#include <cstdint>
#include <cmath>

struct FSphereMeshVertex
{
    Vec3 Position = Vec3::Zero();
    Vec3 Normal = Vec3::Up();
    float HeightMeters = 0.0f;
};

struct FSphereMesh
{
    std::vector<FSphereMeshVertex> Vertices; // row-major, Width*Height, same indexing as FLatLongGrid
    std::vector<uint32_t> Indices; // triangle list
    int32_t Width = 0;
    int32_t Height = 0;
};

template <typename FHeightSampler>
inline FSphereMesh BuildSphereMesh(const FHeightSampler& SampleHeightMeters, int32_t GridWidth, int32_t GridHeight, double RadiusMeters, double HeightExaggeration = 1.0)
{
    FSphereMesh Mesh;
    Mesh.Width = std::max(GridWidth, 2);
    Mesh.Height = std::max(GridHeight, 2);
    const FLatLongGrid Grid(Mesh.Width, Mesh.Height);
    const int32_t NumCells = Mesh.Width * Mesh.Height;

    // Height Exaggeration is user-controlled and unbounded in practice (and even at 1x, Earth-scale
    // Ocean Floor Depth/Mountain Height values can exceed a small preview radius) - without a floor,
    // a large enough negative displacement pushes the radius through zero and INVERTS that vertex to
    // the opposite side of the sphere, producing a self-intersecting mess instead of a dented sphere.
    // Clamping to a small positive fraction of RadiusMeters keeps every vertex on the correct side.
    const double MinRadius = RadiusMeters * 0.05;

    Mesh.Vertices.resize(NumCells);
    Grid.ForEachCell([&](int32_t Idx, const Vec3& Dir, double, double)
    {
        const float HeightMeters = SampleHeightMeters(Dir);
        FSphereMeshVertex& V = Mesh.Vertices[Idx];
        V.HeightMeters = HeightMeters;
        const double DisplacedRadius = std::max(RadiusMeters + HeightMeters * HeightExaggeration, MinRadius);
        V.Position = Dir * DisplacedRadius;
        V.Normal = Dir; // radial approximation - cheap, accepted for a fast-iteration preview tool
    });

    // FHeightField/FLatLongGrid's own documented trade-off is "distorted cell size near poles,
    // correct topology" - index-space neighbors stay index-space neighbors, but their REAL physical
    // separation shrinks toward zero as a row approaches either pole (circumference ~ sin(Polar), not
    // constant). A per-cell height source with no awareness of that distortion - HydraulicErosionNode
    // most of all, which operates directly in index space - produces perfectly ordinary-sized height
    // differences between adjacent cells there, but "ordinary" relative to an INDEX-space neighbor
    // distance that no longer matches the real one. Once radially displaced, that ordinary difference
    // becomes an enormous LOCAL SLOPE exactly where cells are nearly coincident, rendering as a
    // "sunburst" of needle spikes converging on the pole (confirmed via screenshot - present after
    // running Hydraulic Erosion, clean before it). Originally fixed by flattening only the exact pole
    // row (Y=0/Height-1) to a single average height - insufficient: the row(s) just next to the pole
    // are still compressed enough, while not literally coincident, to show the same needling (also
    // confirmed via screenshot, same repro, after that first fix). Generalized here into a smoothing
    // pass across every row within PoleSmoothingSinThreshold of either pole, blending each cell's
    // height toward its own row's average by a weight that scales with how compressed that row
    // actually is (sin(Polar) - the same quantity driving the real distortion) rather than a hard
    // cutoff at a single row, so there's no sharp boundary where the effect just stops. Purely a
    // mesh/visualization smoothing - the underlying FHeightField data (CLI/biome/climate) is untouched.
    constexpr double PoleSmoothingSinThreshold = 0.25; // ~14.5 degrees of polar angle from either pole
    for (int32_t Y = 0; Y < Mesh.Height; ++Y)
    {
        const double Polar = ((double)Y / (double)(Mesh.Height - 1)) * MathUtil::PI_D;
        const double SinPolar = std::sin(Polar);
        if (SinPolar >= PoleSmoothingSinThreshold) continue; // far enough from both poles - untouched

        const double BlendWeight = MathUtil::Clamp(1.0 - SinPolar / PoleSmoothingSinThreshold, 0.0, 1.0);
        double HeightSum = 0.0;
        for (int32_t X = 0; X < Mesh.Width; ++X) HeightSum += Mesh.Vertices[Y * Mesh.Width + X].HeightMeters;
        const float RowAverageHeight = (float)(HeightSum / Mesh.Width);

        for (int32_t X = 0; X < Mesh.Width; ++X)
        {
            FSphereMeshVertex& V = Mesh.Vertices[Y * Mesh.Width + X];
            const float BlendedHeight = (float)(V.HeightMeters * (1.0 - BlendWeight) + RowAverageHeight * BlendWeight);
            const double DisplacedRadius = std::max(RadiusMeters + BlendedHeight * HeightExaggeration, MinRadius);
            const Vec3 Dir = V.Position.GetSafeNormal();
            V.HeightMeters = BlendedHeight;
            V.Position = Dir * DisplacedRadius;
        }
    }

    // Smooth-ish per-vertex normals from the edges actually in the mesh (longitude wraps, latitude
    // does not - matching FLatLongGrid/GetGridNeighbors4's own convention).
    //
    // CENTERED differences (both neighbors on each axis), not one-sided forward differences (only
    // X+1/Y+1, the original approach here) - a forward-only normal at vertex P is blind to the slope
    // on P's OWN trailing side (toward X-1/Y-1), so it only ever sees half of the local curvature.
    // That's invisible on gently-varying terrain, but on anything with sharp, high-frequency local
    // slope changes - Ridged noise's abs()-folded creases stacked on Plate Tectonics' own mountain-
    // range steps, specifically - adjacent vertices end up with systematically DIFFERENT one-sided
    // normals depending on which side of a crease they're forward-looking across, and that bias
    // repeats every row/column because it's tied to the grid axes themselves: it renders as a
    // regular, grid-aligned light/dark plaid pattern over the terrain (confirmed via screenshot on a
    // Plate Tectonics landmass with Ridged noise - persisted unchanged even after fixing a real but
    // unrelated bug, NoiseNode/CombineNode silently dropping multi-octave accumulation for every
    // non-Perlin basis; a small Radial Seeds island at the same noise settings showed clean ridgelines,
    // no plaid, since its gentler base terrain doesn't expose the one-sided bias). Averaging in the
    // trailing neighbor too cancels that per-axis bias: the normal now reflects the FULL local slope
    // instead of half of it, matching how every other normal-from-heightfield implementation does it.
    for (int32_t Y = 0; Y < Mesh.Height; ++Y)
    {
        for (int32_t X = 0; X < Mesh.Width; ++X)
        {
            const int32_t Idx = Y * Mesh.Width + X;
            const int32_t XPrev = (X - 1 + Mesh.Width) % Mesh.Width;
            const int32_t XNext = (X + 1) % Mesh.Width;
            const int32_t YPrev = std::max(Y - 1, 0);
            const int32_t YNext = std::min(Y + 1, Mesh.Height - 1);
            if (YPrev == YNext) continue; // degenerate single-row mesh - keep the radial-approximation normal

            const Vec3& PXPrev = Mesh.Vertices[Y * Mesh.Width + XPrev].Position;
            const Vec3& PXNext = Mesh.Vertices[Y * Mesh.Width + XNext].Position;
            const Vec3& PYPrev = Mesh.Vertices[YPrev * Mesh.Width + X].Position;
            const Vec3& PYNext = Mesh.Vertices[YNext * Mesh.Width + X].Position;
            // Same TangentY-then-TangentX operand order as the original forward-diff version (see its
            // own long-since-verified comment on why that order, not the reverse, lands outward-facing
            // once main.cpp's X/Z/Y repacking is applied) - only the tangents themselves are now
            // centered spans instead of single forward edges.
            const Vec3 TangentY = PYNext - PYPrev;
            const Vec3 TangentX = PXNext - PXPrev;
            const Vec3 Normal = Vec3::Cross(TangentY, TangentX).GetSafeNormal();
            if (!Normal.IsNearlyZero())
            {
                Mesh.Vertices[Idx].Normal = Normal;
            }
        }
    }

    Mesh.Indices.reserve((size_t)(Mesh.Width) * (Mesh.Height - 1) * 6);
    for (int32_t Y = 0; Y < Mesh.Height - 1; ++Y)
    {
        for (int32_t X = 0; X < Mesh.Width; ++X)
        {
            const int32_t XNext = (X + 1) % Mesh.Width;
            const uint32_t I00 = (uint32_t)(Y * Mesh.Width + X);
            const uint32_t I10 = (uint32_t)(Y * Mesh.Width + XNext);
            const uint32_t I01 = (uint32_t)((Y + 1) * Mesh.Width + X);
            const uint32_t I11 = (uint32_t)((Y + 1) * Mesh.Width + XNext);

            // Reversed from the "obvious" (I00,I10,I11)/(I00,I11,I01) order - verified empirically
            // against raylib's front-face convention (see src/gui/main.cpp's CreatePreviewModel,
            // which duplicates this same topology into its own static GPU index buffer): this order
            // is what actually renders as the visible outer surface once backface culling is enabled.
            Mesh.Indices.push_back(I00); Mesh.Indices.push_back(I11); Mesh.Indices.push_back(I10);
            Mesh.Indices.push_back(I00); Mesh.Indices.push_back(I01); Mesh.Indices.push_back(I11);
        }
    }

    return Mesh;
}
