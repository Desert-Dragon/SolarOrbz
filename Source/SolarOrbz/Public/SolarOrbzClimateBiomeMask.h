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
	/** Elevation range, meters, relative to the planet's base radius. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate")
	FSolarOrbzMaskRange Elevation;

	/** 0 = equator, 1 = pole. Purely geometric - doesn't account for elevation. Prefer Temperature below once a ClimateSimulation asset is assigned; keep using this one for simple cases or when no simulation exists. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate")
	FSolarOrbzMaskRange Latitude;

	/**
	 * Absolute temperature in Kelvin - not normalized. Simulated (latitude baseline minus elevation
	 * lapse rate) when the actor has a ClimateSimulation asset assigned; otherwise falls back to a
	 * generic Earth-like Lerp by latitude (288K equator .. 255K pole) so this axis is at least in the
	 * right ballpark without a simulation. Author the Min/Max below in Kelvin to match whatever your
	 * ClimateSimulation asset actually produces - this works equally for an icy moon or a Venus-hot world.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate", meta = (Units = "Kelvin"))
	FSolarOrbzMaskRange Temperature;

	/** 0 = flat ground, 1 = vertical cliff face. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate")
	FSolarOrbzMaskRange Slope;

	/**
	 * 0 = driest, 1 = wettest. Comes from a wind/orographic simulation (real rain shadows behind
	 * mountains) when the actor has a ClimateSimulation asset assigned; otherwise falls back to the
	 * low-frequency noise field below, standing in for rainfall/humidity until a simulation exists.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate")
	FSolarOrbzMaskRange Moisture;

	/** Only used as the noise fallback when no ClimateSimulation asset is assigned upstream. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate", meta = (EditCondition = "Moisture.bEnabled"))
	int32 MoistureSeed = 0;

	/** Only used as the noise fallback when no ClimateSimulation asset is assigned upstream. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate", meta = (EditCondition = "Moisture.bEnabled", ClampMin = "0.01"))
	float MoistureFrequency = 1.5f;

	virtual float GetWeight(const FSolarOrbzBiomeSampleContext& Context) const override;
};
