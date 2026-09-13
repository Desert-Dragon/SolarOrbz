// SolarOrbz - Editor preview actor implementation

#include "SolarOrbzIcoSphereActor.h"
#include "SolarOrbzTerrainLayerStack.h"
#include "SolarOrbzBiomeStack.h"
#include "SolarOrbzBiome.h"
#include "SolarOrbzClimateSimulation.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"

DEFINE_LOG_CATEGORY_STATIC(LogSolarOrbz, Log, All);

ASolarOrbzIcoSphereActor::ASolarOrbzIcoSphereActor()
{
	PrimaryActorTick.bCanEverTick = false;

	ProcMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("ProcMesh"));
	SetRootComponent(ProcMesh);
	ProcMesh->bUseAsyncCooking = true;
}

void ASolarOrbzIcoSphereActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RegenerateMesh();
}

#if WITH_EDITOR
void ASolarOrbzIcoSphereActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	static const TSet<FName> RegenTriggers =
	{
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, RadiusMeters),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, VerticesPerMeter),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, MaxSubdivisions),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, bEnablePreviewCollision),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, TerrainStack),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, BiomeStack),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, ClimateSimulation),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, bShowBiomeDebugColors),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, DebugBiomeMaterial),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, DefaultMaterial),
	};

	if (RegenTriggers.Contains(PropertyChangedEvent.GetPropertyName()))
	{
		RegenerateMesh();
	}
}
#endif

