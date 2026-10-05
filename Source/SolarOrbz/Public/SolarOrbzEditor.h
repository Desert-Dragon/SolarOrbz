// SolarOrbz - Editor module & UI subsystem. Combines the plugin's module entry point
// (FSolarOrbzModule), Slate style set (FSolarOrbzStyle), toolbar/menu command list
// (FSolarOrbzCommands), and the main dock-tab panel (SSolarOrbzMainPanel) into one file,
// since none of them are UObjects/UCLASSes and they're all small, tightly-coupled editor
// plumbing with no reason to live in separate translation units.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Styling/SlateStyle.h"
#include "Framework/Commands/Commands.h"
#include "Widgets/SCompoundWidget.h"
#include "UObject/StrongObjectPtr.h"

class FToolBarBuilder;
class FMenuBuilder;
class FUICommandList;
class ASolarOrbzIcoSphereActor;
class AASolarOrbzChunkedPlanetActor;
class USolarOrbzPlanetSpawnerGraph;
class SGraphEditor;
class IDetailsView;

// ================================================================================================
// FSolarOrbzStyle - Slate style set (icons, brushes) for the plugin's toolbar button.
// ================================================================================================
class FSolarOrbzStyle
{
public:

	static void Initialize();

	static void Shutdown();

	/** reloads textures used by slate renderer */
	static void ReloadTextures();

	/** @return The Slate style set for the Shooter game */
	static const ISlateStyle& Get();

	static FName GetStyleSetName();

private:

	static TSharedRef< class FSlateStyleSet > Create();

private:

	static TSharedPtr< class FSlateStyleSet > StyleInstance;
};

// ================================================================================================
// FSolarOrbzCommands - toolbar/menu command list (the "Open SolarOrbz window" button).
// ================================================================================================
class FSolarOrbzCommands : public TCommands<FSolarOrbzCommands>
{
public:

	FSolarOrbzCommands()
		: TCommands<FSolarOrbzCommands>(TEXT("SolarOrbz"), NSLOCTEXT("Contexts", "SolarOrbz", "SolarOrbz Plugin"), NAME_None, FSolarOrbzStyle::GetStyleSetName())
	{
	}

	// TCommands<> interface
	virtual void RegisterCommands() override;

public:
	TSharedPtr< FUICommandInfo > OpenPluginWindow;
};

