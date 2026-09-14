// SolarOrbz - shared equirectangular (lat/long) grid helpers. Walking every cell as a
// UnitDirection+UV pair, and longitude-wrapping/latitude-clamping bilinear sampling of a
// Width*Height row-major float array, was independently reimplemented (each with its own private
// PI_D constant) across Erosion Layer, Terrace Layer, Continent Layer, and Climate Simulation's
// Bake()/GetRawHeight()/Simulate() methods. This header centralizes that logic so future layers
// don't need a sixth copy.
//
// Convention matches FSolarOrbzIcoSphereGenerator::ComputeUV throughout: longitude wraps around Z
// (up in UE); latitude runs from +Z (north pole, V=0) to -Z (south pole, V=1).

#pragma once

#include "CoreMinimal.h"

struct FSolarOrbzLatLongGrid
{
	// Using an explicit double constant rather than the engine's PI macro, same rationale as
	// FSolarOrbzIcoSphereGenerator's own copy - avoids precision loss now that FVector components
	// are double (LWC) by default. Single canonical copy; every caller that used to keep its own
	// private PI_D should use this one instead.
	static constexpr double PI_D = 3.14159265358979323846;

	int32 Width = 1;
	int32 Height = 1;

	FSolarOrbzLatLongGrid(int32 InWidth, int32 InHeight)
		: Width(FMath::Max(InWidth, 1))
		, Height(FMath::Max(InHeight, 1))
	{
	}

	/** UnitDirection + mesh-convention UV for grid cell (X,Y). Matches FSolarOrbzIcoSphereGenerator::ComputeUV exactly. */
	void CellToDirectionAndUV(int32 X, int32 Y, FVector& OutUnitDirection, FVector2D& OutUV) const
	{
		const double V = (double)Y / (double)FMath::Max(Height - 1, 1); // 0 north pole .. 1 south pole
		const double Polar = V * PI_D;
		const double Z = FMath::Cos(Polar);
		const double SinPolar = FMath::Sin(Polar);

		const double U = (double)X / (double)Width; // wraps - no -1, Width steps tile exactly around
		const double Azimuth = (U - 0.5) * 2.0 * PI_D;

		OutUnitDirection = FVector(
			(float)(SinPolar * FMath::Cos(Azimuth)),
			(float)(SinPolar * FMath::Sin(Azimuth)),
			(float)Z);
		OutUV = FVector2D((float)U, (float)V);
	}

	/** Iterates every (X,Y) cell as Idx = Y*Width+X, calling Visitor(Idx, UnitDirection, UV) - the "walk the whole equirectangular grid" loop every Bake()/Simulate() reimplemented separately. */
	template <typename FVisitor>
	void ForEachCell(FVisitor&& Visitor) const
	{
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				FVector Dir;
				FVector2D UV;
				CellToDirectionAndUV(X, Y, Dir, UV);
				Visitor(Y * Width + X, Dir, UV);
			}
		}
	}

	struct FBilinearCell
	{
		int32 X0 = 0, X1 = 0, Y0 = 0, Y1 = 0;
		float Tx = 0.0f, Ty = 0.0f;
	};

	/** Longitude-wrapping, latitude-clamping bilinear cell lookup for UnitDirection against this grid's Width/Height - computed once, reusable across multiple parallel arrays (e.g. Temperature + Moisture) sampled at the same point. */
	FBilinearCell ComputeBilinearCell(const FVector& UnitDirection) const
	{
		// Same convention as FSolarOrbzIcoSphereGenerator::ComputeUV.
		const double Azimuth = FMath::Atan2((double)UnitDirection.Y, (double)UnitDirection.X); // -PI .. PI
		const double U = 0.5 + Azimuth / (2.0 * PI_D);
		const double Polar = FMath::Acos(FMath::Clamp((double)UnitDirection.Z, -1.0, 1.0)); // 0 .. PI
		const double V = Polar / PI_D;

		const double Fx = FMath::Frac(U) * (double)Width;
		const double Fy = FMath::Clamp(V, 0.0, 1.0) * (double)(Height - 1);

		FBilinearCell Cell;
		Cell.X0 = FMath::FloorToInt(Fx) % Width;
		Cell.X1 = (Cell.X0 + 1) % Width;
		Cell.Y0 = FMath::Clamp(FMath::FloorToInt(Fy), 0, Height - 1);
		Cell.Y1 = FMath::Clamp(Cell.Y0 + 1, 0, Height - 1);
		Cell.Tx = (float)(Fx - FMath::FloorToDouble(Fx));
		Cell.Ty = (float)(Fy - FMath::FloorToDouble(Fy));
		return Cell;
	}

	static float SampleAtCell(const TArray<float>& Grid, int32 GridWidth, const FBilinearCell& Cell)
	{
		const float A = FMath::Lerp(Grid[Cell.Y0 * GridWidth + Cell.X0], Grid[Cell.Y0 * GridWidth + Cell.X1], Cell.Tx);
		const float B = FMath::Lerp(Grid[Cell.Y1 * GridWidth + Cell.X0], Grid[Cell.Y1 * GridWidth + Cell.X1], Cell.Tx);
		return FMath::Lerp(A, B, Cell.Ty);
	}

	/** Convenience one-shot bilinear sample of a single row-major Width*Height float array. */
	float SampleBilinear(const TArray<float>& Grid, const FVector& UnitDirection) const
	{
		return SampleAtCell(Grid, Width, ComputeBilinearCell(UnitDirection));
	}
};
