// SolarOrbz - Planetary (sea-level-relative) noise terrain layer implementation.

#include "SolarOrbzPlanetaryNoiseTerrainLayer.h"

float USolarOrbzPlanetaryNoiseTerrainLayer::GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const
{
	const float Normalized = ComputeNormalizedNoise(UnitDirection); // -1..1, relative to sea level (0 = base radius)

	// Above sea level scales toward MaxElevationMeters; below scales toward MaxDepthMeters (entered
	// as a positive depth, so this stays negative here since Normalized is negative below sea level).
	const float HeightMeters = Normalized >= 0.0f
		? Normalized * MaxElevationMeters
		: Normalized * MaxDepthMeters;

	return HeightMeters * 100.0f; // meters -> UE units (cm)
}
