// Portable port of the UE plugin's FSolarOrbzLatLongGrid (SolarOrbzLatLongGrid.h) - equirectangular
// grid walking + longitude-wrapping/latitude-clamping bilinear sampling. Same cell<->direction
// convention: longitude wraps around Z (up); latitude runs from +Z (north pole, V=0) to -Z (south
// pole, V=1).
#pragma once

#include "Vec3.h"
#include <vector>
#include <cstdint>

struct FLatLongGrid
{
    int32_t Width = 1;
    int32_t Height = 1;

    FLatLongGrid(int32_t InWidth, int32_t InHeight)
        : Width(std::max(InWidth, 1)), Height(std::max(InHeight, 1))
    {
    }

    void CellToDirection(int32_t X, int32_t Y, Vec3& OutUnitDirection, double& OutU, double& OutV) const
    {
        const double V = (double)Y / (double)std::max(Height - 1, 1);
        const double Polar = V * MathUtil::PI_D;
        const double Z = std::cos(Polar);
        const double SinPolar = std::sin(Polar);

        const double U = (double)X / (double)Width;
        const double Azimuth = (U - 0.5) * 2.0 * MathUtil::PI_D;

        OutUnitDirection = Vec3(SinPolar * std::cos(Azimuth), SinPolar * std::sin(Azimuth), Z);
        OutU = U;
        OutV = V;
    }

    template <typename FVisitor>
    void ForEachCell(FVisitor&& Visitor) const
    {
        for (int32_t Y = 0; Y < Height; ++Y)
        {
            for (int32_t X = 0; X < Width; ++X)
            {
                Vec3 Dir; double U, V;
                CellToDirection(X, Y, Dir, U, V);
                Visitor(Y * Width + X, Dir, U, V);
            }
        }
    }

    struct FBilinearCell
    {
        int32_t X0 = 0, X1 = 0, Y0 = 0, Y1 = 0;
        double Tx = 0.0, Ty = 0.0;
    };

    FBilinearCell ComputeBilinearCell(const Vec3& UnitDirection) const
    {
        const double Azimuth = std::atan2(UnitDirection.Y, UnitDirection.X);
        const double U = 0.5 + Azimuth / (2.0 * MathUtil::PI_D);
        const double Polar = std::acos(MathUtil::Clamp(UnitDirection.Z, -1.0, 1.0));
        const double V = Polar / MathUtil::PI_D;

        double IntPart;
        const double Fx = std::modf(U, &IntPart) * (double)Width; // Frac(U) * Width - modf gives a signed fractional part
        const double FxWrapped = Fx < 0.0 ? Fx + Width : Fx;
        const double Fy = MathUtil::Clamp(V, 0.0, 1.0) * (double)(Height - 1);

        FBilinearCell Cell;
        Cell.X0 = ((int32_t)std::floor(FxWrapped)) % Width;
        if (Cell.X0 < 0) Cell.X0 += Width;
        Cell.X1 = (Cell.X0 + 1) % Width;
        Cell.Y0 = (int32_t)MathUtil::Clamp((double)std::floor(Fy), 0.0, (double)(Height - 1));
        Cell.Y1 = (int32_t)MathUtil::Clamp((double)(Cell.Y0 + 1), 0.0, (double)(Height - 1));
        Cell.Tx = FxWrapped - std::floor(FxWrapped);
        Cell.Ty = Fy - std::floor(Fy);
        return Cell;
    }

    static float SampleAtCell(const std::vector<float>& Grid, int32_t GridWidth, const FBilinearCell& Cell)
    {
        const double A = MathUtil::Lerp(Grid[Cell.Y0 * GridWidth + Cell.X0], Grid[Cell.Y0 * GridWidth + Cell.X1], Cell.Tx);
        const double B = MathUtil::Lerp(Grid[Cell.Y1 * GridWidth + Cell.X0], Grid[Cell.Y1 * GridWidth + Cell.X1], Cell.Tx);
        return (float)MathUtil::Lerp(A, B, Cell.Ty);
    }

    float SampleBilinear(const std::vector<float>& Grid, const Vec3& UnitDirection) const
    {
        return SampleAtCell(Grid, Width, ComputeBilinearCell(UnitDirection));
    }
};

/** 4-connected neighbor cell indices for Idx on a GridW x GridH FLatLongGrid - wraps longitude (X), clamps latitude (Y). */
inline void GetGridNeighbors4(int32_t Idx, int32_t GridW, int32_t GridH, int32_t (&OutNeighbors)[4])
{
    const int32_t X = Idx % GridW;
    const int32_t Y = Idx / GridW;
    OutNeighbors[0] = Y * GridW + (X + 1) % GridW;
    OutNeighbors[1] = Y * GridW + (X - 1 + GridW) % GridW;
    OutNeighbors[2] = (int32_t)MathUtil::Clamp(Y + 1, 0, GridH - 1) * GridW + X;
    OutNeighbors[3] = (int32_t)MathUtil::Clamp(Y - 1, 0, GridH - 1) * GridW + X;
}

/**
 * Inverse of FLatLongGrid::CellToDirection's own convention (not a member since it needs no Width/
 * Height - it's exact for any point, not just a grid cell), for features placed by explicit
 * coordinate rather than grown from a seed (e.g. ShapeStampNode) - LatitudeDegrees: +90 at the
 * north pole (+Z) .. -90 at the south pole; LongitudeDegrees: matches CellToDirection's own
 * Azimuth = (U-0.5)*2*PI, so 0 deg sits at U=0.5.
 */
inline Vec3 DirectionFromLatLongDegrees(double LatitudeDegrees, double LongitudeDegrees)
{
    const double Polar = MathUtil::DegreesToRadians(90.0 - LatitudeDegrees);
    const double Azimuth = MathUtil::DegreesToRadians(LongitudeDegrees);
    const double Z = std::cos(Polar);
    const double SinPolar = std::sin(Polar);
    return Vec3(SinPolar * std::cos(Azimuth), SinPolar * std::sin(Azimuth), Z);
}

/** Nearest grid cell index for an arbitrary direction - good enough for picking a growth/plate seed's starting cell. */
inline int32_t DirectionToNearestCellIdx(const FLatLongGrid& Grid, const Vec3& Dir)
{
    const FLatLongGrid::FBilinearCell Cell = Grid.ComputeBilinearCell(Dir);
    const int32_t X = Cell.Tx < 0.5 ? Cell.X0 : Cell.X1;
    const int32_t Y = Cell.Ty < 0.5 ? Cell.Y0 : Cell.Y1;
    return Y * Grid.Width + X;
}
