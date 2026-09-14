// SolarOrbz - Climate simulation implementation.

#include "SolarOrbzClimateSimulation.h"
#include "SolarOrbzLatLongGrid.h"
#include "SolarOrbzTerrainLayers.h"

void FSolarOrbzClimateGrid::Sample(const FVector& UnitDirection, float& OutTemperatureKelvin, float& OutMoisture01) const
{
	if (!IsValid())
	{
		OutTemperatureKelvin = 288.0f;
		OutMoisture01 = 0.5f;
		return;
	}

	// Computed once, reused for both parallel arrays sampled at this same point.
	const FSolarOrbzLatLongGrid Grid(Width, Height);
	const FSolarOrbzLatLongGrid::FBilinearCell Cell = Grid.ComputeBilinearCell(UnitDirection);

	OutTemperatureKelvin = FSolarOrbzLatLongGrid::SampleAtCell(TemperatureKelvin, Width, Cell);
	OutMoisture01 = FSolarOrbzLatLongGrid::SampleAtCell(Moisture01, Width, Cell);
}

void USolarOrbzClimateSimulationAsset::Simulate(const USolarOrbzTerrainLayerStack* TerrainStack, double RadiusCm, float AtmosphereDensityAtSeaLevel, FSolarOrbzClimateGrid& OutGrid) const
{
	const int32 W = FMath::Max(GridWidth, 8);
	const int32 H = FMath::Max(GridHeight, 4);

	OutGrid.Reset();
	OutGrid.Width = W;
	OutGrid.Height = H;
	OutGrid.TemperatureKelvin.SetNumZeroed(W * H);
	OutGrid.Moisture01.SetNumZeroed(W * H);

	// Earth's sea-level air density is the reference point Atmosphere Density scales against - not
	// authored anywhere, just the baseline "1x" a thin or thick atmosphere is relative to. Square-rooted
	// so the huge real-world range (Mars ~0.02, Venus ~65) doesn't translate into an equally huge,
	// unusable multiplier on moisture - it's a directional nudge, not exact atmospheric physics.
	constexpr float EarthReferenceDensityKgPerM3 = 1.225f;
	const float AtmosphereDensityFactor = FMath::Sqrt(FMath::Max(AtmosphereDensityAtSeaLevel, 0.0f) / EarthReferenceDensityKgPerM3);
	const float EffectiveMoistureCapacity = MoistureCapacity * AtmosphereDensityFactor;
	const float EffectiveEvaporationRate = EvaporationRate * AtmosphereDensityFactor;

	const float SeaLevelCm = SeaLevel * 100.0f; // meters -> UE units (cm), to compare against Elevation which stays in cm internally

	// --- Pass 1: sample elevation (via the same TerrainStack the mesh uses) and derive temperature for every cell. ---
	TArray<float> ElevationCm;
	ElevationCm.SetNumUninitialized(W * H);

	FSolarOrbzLatLongGrid(W, H).ForEachCell([&](int32 Idx, const FVector& UnitDirection, const FVector2D& UV)
	{
		const float Elevation = TerrainStack ? TerrainStack->EvaluateHeight(UnitDirection, UV) : 0.0f;
		ElevationCm[Idx] = Elevation;

		const float LatitudeAbs = FMath::Abs(UnitDirection.Z); // 0 equator .. 1 pole
		const float ElevationAboveSeaKm = FMath::Max(Elevation - SeaLevelCm, 0.0f) / 100000.0f; // cm -> km
		const float BaseTemp = FMath::Lerp(EquatorTemperature, PoleTemperature, LatitudeAbs);
		// Floored at absolute zero only - deliberately not clamped to any Earth-relative range,
		// so a lava world or a cryogenic moon are both representable.
		OutGrid.TemperatureKelvin[Idx] = FMath::Max(BaseTemp - LapseRatePerKm * ElevationAboveSeaKm, 0.0f);
	});

	// --- Pass 2: march wind along each latitude row, wrapping WindLoops times so the carried-moisture ---
	// value settles into a repeating steady state before the final circuit is recorded. Each row's wind
	// direction comes from a simplified three-cell circulation model (tropical easterlies / mid-latitude
	// westerlies / polar easterlies, mirrored across both hemispheres) rather than one global direction -
	// this is what turns "noise that happens to look like rain" into an actual rain shadow that lands on
	// the correct side of a mountain range for its latitude.
	TArray<float> Precipitation;
	Precipitation.SetNumZeroed(W * H);

	const int32 LoopsClamped = FMath::Max(WindLoops, 1);
	const int32 TotalSteps = W * LoopsClamped;

	for (int32 Y = 0; Y < H; ++Y)
	{
		const double V = (double)Y / (double)FMath::Max(H - 1, 1);
		const double PolarDeg = V * 180.0;
		const double LatitudeAbsDeg = FMath::Abs(90.0 - PolarDeg); // 0 at equator .. 90 at either pole

		// Tropical and polar cells blow east-to-west (decreasing longitude); the mid-latitude
		// westerlies blow west-to-east (increasing longitude) - Earth's three-cell pattern.
		const bool bMidLatitudeBand = LatitudeAbsDeg >= TradeWindEdgeDegrees && LatitudeAbsDeg < WesterliesEdgeDegrees;
		const bool bRowReverse = bMidLatitudeBand ? bReverseAllBands : !bReverseAllBands;

		float Carried = 0.0f;
		// Prime with the "upwind" neighbor so the very first step's uphill delta is meaningful.
		float PrevElevationCm = ElevationCm[Y * W + (bRowReverse ? 0 : (W - 1))];

		for (int32 Step = 0; Step < TotalSteps; ++Step)
		{
			const int32 RawX = bRowReverse ? (W - 1 - (Step % W)) : (Step % W);
			const int32 Idx = Y * W + RawX;
			const float Elevation = ElevationCm[Idx];

			if (Elevation <= SeaLevelCm)
			{
				Carried = FMath::Min(EffectiveMoistureCapacity, Carried + EffectiveEvaporationRate);
			}
			else
			{
				const float UphillKm = FMath::Max(Elevation - PrevElevationCm, 0.0f) / 100000.0f;
				const float Rainfall = FMath::Min(Carried, Carried * BaseRainfallRate + OrographicRainfallFactor * UphillKm);
				Carried = FMath::Max(Carried - Rainfall, 0.0f);

				// Only the final circuit is recorded - earlier circuits exist purely to let Carried settle.
				if (Step >= TotalSteps - W)
				{
					Precipitation[Idx] = Rainfall;
				}
			}

			PrevElevationCm = Elevation;
		}
	}

	// Normalize precipitation across the whole grid to 0..1 so it drops straight into a Mask Range.
	float MaxPrecip = KINDA_SMALL_NUMBER;
	for (const float P : Precipitation)
	{
		MaxPrecip = FMath::Max(MaxPrecip, P);
	}

	for (int32 Idx = 0; Idx < Precipitation.Num(); ++Idx)
	{
		OutGrid.Moisture01[Idx] = Precipitation[Idx] / MaxPrecip;
	}

	// Ocean cells are the moisture source, not a rainfall destination - treat them as fully moist
	// so coastlines read as wet even though they never "receive" precipitation in the sim above.
	for (int32 Idx = 0; Idx < ElevationCm.Num(); ++Idx)
	{
		if (ElevationCm[Idx] <= SeaLevelCm)
		{
			OutGrid.Moisture01[Idx] = 1.0f;
		}
	}
}
