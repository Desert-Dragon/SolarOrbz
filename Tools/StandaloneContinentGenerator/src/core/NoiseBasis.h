// Portable port of the UE plugin's SolarOrbzNoiseBasis namespace (SolarOrbzTerrainLayers.cpp) -
// same basis functions (Perlin/Ridged/Billow/Value/Voronoi), same fractal (fBm) accumulation
// convention. The one substitution: UE's FMath::PerlinNoise3D (Epic's internal implementation, not
// exposed outside the engine) is replaced with a standard public-domain improved Perlin noise
// (Ken Perlin, 2002) - same noise CHARACTER, not bit-identical output. See Random.h's own note:
// this tool does not aim for pixel-identical reproduction of the UE plugin, only the same
// algorithm shape.
#pragma once

#include "Vec3.h"
#include <cstdint>

enum class ENoiseType : uint8_t
{
    Perlin,
    Ridged,
    Billow,
    Value,
    Voronoi,
};

enum class EContinentAlgorithm : uint8_t
{
    RadialSeeds,
    VoronoiGrowth,
    PlateTectonics,
};

namespace NoiseBasis
{
    float PerlinNoise3D(const Vec3& Pos);
    float SampleBasis(ENoiseType Type, const Vec3& Pos, int32_t Seed);

    /** Multi-octave fBm, Perlin basis, normalized to roughly -1..1 regardless of octave count - matches the UE plugin's FractalNoise3D (coastline wobble). */
    float FractalNoise3D(const Vec3& Pos, int32_t Octaves, float Persistence, float Lacunarity, int32_t Seed);

    /** Multi-octave fBm using the SPECIFIED basis per octave (Ridged/Billow/Value/Voronoi included, not just Perlin) -
      * matches the UE plugin's USolarOrbzFractalNoiseTerrainLayerBase::ComputeNormalizedNoiseUncompensated, which is
      * what every noise-type layer in the real plugin actually uses. FractalNoise3D above stays Perlin-only since it
      * backs the coastline-wobble callers that intentionally always want Perlin regardless of a layer's own NoiseType. */
    float FractalNoiseGeneric(ENoiseType Type, const Vec3& Pos, int32_t Octaves, float Persistence, float Lacunarity, int32_t Seed);

    /** Cheap deterministic hash from an integer seed to a 3D offset, so different seeds don't look like the same field shifted by an obvious amount. */
    Vec3 ComputeSeedOffset(int32_t Seed);
}
