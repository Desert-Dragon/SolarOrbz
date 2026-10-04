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
class ASolarOrbzIcoSphereActor;
class USolarOrbzPlanetSpawnerGraph;
class UDataTable;
struct FAssetData;
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
// SSolarOrbzMainPanel - Main Slate panel: a fixed node graph (Docs/SolarOrbzPlanetSpawnerGraph.md)
// for staging a planet's base information - Base Sphere/Terrain Stack/Biome Stack/Climate
// Simulation/Profile, each its own selectable node feeding a "Planet" sentinel - plus an optional
// Planet Catalog Data Table to fill those nodes from a prebuilt row, live preview spawning, and
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

	// --- Optional: a DataTable of FSolarOrbzPlanetCatalogRow, to fill the graph above from a
	// prebuilt row instead of authoring every field by hand. ---
	TStrongObjectPtr<UDataTable> CatalogDataTable;

	FString BakePackagePath = TEXT("/Game/SolarOrbz/Meshes");
	FString BakeAssetName = TEXT("SM_IcoSphere");

	TWeakObjectPtr<ASolarOrbzIcoSphereActor> PreviewActor;

	void OnCatalogDataTableChanged(const FAssetData& NewAssetData);
	TSharedRef<SWidget> BuildApplyRowMenu();
	void ApplyCatalogRow(FName RowName);

	void HandleGraphSelectionChanged(const TSet<UObject*>& NewSelection);

	FReply OnGenerateClicked();
	FReply OnBakeClicked();
	FReply OnClearPreviewClicked();

	FText GetStatsText() const;
	bool IsPreviewValid() const;
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
