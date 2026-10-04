// SolarOrbz - Terrain Graph Editor asset editor UI (Phase 2 of Docs/SolarOrbzTerrainGraphEditor.md).
// Combines the asset-registration entry point (USolarOrbzTerrainLayerStackAssetDefinition) and the
// actual editor window (FSolarOrbzTerrainGraphEditorToolkit) into one file - the same "small,
// tightly-coupled editor plumbing" grouping SolarOrbzEditor.h already uses for the plugin's own
// style/commands/panel/module. Neither class here ever touches
// USolarOrbzTerrainLayerStack::Layers directly - both only ever go through
// USolarOrbzTerrainGraph::RebuildFromLayers()/CompileToLayers() (Phase 1, SolarOrbzTerrainGraph.h).

#pragma once

#include "CoreMinimal.h"
#include "AssetDefinition.h"
#include "AssetDefinitionDefault.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "SolarOrbzTerrainGraphEditorToolkit.generated.h"

class IDetailsView;
class SDockTab;
class SGraphEditor;
class SWidget;
class USolarOrbzTerrainGraph;
class USolarOrbzTerrainLayerStack;
struct FEdGraphEditAction;

// ================================================================================================
// USolarOrbzTerrainLayerStackAssetDefinition - the modern UE5.8 asset registration point (Epic's
// current replacement for the older IAssetTypeActions/FAssetTypeActions_Base pattern -
// UAssetDefinitions register automatically with UAssetDefinitionRegistry, no manual registration
// call needed anywhere in this module). Makes double-clicking a USolarOrbzTerrainLayerStack asset
// open FSolarOrbzTerrainGraphEditorToolkit below instead of the generic property matrix.
// ================================================================================================
UCLASS()
class SOLARORBZ_API USolarOrbzTerrainLayerStackAssetDefinition : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:
	//~ Begin UAssetDefinition interface
	virtual FText GetAssetDisplayName() const override;
	virtual FLinearColor GetAssetColor() const override;
	virtual TSoftClassPtr<UObject> GetAssetClass() const override;
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override;
	virtual EAssetCommandResult OpenAssets(const FAssetOpenArgs& OpenArgs) const override;
	//~ End UAssetDefinition interface
};

// ================================================================================================
// FSolarOrbzTerrainGraphEditorToolkit - the actual editor window: an SGraphEditor (bound to
// USolarOrbzTerrainGraph) docked next to an IDetailsView showing whichever node is selected.
// Because a node references the real layer object directly (Phase 1), selecting a node is all
// that's needed to get the exact same property editing experience the Layers array widget already
// provided in the generic editor - there is zero new property UI code here.
//
// "Add Node" is a toolbar combo button (BuildAddLayerMenu), not a native graph right-click context
// menu - a deliberate simplification. Wiring a custom right-click "create node" action menu goes
// through SGraphEditor's OnCreateActionMenu delegate, whose exact signature could not be pinned
// down against documented UE5.8 sources with the confidence this project's verification standard
// calls for; a toolbar dropdown reaches the same outcome (reflection-driven, zero per-layer-type
// code) through APIs (SComboButton + GetDerivedClasses) already proven correct elsewhere in this
// codebase (SolarOrbzEditor.h's SSolarOrbzMainPanel).
// ================================================================================================
class FSolarOrbzTerrainGraphEditorToolkit : public FAssetEditorToolkit
{
public:
	virtual ~FSolarOrbzTerrainGraphEditorToolkit() override;

	/** Opens this toolkit for StackToEdit - called from USolarOrbzTerrainLayerStackAssetDefinition::OpenAssets. */
	void InitEditor(const EToolkitMode::Type Mode, const TSharedPtr<class IToolkitHost>& InitToolkitHost, USolarOrbzTerrainLayerStack* StackToEdit);

	//~ Begin FAssetEditorToolkit interface
	virtual void RegisterTabSpawners(const TSharedRef<class FTabManager>& InTabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<class FTabManager>& InTabManager) override;
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;
	//~ End FAssetEditorToolkit interface

private:
	TSharedRef<SDockTab> SpawnGraphTab(const class FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnDetailsTab(const class FSpawnTabArgs& Args);

	/** The Graph tab's content: a thin "Add Layer" toolbar row above the SGraphEditor canvas. */
	TSharedRef<SWidget> BuildGraphTabContent();

	/** Reflection-driven over non-abstract USolarOrbzTerrainLayer subclasses - adding a brand new layer type needs zero new code here, same promise the design doc makes for a native context menu. */
	TSharedRef<SWidget> BuildAddLayerMenu();

	/** Appends a new instance of LayerClass to EditingStack->Layers and rebuilds the graph to show it. */
	void AddLayerOfClass(UClass* LayerClass);

	/** Pushes the selected node(s)' wrapped layer object(s) into DetailsView - the node IS the layer, so this is the only sync step selection needs. Takes the plain TSet<UObject*> that FGraphPanelSelectionSet is typedef'd to, rather than that typedef itself, so this header doesn't need to pull in SGraphEditor's own header just to name the parameter type. */
	void HandleSelectionChanged(const TSet<UObject*>& NewSelection);

	/** Fired by TerrainGraph on every add/remove/reconnect, including ones RebuildFromLayers() itself makes - guarded by TerrainGraph->IsRebuilding() so only genuine interactive edits reach CompileToLayers(). */
	void HandleGraphChanged(const FEdGraphEditAction& Action);

	/** The stack this toolkit is editing. Kept alive by the asset editor subsystem via the ObjectsToEdit list passed to InitAssetEditor() - this is a convenience cache, not an extra GC root. */
	USolarOrbzTerrainLayerStack* EditingStack = nullptr;

	/** EditingStack->GetOrCreateTerrainGraph()'s result, cached - reachable (and kept alive) through EditingStack's own TerrainGraph UPROPERTY, so this too is a convenience cache, not an extra GC root. */
	USolarOrbzTerrainGraph* TerrainGraph = nullptr;

	TSharedPtr<SGraphEditor> GraphEditorWidget;
	TSharedPtr<IDetailsView> DetailsView;

	FDelegateHandle GraphChangedDelegateHandle;

	static const FName GraphCanvasTabId;
	static const FName DetailsTabId;
};
