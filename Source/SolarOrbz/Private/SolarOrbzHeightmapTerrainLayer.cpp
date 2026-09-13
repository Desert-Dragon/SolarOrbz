// SolarOrbz - Heightmap terrain layer implementation.

#include "SolarOrbzHeightmapTerrainLayer.h"
#include "Engine/Texture2D.h"

float USolarOrbzHeightmapTerrainLayer::GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const
{
	if (!HeightmapTexture || !Sampler.EnsureDecoded(HeightmapTexture))
	{
		return 0.0f;
	}

	const float Height01 = Sampler.SampleBilinear01(UV.X, UV.Y);
	return FMath::Lerp(MinHeightMeters, MaxHeightMeters, Height01) * 100.0f; // meters -> UE units (cm)
}
