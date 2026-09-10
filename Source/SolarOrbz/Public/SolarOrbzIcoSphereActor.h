// SolarOrbz - Editor preview actor: builds a live ProceduralMeshComponent
// icosphere from FSolarOrbzIcoSphereGenerator, and can bake the result
// out to a UStaticMesh asset.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include "SolarOrbzIcoSphereGenerator.h"
#include "SolarOrbzIcoSphereActor.generated.h"

UCLASS(BlueprintType, meta = (DisplayName = "SolarOrbz IcoSphere"))
class SOLARORBZ_API ASolarOrbzIcoSphereActor : public AActor
{
	GENERATED_BODY()

public:
	ASolarOrbzIcoSphereActor();

	/** Sphere radius, UE units (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|IcoSphere", meta = (ClampMin = "1.0"))
	float Radius = 100.0f;

	/** Desired vertex density along the surface, vertices per meter. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|IcoSphere", meta = (ClampMin = "0.01"))
	float VerticesPerMeter = 1.0f;

	/** Safety clamp on subdivision level regardless of requested density. Each +1 is ~4x the triangle count. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|IcoSphere", meta = (ClampMin = "0", ClampMax = "8"))
	int32 MaxSubdivisions = 6;

	/** Optional terrain recipe (procedural noise and/or an authored heightmap) applied as radial displacement after the base sphere is built. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Terrain")
	TObjectPtr<class USolarOrbzTerrainLayerStack> TerrainStack;

	/** Optional biome stack - adds biome-specific terrain detail on top of TerrainStack, masked by climate/composite conditions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Biome")
	TObjectPtr<class USolarOrbzBiomeStack> BiomeStack;

	/** When enabled, colors each vertex by its dominant biome's Preview Color instead of the normal material, so you can see layer boundaries directly. Needs an unlit material that reads vertex color assigned to Debug Biome Material below. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Biome|Debug")
	bool bShowBiomeDebugColors = false;

	/** Simple unlit material with VertexColor wired to Emissive Color - create one once and assign it here. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Biome|Debug")
	TObjectPtr<class UMaterialInterface> DebugBiomeMaterial;

	/** Material restored when Show Biome Debug Colors is turned back off. Leave unset to just fall back to the engine default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Biome|Debug")
	TObjectPtr<class UMaterialInterface> DefaultMaterial;

	/** Build simple collision on the preview mesh. Leave off for large/high-density previews - cheap to add later on the baked mesh instead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|IcoSphere")
	bool bEnablePreviewCollision = false;

	/** Regenerates the ProceduralMeshComponent from the current parameters. Also runs automatically when a parameter above changes. */
	UFUNCTION(CallInEditor, Category = "SolarOrbz|IcoSphere")
	void RegenerateMesh();

	/** Content-browser folder the baked asset is written to, e.g. "/Game/SolarOrbz/Meshes". */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Bake")
	FString BakePackagePath = TEXT("/Game/SolarOrbz/Meshes");

	/** Asset name for the baked static mesh. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Bake")
	FString BakeAssetName = TEXT("SM_IcoSphere");

	/** Bakes the current preview mesh out to a UStaticMesh asset on disk. */
	UFUNCTION(CallInEditor, Category = "SolarOrbz|Bake", meta = (DisplayName = "Bake To Static Mesh"))
	void BakeToStaticMeshAsset();

	/** Subdivision level used for the last generated preview. */
	int32 GetLastSubdivisionLevelUsed() const { return LastSubdivisionLevelUsed; }

	/** Vertex count of the last generated preview (post UV-seam/pole splitting). */
	int32 GetPreviewVertexCount() const { return CachedMeshData.Vertices.Num(); }

	/** Triangle count of the last generated preview. */
	int32 GetPreviewTriangleCount() const { return CachedMeshData.Triangles.Num() / 3; }

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	UPROPERTY(VisibleAnywhere, Category = "SolarOrbz|IcoSphere")
	TObjectPtr<UProceduralMeshComponent> ProcMesh;

private:
	// Cached from the last RegenerateMesh() call so Bake doesn't need to redo the generation work.
	FSolarOrbzIcoSphereMeshData CachedMeshData;
	int32 LastSubdivisionLevelUsed = 0;
};
