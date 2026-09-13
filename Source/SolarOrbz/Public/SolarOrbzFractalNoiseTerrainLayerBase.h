// SolarOrbz - Shared fractal (multi-octave) Perlin noise machinery for terrain layers. Concrete
// layers (USolarOrbzNoiseTerrainLayer, USolarOrbzPlanetaryNoiseTerrainLayer) share this exact noise
// field, domain warp, and seeding, and only differ in how the normalized -1..1 result gets scaled
// into an actual height - see each subclass's header for why that split exists.

#pragma once

#include "CoreMinimal.h"
#include "SolarOrbzTerrainLayer.h"
#include "SolarOrbzFractalNoiseTerrainLayerBase.generated.h"

UCLASS(Abstract, EditInlineNew)
class SOLARORBZ_API USolarOrbzFractalNoiseTerrainLayerBase : public USolarOrbzTerrainLayer
{
	GENERATED_BODY()

public:
	/** Randomizes the noise pattern without changing its statistical character. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise")
	int32 Seed = 0;

	/** Number of fractal octaves summed together - more octaves add finer detail at increasing cost. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise", meta = (ClampMin = "1", ClampMax = "10"))
	int32 Octaves = 5;

	/**
	 * Base frequency, roughly "noise cycles per trip around the sphere". Small values (0.5-3) give
	 * continent-scale features; large values (20+) give fine surface roughness.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise", meta = (ClampMin = "0.01"))
	float Frequency = 2.0f;

	/** Frequency multiplier applied each octave. >1 makes each successive octave finer. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise", meta = (ClampMin = "1.0"))
	float Lacunarity = 2.0f;

	/** Amplitude multiplier applied each octave. <1 makes finer octaves contribute progressively less. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Persistence = 0.5f;

	/** Domain warp strength - pushes the sample position through a second noise field first, breaking up regular noise "grid" patterning. 0 disables it. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise|Warp", meta = (ClampMin = "0.0"))
	float WarpStrength = 0.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise|Warp", meta = (ClampMin = "0.01", EditCondition = "WarpStrength > 0.0"))
	float WarpFrequency = 1.0f;

protected:
	/**
	 * Fractal Perlin sum with seeding and optional domain warp applied, normalized to roughly -1..1.
	 * Subclasses scale this into an actual height however makes sense for them (raw amplitude,
	 * sea-level-relative meters, etc).
	 */
	float ComputeNormalizedNoise(const FVector& UnitDirection) const;
};