// ================================================================================================
// SSolarOrbzMainPanel - Main Slate panel: one node graph (Docs/SolarOrbzPlanetSpawnerGraph.md) for
// staging a planet - fixed Base Sphere/Biome Stack/Climate Simulation/Profile module nodes feeding
// a "Planet" sentinel, plus the embedded terrain layer chain (Start -> Layer -> ... -> Output,
// grown with the "Add Layer" button) living on the same canvas - next to live preview spawning and
// bake-to-static-mesh. This is the content dropped into the plugin's docking tab from
// FSolarOrbzModule::OnSpawnPluginTab below.
// ================================================================================================
class SOLARORBZ_API SSolarOrbzMainPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSolarOrbzMainPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	// --- The fixed planet-module graph: owned here via a strong ref since there's no asset or
	// other UPROPERTY chain to anchor it (it's pure in-memory editor scratch state for the next
	// Generate/Apply Row click, same lifetime as this panel). ---
	TStrongObjectPtr<USolarOrbzPlanetSpawnerGraph> SpawnerGraph;

	TSharedPtr<SGraphEditor> GraphEditorWidget;
	TSharedPtr<IDetailsView> DetailsView;

	/** Maps FGenericCommands::Get().Delete to DeleteSelectedTerrainNodes()/CanDeleteSelectedTerrainNodes() - passed to SGraphEditor's .AdditionalCommands() in Construct(). */
	TSharedPtr<FUICommandList> GraphEditorCommands;

	FString BakePackagePath = TEXT("/Game/SolarOrbz/Meshes");
	FString BakeAssetName = TEXT("SM_IcoSphere");

	TWeakObjectPtr<ASolarOrbzIcoSphereActor> PreviewActor;

	/**
	 * The chunked, planet-scale preview actor the "Generate Chunked Preview" button below drives -
	 * shares the SAME Base Sphere radius and embedded terrain chain as PreviewActor above, so "does
	 * this terrain recipe look good enough at real planetary scale, with real ground-level detail"
	 * can be checked without hand-configuring a second actor from scratch. See OnGenerateChunkedClicked().
	 */
	TWeakObjectPtr<AASolarOrbzChunkedPlanetActor> ChunkedPreviewActor;

	/** Reflection-driven over non-abstract USolarOrbzTerrainLayer subclasses, mirroring FSolarOrbzTerrainGraphEditorToolkit::BuildAddLayerMenu - adding a new layer type needs zero new code here either. */
	TSharedRef<SWidget> BuildAddLayerMenu();

	/** Appends a new instance of LayerClass to SpawnerGraph->EmbeddedTerrainStack->Layers and rebuilds the embedded chain to show it. */
	void AddLayerOfClass(UClass* LayerClass);

	void HandleGraphSelectionChanged(const TSet<UObject*>& NewSelection);

	/** Fired by SpawnerGraph on every add/remove/reconnect, including ones RebuildEmbeddedTerrainChain() itself makes - guarded by SpawnerGraph->IsRebuildingTerrainChain() so only genuine interactive edits reach CompileEmbeddedTerrainChain(). */
	void HandleSpawnerGraphChanged(const struct FEdGraphEditAction& Action);

	/**
	 * Bound to FGenericCommands::Get().Delete. Removes every selected node that is a real terrain
	 * layer (USolarOrbzTerrainGraphNode, skipping the fixed Start/Output sentinels) - a selected
	 * fixed module/Planet node (USolarOrbzPlanetSpawnerGraphNode) is simply left alone, since this
	 * graph's fixed topology (Base Sphere/Biome/Climate/Profile -> Planet) stays non-editable by
	 * design, only the embedded terrain chain is. SpawnerGraph->RemoveNode() already broadcasts the
	 * same OnGraphChanged notification AddNode() does, so HandleSpawnerGraphChanged above recompiles
	 * EmbeddedTerrainStack->Layers for each removal automatically.
	 */
	void DeleteSelectedTerrainNodes();

	/** Enables the Delete command only when at least one selected node is a real (non-sentinel) terrain layer node. */
	bool CanDeleteSelectedTerrainNodes() const;

	FReply OnGenerateClicked();
	FReply OnBakeClicked();
	FReply OnClearPreviewClicked();

	FText GetStatsText() const;
	bool IsPreviewValid() const;

	/** Spawns (first click) or rebuilds (later clicks) ChunkedPreviewActor from the same Base Sphere radius/embedded terrain chain/Biome/Climate/Profile the simple preview above uses, defaults a north-pole viewer vantage if none is set yet, then calls its RebuildChunkedPlanetNow() so chunks appear immediately without entering Play. */
	FReply OnGenerateChunkedClicked();
	FReply OnClearChunkedPreviewClicked();

	FText GetChunkedStatsText() const;
	bool IsChunkedPreviewValid() const;

	/**
	 * Adopts whichever SolarOrbz planet actor (ASolarOrbzIcoSphereActor or
	 * AASolarOrbzChunkedPlanetActor - whichever is found first in the current level selection) is
	 * currently selected: points PreviewActor/ChunkedPreviewActor at it and pulls its Radius/
	 * TerrainStack-Layers/Biome/Climate/Profile back into this panel's graph (BaseSphereConfig/
	 * EmbeddedTerrainStack/BiomeModule/ClimateModule/ProfileModule), then rebuilds the embedded
	 * terrain chain from whatever Layers that stack holds. This is the reverse direction of
	 * Generate - Generate pushes the panel's staged graph onto an actor, this pulls an already-placed
	 * actor's current configuration back into the panel so it can be edited further from here,
	 * without hand-rebuilding the recipe from scratch. Does nothing (just logs) if the selection has
	 * neither actor type.
	 */
	FReply OnUseSelectedActorClicked();
};

// ================================================================================================
// FSolarOrbzModule - plugin module entry point: registers the style/commands/menu entry and
// spawns the dock tab containing SSolarOrbzMainPanel above.
// ================================================================================================
class FSolarOrbzModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** This function will be bound to Command (by default it will bring up plugin window) */
	void PluginButtonClicked();

private:

	void RegisterMenus();

	TSharedRef<class SDockTab> OnSpawnPluginTab(const class FSpawnTabArgs& SpawnTabArgs);

private:
	TSharedPtr<class FUICommandList> PluginCommands;
};
