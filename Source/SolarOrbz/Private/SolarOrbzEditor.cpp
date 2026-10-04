// SolarOrbz - Editor module & UI subsystem implementation.

#include "SolarOrbzEditor.h"

#include "Styling/SlateStyleRegistry.h"
#include "Framework/Application/SlateApplication.h"
#include "Slate/SlateGameResources.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyleMacros.h"

#include "SolarOrbzIcoSphere.h"
#include "SolarOrbzPlanetSpawnerGraph.h"
#include "SolarOrbzTerrainGraph.h"
#include "SolarOrbzTerrainLayers.h"

#include "LevelEditor.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Text/STextBlock.h"
#include "ToolMenus.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "GraphEditor.h"
#include "PropertyEditorModule.h"
#include "IDetailsView.h"
#include "Framework/Commands/UICommandList.h"

// ================================================================================================
// FSolarOrbzStyle
// ================================================================================================
#define RootToContentDir Style->RootToContentDir

TSharedPtr<FSlateStyleSet> FSolarOrbzStyle::StyleInstance = nullptr;

void FSolarOrbzStyle::Initialize()
{
	if (!StyleInstance.IsValid())
	{
		StyleInstance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
	}
}

void FSolarOrbzStyle::Shutdown()
{
	FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
	ensure(StyleInstance.IsUnique());
	StyleInstance.Reset();
}

FName FSolarOrbzStyle::GetStyleSetName()
{
	static FName StyleSetName(TEXT("SolarOrbzStyle"));
	return StyleSetName;
}

const FVector2D Icon16x16(16.0f, 16.0f);
const FVector2D Icon20x20(20.0f, 20.0f);

TSharedRef< FSlateStyleSet > FSolarOrbzStyle::Create()
{
	TSharedRef< FSlateStyleSet > Style = MakeShareable(new FSlateStyleSet("SolarOrbzStyle"));
	Style->SetContentRoot(IPluginManager::Get().FindPlugin("SolarOrbz")->GetBaseDir() / TEXT("Resources"));

	// A distinct icon, not Epic's shared template placeholder - another plugin in this project
	// (ASNMechLab) left its own toolbar button on that same unmodified placeholder too, and both
	// buttons land in the same "LevelEditor.LevelEditorToolBar.PlayToolBar" / "PluginTools" section,
	// so two identical-looking icons there made it easy to click the wrong one.
	Style->Set("SolarOrbz.OpenPluginWindow", new IMAGE_BRUSH_SVG(TEXT("SolarOrbzButtonIcon"), Icon20x20));

	return Style;
}

void FSolarOrbzStyle::ReloadTextures()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
	}
}

const ISlateStyle& FSolarOrbzStyle::Get()
{
	return *StyleInstance;
}

#undef RootToContentDir

// ================================================================================================
// FSolarOrbzCommands
// ================================================================================================
#define LOCTEXT_NAMESPACE "FSolarOrbzModule"

void FSolarOrbzCommands::RegisterCommands()
{
	UI_COMMAND(OpenPluginWindow, "SolarOrbz", "Bring up SolarOrbz window", EUserInterfaceActionType::Button, FInputChord());
}

#undef LOCTEXT_NAMESPACE

// ================================================================================================
// SSolarOrbzMainPanel
// ================================================================================================
#define LOCTEXT_NAMESPACE "SolarOrbzMainPanel"

namespace SolarOrbzUI
{
	// Small helper so labeled rows don't repeat the same padding/width boilerplate everywhere.
	TSharedRef<SWidget> MakeLabeledRow(const FText& Label, TSharedRef<SWidget> Content)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(0.45f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Label)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.55f)
			.VAlign(VAlign_Center)
			[
				Content
			];
	}

	TSharedRef<SWidget> MakeSectionHeader(const FText& Label)
	{
		return SNew(SBox)
			.Padding(FMargin(0.0f, 8.0f, 0.0f, 4.0f))
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(FCoreStyle::Get().GetFontStyle("BoldFont"))
			];
	}
}

