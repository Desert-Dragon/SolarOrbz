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

    // Smooth-ish per-vertex normals from the two neighboring edges actually in the mesh (longitude
    // wraps, latitude does not - matching FLatLongGrid/GetGridNeighbors4's own convention).
    for (int32_t Y = 0; Y < Mesh.Height; ++Y)
    {
        for (int32_t X = 0; X < Mesh.Width; ++X)
        {
            const int32_t Idx = Y * Mesh.Width + X;
            const int32_t XNext = (X + 1) % Mesh.Width;
            const int32_t YNext = std::min(Y + 1, Mesh.Height - 1);
            if (Y == YNext) continue; // bottom row - keep the radial-approximation normal

            const Vec3& P = Mesh.Vertices[Idx].Position;
            const Vec3& PX = Mesh.Vertices[Y * Mesh.Width + XNext].Position;
            const Vec3& PY = Mesh.Vertices[YNext * Mesh.Width + X].Position;
            // Cross(PY-P, PX-P), not Cross(PX-P, PY-P): this struct's own Vec3 axes don't match
            // raylib's (see main.cpp's X/Z/Y repacking when writing to Mesh.normals) - that repacking
            // is itself orientation-reversing (swapping two axes has a -1 determinant), so a normal
            // that's outward-facing in THIS struct's axes still ends up outward after repacking, but
            // only because this operand order already compensates for it; cross(PX-P,PY-P) here would
            // point outward in this struct's own axes yet land inward once repacked into raylib's -
            // verified numerically (every non-degenerate vertex normal checked against its own outward
            // position direction, before vs. after repacking).
            const Vec3 Normal = Vec3::Cross(PY - P, PX - P).GetSafeNormal();
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
