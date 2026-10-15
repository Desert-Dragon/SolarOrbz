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

	// --- D8 (8-neighbor steepest-descent) flow routing - shared by Erosion Layer's hydraulic pass ---
	// and Hydrology Simulation's flow accumulation (SolarOrbzHydrologySimulation.h/.cpp). Erosion Layer
	// predates this and still keeps its own file-static copy in SolarOrbzTerrainLayers.cpp (identical
	// logic) rather than being migrated to call through here - not touched when these were added, to
	// avoid any risk to already-working code for a purely stylistic dedup with no compiler available to
	// verify the refactor. New flow-routing code should use these instead of adding a third copy.

	/** Wraps a longitude index around the seam; latitude has no wrap (poles are edges, not a loop). */
	static int32 WrapX(int32 X, int32 W) { return ((X % W) + W) % W; }

	static bool InBoundsY(int32 Y, int32 H) { return Y >= 0 && Y < H; }

	/** Finds the lowest of a cell's 8 neighbors. Returns INDEX_NONE if this cell is already a local minimum (a basin - nowhere lower to route to). */
	static int32 FindSteepestDescentNeighbor(const TArray<float>& HeightField, int32 X, int32 Y, int32 W, int32 H, float SelfHeight)
	{
		static constexpr int32 NeighborOffsetX[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
		static constexpr int32 NeighborOffsetY[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };

		int32 LowestIdx = INDEX_NONE;
		float LowestHeight = SelfHeight;

		for (int32 N = 0; N < 8; ++N)
		{
			const int32 NX = WrapX(X + NeighborOffsetX[N], W);
			const int32 NY = Y + NeighborOffsetY[N];
			if (!InBoundsY(NY, H))
			{
				continue;
			}

			const int32 NIdx = NY * W + NX;
			if (HeightField[NIdx] < LowestHeight)
			{
				LowestHeight = HeightField[NIdx];
				LowestIdx = NIdx;
			}
		}

		return LowestIdx;
	}
};
