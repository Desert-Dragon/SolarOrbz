// SolarOrbz - Climate biome mask. Classic Whittaker-diagram-style classifier:
// combines elevation, latitude (as a temperature proxy), slope and a moisture
// noise field, each as a soft-edged range, multiplied together. Soft edges
// mean adjacent biomes blend across a transition band instead of hard-cutting.

#pragma once

#include "CoreMinimal.h"
#include "SolarOrbzBiomeMask.h"
#include "SolarOrbzClimateBiomeMask.generated.h"

/** A soft-edged [Min, Max] range: 1.0 inside, ramping to 0.0 over Falloff at each edge. Disabled = always 1.0 (no restriction on that axis). */
USTRUCT(BlueprintType)
struct SOLARORBZ_API FSolarOrbzMaskRange
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Range")
	bool bEnabled = false;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Range", meta = (EditCondition = "bEnabled"))
	float Min = 0.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Range", meta = (EditCondition = "bEnabled"))
	float Max = 1.0f;

	/** How gradual the transition at the edges is, in the same units as Min/Max. 0 = hard cutoff. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Range", meta = (EditCondition = "bEnabled", ClampMin = "0.0"))
	float Falloff = 0.1f;

	float Evaluate(float Value) const;
};

UCLASS(EditInlineNew, meta = (DisplayName = "Climate Mask"))
class SOLARORBZ_API USolarOrbzClimateBiomeMask : public USolarOrbzBiomeMask
{
	GENERATED_BODY()

public:
	/** Elevation range, UE units (cm), relative to the planet's base radius. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate")
	FSolarOrbzMaskRange Elevation;

	/** 0 = equator, 1 = pole. A stand-in for temperature until axial tilt / actual thermal simulation exists. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate")
	FSolarOrbzMaskRange Latitude;

	/** 0 = flat ground, 1 = vertical cliff face. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate")
	FSolarOrbzMaskRange Slope;

	/** 0..1, driven by a low-frequency noise field standing in for rainfall/humidity/volatile concentration. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate")
	FSolarOrbzMaskRange Moisture;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate", meta = (EditCondition = "Moisture.bEnabled"))
	int32 MoistureSeed = 0;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate", meta = (EditCondition = "Moisture.bEnabled", ClampMin = "0.01"))
	float MoistureFrequency = 1.5f;

	virtual float GetWeight(const FSolarOrbzBiomeSampleContext& Context) const override;
};
