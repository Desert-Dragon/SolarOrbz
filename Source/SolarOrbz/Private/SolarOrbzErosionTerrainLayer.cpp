// SolarOrbz - Erosion terrain layer implementation.

#include "SolarOrbzErosionTerrainLayer.h"

DEFINE_LOG_CATEGORY_STATIC(LogSolarOrbzErosion, Log, All);

namespace SolarOrbzErosion
{
	// Same rationale as SolarOrbzIcoSphereGenerator.cpp / SolarOrbzClimateSimulation.cpp: an explicit
	// double constant rather than the engine's PI macro, since FVector components are double (LWC).
	static constexpr double PI_D = 3.14159265358979323846;

	// 8-neighbor (D8) offsets on the (X = longitude, Y = latitude) grid.
	static constexpr int32 NeighborOffsetX[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
	static constexpr int32 NeighborOffsetY[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };

	// Wraps a longitude index around the seam; latitude has no wrap (poles are edges, not a loop).
	static int32 WrapX(int32 X, int32 W) { return ((X % W) + W) % W; }
	static bool InBoundsY(int32 Y, int32 H) { return Y >= 0 && Y < H; }

	/** Finds the lowest of a cell's 8 neighbors. Returns INDEX_NONE if this cell is already a local minimum. */
	static int32 FindLowestNeighbor(const TArray<float>& Height, int32 X, int32 Y, int32 W, int32 H, float SelfHeight)
	{
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
			if (Height[NIdx] < LowestHeight)
			{
				LowestHeight = Height[NIdx];
				LowestIdx = NIdx;
			}
		}

		return LowestIdx;
	}
}

void USolarOrbzErosionTerrainLayer::Bake(const TFunctionRef<float(const FVector& UnitDirection, const FVector2D& UV)>& PriorLayersHeight, float RadiusCm)
{
	using namespace SolarOrbzErosion;

	const int32 W = FMath::Max(GridWidth, 8);
	const int32 H = FMath::Max(GridHeight, 4);

	BakedWidth = W;
	BakedHeight = H;

	TArray<float> Height;
	Height.SetNumUninitialized(W * H);
	TArray<float> OriginalHeight;
	OriginalHeight.SetNumUninitialized(W * H);

	// --- Seed the bake grid from every layer below this one in the stack (see USolarOrbzTerrainLayerStack::PrepareLayers). ---
	for (int32 Y = 0; Y < H; ++Y)
	{
		const double V = (double)Y / (double)FMath::Max(H - 1, 1); // 0 north pole .. 1 south pole
		const double Polar = V * PI_D;
		const double Z = FMath::Cos(Polar);
		const double SinPolar = FMath::Sin(Polar);

		for (int32 X = 0; X < W; ++X)
		{
			const double U = (double)X / (double)W; // wraps - no -1, W steps tile exactly around
			const double Azimuth = (U - 0.5) * 2.0 * PI_D;

			const FVector UnitDirection(
				(float)(SinPolar * FMath::Cos(Azimuth)),
				(float)(SinPolar * FMath::Sin(Azimuth)),
				(float)Z);
			const FVector2D UV((float)U, (float)V);

			const int32 Idx = Y * W + X;
			const float H0 = PriorLayersHeight(UnitDirection, UV);
			Height[Idx] = H0;
			OriginalHeight[Idx] = H0;
		}
	}

	// Approximate physical spacing between adjacent grid cells at the equator - a uniform stand-in
	// for talus/slope thresholds everywhere on the grid (see the pole-accuracy note at the top of the header).
	const float CellSpacingCm = (float)((2.0 * PI_D * (double)FMath::Max(RadiusCm, 1.0f)) / (double)W);

	// --- Thermal erosion: material above the talus angle slides toward its lowest neighbor. ---
	if (bEnableThermalErosion && ThermalIterations > 0)
	{
		const float TalusHeightPerCell = FMath::Tan(FMath::DegreesToRadians(TalusAngleDegrees)) * CellSpacingCm;

		TArray<float> Delta;
		for (int32 Iter = 0; Iter < ThermalIterations; ++Iter)
		{
			// Accumulated into a separate buffer and applied after the full pass, so the order cells
			// happen to be visited in doesn't bias which direction material moves this iteration.
			Delta.Init(0.0f, W * H);

			for (int32 Y = 0; Y < H; ++Y)
			{
				for (int32 X = 0; X < W; ++X)
				{
					const int32 Idx = Y * W + X;
					const float SelfHeight = Height[Idx];

					const int32 LowestIdx = FindLowestNeighbor(Height, X, Y, W, H, SelfHeight);
					if (LowestIdx == INDEX_NONE)
					{
						continue;
					}

					const float Diff = SelfHeight - Height[LowestIdx];
					if (Diff > TalusHeightPerCell)
					{
						const float Transfer = ThermalStrength * (Diff - TalusHeightPerCell) * 0.5f;
						Delta[Idx] -= Transfer;
						Delta[LowestIdx] += Transfer;
					}
				}
			}

			for (int32 Idx = 0; Idx < Height.Num(); ++Idx)
			{
				Height[Idx] += Delta[Idx];
			}
		}
	}

	// --- Hydraulic erosion: water routed downhill (steepest descent) each pass, carving where flow ---
	// is strong and depositing where it stalls.
	if (bEnableHydraulicErosion && HydraulicIterations > 0)
	{
		TArray<float> Water;
		TArray<float> Sediment;
		TArray<int32> SortedIndices;
		SortedIndices.SetNumUninitialized(W * H);

		for (int32 Iter = 0; Iter < HydraulicIterations; ++Iter)
		{
			Water.Init(RainfallAmount, W * H);
			Sediment.Init(0.0f, W * H);

			for (int32 i = 0; i < SortedIndices.Num(); ++i)
			{
				SortedIndices[i] = i;
			}
			// Highest first - by the time a cell is processed, every higher neighbor that could flow
			// into it this pass already has, so water/sediment only ever moves downhill in one pass.
			SortedIndices.Sort([&Height](int32 A, int32 B) { return Height[A] > Height[B]; });

			for (const int32 Idx : SortedIndices)
			{
				const int32 X = Idx % W;
				const int32 Y = Idx / W;
				const float SelfHeight = Height[Idx];

				const int32 LowestIdx = FindLowestNeighbor(Height, X, Y, W, H, SelfHeight);

				const float FlowWater = Water[Idx];
				float FlowSediment = Sediment[Idx];

				if (LowestIdx == INDEX_NONE)
				{
					// Local basin - nowhere lower to go. Drop everything being carried here.
					Height[Idx] += FlowSediment;
					continue;
				}

				const float Slope = FMath::Max((SelfHeight - Height[LowestIdx]) / CellSpacingCm, 0.0f);
				const float Capacity = FlowWater * Slope * ErosionRate;

				if (FlowSediment < Capacity)
				{
					// Room to carry more - carve material out of this cell and pick it up. Never carve
					// past the neighbor's height, so a single step can't invert the slope it's carving along.
					const float Carve = FMath::Min(Capacity - FlowSediment, SelfHeight - Height[LowestIdx]);
					Height[Idx] -= Carve;
					FlowSediment += Carve;
				}
				else
				{
					// Overloaded - drop the excess here.
					const float Deposit = (FlowSediment - Capacity) * DepositionRate;
					Height[Idx] += Deposit;
					FlowSediment -= Deposit;
				}

				Water[LowestIdx] += FlowWater;
				Sediment[LowestIdx] += FlowSediment;
			}
		}
	}

	// --- Store the net change, not the absolute height - GetRawHeight returns a DELTA that gets ---
	// added on top of the same layers it eroded, via the stack's normal blend logic.
	BakedDeltaHeightCm.SetNumUninitialized(W * H);
	float MinDelta = TNumericLimits<float>::Max(), MaxDelta = TNumericLimits<float>::Lowest();
	for (int32 Idx = 0; Idx < Height.Num(); ++Idx)
	{
		const float Delta = Height[Idx] - OriginalHeight[Idx];
		BakedDeltaHeightCm[Idx] = Delta;
		MinDelta = FMath::Min(MinDelta, Delta);
		MaxDelta = FMath::Max(MaxDelta, Delta);
	}

	UE_LOG(LogSolarOrbzErosion, Log,
		TEXT("SolarOrbz Erosion: baked %dx%d grid (Thermal=%s x%d, Hydraulic=%s x%d) - delta height ranges %.1fcm (deposited) .. %.1fcm (carved: %.1fcm)"),
		W, H,
		bEnableThermalErosion ? TEXT("on") : TEXT("off"), ThermalIterations,
		bEnableHydraulicErosion ? TEXT("on") : TEXT("off"), HydraulicIterations,
		MaxDelta, MinDelta, -MinDelta);

	if (FMath::IsNearlyEqual(MinDelta, MaxDelta))
	{
		UE_LOG(LogSolarOrbzErosion, Warning,
			TEXT("SolarOrbz Erosion: delta height is completely flat (%.3fcm everywhere) - either both passes are disabled, iterations are 0, or the terrain below this layer has no elevation variation for erosion to act on."),
			MinDelta);
	}
}

