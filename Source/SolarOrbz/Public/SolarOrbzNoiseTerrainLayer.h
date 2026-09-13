// SolarOrbz - Fractal Perlin noise terrain layer, scaled by a raw amplitude in UE units (cm).
// Good for small or irregular bodies (asteroids, tiny moons) where "meters above sea level" isn't
// a meaningful concept - just dial in a height range directly. For planet-scale terrain where you
// want elevation to mean the same thing regardless of Radius, use Planetary Noise Layer instead.

#pragma once

#include "CoreMinimal.h"
#include "SolarOrbzFractalNoiseTerrainLayerBase.h"
#include "SolarOrbzNoiseTerrainLayer.generated.h"

/** Fractal (multi-octave) Perlin noise, scaled by a raw amplitude - the original, scale-agnostic noise layer. */
UCLASS(EditInlineNew, meta = (DisplayName = "Noise Layer"))
class SOLARORBZ_API USolarOrbzNoiseTerrainLayer : public USolarOrbzFractalNoiseTerrainLayerBase
{
	GENERATED_BODY()

public:
	/** Final height scale, in UE units (cm) - the peak-to-peak range the fractal sum is mapped onto. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise")
	float Amplitude = 500.0f;

	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const override;
};
