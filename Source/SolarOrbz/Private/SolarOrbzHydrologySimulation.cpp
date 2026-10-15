// SolarOrbz - Hydrology simulation implementation.

#include "SolarOrbzHydrologySimulation.h"
#include "SolarOrbzClimateSimulation.h"
#include "SolarOrbzLatLongGrid.h"
#include "SolarOrbzTerrainLayers.h"

DEFINE_LOG_CATEGORY_STATIC(LogSolarOrbzHydrology, Log, All);

float FSolarOrbzHydrologyGrid::Sample(const FVector& UnitDirection) const
{
	if (!IsValid())
	{
		return 0.0f;
	}

	const FSolarOrbzLatLongGrid Grid(Width, Height);
	return Grid.SampleBilinear(FlowAccumulation01, UnitDirection);
}

void USolarOrbzHydrologySimulationAsset::Simulate(const USolarOrbzTerrainLayerStack* TerrainStack, double RadiusCm, const FSolarOrbzClimateGrid* ClimateGridForRainfall, FSolarOrbzHydrologyGrid& OutGrid) const
{
	// Unlike Climate Simulation/Erosion, pure D8 flow ROUTING only needs relative height ordering and
	// grid adjacency - no real physical cell spacing - so RadiusCm goes unused here. Kept in the
	// signature anyway for parity with USolarOrbzClimateSimulationAsset::Simulate's own shape, and in
	// case a future refinement weights flow by real distance between a cell and its downhill neighbor
	// (shorter steps near the poles vs. the equator on this grid) rather than treating every step as equal.
	(void)RadiusCm;

	const int32 W = FMath::Max(GridWidth, 8);
	const int32 H = FMath::Max(GridHeight, 4);

	OutGrid.Reset();
	OutGrid.Width = W;
	OutGrid.Height = H;

	const float SeaLevelCm = SeaLevel * 100.0f; // meters -> UE units (cm), matching ElevationCm below

	// --- Pass 1: sample elevation (via the same TerrainStack the mesh uses) and seed each cell's own rainfall. ---
	TArray<float> ElevationCm;
	ElevationCm.SetNumUninitialized(W * H);

	TArray<float> FlowAccum;
	FlowAccum.SetNumUninitialized(W * H);

	FSolarOrbzLatLongGrid(W, H).ForEachCell([&](int32 Idx, const FVector& UnitDirection, const FVector2D& UV)
	{
		ElevationCm[Idx] = TerrainStack ? TerrainStack->EvaluateHeight(UnitDirection, UV) : 0.0f;

		float Rainfall = BaseRainfallAmount;
		if (ClimateGridForRainfall && ClimateGridForRainfall->IsValid())
		{
			float Temperature, Moisture;
			ClimateGridForRainfall->Sample(UnitDirection, Temperature, Moisture);
			Rainfall = BaseRainfallAmount * Moisture;
		}
		FlowAccum[Idx] = FMath::Max(Rainfall, 0.0f);
	});

	// --- Pass 2: D8 flow accumulation - process cells highest-first so every upstream neighbor that ---
	// drains into a cell has already added its own flow by the time that cell is processed, letting a
	// single pass correctly accumulate flow with no iteration needed (same ordering trick
	// USolarOrbzErosionTerrainLayer's hydraulic pass already relies on for the same reason).
	TArray<int32> SortedIndices;
	SortedIndices.SetNumUninitialized(W * H);
	for (int32 i = 0; i < SortedIndices.Num(); ++i)
	{
		SortedIndices[i] = i;
	}
	SortedIndices.Sort([&ElevationCm](int32 A, int32 B) { return ElevationCm[A] > ElevationCm[B]; });

	for (const int32 Idx : SortedIndices)
	{
		// Ocean cells are a terminal sink - they still RECEIVE whatever flow reaches them (so a
		// coastline/river-mouth cell correctly reads as high flow), but don't route it onward to a
		// neighbor. Without this, accumulated flow would keep compounding across the whole connected
		// ocean body cell-by-cell, swamping the normalization below so every real river on land reads
		// as indistinguishable near-zero next to the deepest ocean trench.
		if (ElevationCm[Idx] <= SeaLevelCm)
		{
			continue;
		}

		const int32 X = Idx % W;
		const int32 Y = Idx / W;
		const int32 LowestIdx = FSolarOrbzLatLongGrid::FindSteepestDescentNeighbor(ElevationCm, X, Y, W, H, ElevationCm[Idx]);
		if (LowestIdx != INDEX_NONE)
		{
			FlowAccum[LowestIdx] += FlowAccum[Idx];
		}
		// LowestIdx == INDEX_NONE: a local minimum/basin with nowhere lower to route to (an endorheic
		// basin) - its accumulated flow simply stays pooled there rather than being discarded.
	}

	// --- Log-compress then min-max normalize to 0..1, so an authored Flow Threshold range has a ---
	// sane 0..1 scale to work with regardless of grid resolution or how many cells feed any one river.
	OutGrid.FlowAccumulation01.SetNumUninitialized(W * H);
	float MaxLogFlow = 0.0f;
	for (int32 Idx = 0; Idx < FlowAccum.Num(); ++Idx)
	{
		const float LogFlow = FMath::Loge(1.0f + FMath::Max(FlowAccum[Idx], 0.0f));
		OutGrid.FlowAccumulation01[Idx] = LogFlow; // temporarily holds the log-compressed value, normalized below
		MaxLogFlow = FMath::Max(MaxLogFlow, LogFlow);
	}

	MaxLogFlow = FMath::Max(MaxLogFlow, KINDA_SMALL_NUMBER);
	float MinNormalized = 1.0f, MaxNormalized = 0.0f, SumNormalized = 0.0f;
	for (int32 Idx = 0; Idx < OutGrid.FlowAccumulation01.Num(); ++Idx)
	{
		const float Normalized = FMath::Clamp(OutGrid.FlowAccumulation01[Idx] / MaxLogFlow, 0.0f, 1.0f);
		OutGrid.FlowAccumulation01[Idx] = Normalized;
		MinNormalized = FMath::Min(MinNormalized, Normalized);
		MaxNormalized = FMath::Max(MaxNormalized, Normalized);
		SumNormalized += Normalized;
	}

	UE_LOG(LogSolarOrbzHydrology, Log,
		TEXT("SolarOrbz Hydrology: baked %dx%d flow accumulation grid - FlowAccumulation01 %.3f..%.3f (avg %.3f), rainfall %s"),
		W, H, MinNormalized, MaxNormalized, OutGrid.FlowAccumulation01.Num() > 0 ? SumNormalized / OutGrid.FlowAccumulation01.Num() : 0.0f,
		(ClimateGridForRainfall && ClimateGridForRainfall->IsValid()) ? TEXT("Moisture-weighted") : TEXT("flat (no Climate Simulation assigned)"));
}