float USolarOrbzErosionTerrainLayer::GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const
{
	using namespace SolarOrbzErosion;

	if (BakedWidth <= 0 || BakedHeight <= 0 || BakedDeltaHeightCm.Num() != BakedWidth * BakedHeight)
	{
		return 0.0f; // Not baked yet (e.g. layer just added and RegenerateMesh hasn't run) - contribute nothing rather than garbage.
	}

	// Same convention as FSolarOrbzIcoSphereGenerator::ComputeUV / FSolarOrbzClimateGrid::Sample -
	// derived directly from UnitDirection rather than trusting the caller's UV, so this is robust to
	// any seam-fixing quirks the mesh's own UVs might have near the poles/seam.
	const double Azimuth = FMath::Atan2((double)UnitDirection.Y, (double)UnitDirection.X);
	const double U = 0.5 + Azimuth / (2.0 * PI_D);
	const double Polar = FMath::Acos(FMath::Clamp((double)UnitDirection.Z, -1.0, 1.0));
	const double V = Polar / PI_D;

	const double Fx = FMath::Frac(U) * (double)BakedWidth;
	const double Fy = FMath::Clamp(V, 0.0, 1.0) * (double)(BakedHeight - 1);

	const int32 X0 = FMath::FloorToInt(Fx) % BakedWidth;
	const int32 X1 = (X0 + 1) % BakedWidth;
	const int32 Y0 = FMath::Clamp(FMath::FloorToInt(Fy), 0, BakedHeight - 1);
	const int32 Y1 = FMath::Clamp(Y0 + 1, 0, BakedHeight - 1);

	const float Tx = (float)(Fx - FMath::FloorToDouble(Fx));
	const float Ty = (float)(Fy - FMath::FloorToDouble(Fy));

	const float A = FMath::Lerp(BakedDeltaHeightCm[Y0 * BakedWidth + X0], BakedDeltaHeightCm[Y0 * BakedWidth + X1], Tx);
	const float B = FMath::Lerp(BakedDeltaHeightCm[Y1 * BakedWidth + X0], BakedDeltaHeightCm[Y1 * BakedWidth + X1], Tx);
	return FMath::Lerp(A, B, Ty);
}
