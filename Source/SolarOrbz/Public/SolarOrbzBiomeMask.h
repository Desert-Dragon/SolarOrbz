// SolarOrbz - Biome mask base class. A mask answers "how strongly does this
// biome apply here", 0..1, given a point's climate/shape inputs. Masks are
// composable (see SolarOrbzCompositeBiomeMask) so compound biomes like
// "Icy Chemical Mountain" fall out of stacking simple masks rather than
// needing a hardcoded category for every combination.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SolarOrbzBiomeMask.generated.h"

/** Everything a mask (or a biome's terrain detail layer) might need to know about a point on the planet. */
USTRUCT(BlueprintType)
struct SOLARORBZ_API FSolarOrbzBiomeSampleContext
{
	GENERATED_BODY()

	/** Normalized direction from the planet center - the point's position on the base unit sphere. */
	UPROPERTY(BlueprintReadOnly, Category = "SolarOrbz|Biome")
	FVector UnitDirection = FVector::UpVector;

	/** The mesh's spherical UV at this point (matches FSolarOrbzIcoSphereMeshData::UVs). */
	UPROPERTY(BlueprintReadOnly, Category = "SolarOrbz|Biome")
	FVector2D UV = FVector2D::ZeroVector;

	/** Height above/below the base radius after the base terrain stack has been applied, in UE units (cm). */
	UPROPERTY(BlueprintReadOnly, Category = "SolarOrbz|Biome")
	float Elevation = 0.0f;

	/** 0 = flat ground, 1 = vertical cliff face. Derived from how far the surface normal has tilted away from UnitDirection. */
	UPROPERTY(BlueprintReadOnly, Category = "SolarOrbz|Biome")
	float Slope = 0.0f;

	/**
	 * 0 = coldest simulated temperature, 1 = hottest. Only meaningful when bHasClimateData is true -
	 * i.e. the actor has a ClimateSimulation asset assigned. Otherwise left at its default and masks
	 * fall back to a plain latitude proxy.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "SolarOrbz|Biome")
	float Temperature = 0.5f;

	/**
	 * 0 = driest, 1 = wettest (oceans are always 1.0). Only meaningful when bHasClimateData is true;
	 * comes from a whole-planet wind/orographic simulation rather than a per-point noise field, so
	 * real rain shadows show up behind mountain ranges. Otherwise left at its default.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "SolarOrbz|Biome")
	float Moisture = 0.5f;

	/** True once Temperature/Moisture above have been filled in by a ClimateSimulation asset. False = no simulation is assigned; masks should use their own fallback logic instead. */
	UPROPERTY(BlueprintReadOnly, Category = "SolarOrbz|Biome")
	bool bHasClimateData = false;
};

UCLASS(Abstract, EditInlineNew, DefaultToInstanced, BlueprintType)
class SOLARORBZ_API USolarOrbzBiomeMask : public UObject
{
	GENERATED_BODY()

public:
	/** Returns how strongly this mask applies at the given point, 0 (not at all) .. 1 (fully). */
	virtual float GetWeight(const FSolarOrbzBiomeSampleContext& Context) const { return 1.0f; }
};