void SSolarOrbzMainPanel::Construct(const FArguments& InArgs)
{
	using namespace SolarOrbzUI;

	SpawnerGraph.Reset(NewObject<USolarOrbzPlanetSpawnerGraph>(GetTransientPackage(), NAME_None, RF_Transient));
	SpawnerGraph->BuildFixedLayout();

	SpawnerGraph->AddOnGraphChangedHandler(FOnGraphChanged::FDelegate::CreateSP(this, &SSolarOrbzMainPanel::HandleSpawnerGraphChanged));

	SGraphEditor::FGraphEditorEvents GraphEvents;
	GraphEvents.OnSelectionChanged = SGraphEditor::FOnSelectionChanged::CreateSP(this, &SSolarOrbzMainPanel::HandleGraphSelectionChanged);

	GraphEditorWidget = SNew(SGraphEditor)
		.AdditionalCommands(MakeShared<FUICommandList>())
		.GraphToEdit(SpawnerGraph.Get())
		.GraphEvents(GraphEvents);

	FPropertyEditorModule& PropertyEditorModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs DetailsArgs;
	DetailsArgs.bAllowSearch = true;
	DetailsView = PropertyEditorModule.CreateDetailView(DetailsArgs);

	ChildSlot
	[
		SNew(SBox)
		.Padding(FMargin(12.0f))
		[
			SNew(SVerticalBox)

			// --- Planet graph toolbar: grows the embedded terrain chain below ---
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SComboButton)
					.OnGetMenuContent(this, &SSolarOrbzMainPanel::BuildAddLayerMenu)
					.ButtonContent()
					[
						SNew(STextBlock).Text(LOCTEXT("AddLayerButton", "Add Layer"))
					]
				]
			]

			// --- Planet module graph + Details ---
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SBox)
				.MinDesiredHeight(260.0f)
				[
					SNew(SSplitter)
					+ SSplitter::Slot().Value(0.65f)
					[
						GraphEditorWidget.ToSharedRef()
					]
					+ SSplitter::Slot().Value(0.35f)
					[
						DetailsView.ToSharedRef()
					]
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SButton)
					.HAlign(HAlign_Center)
					.Text(LOCTEXT("GenerateButton", "Generate / Update Preview"))
					.OnClicked(this, &SSolarOrbzMainPanel::OnGenerateClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.HAlign(HAlign_Center)
					.Text(LOCTEXT("ClearButton", "Clear"))
					.IsEnabled(this, &SSolarOrbzMainPanel::IsPreviewValid)
					.OnClicked(this, &SSolarOrbzMainPanel::OnClearPreviewClicked)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 8.0f)
			[
				SNew(STextBlock)
				.Text(this, &SSolarOrbzMainPanel::GetStatsText)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
			[
				SNew(SSeparator)
			]

			// --- Bake ---
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeSectionHeader(LOCTEXT("BakeHeader", "Bake To Static Mesh"))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
			[
				MakeLabeledRow(LOCTEXT("BakePathLabel", "Package Path"),
					SNew(SEditableTextBox)
					.Text_Lambda([this]() { return FText::FromString(BakePackagePath); })
					.OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type) { BakePackagePath = NewText.ToString(); })
				)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
			[
				MakeLabeledRow(LOCTEXT("BakeNameLabel", "Asset Name"),
					SNew(SEditableTextBox)
					.Text_Lambda([this]() { return FText::FromString(BakeAssetName); })
					.OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type) { BakeAssetName = NewText.ToString(); })
				)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.Text(LOCTEXT("BakeButton", "Bake To Static Mesh"))
				.IsEnabled(this, &SSolarOrbzMainPanel::IsPreviewValid)
				.OnClicked(this, &SSolarOrbzMainPanel::OnBakeClicked)
			]
		]
	];
}

