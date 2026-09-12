// SolarOrbz - Climate biome mask implementation.

#include "SolarOrbzClimateBiomeMask.h"

float FSolarOrbzMaskRange::Evaluate(float Value) const
{
	if (!bEnabled)
	{
		return 1.0f;
	}

	if (Falloff <= KINDA_SMALL_NUMBER)
	{
		return (Value >= Min && Value <= Max) ? 1.0f : 0.0f;
	}

	const float LowEdge = FMath::SmoothStep(Min - Falloff, Min + Falloff, Value);
	const float HighEdge = 1.0f - FMath::SmoothStep(Max - Falloff, Max + Falloff, Value);
	return FMath::Clamp(FMath::Min(LowEdge, HighEdge), 0.0f, 1.0f);
}

float USolarOrbzClimateBiomeMask::GetWeight(const FSolarOrbzBiomeSampleContext& Context) const
{
	float Weight = 1.0f;

	Weight *= Elevation.Evaluate(Context.Elevation);
	if (Weight <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	Weight *= Latitude.Evaluate(FMath::Abs(Context.UnitDirection.Z));
	if (Weight <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	Weight *= Slope.Evaluate(Context.Slope);
	if (Weight <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	if (Temperature.bEnabled)
	{
		// Simulated when available; otherwise the same latitude proxy Latitude above uses, inverted
		// (equator = hottest = 1) so the axis is at least directionally correct without a simulation.
		const float TemperatureValue = Context.bHasClimateData
			? Context.Temperature
			: (1.0f - FMath::Abs(Context.UnitDirection.Z));
		Weight *= Temperature.Evaluate(TemperatureValue);
		if (Weight <= KINDA_SMALL_NUMBER)
		{
			return 0.0f;
		}
	}

	if (Moisture.bEnabled)
	{
		float MoistureValue;
		if (Context.bHasClimateData)
		{
			MoistureValue = Context.Moisture;
		}
		else
		{
			const FVector SeedOffset(
				FMath::Frac(FMath::Sin((float)MoistureSeed * 91.345f) * 47453.7f) * 1000.0f,
				FMath::Frac(FMath::Sin((float)MoistureSeed * 13.71f) * 47453.7f) * 1000.0f,
				FMath::Frac(FMath::Sin((float)MoistureSeed * 58.92f) * 47453.7f) * 1000.0f);

			MoistureValue = FMath::PerlinNoise3D(Context.UnitDirection * MoistureFrequency + SeedOffset) * 0.5f + 0.5f; // 0..1
		}
		Weight *= Moisture.Evaluate(MoistureValue);
	}

	return FMath::Clamp(Weight, 0.0f, 1.0f);
}
