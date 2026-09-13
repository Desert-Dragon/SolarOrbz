// SolarOrbz - Climate simulation implementation.

#include "SolarOrbzClimateSimulation.h"
#include "SolarOrbzTerrainLayers.h"

namespace SolarOrbzClimate
{
	// Same rationale as FSolarOrbzIcoSphereGenerator: an explicit double constant rather than the
	// engine's PI macro, since FVector components are double (LWC) by default. PI_D itself is a
	// private constant scoped to SolarOrbzIcoSphereGenerator.cpp, so it isn't visible here - this
	// is this file's own copy of the same value.
	static constexpr double PI_D = 3.14159265358979323846;
}

void FSolarOrbzClimateGrid::Sample(const FVector& UnitDirection, float& OutTemperatureKelvin, float& OutMoisture01) const
{
	using namespace SolarOrbzClimate;

	if (!IsValid())
	{
		OutTemperatureKelvin = 288.0f;
		OutMoisture01 = 0.5f;
		return;
	}

	// Same convention as FSolarOrbzIcoSphereGenerator::ComputeUV.
	const double Azimuth = FMath::Atan2((double)UnitDirection.Y, (double)UnitDirection.X); // -PI .. PI
	const double U = 0.5 + Azimuth / (2.0 * PI_D);
	const double Polar = FMath::Acos(FMath::Clamp((double)UnitDirection.Z, -1.0, 1.0)); // 0 .. PI
	const double V = Polar / PI_D;

	const double Fx = FMath::Frac(U) * (double)Width;
	const double Fy = FMath::Clamp(V, 0.0, 1.0) * (double)(Height - 1);

	const int32 X0 = FMath::FloorToInt(Fx) % Width;
	const int32 X1 = (X0 + 1) % Width;
	const int32 Y0 = FMath::Clamp(FMath::FloorToInt(Fy), 0, Height - 1);
	const int32 Y1 = FMath::Clamp(Y0 + 1, 0, Height - 1);

	const float Tx = (float)(Fx - FMath::FloorToDouble(Fx));
	const float Ty = (float)(Fy - FMath::FloorToDouble(Fy));

	auto SampleBilinear = [&](const TArray<float>& Grid) -> float
	{
		const float A = FMath::Lerp(Grid[Y0 * Width + X0], Grid[Y0 * Width + X1], Tx);
		const float B = FMath::Lerp(Grid[Y1 * Width + X0], Grid[Y1 * Width + X1], Tx);
		return FMath::Lerp(A, B, Ty);
	};

	OutTemperatureKelvin = SampleBilinear(TemperatureKelvin);
	OutMoisture01 = SampleBilinear(Moisture01);
}

void USolarOrbzClimateSimulationAsset::Simulate(const USolarOrbzTerrainLayerStack* TerrainStack, float RadiusCm, float AtmosphereDensityAtSeaLevel, FSolarOrbzClimateGrid& OutGrid) const
{
	using namespace SolarOrbzClimate;

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
			const float Elevation = TerrainStack ? TerrainStack->EvaluateHeight(UnitDirection, UV) : 0.0f;
			ElevationCm[Idx] = Elevation;

			const float LatitudeAbs = FMath::Abs((float)Z); // 0 equator .. 1 pole
			const float ElevationAboveSeaKm = FMath::Max(Elevation - SeaLevelCm, 0.0f) / 100000.0f; // cm -> km
			const float BaseTemp = FMath::Lerp(EquatorTemperature, PoleTemperature, LatitudeAbs);
			// Floored at absolute zero only - deliberately not clamped to any Earth-relative range,
			// so a lava world or a cryogenic moon are both representable.
			OutGrid.TemperatureKelvin[Idx] = FMath::Max(BaseTemp - LapseRatePerKm * ElevationAboveSeaKm, 0.0f);
		}
	}

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