TSharedRef<SWidget> SSolarOrbzMainPanel::BuildAddLayerMenu()
{
	TArray<UClass*> LayerClasses;
	GetDerivedClasses(USolarOrbzTerrainLayer::StaticClass(), LayerClasses, true);
	// TArray<T*>::Sort wraps the predicate in TDereferenceWrapper, which always dereferences the
	// pointers before calling it - so the predicate takes the pointee type (UClass&), not UClass*.
	LayerClasses.Sort([](const UClass& A, const UClass& B)
	{
		return A.GetDisplayNameText().CompareTo(B.GetDisplayNameText()) < 0;
	});

	TSharedRef<SVerticalBox> MenuBox = SNew(SVerticalBox);

	for (UClass* LayerClass : LayerClasses)
	{
		if (!LayerClass || LayerClass->HasAnyClassFlags(CLASS_Abstract))
		{
			continue;
		}

		MenuBox->AddSlot()
			.AutoHeight()
			.Padding(2.0f)
			[
				SNew(SButton)
				.OnClicked_Lambda([this, LayerClass]()
				{
					AddLayerOfClass(LayerClass);
					return FReply::Handled();
				})
				[
					SNew(STextBlock).Text(LayerClass->GetDisplayNameText())
				]
			];
	}

	if (LayerClasses.Num() == 0)
	{
		UE_LOG(LogSolarOrbzPlanetSpawner, Warning, TEXT("BuildAddLayerMenu: GetDerivedClasses found no concrete USolarOrbzTerrainLayer subclasses - the Add Layer menu will be empty"));
	}

	return SNew(SBox)
		.Padding(2.0f)
		[
			MenuBox
		];
}

void SSolarOrbzMainPanel::AddLayerOfClass(UClass* LayerClass)
{
	if (!SpawnerGraph || !SpawnerGraph->EmbeddedTerrainStack || !LayerClass)
	{
		UE_LOG(LogSolarOrbzPlanetSpawner, Warning, TEXT("AddLayerOfClass: called with no spawner graph/stack (%d) or a null class (%d) - ignoring"), SpawnerGraph && SpawnerGraph->EmbeddedTerrainStack ? 1 : 0, LayerClass != nullptr);
		FSlateApplication::Get().DismissAllMenus();
		return;
	}

	USolarOrbzTerrainLayerStack* Stack = SpawnerGraph->EmbeddedTerrainStack;
	Stack->Modify();

	USolarOrbzTerrainLayer* NewLayer = NewObject<USolarOrbzTerrainLayer>(Stack, LayerClass, NAME_None, RF_Transactional);
	NewLayer->EnsureEditorNodeId();
	Stack->Layers.Add(NewLayer);

	UE_LOG(LogSolarOrbzPlanetSpawner, Log, TEXT("AddLayerOfClass: added a %s to the embedded terrain chain (now %d layer(s))"), *LayerClass->GetName(), Stack->Layers.Num());

	SpawnerGraph->RebuildEmbeddedTerrainChain();
	if (GraphEditorWidget.IsValid())
	{
		GraphEditorWidget->NotifyGraphChanged();
	}

	FSlateApplication::Get().DismissAllMenus();
}

void SSolarOrbzMainPanel::HandleGraphSelectionChanged(const TSet<UObject*>& NewSelection)
{
	TArray<UObject*> SelectedConfigs;
	for (UObject* Selected : NewSelection)
	{
		if (USolarOrbzPlanetSpawnerGraphNode* Node = Cast<USolarOrbzPlanetSpawnerGraphNode>(Selected))
		{
			if (Node->ModuleConfig)
			{
				SelectedConfigs.Add(Node->ModuleConfig);
			}
		}
		else if (USolarOrbzTerrainGraphNode* TerrainNode = Cast<USolarOrbzTerrainGraphNode>(Selected))
		{
			if (TerrainNode->Layer)
			{
				SelectedConfigs.Add(TerrainNode->Layer);
			}
		}
	}

	UE_LOG(LogSolarOrbzPlanetSpawner, Verbose, TEXT("HandleGraphSelectionChanged: %d node(s) selected, %d editable object(s)"), NewSelection.Num(), SelectedConfigs.Num());

	if (DetailsView.IsValid())
	{
		DetailsView->SetObjects(SelectedConfigs);
	}
}

