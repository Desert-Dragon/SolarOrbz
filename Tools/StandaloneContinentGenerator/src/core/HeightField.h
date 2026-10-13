// A planar (equirectangular) grid of heights in meters, index-addressed (X,Y) rather than by
// direction - the shared working representation every pipeline node (noise, continent, erosion,
// adjust) reads and writes in place. FLatLongGrid still owns the direction<->cell mapping used to
// seed/sample this grid from the rest of the core library (ContinentLayer, NoiseBasis).
//
// Longitude (X) wraps around the sphere; latitude (Y) does not - same convention as
// FLatLongGrid/GetGridNeighbors4. Erosion and climate simulation both walk this grid in plain
// index space (treating each cell as a unit square) rather than true great-circle distances - a
// deliberate, documented simplification (matching this codebase's existing "accepted pole
// pinching" stance on the lat/long mesh): correct cell-adjacency topology, distorted cell-size
// near the poles. Good enough for a fast-iteration preview tool, not a physically exact model.
#pragma once

#include "Vec3.h"
#include "LatLongGrid.h"
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cmath>

struct FHeightField
{
    int32_t Width = 0;
    int32_t Height = 0;
    std::vector<float> HeightMeters;

    FHeightField() = default;
    FHeightField(int32_t InWidth, int32_t InHeight, float InitialValue = 0.0f)
        : Width(std::max(InWidth, 2)), Height(std::max(InHeight, 2))
        , HeightMeters((size_t)Width * (size_t)Height, InitialValue)
    {
    }

    int32_t WrapX(int32_t X) const { return ((X % Width) + Width) % Width; }
    int32_t ClampY(int32_t Y) const { return (int32_t)MathUtil::Clamp(Y, 0, Height - 1); }

    float& At(int32_t X, int32_t Y) { return HeightMeters[(size_t)ClampY(Y) * Width + WrapX(X)]; }
    float At(int32_t X, int32_t Y) const { return HeightMeters[(size_t)ClampY(Y) * Width + WrapX(X)]; }

    // Bilinear sample at fractional grid coordinates (not direction) - X wraps, Y clamps.
    float SampleBilinear(double Gx, double Gy) const
    {
        double IntPart;
        const double FxFrac = std::modf(Gx, &IntPart);
        const double Fx = FxFrac < 0.0 ? FxFrac + 1.0 : FxFrac;
        const int32_t X0 = WrapX((int32_t)std::floor(Gx));
        const int32_t X1 = WrapX(X0 + 1);
        const double Gy_c = MathUtil::Clamp(Gy, 0.0, (double)(Height - 1));
        const int32_t Y0 = ClampY((int32_t)std::floor(Gy_c));
        const int32_t Y1 = ClampY(Y0 + 1);
        const double Fy = Gy_c - std::floor(Gy_c);

        const double A = MathUtil::Lerp(At(X0, Y0), At(X1, Y0), Fx);
        const double B = MathUtil::Lerp(At(X0, Y1), At(X1, Y1), Fx);
        return (float)MathUtil::Lerp(A, B, Fy);
    }

    // Convenience: sample via the shared FLatLongGrid direction mapping (for nodes that generate
    // height from a 3D direction, e.g. Continent/Noise).
    float SampleDirection(const FLatLongGrid& Grid, const Vec3& Dir) const
    {
        return Grid.SampleBilinear(HeightMeters, Dir);
    }

    // Central-difference gradient at an integer cell, in meters per grid cell (X wraps, Y clamps).
    void Gradient(int32_t X, int32_t Y, float& OutDx, float& OutDy) const
    {
        OutDx = (At(X + 1, Y) - At(X - 1, Y)) * 0.5f;
        OutDy = (At(X, Y + 1) - At(X, Y - 1)) * 0.5f;
    }

    template <typename FVisitor>
    void ForEachCell(FVisitor&& Visitor)
    {
        for (int32_t Y = 0; Y < Height; ++Y)
        {
            for (int32_t X = 0; X < Width; ++X)
            {
                Visitor(X, Y, HeightMeters[(size_t)Y * Width + X]);
            }
        }
    }

    template <typename FVisitor>
    void ForEachCell(FVisitor&& Visitor) const
    {
        for (int32_t Y = 0; Y < Height; ++Y)
        {
            for (int32_t X = 0; X < Width; ++X)
            {
                Visitor(X, Y, HeightMeters[(size_t)Y * Width + X]);
            }
        }
    }
};

// Latitude in degrees for a grid row - +90 at Y=0 (north pole) to -90 at Y=Height-1 (south pole),
// matching FLatLongGrid::CellToDirection's own V=Y/(Height-1) convention.
inline double RowLatitudeDegrees(int32_t Y, int32_t Height)
{
    const double V = (double)Y / (double)std::max(Height - 1, 1);
    return 90.0 - V * 180.0;
}
