// Builds a small, FLAT local-tangent-plane mesh from a (local offset in meters) -> height-in-meters
// sampler - the "stand on the ground and look around" counterpart to SphereMesh.h's whole-planet
// preview. Exists because BuildSphereMesh (and the FHeightField bake grid behind it) is fundamentally
// a whole-planet equirectangular grid, at most a few hundred columns wide around an entire planet's
// circumference - many kilometers between samples at planet scale, nowhere near fine enough to
// represent a meter-scale noise layer's actual shape (it can only alias that layer into meaningless
// per-cell jitter). A ground patch sidesteps that by sampling a TINY area (tens of meters) at high
// resolution directly from a continuous height function instead of a baked grid - see
// NoiseNode::EvaluateAt's own comment for the other half of this.
//
// Deliberately flat, not curved to the planet's actual sphere: at a few tens of meters across, the
// sphere's own curvature (sagitta = HalfSize^2 / (2*RadiusMeters), e.g. 20^2/(2*400000) = 0.0005m for
// a 40m patch on a 400km-radius planet) is many orders of magnitude smaller than the terrain detail
// this exists to show - modeling it would add real complexity for a visual difference no viewer could
// ever actually see at this scale.
#pragma once

#include "Vec3.h"
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>

struct FGroundPatchVertex
{
    // Local meters, X/Z horizontal (matches this tool's Z-up convention rotated into a walking-eye
    // frame - see BuildGroundPatchMesh's own comment on the remap), Y = height above the patch's own
    // reference plane (0 at the patch center, same sign convention as every other HeightMeters value
    // in this tool).
    float X = 0.0f, Y = 0.0f, Z = 0.0f;
    Vec3 Normal = Vec3::Up();
};

struct FGroundPatchMesh
{
    std::vector<FGroundPatchVertex> Vertices; // row-major, Resolution*Resolution
    std::vector<uint32_t> Indices;
    int32_t Resolution = 0;
};

// FHeightSampler: float(float LocalXMeters, float LocalZMeters) -> height-in-meters relative to the
// patch center (0 at the center by construction - see BuildGroundPatchMesh's own callers, which
// always sample a DELTA from the center rather than an absolute elevation, so the whole patch sits
// near Y=0 regardless of the planet's actual elevation at this spot).
template <typename FHeightSampler>
inline FGroundPatchMesh BuildGroundPatchMesh(const FHeightSampler& SampleHeightMeters, float PatchSizeMeters, int32_t Resolution)
{
    FGroundPatchMesh Mesh;
    Mesh.Resolution = std::max(Resolution, 2);
    const int32_t Res = Mesh.Resolution;
    const int32_t NumVerts = Res * Res;
    Mesh.Vertices.resize(NumVerts);

    const float HalfSize = PatchSizeMeters * 0.5f;
    const float Step = PatchSizeMeters / (float)(Res - 1);

    std::vector<float> Heights(NumVerts);
    for (int32_t Row = 0; Row < Res; ++Row)
    {
        const float LocalZ = -HalfSize + Step * (float)Row;
        for (int32_t Col = 0; Col < Res; ++Col)
        {
            const float LocalX = -HalfSize + Step * (float)Col;
            const int32_t Idx = Row * Res + Col;
            const float H = SampleHeightMeters(LocalX, LocalZ);
            Heights[Idx] = H;
            Mesh.Vertices[Idx].X = LocalX;
            Mesh.Vertices[Idx].Y = H;
            Mesh.Vertices[Idx].Z = LocalZ;
        }
    }

    // Central-difference normals from the height samples already taken - same cheap approach
    // BuildSphereMesh takes (a radial approximation there; a flat-plane finite-difference here),
    // good enough for an unlit/vertex-lit preview.
    auto HeightAt = [&](int32_t Row, int32_t Col) -> float
    {
        Row = MathUtil::Clamp(Row, 0, Res - 1);
        Col = MathUtil::Clamp(Col, 0, Res - 1);
        return Heights[Row * Res + Col];
    };
    for (int32_t Row = 0; Row < Res; ++Row)
    {
        for (int32_t Col = 0; Col < Res; ++Col)
        {
            const float HLeft = HeightAt(Row, Col - 1);
            const float HRight = HeightAt(Row, Col + 1);
            const float HDown = HeightAt(Row - 1, Col);
            const float HUp = HeightAt(Row + 1, Col);
            const Vec3 TangentX(2.0 * Step, (double)(HRight - HLeft), 0.0);
            const Vec3 TangentZ(0.0, (double)(HUp - HDown), 2.0 * Step);
            Mesh.Vertices[Row * Res + Col].Normal = Vec3::Cross(TangentZ, TangentX).GetSafeNormal();
        }
    }

    Mesh.Indices.reserve((size_t)(Res - 1) * (Res - 1) * 6);
    for (int32_t Row = 0; Row < Res - 1; ++Row)
    {
        for (int32_t Col = 0; Col < Res - 1; ++Col)
        {
            const uint32_t I00 = (uint32_t)(Row * Res + Col);
            const uint32_t I10 = (uint32_t)(Row * Res + Col + 1);
            const uint32_t I01 = (uint32_t)((Row + 1) * Res + Col);
            const uint32_t I11 = (uint32_t)((Row + 1) * Res + Col + 1);
            // Winding matches SphereMesh.h's own CCW-from-outside convention, adapted to this patch's
            // Y-up local frame (outside = viewed from above, +Y).
            Mesh.Indices.push_back(I00); Mesh.Indices.push_back(I11); Mesh.Indices.push_back(I10);
            Mesh.Indices.push_back(I00); Mesh.Indices.push_back(I01); Mesh.Indices.push_back(I11);
        }
    }

    return Mesh;
}

// Builds an orthonormal (Tangent, Bitangent) basis perpendicular to CenterDir - lets a caller turn a
// small local (X, Z) meters offset into a real unit direction via the standard tangent-plane/gnomonic
// approximation: Direction = normalize(CenterDir + (X*Tangent + Z*Bitangent) / RadiusMeters). Exact
// at the origin, and accurate to several significant figures at the patch sizes this is built for
// (tens of meters against a planet-scale radius - see this file's own top comment on why curvature
// itself is negligible at this scale).
inline void ComputeTangentBasis(const Vec3& CenterDir, Vec3& OutTangent, Vec3& OutBitangent)
{
    // Any vector not parallel to CenterDir works as the seed; (0,0,1) fails only when CenterDir
    // itself is +-Z (the poles), so fall back to (1,0,0) there - same pole-handling shape this tool's
    // other direction-basis code (e.g. ContinentLayer's seed placement) already uses.
    const Vec3 Seed = (std::abs(CenterDir.Z) > 0.999) ? Vec3(1.0, 0.0, 0.0) : Vec3(0.0, 0.0, 1.0);
    OutTangent = Vec3::Cross(Seed, CenterDir).GetSafeNormal();
    OutBitangent = Vec3::Cross(CenterDir, OutTangent).GetSafeNormal();
}
