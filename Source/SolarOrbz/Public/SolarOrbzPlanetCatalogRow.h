// SolarOrbz - Planet catalog row: a DataTable row (import from a .csv, Row Struct = this type)
// that can fill in a planet spawner's base information in one step - Radius/Vertices Per Meter/
// Max Subdivisions/Collision plus the four module assets (Terrain Stack/Biome Stack/Climate
// Simulation/Profile), the same fields USolarOrbzPlanetSpawnerGraph's module nodes hold. See
// Docs/SolarOrbzPlanetSpawnerGraph.md.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "SolarOrbzPlanetCatalogRow.generated.h"

class USolarOrbzTerrainLayerStack;
class USolarOrbzBiomeStack;
class USolarOrbzClimateSimulationAsset;
class USolarOrbzCelestialBodyProfile;

/**
 * One row = one planet's base recipe. Meant to be imported from a .csv (Content Browser -> Import,
 * picking this as the Row Struct) and applied via the Planet Spawner graph's "Apply Row" button.
 *
 * The object-reference columns (TerrainStack/BiomeStack/ClimateSimulation/Profile) are
 * TSoftObjectPtr - a CSV cell for one of these needs the asset's full path (e.g.
 * "/Game/SolarOrbz/Biomes/ASN_BIOME_DESERT.ASN_BIOME_DESERT"), which the engine's CSV->DataTable
 * importer resolves into a soft reference automatically, the same way it does for any other
 * object-reference column - no custom parsing needed here. An empty cell leaves that field unset
 * (FSoftObjectPath::IsNull() true), and Apply Row treats unset as "don't touch the existing
 * assignment" rather than clearing it - so a row only needs to specify the fields it actually wants
 * to drive.
 */
USTRUCT(BlueprintType)
struct SOLARORBZ_API FSolarOrbzPlanetCatalogRow : public FTableRowBase
{
	GENERATED_BODY()

public:
	/** Sphere radius, meters. Matches ASolarOrbzIcoSphereActor::RadiusMeters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Catalog", meta = (ClampMin = "0.01"))
	double RadiusMeters = 1000.0;

	/** Surface vertex density target. Matches ASolarOrbzIcoSphereActor::VerticesPerMeter. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Catalog", meta = (ClampMin = "0.001"))
	float VerticesPerMeter = 1.0f;

	/** Subdivision level cap. Matches ASolarOrbzIcoSphereActor::MaxSubdivisions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Catalog", meta = (ClampMin = "0", ClampMax = "11"))
	int32 MaxSubdivisions = 6;

	/** Build simple collision on the preview mesh. Matches ASolarOrbzIcoSphereActor::bEnablePreviewCollision. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Catalog")
	bool bEnablePreviewCollision = false;

	/** Terrain recipe to assign. Leave unset to not override whatever's already assigned. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Catalog")
	TSoftObjectPtr<USolarOrbzTerrainLayerStack> TerrainStack;

	/** Biome stack to assign - e.g. a prebuilt "ASN_BIOME_DESERT"/"ASN_BIOME_OCEANDEEP" asset. Leave unset to not override. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Catalog")
	TSoftObjectPtr<USolarOrbzBiomeStack> BiomeStack;

	/** Climate simulation to assign. Leave unset to not override. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Catalog")
	TSoftObjectPtr<USolarOrbzClimateSimulationAsset> ClimateSimulation;

	/** Planet/Star/Asteroid Profile to assign. Leave unset to not override. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Catalog")
	TSoftObjectPtr<USolarOrbzCelestialBodyProfile> Profile;
};
