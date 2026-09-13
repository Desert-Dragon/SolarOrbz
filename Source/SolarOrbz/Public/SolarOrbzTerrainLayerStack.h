// SolarOrbz - Terrain layer stack: an ordered, saveable recipe combining
// procedural noise and/or authored heightmap layers into one planet's terrain.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SolarOrbzTerrainLayerStack.generated.h"

class USolarOrbzTerrainLayer;

/**
 * Author this once and reference it from as many SolarOrbz IcoSphere actors as you like -
 * e.g. one stack for "Earthlike", one for "Mars", one for a procedural gas giant moon.
 */
UCLASS(BlueprintType)
class SOLARORBZ_API USolarOrbzTerrainLayerStack : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Instanced, Category = "SolarOrbz|Terrain")
	TArray<TObjectPtr<USolarOrbzTerrainLayer>> Layers;

	/**
	 * Call once per regenerate, before any EvaluateHeight calls (including from a ClimateSimulation
	 * sampling this same stack) - gives layers that need whole-surface data (e.g. erosion) a chance
	 * to bake it. No-op for layers that don't override RequiresWholeSurfaceBake().
	 */
	void PrepareLayers(float RadiusCm) const;

	/** Evaluates every enabled layer in order and returns the combined height, in UE units (cm). */
	float EvaluateHeight(const FVector& UnitDirection, const FVector2D& UV) const;

	/** Same as EvaluateHeight, but only accumulates layers with index < EndIndexExclusive - i.e. what a layer at that index would see as "everything below it". */
	float EvaluateHeightUpTo(int32 EndIndexExclusive, const FVector& UnitDirection, const FVector2D& UV) const;
};
