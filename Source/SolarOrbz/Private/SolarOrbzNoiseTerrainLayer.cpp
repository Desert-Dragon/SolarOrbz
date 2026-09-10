// SolarOrbz - Fractal noise terrain layer implementation.

#include "SolarOrbzNoiseTerrainLayer.h"

float USolarOrbzNoiseTerrainLayer::GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const
{
	// Cheap deterministic hash so different seeds don't just look like the same
	// field shifted by a fixed, obvious amount.
	const FVector SeedOffset(
		FMath::Frac(FMath::Sin((float)Seed * 12.9898f) * 43758.5453f) * 1000.0f,
		FMath::Frac(FMath::Sin((float)Seed * 78.233f) * 43758.5453f) * 1000.0f,
		FMath::Frac(FMath::Sin((float)Seed * 37.719f) * 43758.5453f) * 1000.0f);

	FVector SamplePos = UnitDirection * Frequency + SeedOffset;

	if (WarpStrength > 0.0f)
	{
		const FVector WarpPos = UnitDirection * WarpFrequency + SeedOffset;
		const FVector Warp(
			FMath::PerlinNoise3D(WarpPos),
			FMath::PerlinNoise3D(WarpPos + FVector(31.7f, 0.0f, 0.0f)),
			FMath::PerlinNoise3D(WarpPos + FVector(0.0f, 57.3f, 0.0f)));
		SamplePos += Warp * WarpStrength;
	}

	float Sum = 0.0f;
	float MaxPossible = 0.0f;
	float OctaveAmplitude = 1.0f;
	FVector OctavePos = SamplePos;

	for (int32 Octave = 0; Octave < Octaves; ++Octave)
	{
		Sum += FMath::PerlinNoise3D(OctavePos) * OctaveAmplitude;
		MaxPossible += OctaveAmplitude;
		OctaveAmplitude *= Persistence;
		OctavePos *= Lacunarity;
	}

	const float Normalized = MaxPossible > KINDA_SMALL_NUMBER ? (Sum / MaxPossible) : 0.0f; // roughly -1..1
	return Normalized * Amplitude;
}
