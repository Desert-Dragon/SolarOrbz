#include "NoiseBasis.h"
#include <algorithm>
#include <cmath>

namespace
{
    // Ken Perlin's 2002 "improved noise" reference permutation table, doubled to avoid wrapping.
    const int Permutation[256] = {
        151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,
        190, 6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,88,237,149,56,87,174,
        20,125,136,171,168, 68,175,74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,
        230,220,105,92,41,55,46,245,40,244,102,143,54, 65,25,63,161, 1,216,80,73,209,76,132,187,208, 89,
        18,169,200,196,135,130,116,188,159,86,164,100,109,198,173,186, 3,64,52,217,226,250,124,123,5,202,
        38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,119,248,152,
        2,44,154,163, 70,221,153,101,155,167, 43,172,9,129,22,39,253, 19,98,108,110,79,113,224,232,178,185,
        112,104,218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241, 81,51,145,235,249,14,239,107,
        49,192,214, 31,181,199,106,157,184, 84,204,176,215,61,220, 70,21,36,221,41,214,139,207
    };
    int Perm[512];
    struct FPermInit
    {
        FPermInit()
        {
            for (int i = 0; i < 512; ++i) Perm[i] = Permutation[i % 256];
        }
    } GPermInit;

    double Fade6(double T) { return T * T * T * (T * (T * 6.0 - 15.0) + 10.0); }
    double LerpD(double A, double B, double T) { return A + T * (B - A); }

    double Grad(int Hash, double X, double Y, double Z)
    {
        const int H = Hash & 15;
        const double U = H < 8 ? X : Y;
        const double V = H < 4 ? Y : (H == 12 || H == 14 ? X : Z);
        return ((H & 1) == 0 ? U : -U) + ((H & 2) == 0 ? V : -V);
    }

    // Deterministic integer hash -> 0..1, plain lattice-value hash (Bob Jenkins-style mix) - used by
    // Value/Voronoi noise, not related to Perlin's own gradient hashing above.
    float Hash3D(int32_t X, int32_t Y, int32_t Z, int32_t Seed)
    {
        uint32_t H = (uint32_t)(X * 374761393 + Y * 668265263 + Z * 2147483647 + Seed * 3266489917);
        H = (H ^ (H >> 13)) * 1274126177u;
        H = H ^ (H >> 16);
        return (float)(H & 0xFFFFFF) / (float)0xFFFFFF;
    }

    // Quintic fade curve, same shape Perlin noise itself uses.
    float FadeF(float T) { return T * T * T * (T * (T * 6.0f - 15.0f) + 10.0f); }

    float ValueNoise3D(const Vec3& Pos, int32_t Seed)
    {
        const int32_t X0 = (int32_t)std::floor(Pos.X), Y0 = (int32_t)std::floor(Pos.Y), Z0 = (int32_t)std::floor(Pos.Z);
        const int32_t X1 = X0 + 1, Y1 = Y0 + 1, Z1 = Z0 + 1;

        const float Tx = FadeF((float)(Pos.X - X0));
        const float Ty = FadeF((float)(Pos.Y - Y0));
        const float Tz = FadeF((float)(Pos.Z - Z0));

        const float C000 = Hash3D(X0, Y0, Z0, Seed), C100 = Hash3D(X1, Y0, Z0, Seed);
        const float C010 = Hash3D(X0, Y1, Z0, Seed), C110 = Hash3D(X1, Y1, Z0, Seed);
        const float C001 = Hash3D(X0, Y0, Z1, Seed), C101 = Hash3D(X1, Y0, Z1, Seed);
        const float C011 = Hash3D(X0, Y1, Z1, Seed), C111 = Hash3D(X1, Y1, Z1, Seed);

        const float X00 = C000 + Tx * (C100 - C000);
        const float X10 = C010 + Tx * (C110 - C010);
        const float X01 = C001 + Tx * (C101 - C001);
        const float X11 = C011 + Tx * (C111 - C011);
        const float Y0v = X00 + Ty * (X10 - X00);
        const float Y1v = X01 + Ty * (X11 - X01);
        const float Result01 = Y0v + Tz * (Y1v - Y0v);

        return Result01 * 2.0f - 1.0f;
    }

