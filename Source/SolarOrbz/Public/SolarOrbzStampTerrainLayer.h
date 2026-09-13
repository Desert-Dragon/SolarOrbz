// SolarOrbz - Stamp terrain layer. Places a single feature (a heightmap
// texture, or a procedural dome/crater) at a specific point on the sphere,
// with an angular radius and soft edge falloff. Mirrors World Creator's
// terrain stamping - use several of these for individual mountain ranges,
// craters, or impact basins instead of covering the whole globe with noise.

#pragma once

#include "CoreMinimal.h"
#include "SolarOrbzTerrainLayer.h"
#include "SolarOrbzTextureHeightSampler.h"
#include "SolarOrbzStampTerrainLayer.generated.h"

class UTexture2D;

UCLASS(EditInlineNew, meta = (DisplayName = "Stamp Layer"))
class SOLARORBZ_API USolarOrbzStampTerrainLayer : public USolarOrbzTerrainLayer
{
	GENERATED_BODY()

public:
	/** Where the stamp is centered, in degrees. Latitude: -90 (south pole) .. 90 (north pole). Longitude: -180 .. 180. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Stamp", meta = (ClampMin = "-90.0", ClampMax = "90.0"))
	float Latitude = 0.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Stamp", meta = (ClampMin = "-180.0", ClampMax = "180.0"))
	float Longitude = 0.0f;

	/** How far the stamp reaches from its center, in degrees of arc along the sphere's surface. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Stamp", meta = (ClampMin = "0.1", ClampMax = "180.0"))
	float AngularRadius = 15.0f;

	/** Fraction of AngularRadius (0..1) over which the stamp fades out at its edge, so it blends instead of cliffing off. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Stamp", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float EdgeFalloff = 0.25f;

	/** Rotates the stamp around its own center - only matters with a heightmap assigned. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Stamp", meta = (ClampMin = "-180.0", ClampMax = "180.0"))
	float StampRotationDegrees = 0.0f;

	/** Optional grayscale heightmap sampled across the stamp's footprint (orthographic projection, not equirectangular - no seam concerns). Leave unset for a procedural dome/crater instead. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Stamp")
	TObjectPtr<UTexture2D> StampHeightmap;

	/** Peak height (meters) - the dome/crater's extremum, or the heightmap's white value. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Stamp", meta = (ClampMin = "0.0", ClampMax = "1000000.0", Units = "m"))
	float AmplitudeMeters = 10.0f;

	/** Only used without a heightmap: carves a crater (dips down at the center, optionally with a rim) instead of a dome. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Stamp", meta = (EditCondition = "StampHeightmap == nullptr"))
	bool bCrater = false;

	/** Only used with bCrater: adds a raised rim at the crater's edge, as a fraction of Amplitude Meters. 0 disables the rim. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Stamp", meta = (EditCondition = "bCrater && StampHeightmap == nullptr", ClampMin = "0.0"))
	float CraterRimHeight = 0.3f;

	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const override;

private:
	mutable FSolarOrbzTextureHeightSampler Sampler;

	FVector GetStampDirection() const;
};
