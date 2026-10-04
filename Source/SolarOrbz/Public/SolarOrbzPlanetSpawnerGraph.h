// SolarOrbz - Planet Spawner graph (Docs/SolarOrbzPlanetSpawnerGraph.md). A node graph for the
// plugin's own SolarOrbz dock tab (SSolarOrbzMainPanel) - NOT an asset editor, and NOT a view over
// a persisted asset the way the Terrain Graph Editor is over Layers (the embedded terrain chain
// below is the one exception - see its own comment). This graph's nodes and their module config
// objects are purely in-memory editor scratch state, owned by the open panel (kept alive via a
// TStrongObjectPtr there, not by any asset or UPROPERTY chain).
//
// Four FIXED module nodes (Base Sphere, Biome Stack, Climate Simulation, Profile) feed a fixed
// "Planet" sentinel - those connections are built once by BuildFixedLayout() and are not
// user-editable, because there is no real ordering/combination semantic between a planet's modules
// the way there is between terrain layers (each one is an independent assignment, not a step in a
// blend chain).
//
// Terrain is different: instead of a single "Terrain Stack" node referencing an external
// USolarOrbzTerrainLayerStack asset, the actual terrain layer authoring chain - the same
// Start -> Layer -> ... -> Output mechanism the standalone Terrain Graph Editor uses
// (SolarOrbzTerrainGraph.h), noise layers included - is embedded directly into THIS graph's canvas,
// reusing USolarOrbzTerrainGraphNode (it's graph-agnostic) rather than requiring a separate asset.
// Biome Stack stays a plain external asset reference, by explicit design choice - only Terrain gets
// inlined.
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
class USolarOrbzTerrainGraphNode;

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
// USolarOrbzPlanetSpawnerGraphSchema - the four fixed module nodes + Planet sentinel's topology is
// fixed by BuildFixedLayout() and never user-editable; any pin using
// USolarOrbzPlanetSpawnerGraphNode::ModulePinCategory always disallows a new connection. The
// embedded terrain chain's pins (USolarOrbzTerrainGraphNode::HeightPinCategory) are the exception -
// those follow the same single-connection/replace-on-reconnect rule as the standalone Terrain Graph
// Editor's schema (duplicated here rather than inherited, since this graph owns a type-filtered mix
// of both node kinds and reconnection must never touch the fixed module topology).
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
// Terrain Graph Editor's graph is anchored by the stack it's rebuilt from). Generate logic in the
// panel reads BaseSphereConfig/EmbeddedTerrainStack/etc. directly - never by walking Nodes - so
// deleting a fixed module node in the graph (Delete key) only hides that module's Details-panel
// entry point, it doesn't lose the underlying config object or break Generate; Ctrl+Z restores the
// node. The embedded terrain chain is the exception: that part of Nodes IS the source of truth
// (EmbeddedTerrainStack->Layers is written back from it by CompileEmbeddedTerrainChain(), the same
// relationship USolarOrbzTerrainGraph has with its owning stack), so editing/deleting those nodes
// really does change what Generate will bake.
// ================================================================================================
UCLASS()
class SOLARORBZ_API USolarOrbzPlanetSpawnerGraph : public UEdGraph
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TObjectPtr<USolarOrbzPlanetBaseSphereConfig> BaseSphereConfig;

	UPROPERTY()
	TObjectPtr<USolarOrbzPlanetBiomeModule> BiomeModule;

	UPROPERTY()
	TObjectPtr<USolarOrbzPlanetClimateModule> ClimateModule;

	UPROPERTY()
	TObjectPtr<USolarOrbzPlanetProfileModule> ProfileModule;

	UPROPERTY()
	TObjectPtr<USolarOrbzPlanetSpawnerGraphNode> PlanetNode;

	/**
	 * The terrain recipe this graph's embedded Start->Layer->...->Output chain edits. Transient,
	 * created fresh by BuildFixedLayout() (never loaded/saved as an asset) - RebuildEmbeddedTerrainChain()/
	 * CompileEmbeddedTerrainChain() are the only two places that cross between this object's Layers
	 * array and the chain of USolarOrbzTerrainGraphNode instances living in this graph's own Nodes,
	 * exactly mirroring USolarOrbzTerrainGraph's relationship to an external USolarOrbzTerrainLayerStack.
	 */
	UPROPERTY()
	TObjectPtr<USolarOrbzTerrainLayerStack> EmbeddedTerrainStack;

	/** The embedded chain's Start sentinel. Always present after RebuildEmbeddedTerrainChain(). */
	UPROPERTY()
	TObjectPtr<USolarOrbzTerrainGraphNode> EmbeddedTerrainStartNode;

	/** The embedded chain's Output sentinel. Always present after RebuildEmbeddedTerrainChain(). */
	UPROPERTY()
	TObjectPtr<USolarOrbzTerrainGraphNode> EmbeddedTerrainOutputNode;

	/**
	 * Builds the four fixed module nodes plus the fixed Planet sentinel and wires them, then calls
	 * RebuildEmbeddedTerrainChain() to seed the (initially empty) terrain chain. Call once, right
	 * after constructing this graph - this is a one-time setup, not a resync like
	 * RebuildEmbeddedTerrainChain() itself: calling it again would create fresh, empty config objects
	 * and discard whatever was staged in the old ones, since there's no external array the fixed
	 * module nodes are a view over.
	 */
	void BuildFixedLayout();

	/**
	 * Discards every USolarOrbzTerrainGraphNode currently in this graph's Nodes and rebuilds the
	 * chain from EmbeddedTerrainStack->Layers - the same algorithm as
	 * USolarOrbzTerrainGraph::RebuildFromLayers(), duplicated rather than inherited so it can filter
	 * Nodes by type (Cast<USolarOrbzTerrainGraphNode>) and leave this graph's fixed
	 * USolarOrbzPlanetSpawnerGraphNode module/Planet nodes - which share the same Nodes array -
	 * completely untouched. Call whenever EmbeddedTerrainStack->Layers might have changed outside
	 * this graph (there's currently no such path, but this mirrors RebuildFromLayers() for the same
	 * reason it exists there: cheap insurance against future drift).
	 */
	void RebuildEmbeddedTerrainChain();

	/**
	 * Walks the chain from EmbeddedTerrainStartNode to EmbeddedTerrainOutputNode and writes the
	 * visited nodes' Layer references into EmbeddedTerrainStack->Layers in that order, snapshotting
	 * positions into EmbeddedTerrainStack->EditorNodePositions - the same algorithm as
	 * USolarOrbzTerrainGraph::CompileToLayers(), duplicated for the same node-type-filtering reason
	 * RebuildEmbeddedTerrainChain() is. Call after every structural edit to the terrain chain (add
	 * layer, reconnect, delete) - never lazily.
	 */
	void CompileEmbeddedTerrainChain() const;

	/** True while RebuildEmbeddedTerrainChain() is actively tearing down/recreating terrain-chain nodes - mirrors USolarOrbzTerrainGraph::IsRebuilding(), see that comment for why a caller needs to check this before compiling. */
	bool IsRebuildingTerrainChain() const { return bIsRebuildingTerrainChain; }

private:
	bool bIsRebuildingTerrainChain = false;
};
