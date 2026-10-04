// SolarOrbz - Planet Spawner graph (Docs/SolarOrbzPlanetSpawnerGraph.md). A fixed-topology node
// graph for the plugin's own SolarOrbz dock tab (SSolarOrbzMainPanel) - NOT an asset editor, and
// NOT a view over a persisted asset the way the Terrain Graph Editor is over Layers. This graph's
// nodes and their module config objects are purely in-memory editor scratch state, owned by the
// open panel (kept alive via a TStrongObjectPtr there, not by any asset or UPROPERTY chain). Five
// module nodes (Base Sphere, Terrain Stack, Biome Stack, Climate Simulation, Profile) feed a fixed
// "Planet" sentinel - the connections are built once by BuildFixedLayout() and are not
// user-editable, because there is no real ordering/combination semantic between a planet's modules
// the way there is between terrain layers (each one is an independent assignment, not a step in a
// blend chain). So this graph is a readable layout of what's currently assigned, not an editable
// chain - the only interactions are dragging nodes around and selecting one to edit its module's
// fields in the Details panel, exactly one concern at a time.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphSchema.h"
#include "SolarOrbzPlanetSpawnerGraph.generated.h"

class USolarOrbzTerrainLayerStack;
class USolarOrbzBiomeStack;
class USolarOrbzClimateSimulationAsset;
class USolarOrbzCelestialBodyProfile;

/** Shared by every class in this file - filtering the Output Log for this shows the whole planet-spawner-graph story for one editor session. */
SOLARORBZ_API DECLARE_LOG_CATEGORY_EXTERN(LogSolarOrbzPlanetSpawner, Log, All);

// ================================================================================================
// Module config objects - one small UObject per node, each holding exactly the properties that
// module needs. Transient, in-memory only (never saved, never an asset) - owned by the graph that
// built them (USolarOrbzPlanetSpawnerGraph below), which is in turn owned by the panel.
// ================================================================================================

UCLASS()
class SOLARORBZ_API USolarOrbzPlanetBaseSphereConfig : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|IcoSphere", meta = (ClampMin = "0.01"))
	double RadiusMeters = 1000.0;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|IcoSphere", meta = (ClampMin = "0.001"))
	float VerticesPerMeter = 1.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|IcoSphere", meta = (ClampMin = "0", ClampMax = "11", UIMax = "9"))
	int32 MaxSubdivisions = 6;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|IcoSphere")
	bool bEnablePreviewCollision = false;
};

UCLASS()
class SOLARORBZ_API USolarOrbzPlanetTerrainModule : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Terrain")
	TObjectPtr<USolarOrbzTerrainLayerStack> TerrainStack;
};

UCLASS()
class SOLARORBZ_API USolarOrbzPlanetBiomeModule : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Biome")
	TObjectPtr<USolarOrbzBiomeStack> BiomeStack;
};

UCLASS()
class SOLARORBZ_API USolarOrbzPlanetClimateModule : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate")
	TObjectPtr<USolarOrbzClimateSimulationAsset> ClimateSimulation;
};

UCLASS()
class SOLARORBZ_API USolarOrbzPlanetProfileModule : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Profile")
	TObjectPtr<USolarOrbzCelestialBodyProfile> Profile;
};

// ================================================================================================
// USolarOrbzPlanetSpawnerGraphNode - ONE generic node class, the same "wraps a real object
// directly" pattern as the Terrain Graph Editor's node - selecting it and editing via the Details
// panel edits the real config object, no sync step. ModuleConfig is null only for the fixed
// "Planet" sentinel node.
// ================================================================================================
UCLASS()
class SOLARORBZ_API USolarOrbzPlanetSpawnerGraphNode : public UEdGraphNode
{
	GENERATED_BODY()

public:
	/** The module config object this node represents. Null for the fixed Planet sentinel node. */
	UPROPERTY()
	TObjectPtr<UObject> ModuleConfig;

	/** True for the fixed "Planet" sentinel - no config, no output pin; every module node's pin connects here instead. */
	UPROPERTY()
	bool bIsPlanetNode = false;

	/** Set directly by BuildFixedLayout() - there's no polymorphic "layer type" to derive a title from here, every node's shape is already known. */
	UPROPERTY()
	FText DisplayTitle;

	/** Pin category shared by every pin in this graph - like the Terrain Graph's, this is a label for diagnostics, not a type system (CanCreateConnection disallows everything regardless). */
	static const FName ModulePinCategory;
	static const FName ModuleOutPinName;

	//~ Begin UEdGraphNode interface
	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	//~ End UEdGraphNode interface

	/** This node's single output pin. Only valid when !bIsPlanetNode. */
	UEdGraphPin* GetModuleOutPin() const;

	/** One of the Planet sentinel's five named input pins ("Base Sphere"/"Terrain"/"Biome"/"Climate"/"Profile"). Only valid when bIsPlanetNode. */
	UEdGraphPin* GetPlanetInputPin(FName InputPinName) const;
};

// ================================================================================================
// USolarOrbzPlanetSpawnerGraphSchema - the topology is fixed by BuildFixedLayout() and never
// user-editable: CanCreateConnection always disallows. There is no "Add Node"/"Remove Node" either
// - the five module nodes and the Planet sentinel are the whole graph, always.
// ================================================================================================
UCLASS()
class SOLARORBZ_API USolarOrbzPlanetSpawnerGraphSchema : public UEdGraphSchema
{
	GENERATED_BODY()

public:
	//~ Begin UEdGraphSchema interface
	virtual const FPinConnectionResponse CanCreateConnection(const UEdGraphPin* PinA, const UEdGraphPin* PinB) const override;
	//~ End UEdGraphSchema interface
};

// ================================================================================================
// USolarOrbzPlanetSpawnerGraph - built once by BuildFixedLayout(), owned by the panel (via a
// TStrongObjectPtr there, since there's no asset or other UPROPERTY chain to anchor it the way the
// Terrain Graph Editor's graph is anchored by the stack it's rebuilt from). Generate/Apply Row
// logic in the panel reads BaseSphereConfig/TerrainModule/etc. directly - never by walking Nodes -
// so deleting a node in the graph (Delete key) only hides that module's Details-panel entry point,
// it doesn't lose the underlying config object or break Generate; Ctrl+Z restores the node.
// ================================================================================================
UCLASS()
class SOLARORBZ_API USolarOrbzPlanetSpawnerGraph : public UEdGraph
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TObjectPtr<USolarOrbzPlanetBaseSphereConfig> BaseSphereConfig;

	UPROPERTY()
	TObjectPtr<USolarOrbzPlanetTerrainModule> TerrainModule;

	UPROPERTY()
	TObjectPtr<USolarOrbzPlanetBiomeModule> BiomeModule;

	UPROPERTY()
	TObjectPtr<USolarOrbzPlanetClimateModule> ClimateModule;

	UPROPERTY()
	TObjectPtr<USolarOrbzPlanetProfileModule> ProfileModule;

	UPROPERTY()
	TObjectPtr<USolarOrbzPlanetSpawnerGraphNode> PlanetNode;

	/**
	 * Builds the five module nodes plus the fixed Planet sentinel and wires them. Call once, right
	 * after constructing this graph - this is a one-time setup, not a resync like the Terrain Graph
	 * Editor's RebuildFromLayers(): calling it again would create fresh, empty config objects and
	 * discard whatever was staged in the old ones, since there's no external array this graph is a
	 * view over.
	 */
	void BuildFixedLayout();
};
