// SolarOrbz - Biome preset. A reusable bundle: optional extra terrain detail
// (blended in wherever this biome's mask is active) and scatter definitions
// that will feed the PCG/PCGEx hookup. Store these as assets and reuse them
// across planets, the same way World Creator's biome presets work.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SolarOrbzBiome.generated.h"

class USolarOrbzTerrainLayerStack;

/**
 * Placeholder scatter rule - enough to describe "what goes where and how densely" for now.
 * The PCG/PCGEx graph will read these (matched by Tag) once that hookup exists.
 */
USTRUCT(BlueprintType)
struct SOLARORBZ_API FSolarOrbzBiomeScatterEntry
{
	GENERATED_BODY()

	/** Matched against PCG graph settings later - e.g. "Rock_Large", "Tree_Pine", "Wildlife_Deer". */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Scatter")
	FName Tag;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Scatter", meta = (ClampMin = "0.0"))
	float DensityPerSquareMeter = 0.01f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Scatter", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinSlope = 0.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Scatter", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MaxSlope = 1.0f;
};

UCLASS(BlueprintType)
class SOLARORBZ_API USolarOrbzBiome : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Identifier color for this biome in editor visualizations (mask previews, debug views, etc). */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Biome")
	FLinearColor PreviewColor = FLinearColor::White;

	/** Extra terrain layers specific to this biome - e.g. a "Mountain" biome's own ruggedness noise. Blended in wherever this biome's mask is active, on top of the planet's base terrain stack. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Biome")
	TObjectPtr<USolarOrbzTerrainLayerStack> TerrainDetail;

	/** Placeholder for the future PCG/PCGEx hookup - what this biome scatters, and how densely. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Biome")
	TArray<FSolarOrbzBiomeScatterEntry> ScatterEntries;
};
