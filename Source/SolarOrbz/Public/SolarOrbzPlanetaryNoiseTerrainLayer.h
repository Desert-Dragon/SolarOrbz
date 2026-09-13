// SolarOrbz - Fractal Perlin noise terrain layer, scaled by real-world meters above/below sea
// level rather than a raw amplitude - the same value means the same thing whether your planet is
// Earth-scale or a 50m test sphere, so changing Radius never requires re-tuning this layer. For
// small or irregular bodies where "sea level" isn't a meaningful concept (asteroids, tiny moons),
// use Noise Layer instead.

#pragma once

#include "CoreMinimal.h"
#include "SolarOrbzFractalNoiseTerrainLayerBase.h"
#include "SolarOrbzPlanetaryNoiseTerrainLayer.generated.h"

/** Fractal (multi-octave) Perlin noise, scaled by real meters relative to sea level - the planet-scale-independent noise layer. */
UCLASS(EditInlineNew, meta = (DisplayName = "Planetary Noise Layer"))
class SOLARORBZ_API USolarOrbzPlanetaryNoiseTerrainLayer : public USolarOrbzFractalNoiseTerrainLayerBase
{
	GENERATED_BODY()

public:
	/**
	 * How high above sea level (meters) this layer's tallest peaks should reach - authored in real,
	 * planet-scale-independent meters rather than a raw amplitude. "Sea level" here is the same zero
	 * point ClimateSimulation's Sea Level measures from: the planet's base radius. Reference: Earth's
	 * Everest is ~8,850m.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise", meta = (ClampMin = "0.0", ClampMax = "200000.0", Units = "m"))
	float MaxElevationMeters = 8000.0f;

	/**
	 * How far below sea level (meters, entered as a positive depth) this layer's lowest points should
	 * reach. Reference: Earth's Mariana Trench is ~10,900m deep; average ocean depth is closer to 3,700m.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise", meta = (ClampMin = "0.0", ClampMax = "200000.0", Units = "m"))
	float MaxDepthMeters = 4000.0f;

	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const override;
};