void ASolarOrbzIcoSphereActor::RegenerateMesh()
{
	if (!ProcMesh)
	{
		return;
	}

	// The generator's own API works in UE units (cm) throughout, matching every other UE
	// system (collision, physics, etc). RadiusMeters is purely a user-facing convenience -
	// convert once, right here, and every internal calculation below stays in cm.
	const float RadiusCm = RadiusMeters * 100.0f;

	LastSubdivisionLevelUsed = FSolarOrbzIcoSphereGenerator::Generate(RadiusCm, VerticesPerMeter, CachedMeshData, MaxSubdivisions, &LastRequestedSubdivisionLevel);

	if (LastRequestedSubdivisionLevel > LastSubdivisionLevelUsed)
	{
		UE_LOG(LogSolarOrbz, Warning,
			TEXT("SolarOrbz: Vertices Per Meter (%.4f) would need subdivision level %d at this radius, but Max Subdivisions caps it at %d - the density setting is NOT being reached. Raise Max Subdivisions or lower Vertices Per Meter."),
			VerticesPerMeter, LastRequestedSubdivisionLevel, LastSubdivisionLevelUsed);
	}

	// Keep the pristine outward sphere direction for every vertex - both passes displace
	// along this, not along the (changing) recomputed normal, so height stays purely radial.
	TArray<FVector> OriginalUnitDirections = CachedMeshData.Normals;

	// --- Pass A: base terrain (procedural noise and/or authored heightmap). ---
	if (TerrainStack)
	{
		// Whole-surface bake first (e.g. erosion) - must happen before any per-point EvaluateHeight
		// calls below, including the ones the Climate Simulation will make against this same stack,
		// so rain shadows react to eroded terrain rather than the pre-erosion noise.
		TerrainStack->PrepareLayers(RadiusCm);

		for (int32 i = 0; i < CachedMeshData.Vertices.Num(); ++i)
		{
			const FVector& UnitDirection = OriginalUnitDirections[i];
			const float Height = TerrainStack->EvaluateHeight(UnitDirection, CachedMeshData.UVs[i]);
			CachedMeshData.Vertices[i] += UnitDirection * Height;
		}

		// Recompute now so Pass B has real slope data to mask against, not the pristine sphere's.
		FSolarOrbzIcoSphereGenerator::RecomputeSmoothNormals(CachedMeshData);
	}

	// --- Climate simulation: runs once on its own lat/long grid (not per-vertex), sampling elevation ---
	// from TerrainStack the same way the mesh does. Feeds Pass B's sample context below.
	CachedClimateGrid.Reset();
	if (ClimateSimulation)
	{
		ClimateSimulation->Simulate(TerrainStack, RadiusCm, CachedClimateGrid);

		if (CachedClimateGrid.IsValid())
		{
			float MinTemp = TNumericLimits<float>::Max(), MaxTemp = TNumericLimits<float>::Lowest(), SumTemp = 0.0f;
			float MinMoist = TNumericLimits<float>::Max(), MaxMoist = TNumericLimits<float>::Lowest(), SumMoist = 0.0f;
			for (int32 Idx = 0; Idx < CachedClimateGrid.TemperatureKelvin.Num(); ++Idx)
			{
				const float T = CachedClimateGrid.TemperatureKelvin[Idx];
				const float M = CachedClimateGrid.Moisture01[Idx];
				MinTemp = FMath::Min(MinTemp, T); MaxTemp = FMath::Max(MaxTemp, T); SumTemp += T;
				MinMoist = FMath::Min(MinMoist, M); MaxMoist = FMath::Max(MaxMoist, M); SumMoist += M;
			}
			const int32 CellCount = CachedClimateGrid.TemperatureKelvin.Num();
			UE_LOG(LogSolarOrbz, Log,
				TEXT("SolarOrbz Climate: grid %dx%d - Temperature %.1fK..%.1fK (avg %.1fK), Moisture %.3f..%.3f (avg %.3f)"),
				CachedClimateGrid.Width, CachedClimateGrid.Height, MinTemp, MaxTemp, SumTemp / CellCount, MinMoist, MaxMoist, SumMoist / CellCount);

			if (MaxMoist - MinMoist < KINDA_SMALL_NUMBER)
			{
				UE_LOG(LogSolarOrbz, Warning,
					TEXT("SolarOrbz Climate: Moisture is completely flat (%.3f everywhere) - either every cell is below Sea Level (all ocean) or TerrainStack has no real elevation variation, so wind/atmosphere settings have nothing to act on."),
					MinMoist);
			}
		}
		else
		{
			UE_LOG(LogSolarOrbz, Warning, TEXT("SolarOrbz Climate: ClimateSimulation is assigned but produced an invalid grid (check Grid Width/Height)."));
		}
	}
	else if (bShowBiomeDebugColors)
	{
		UE_LOG(LogSolarOrbz, Warning, TEXT("SolarOrbz Climate: no ClimateSimulation assigned on the actor - any Climate Biome Mask using Moisture/Temperature is running on its no-simulation fallback, not real simulated data."));
	}

	// --- Pass B: biome-specific detail, masked by climate/composite conditions and blended on top. ---
	TArray<FLinearColor> BiomeDebugColors;
	if (BiomeStack)
	{
		if (bShowBiomeDebugColors)
		{
			BiomeDebugColors.SetNum(CachedMeshData.Vertices.Num());
		}

		int32 NumWithClimateData = 0;
		int32 NumDominantBiomeHits = 0;

		for (int32 i = 0; i < CachedMeshData.Vertices.Num(); ++i)
		{
			const FVector& UnitDirection = OriginalUnitDirections[i];

			FSolarOrbzBiomeSampleContext Context;
			Context.UnitDirection = UnitDirection;
			Context.UV = CachedMeshData.UVs[i];
			Context.Elevation = FVector::DotProduct(CachedMeshData.Vertices[i], UnitDirection) - RadiusCm;
			Context.Slope = FMath::Clamp(1.0f - FVector::DotProduct(CachedMeshData.Normals[i], UnitDirection), 0.0f, 1.0f);

			if (CachedClimateGrid.IsValid())
			{
				Context.bHasClimateData = true;
				CachedClimateGrid.Sample(UnitDirection, Context.Temperature, Context.Moisture);
				++NumWithClimateData;
			}

			const float BiomeHeight = BiomeStack->EvaluateBiomeTerrainContribution(Context);
			CachedMeshData.Vertices[i] += UnitDirection * BiomeHeight;

			if (bShowBiomeDebugColors)
			{
				if (const USolarOrbzBiome* Dominant = BiomeStack->GetDominantBiome(Context))
				{
					BiomeDebugColors[i] = Dominant->PreviewColor;
					++NumDominantBiomeHits;
				}
				else
				{
					BiomeDebugColors[i] = FLinearColor::Black; // no biome layer applies here
				}
			}
		}

		FSolarOrbzIcoSphereGenerator::RecomputeSmoothNormals(CachedMeshData);

		if (bShowBiomeDebugColors)
		{
			UE_LOG(LogSolarOrbz, Log,
				TEXT("SolarOrbz Biome Debug: %d/%d verts had climate data, %d/%d verts matched a biome layer (rest rendered black = no layer applies there)."),
				NumWithClimateData, CachedMeshData.Vertices.Num(), NumDominantBiomeHits, CachedMeshData.Vertices.Num());

			if (NumDominantBiomeHits == 0)
			{
				UE_LOG(LogSolarOrbz, Warning, TEXT("SolarOrbz Biome Debug: not a single vertex matched any biome layer's mask - check each layer's Mask Preset ranges (Min/Max/Falloff) against the Moisture/Temperature/Elevation stats logged above."));
			}
		}
	}
	else if (bShowBiomeDebugColors)
	{
		UE_LOG(LogSolarOrbz, Warning, TEXT("SolarOrbz Biome Debug: Show Biome Debug Colors is on but no BiomeStack is assigned - there's nothing to color, the mesh will render fully black (or white, if the material has no vertex colors at all)."));
	}

	if (bShowBiomeDebugColors && !DebugBiomeMaterial)
	{
		UE_LOG(LogSolarOrbz, Warning, TEXT("SolarOrbz Biome Debug: Show Biome Debug Colors is on but Debug Biome Material is not assigned - slot 0 will get a null material (default checker/gray) regardless of the vertex colors computed above."));
	}

	ProcMesh->SetMaterial(0, bShowBiomeDebugColors ? DebugBiomeMaterial : DefaultMaterial);
	UE_LOG(LogSolarOrbz, Log, TEXT("SolarOrbz: material slot 0 set to '%s' (bShowBiomeDebugColors=%s)"),
		*GetNameSafe(bShowBiomeDebugColors ? DebugBiomeMaterial : DefaultMaterial),
		bShowBiomeDebugColors ? TEXT("true") : TEXT("false"));

	TArray<FProcMeshTangent> ProcTangents;
	ProcTangents.Reserve(CachedMeshData.Tangents.Num());
	for (const FVector& T : CachedMeshData.Tangents)
	{
		ProcTangents.Add(FProcMeshTangent(T, false));
	}

	const TArray<FVector2D> EmptyUVChannel;

	ProcMesh->ClearAllMeshSections();
	ProcMesh->CreateMeshSection_LinearColor(
		0,
		CachedMeshData.Vertices,
		CachedMeshData.Triangles,
		CachedMeshData.Normals,
		CachedMeshData.UVs,
		EmptyUVChannel, EmptyUVChannel, EmptyUVChannel,
		BiomeDebugColors,
		ProcTangents,
		bEnablePreviewCollision);

	UE_LOG(LogSolarOrbz, Log, TEXT("SolarOrbz: generated icosphere at subdivision level %d (%d verts, %d tris)"),
		LastSubdivisionLevelUsed, CachedMeshData.Vertices.Num(), CachedMeshData.Triangles.Num() / 3);
}

