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

class FToolBarBuilder;
class FMenuBuilder;
class ASolarOrbzIcoSphereActor;

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
// SSolarOrbzMainPanel - Main Slate panel: parametric icosphere controls, live preview spawning,
// and bake-to-static-mesh. This is the content dropped into the plugin's docking tab from
// FSolarOrbzModule::OnSpawnPluginTab below.
// ================================================================================================
class SOLARORBZ_API SSolarOrbzMainPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSolarOrbzMainPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	// --- Staged parameters, pushed onto the preview actor when Generate is pressed. ---
	double RadiusMeters = 1000.0;
	float VerticesPerMeter = 1.0f;
	int32 MaxSubdivisions = 6;
	bool bEnablePreviewCollision = false;

	FString BakePackagePath = TEXT("/Game/SolarOrbz/Meshes");
	FString BakeAssetName = TEXT("SM_IcoSphere");

	TWeakObjectPtr<ASolarOrbzIcoSphereActor> PreviewActor;

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