void SSolarOrbzMainPanel::HandleSpawnerGraphChanged(const FEdGraphEditAction& Action)
{
	if (!SpawnerGraph || SpawnerGraph->IsRebuildingTerrainChain())
	{
		// RebuildEmbeddedTerrainChain() itself adds/removes nodes through the normal API, which
		// broadcasts here too - compiling mid-rebuild would write a half-built chain back into
		// EmbeddedTerrainStack->Layers. See IsRebuildingTerrainChain()'s own comment.
		return;
	}

	UE_LOG(LogSolarOrbzPlanetSpawner, Verbose, TEXT("HandleSpawnerGraphChanged: graph edited, compiling embedded terrain chain"));
	SpawnerGraph->CompileEmbeddedTerrainChain();
}

FReply SSolarOrbzMainPanel::OnGenerateClicked()
{
	if (!SpawnerGraph || !SpawnerGraph->BaseSphereConfig)
	{
		UE_LOG(LogSolarOrbzPlanetSpawner, Error, TEXT("OnGenerateClicked: spawner graph not built - this should never happen"));
		return FReply::Handled();
	}

	if (!PreviewActor.IsValid() && GEditor)
	{
		if (UWorld* World = GEditor->GetEditorWorldContext().World())
		{
			// Deliberately NOT requesting an explicit Name here. SpawnActor treats a naming
			// collision on an explicitly-requested name as fatal (crashes the editor) rather than
			// picking a different one - and a stale name can linger from Undo history or a
			// not-yet-garbage-collected actor from a previous Generate/Clear cycle. Leaving Name
			// unset lets the engine generate its own guaranteed-unique internal name instead;
			// the human-readable Outliner label is set separately below and has no such constraint.
			FActorSpawnParameters SpawnParams;
			SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;

			PreviewActor = World->SpawnActor<ASolarOrbzIcoSphereActor>(SpawnParams);

#if WITH_EDITOR
			if (ASolarOrbzIcoSphereActor* NewActor = PreviewActor.Get())
			{
				NewActor->SetActorLabel(TEXT("SolarOrbzIcoSphere"));
			}
#endif
		}
	}

	if (ASolarOrbzIcoSphereActor* Actor = PreviewActor.Get())
	{
		const USolarOrbzPlanetBaseSphereConfig* BaseSphere = SpawnerGraph->BaseSphereConfig;
		Actor->RadiusMeters = BaseSphere->RadiusMeters;
		Actor->VerticesPerMeter = BaseSphere->VerticesPerMeter;
		Actor->MaxSubdivisions = BaseSphere->MaxSubdivisions;
		Actor->bEnablePreviewCollision = BaseSphere->bEnablePreviewCollision;

		// Defensive: make sure the chain currently shown on the canvas is what gets baked, even if
		// some edit landed without going through HandleSpawnerGraphChanged (e.g. programmatic change).
		SpawnerGraph->CompileEmbeddedTerrainChain();
		Actor->TerrainStack = SpawnerGraph->EmbeddedTerrainStack;

		if (const USolarOrbzPlanetBiomeModule* BiomeModule = SpawnerGraph->BiomeModule)
		{
			Actor->BiomeStack = BiomeModule->BiomeStack;
		}
		if (const USolarOrbzPlanetClimateModule* ClimateModule = SpawnerGraph->ClimateModule)
		{
			Actor->ClimateSimulation = ClimateModule->ClimateSimulation;
		}
		if (const USolarOrbzPlanetProfileModule* ProfileModule = SpawnerGraph->ProfileModule)
		{
			Actor->Profile = ProfileModule->Profile;
		}

		UE_LOG(LogSolarOrbzPlanetSpawner, Log, TEXT("OnGenerateClicked: regenerating %s (Radius=%f m)"), *Actor->GetName(), BaseSphere->RadiusMeters);
		Actor->RegenerateMesh();

		if (GEditor)
		{
			GEditor->SelectNone(/*bNoteSelectionChange=*/false, /*bDeselectBSPSurfs=*/true);
			GEditor->SelectActor(Actor, /*bInSelected=*/true, /*bNotify=*/true);
		}
	}

	return FReply::Handled();
}