void ASolarOrbzIcoSphereActor::BakeToStaticMeshAsset()
{
#if WITH_EDITOR
	if (CachedMeshData.Vertices.Num() == 0)
	{
		RegenerateMesh();
	}

	if (CachedMeshData.Triangles.Num() == 0)
	{
		UE_LOG(LogSolarOrbz, Warning, TEXT("SolarOrbz: nothing to bake, mesh data is empty."));
		return;
	}

	const FString CleanAssetName = BakeAssetName.IsEmpty() ? TEXT("SM_IcoSphere") : BakeAssetName;
	const FString ObjectPath = FPaths::Combine(BakePackagePath, CleanAssetName);
	const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		UE_LOG(LogSolarOrbz, Error, TEXT("SolarOrbz: failed to create package '%s'"), *PackageName);
		return;
	}
	Package->FullyLoad();

	UStaticMesh* NewStaticMesh = NewObject<UStaticMesh>(Package, FName(*CleanAssetName), RF_Public | RF_Standalone);
	if (!NewStaticMesh)
	{
		UE_LOG(LogSolarOrbz, Error, TEXT("SolarOrbz: failed to create UStaticMesh object."));
		return;
	}

	// --- Build a MeshDescription from our generator output. ---
	// Vertices sharing an exact position get a single FVertexID (so normal/tangent-generation and
	// any future LOD reduction see correct topology); each array entry still gets its own
	// FVertexInstanceID, which is exactly what lets the UV-seam and pole duplicates carry different UVs.
	FMeshDescription MeshDescription;
	FStaticMeshAttributes Attributes(MeshDescription);
	Attributes.Register();

	TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> InstanceNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> InstanceTangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> InstanceBinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector4f> InstanceColors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> InstanceUVs = Attributes.GetVertexInstanceUVs();
	InstanceUVs.SetNumChannels(1);

	const FPolygonGroupID PolygonGroupID = MeshDescription.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames()[PolygonGroupID] = FName(TEXT("Default"));

	TMap<FVector, FVertexID> PositionToVertexID;
	PositionToVertexID.Reserve(CachedMeshData.Vertices.Num());

	TArray<FVertexInstanceID> InstanceIDs;
	InstanceIDs.SetNum(CachedMeshData.Vertices.Num());

	for (int32 i = 0; i < CachedMeshData.Vertices.Num(); ++i)
	{
		const FVector& Pos = CachedMeshData.Vertices[i];

		FVertexID VertexID;
		if (const FVertexID* Existing = PositionToVertexID.Find(Pos))
		{
			VertexID = *Existing;
		}
		else
		{
			VertexID = MeshDescription.CreateVertex();
			VertexPositions[VertexID] = FVector3f(Pos);
			PositionToVertexID.Add(Pos, VertexID);
		}

		const FVertexInstanceID InstanceID = MeshDescription.CreateVertexInstance(VertexID);
		InstanceNormals[InstanceID] = FVector3f(CachedMeshData.Normals[i]);
		InstanceTangents[InstanceID] = FVector3f(CachedMeshData.Tangents[i]);
		InstanceBinormalSigns[InstanceID] = 1.0f;
		InstanceColors[InstanceID] = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
		InstanceUVs.Set(InstanceID, 0, FVector2f(CachedMeshData.UVs[i]));

		InstanceIDs[i] = InstanceID;
	}

	for (int32 TriStart = 0; TriStart < CachedMeshData.Triangles.Num(); TriStart += 3)
	{
		const FVertexInstanceID Corners[3] =
		{
			InstanceIDs[CachedMeshData.Triangles[TriStart]],
			InstanceIDs[CachedMeshData.Triangles[TriStart + 1]],
			InstanceIDs[CachedMeshData.Triangles[TriStart + 2]],
		};
		MeshDescription.CreateTriangle(PolygonGroupID, Corners);
	}

	UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
	BuildParams.bBuildSimpleCollision = true;
	BuildParams.bFastBuild = false;
	BuildParams.bCommitMeshDescription = true;

	FMeshNaniteSettings NewNaniteSettings = NewStaticMesh->GetNaniteSettings();
	NewNaniteSettings.bEnabled = false; // flip on later once you're baking at final terrain density.
	NewStaticMesh->SetNaniteSettings(NewNaniteSettings);

	NewStaticMesh->BuildFromMeshDescriptions({ &MeshDescription }, BuildParams);

	NewStaticMesh->GetStaticMaterials().Empty();
	NewStaticMesh->GetStaticMaterials().Add(FStaticMaterial());

	NewStaticMesh->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewStaticMesh);
	Package->SetDirtyFlag(true);

	const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(Package, NewStaticMesh, *PackageFileName, SaveArgs);

	UE_LOG(LogSolarOrbz, Log, TEXT("SolarOrbz: baked '%s' -> %s"), *CleanAssetName, bSaved ? TEXT("saved to disk") : TEXT("created in memory but NOT saved"));
#endif
}
