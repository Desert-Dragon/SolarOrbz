// Copyright Epic Games, Inc. All Rights Reserved.

#include "SolarOrbz.h"
#include "SolarOrbzStyle.h"
#include "SolarOrbzCommands.h"
#include "LevelEditor.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "ToolMenus.h"
#include "SolarOrbzMainPanel.h"

static const FName SolarOrbzTabName("SolarOrbz");

#define LOCTEXT_NAMESPACE "FSolarOrbzModule"

void FSolarOrbzModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	
	FSolarOrbzStyle::Initialize();
	FSolarOrbzStyle::ReloadTextures();

	FSolarOrbzCommands::Register();
	
	PluginCommands = MakeShareable(new FUICommandList);

	PluginCommands->MapAction(
		FSolarOrbzCommands::Get().OpenPluginWindow,
		FExecuteAction::CreateRaw(this, &FSolarOrbzModule::PluginButtonClicked),
		FCanExecuteAction());

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FSolarOrbzModule::RegisterMenus));
	
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(SolarOrbzTabName, FOnSpawnTab::CreateRaw(this, &FSolarOrbzModule::OnSpawnPluginTab))
		.SetDisplayName(LOCTEXT("FSolarOrbzTabTitle", "SolarOrbz"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void FSolarOrbzModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.

	UToolMenus::UnRegisterStartupCallback(this);

	UToolMenus::UnregisterOwner(this);

	FSolarOrbzStyle::Shutdown();

	FSolarOrbzCommands::Unregister();

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(SolarOrbzTabName);
}

TSharedRef<SDockTab> FSolarOrbzModule::OnSpawnPluginTab(const FSpawnTabArgs& SpawnTabArgs)
{
	FText WidgetText = FText::Format(
		LOCTEXT("WindowWidgetText", "Add code to {0} in {1} to override this window's contents"),
		FText::FromString(TEXT("FSolarOrbzModule::OnSpawnPluginTab")),
		FText::FromString(TEXT("SolarOrbz.cpp"))
		);

	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SSolarOrbzMainPanel)
		];
}

void FSolarOrbzModule::PluginButtonClicked()
{
	FGlobalTabmanager::Get()->TryInvokeTab(SolarOrbzTabName);
}

void FSolarOrbzModule::RegisterMenus()
{
	// Owner will be used for cleanup in call to UToolMenus::UnregisterOwner
	FToolMenuOwnerScoped OwnerScoped(this);

	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
		{
			FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");
			Section.AddMenuEntryWithCommandList(FSolarOrbzCommands::Get().OpenPluginWindow, PluginCommands);
		}
	}

	{
		UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
		{
			FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("PluginTools");
			{
				FToolMenuEntry& Entry = Section.AddEntry(FToolMenuEntry::InitToolBarButton(FSolarOrbzCommands::Get().OpenPluginWindow));
				Entry.SetCommandList(PluginCommands);
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FSolarOrbzModule, SolarOrbz)