FReply SSolarOrbzMainPanel::OnBakeClicked()
{
	if (ASolarOrbzIcoSphereActor* Actor = PreviewActor.Get())
	{
		Actor->BakePackagePath = BakePackagePath;
		Actor->BakeAssetName = BakeAssetName;
		Actor->BakeToStaticMeshAsset();
	}
	return FReply::Handled();
}

FReply SSolarOrbzMainPanel::OnClearPreviewClicked()
{
	if (ASolarOrbzIcoSphereActor* Actor = PreviewActor.Get())
	{
		Actor->Destroy();
	}
	PreviewActor.Reset();
	return FReply::Handled();
}

FText SSolarOrbzMainPanel::GetStatsText() const
{
	if (const ASolarOrbzIcoSphereActor* Actor = PreviewActor.Get())
	{
		FText Stats = FText::Format(
			LOCTEXT("StatsFormat", "Subdivision level {0}   |   {1} verts   |   {2} tris"),
			FText::AsNumber(Actor->GetLastSubdivisionLevelUsed()),
			FText::AsNumber(Actor->GetPreviewVertexCount()),
			FText::AsNumber(Actor->GetPreviewTriangleCount()));

		if (Actor->WasLastGenerationDensityLimited())
		{
			Stats = FText::Format(
				LOCTEXT("StatsFormatDensityCapped", "{0}\n\u26A0 Vertices Per Meter would need level {1} here - Max Subdivisions is capping it. The density value is not being reached."),
				Stats,
				FText::AsNumber(Actor->GetLastRequestedSubdivisionLevel()));
		}

		return Stats;
	}
	return LOCTEXT("StatsEmpty", "No preview generated yet.");
}

bool SSolarOrbzMainPanel::IsPreviewValid() const
{
	return PreviewActor.IsValid();
}

#undef LOCTEXT_NAMESPACE

// ================================================================================================
// FSolarOrbzModule
// ================================================================================================
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
		FText::FromString(TEXT("SolarOrbzEditor.cpp"))
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

	// Both entries below are given an explicit, project-unique Name rather than letting it default
	// to the command's own internal name ("OpenPluginWindow" - the literal C++ identifier UI_COMMAND
	// stringifies, shared by any other plugin built from the same Editor Standalone Window template
	// that didn't rename its own command variable - this project's ASNMechLab editor module is one).
	// LevelEditor.MainMenu.Window's "WindowLayout" section and
	// LevelEditor.LevelEditorToolBar.PlayToolBar's "PluginTools" section are both shared by every
	// plugin that extends them, so two plugins' entries landing on the same implicit Name in the
	// same section collide - whichever module's RegisterMenus() runs last (module load order, which
	// isn't stable across editor sessions) is the one left visible there, in both the Window menu and
	// the toolbar at once, which is exactly the "the button disappears/flips to the other plugin's"
	// symptom this was causing. An explicit Name makes this entry's slot SolarOrbz's alone, regardless
	// of what any other current or future plugin's own command happens to be called.
	static const FName EntryName(TEXT("SolarOrbz_OpenPluginWindow"));

	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
		{
			FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");
			Section.AddMenuEntryWithCommandList(
				FSolarOrbzCommands::Get().OpenPluginWindow,
				PluginCommands,
				TAttribute<FText>(),
				TAttribute<FText>(),
				TAttribute<FSlateIcon>(),
				NAME_None,
				EntryName);
		}
	}

	{
		UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
		{
			FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("PluginTools");
			{
				FToolMenuEntry NewEntry = FToolMenuEntry::InitToolBarButton(FSolarOrbzCommands::Get().OpenPluginWindow);
				NewEntry.Name = EntryName;
				FToolMenuEntry& Entry = Section.AddEntry(NewEntry);
				Entry.SetCommandList(PluginCommands);
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSolarOrbzModule, SolarOrbz)
