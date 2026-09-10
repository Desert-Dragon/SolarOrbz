// SolarOrbz - Authored heightmap terrain layer. Samples an equirectangular
// heightmap texture (e.g. a real-world DEM for Earth or Mars) using the
// mesh's spherical UV.

#pragma once

#include "CoreMinimal.h"
#include "SolarOrbzTerrainLayer.h"
#include "SolarOrbzTextureHeightSampler.h"
#include "SolarOrbzHeightmapTerrainLayer.generated.h"

class UTexture2D;

/**
 * Editor-only: reads the texture's source pixel data directly rather than sampling the GPU
 * resource, so it works regardless of compression settings. Import with compression set to
 * None so full precision survives.
 */
UCLASS(EditInlineNew, meta = (DisplayName = "Heightmap Layer"))
class SOLARORBZ_API USolarOrbzHeightmapTerrainLayer : public USolarOrbzTerrainLayer
{
	GENERATED_BODY()

public:
	/** Equirectangular (lat/long) heightmap - X = longitude, Y = latitude (north pole at top). Grayscale; 16-bit or float source recommended for real-world DEMs. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Heightmap")
	TObjectPtr<UTexture2D> HeightmapTexture;

	/** World-space height (cm) that black (0.0) in the heightmap maps to. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Heightmap")
	float MinHeight = -1100000.0f; // ~ -11,000 m (Mariana Trench), in cm

	/** World-space height (cm) that white (1.0) in the heightmap maps to. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Heightmap")
	float MaxHeight = 884800.0f; // ~ 8,848 m (Everest), in cm

	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const override;

private:
	mutable FSolarOrbzTextureHeightSampler Sampler;
};
