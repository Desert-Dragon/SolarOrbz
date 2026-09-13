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

	/** World-space height (meters) that black (0.0) in the heightmap maps to. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Heightmap", meta = (ClampMin = "-200000.0", ClampMax = "200000.0", Units = "m"))
	float MinHeightMeters = -11000.0f; // Mariana Trench

	/** World-space height (meters) that white (1.0) in the heightmap maps to. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Heightmap", meta = (ClampMin = "-200000.0", ClampMax = "200000.0", Units = "m"))
	float MaxHeightMeters = 8848.0f; // Everest

	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const override;

private:
	mutable FSolarOrbzTextureHeightSampler Sampler;
};
