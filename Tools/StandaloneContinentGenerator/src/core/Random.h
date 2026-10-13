// Seeded RNG, named/shaped to match UE's FRandomStream call sites in the ported Continent/Tectonics
// code (FRandRange/RandRange) so the port reads almost identically to the original - but this is
// std::mt19937 under the hood, NOT UE's actual FRandomStream algorithm. Deliberate, documented
// limitation: a given Seed will NOT reproduce the exact same layout as the UE plugin, only the same
// algorithm/statistical shape. Bit-for-bit parity wasn't worth reverse-engineering Epic's internal
// constants for a tool whose job is fast iteration/visual verification, not exact reproduction.
#pragma once

#include "Vec3.h"
#include <random>
#include <cstdint>

struct FRandomStream
{
    explicit FRandomStream(int32_t Seed) : Engine(static_cast<uint32_t>(Seed)) {}

    float FRand() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(Engine); }
    float FRandRange(float Min, float Max) { return std::uniform_real_distribution<float>(Min, Max)(Engine); }
    int32_t RandRange(int32_t Min, int32_t Max) { return std::uniform_int_distribution<int32_t>(Min, Max)(Engine); }

private:
    std::mt19937 Engine;
};

// Uniformly-distributed random point on the unit sphere - NOT naive random lat/long, which clusters
// at the poles. Same construction as the UE plugin's RandomPointOnUnitSphere.
inline Vec3 RandomPointOnUnitSphere(FRandomStream& Stream)
{
    const double Z = Stream.FRandRange(-1.0f, 1.0f);
    const double Theta = Stream.FRandRange(0.0f, 2.0f * (float)MathUtil::PI_D);
    const double R = std::sqrt(std::max(1.0 - Z * Z, 0.0));
    return Vec3(R * std::cos(Theta), R * std::sin(Theta), Z);
}
