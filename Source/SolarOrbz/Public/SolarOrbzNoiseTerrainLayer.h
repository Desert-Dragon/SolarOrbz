// SolarOrbz - Layered (fractal) Perlin noise terrain layer.

#pragma once

#include "CoreMinimal.h"
#include "SolarOrbzTerrainLayer.h"
#include "SolarOrbzNoiseTerrainLayer.generated.h"

/** Fractal (multi-octave) Perlin noise - the procedural half of the terrain stack. */
UCLASS(EditInlineNew, meta = (DisplayName = "Noise Layer"))
class SOLARORBZ_API USolarOrbzNoiseTerrainLayer : public USolarOrbzTerrainLayer
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

	/** Final height scale, in UE units (cm) - the peak-to-peak range the fractal sum is mapped onto. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise")
	float Amplitude = 500.0f;

	/** Domain warp strength - pushes the sample position through a second noise field first, breaking up regular noise "grid" patterning. 0 disables it. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise|Warp", meta = (ClampMin = "0.0"))
	float WarpStrength = 0.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise|Warp", meta = (ClampMin = "0.01", EditCondition = "WarpStrength > 0.0"))
	float WarpFrequency = 1.0f;

	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const override;
};
