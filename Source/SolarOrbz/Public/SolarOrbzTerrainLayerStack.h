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

	/** Evaluates every enabled layer in order and returns the combined height, in UE units (cm). */
	float EvaluateHeight(const FVector& UnitDirection, const FVector2D& UV) const;
};