    float CellularNoise3D(const Vec3& Pos, int32_t Seed)
    {
        const int32_t CX = (int32_t)std::floor(Pos.X), CY = (int32_t)std::floor(Pos.Y), CZ = (int32_t)std::floor(Pos.Z);
        float MinDistSq = 1e30f;

        for (int32_t OffZ = -1; OffZ <= 1; ++OffZ)
        {
            for (int32_t OffY = -1; OffY <= 1; ++OffY)
            {
                for (int32_t OffX = -1; OffX <= 1; ++OffX)
                {
                    const int32_t CellX = CX + OffX, CellY = CY + OffY, CellZ = CZ + OffZ;
                    const float FX = CellX + Hash3D(CellX, CellY, CellZ, Seed);
                    const float FY = CellY + Hash3D(CellX, CellY, CellZ, Seed + 101);
                    const float FZ = CellZ + Hash3D(CellX, CellY, CellZ, Seed + 202);

                    const float DX = (float)(Pos.X - FX), DY = (float)(Pos.Y - FY), DZ = (float)(Pos.Z - FZ);
                    const float DistSq = DX * DX + DY * DY + DZ * DZ;
                    MinDistSq = std::min(MinDistSq, DistSq);
                }
            }
        }

        const float Dist = std::sqrt(MinDistSq);
        return std::clamp(Dist * 1.2f, 0.0f, 1.0f) * 2.0f - 1.0f;
    }
}

namespace NoiseBasis
{
    float PerlinNoise3D(const Vec3& Pos)
    {
        int X = (int)std::floor(Pos.X) & 255;
        int Y = (int)std::floor(Pos.Y) & 255;
        int Z = (int)std::floor(Pos.Z) & 255;

        const double Xf = Pos.X - std::floor(Pos.X);
        const double Yf = Pos.Y - std::floor(Pos.Y);
        const double Zf = Pos.Z - std::floor(Pos.Z);

        const double U = Fade6(Xf), V = Fade6(Yf), W = Fade6(Zf);

        const int A = Perm[X] + Y, AA = Perm[A] + Z, AB = Perm[A + 1] + Z;
        const int B = Perm[X + 1] + Y, BA = Perm[B] + Z, BB = Perm[B + 1] + Z;

        const double Result = LerpD(
            LerpD(LerpD(Grad(Perm[AA], Xf, Yf, Zf), Grad(Perm[BA], Xf - 1, Yf, Zf), U),
                  LerpD(Grad(Perm[AB], Xf, Yf - 1, Zf), Grad(Perm[BB], Xf - 1, Yf - 1, Zf), U), V),
            LerpD(LerpD(Grad(Perm[AA + 1], Xf, Yf, Zf - 1), Grad(Perm[BA + 1], Xf - 1, Yf, Zf - 1), U),
                  LerpD(Grad(Perm[AB + 1], Xf, Yf - 1, Zf - 1), Grad(Perm[BB + 1], Xf - 1, Yf - 1, Zf - 1), U), V),
            W);

        // Improved Perlin noise's raw range is roughly -1..1 already (never quite reaching +-1) - no
        // further rescale needed, matching how the UE plugin treats FMath::PerlinNoise3D's result.
        return (float)Result;
    }

    float SampleBasis(ENoiseType Type, const Vec3& Pos, int32_t Seed)
    {
        switch (Type)
        {
        case ENoiseType::Ridged:
        {
            const float N = PerlinNoise3D(Pos);
            float Ridge = 1.0f - std::abs(N);
            Ridge *= Ridge;
            return Ridge * 2.0f - 1.0f;
        }
        case ENoiseType::Billow:
        {
            const float N = PerlinNoise3D(Pos);
            return std::abs(N) * 2.0f - 1.0f;
        }
        case ENoiseType::Value:
            return ValueNoise3D(Pos, Seed);
        case ENoiseType::Voronoi:
            return CellularNoise3D(Pos, Seed);
        case ENoiseType::Perlin:
        default:
            return PerlinNoise3D(Pos);
        }
    }

    float FractalNoise3D(const Vec3& Pos, int32_t Octaves, float Persistence, float Lacunarity, int32_t Seed)
    {
        float Sum = 0.0f;
        float MaxPossible = 0.0f;
        float OctaveAmplitude = 1.0f;
        Vec3 OctavePos = Pos;
        for (int32_t Octave = 0; Octave < std::max(Octaves, 1); ++Octave)
        {
            Sum += SampleBasis(ENoiseType::Perlin, OctavePos, Seed) * OctaveAmplitude;
            MaxPossible += OctaveAmplitude;
            OctaveAmplitude *= Persistence;
            OctavePos *= Lacunarity;
        }
        return MaxPossible > 1e-6f ? std::clamp(Sum / MaxPossible, -1.0f, 1.0f) : 0.0f;
    }

    Vec3 ComputeSeedOffset(int32_t Seed)
    {
        auto Frac = [](double V) { return V - std::floor(V); };
        return Vec3(
            Frac(std::sin((double)Seed * 12.9898) * 43758.5453) * 1000.0,
            Frac(std::sin((double)Seed * 78.233) * 43758.5453) * 1000.0,
            Frac(std::sin((double)Seed * 37.719) * 43758.5453) * 1000.0);
    }
}
