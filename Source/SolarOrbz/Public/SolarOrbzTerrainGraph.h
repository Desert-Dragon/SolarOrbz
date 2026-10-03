// SolarOrbz - Terrain Graph Editor data model (Phase 1 of Docs/SolarOrbzTerrainGraphEditor.md).
// The classes here are a VIEW over USolarOrbzTerrainLayerStack::Layers, never a second source of
// truth - RebuildFromLayers()/CompileToLayers() are the only two places that cross between the
// array and the graph; every other evaluation code path (EvaluateHeight/Bake/etc.) never sees this
// file at all. No asset editor exists yet (that's Phase 2 - USolarOrbzTerrainLayerStackAssetDefinition
// + FSolarOrbzTerrainGraphEditorToolkit, not started) - these classes are unused by anything today.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphSchema.h"
#include "SolarOrbzTerrainGraph.generated.h"

class USolarOrbzTerrainLayer;
class USolarOrbzTerrainLayerStack;

// ================================================================================================
// USolarOrbzTerrainGraphNode - ONE generic node class for every entry in Layers, plus the fixed
// Start/Output sentinels - not one subclass per layer type. Wraps the real layer object by direct
// reference (never a copy), so selecting a node and editing it in the existing Details panel edits
// the real asset data with zero sync step. This is also why adding a brand new
// USolarOrbzTerrainLayer subclass later needs zero new graph-side code.
// ================================================================================================
UCLASS()
class SOLARORBZ_API USolarOrbzTerrainGraphNode : public UEdGraphNode
{
	GENERATED_BODY()

public:
	/** The real layer instance from USolarOrbzTerrainLayerStack::Layers this node represents. Null for the Start/Output sentinel nodes. */
	UPROPERTY()
	TObjectPtr<USolarOrbzTerrainLayer> Layer;

	/** True for the fixed "Start" sentinel (the base sphere before any layer) - no Height-In pin, no Layer. */
	UPROPERTY()
	bool bIsStartNode = false;

	/** True for the fixed "Output" sentinel (what the actor actually samples) - no Height-Out pin, no Layer. */
	UPROPERTY()
	bool bIsOutputNode = false;

	/** Pin category shared by every pin in this graph - the schema only ever deals in one pin shape (a height value flowing down the chain), so this is a label for diagnostics, not a type system. */
	static const FName HeightPinCategory;
	static const FName HeightInPinName;
	static const FName HeightOutPinName;

	//~ Begin UEdGraphNode interface
	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	//~ End UEdGraphNode interface

	/** This node's single Height-In pin, or nullptr for the Start node. */
	UEdGraphPin* GetHeightInPin() const;

	/** This node's single Height-Out pin, or nullptr for the Output node. */
	UEdGraphPin* GetHeightOutPin() const;
};

// ================================================================================================
// USolarOrbzTerrainGraphSchema - enforces the chain shape that matches the array's actual
// evaluation semantics: a Height-In/Height-Out pin connects to exactly one neighbor. Dragging a
// new wire onto a pin that already has one replaces it rather than allowing a second (the same
// posture K2's own input pins take), since the underlying model has no concept of branching yet -
// see "Why a strict chain, not an arbitrary node graph, for v1" in the design doc.
// ================================================================================================
UCLASS()
class SOLARORBZ_API USolarOrbzTerrainGraphSchema : public UEdGraphSchema
{
	GENERATED_BODY()

public:
	//~ Begin UEdGraphSchema interface
	virtual const FPinConnectionResponse CanCreateConnection(const UEdGraphPin* PinA, const UEdGraphPin* PinB) const override;
	//~ End UEdGraphSchema interface
};

// ================================================================================================
// USolarOrbzTerrainGraph - owned transiently by USolarOrbzTerrainLayerStack (see
// USolarOrbzTerrainLayerStack::TerrainGraph / GetOrCreateTerrainGraph), never serialized.
// RebuildFromLayers()/CompileToLayers() are the ONLY two places this ever touches Layers - every
// other graph operation (Phase 2's Add Node, reconnect, etc.) only ever edits the graph itself,
// then relies on CompileToLayers() to write the result back.
// ================================================================================================
UCLASS()
class SOLARORBZ_API USolarOrbzTerrainGraph : public UEdGraph
{
	GENERATED_BODY()

public:
	/** The Start sentinel node - the base sphere before any layer. Always present after RebuildFromLayers(). */
	UPROPERTY()
	TObjectPtr<USolarOrbzTerrainGraphNode> StartNode;

	/** The Output sentinel node - what the actor actually samples. Always present after RebuildFromLayers(). */
	UPROPERTY()
	TObjectPtr<USolarOrbzTerrainGraphNode> OutputNode;

	/**
	 * Discards every node this graph currently has and rebuilds from scratch by walking
	 * OwningStack's Layers in order - one USolarOrbzTerrainGraphNode per entry, wired
	 * Start -> Layers... -> Output. Call whenever the (future) asset editor opens, or after
	 * anything outside the graph itself (e.g. an undo/redo on the Layers array through the Details
	 * panel) might have changed Layers. Reuses OwningStack->EditorNodePositions for layout where a
	 * saved position exists for that layer's EditorNodeId, otherwise lays the new node out in an
	 * evenly-spaced row; also assigns an EditorNodeId to any layer that doesn't have one yet (e.g. a
	 * stack authored before this system existed).
	 */
	void RebuildFromLayers(USolarOrbzTerrainLayerStack* OwningStack);

	/**
	 * Walks the chain from StartNode to OutputNode and writes the visited nodes' Layer references
	 * into OwningStack->Layers in that order - this is what actually "applies" a reorder/add/remove
	 * made in the graph back onto the real asset data. Also snapshots each visited node's current
	 * canvas position into OwningStack->EditorNodePositions. Call after every structural graph edit
	 * (add node, remove node, reconnect) - never lazily, so Layers never drifts out of sync with
	 * what the graph currently shows. A chain broken partway through (e.g. mid-edit, before a new
	 * node is reconnected) simply truncates Layers at the break - the schema's one-connection-per-pin
	 * rule keeps this from happening except transiently during an edit.
	 */
	void CompileToLayers(USolarOrbzTerrainLayerStack* OwningStack) const;
};
