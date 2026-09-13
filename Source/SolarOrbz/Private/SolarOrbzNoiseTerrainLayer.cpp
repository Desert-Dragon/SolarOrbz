// SolarOrbz - Fractal noise terrain layer implementation (raw amplitude variant).

#include "SolarOrbzNoiseTerrainLayer.h"

float USolarOrbzNoiseTerrainLayer::GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const
{
	return ComputeNormalizedNoise(UnitDirection) * Amplitude;
}
