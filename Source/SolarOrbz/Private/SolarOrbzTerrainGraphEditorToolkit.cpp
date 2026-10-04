#include "SolarOrbzTerrainGraphEditorToolkit.h"
#include "SolarOrbzTerrainGraph.h"
#include "SolarOrbzTerrainLayers.h"
#include "GraphEditor.h"
#include "PropertyEditorModule.h"
#include "IDetailsView.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UICommandList.h"

#define LOCTEXT_NAMESPACE "SolarOrbzTerrainGraphEditor"

const FName FSolarOrbzTerrainGraphEditorToolkit::GraphCanvasTabId(TEXT("SolarOrbzTerrainGraphEditor_GraphCanvas"));
const FName FSolarOrbzTerrainGraphEditorToolkit::DetailsTabId(TEXT("SolarOrbzTerrainGraphEditor_Details"));

// ================================================================================================
// USolarOrbzTerrainLayerStackAssetDefinition
// ================================================================================================

FText USolarOrbzTerrainLayerStackAssetDefinition::GetAssetDisplayName() const
{
	return LOCTEXT("AssetDisplayName", "Terrain Layer Stack");
}

FLinearColor USolarOrbzTerrainLayerStackAssetDefinition::GetAssetColor() const
{
	// Matches the mission-console accent teal used across this subsystem's docs/Artifacts.
	return FLinearColor(0.059f, 0.478f, 0.549f);
}

TSoftClassPtr<UObject> USolarOrbzTerrainLayerStackAssetDefinition::GetAssetClass() const
{
	return USolarOrbzTerrainLayerStack::StaticClass();
}

TConstArrayView<FAssetCategoryPath> USolarOrbzTerrainLayerStackAssetDefinition::GetAssetCategories() const
{
	static const FAssetCategoryPath Categories[] = { FAssetCategoryPath(LOCTEXT("SolarOrbzAssetCategory", "SolarOrbz")) };
	return Categories;
}

EAssetCommandResult USolarOrbzTerrainLayerStackAssetDefinition::OpenAssets(const FAssetOpenArgs& OpenArgs) const
{
	for (USolarOrbzTerrainLayerStack* Stack : OpenArgs.LoadObjects<USolarOrbzTerrainLayerStack>())
	{
		if (!Stack)
		{
			UE_LOG(LogSolarOrbzTerrainGraph, Warning, TEXT("USolarOrbzTerrainLayerStackAssetDefinition::OpenAssets: LoadObjects returned a null stack - skipping"));
			continue;
		}

		UE_LOG(LogSolarOrbzTerrainGraph, Log, TEXT("USolarOrbzTerrainLayerStackAssetDefinition::OpenAssets: opening Terrain Graph Editor for %s"), *Stack->GetName());

		TSharedRef<FSolarOrbzTerrainGraphEditorToolkit> Toolkit = MakeShared<FSolarOrbzTerrainGraphEditorToolkit>();
		Toolkit->InitEditor(OpenArgs.GetToolkitMode(), OpenArgs.ToolkitHost, Stack);
	}
	return EAssetCommandResult::Handled;
}

// ================================================================================================
// FSolarOrbzTerrainGraphEditorToolkit
// ================================================================================================

FSolarOrbzTerrainGraphEditorToolkit::~FSolarOrbzTerrainGraphEditorToolkit()
{
	if (TerrainGraph)
	{
		TerrainGraph->RemoveOnGraphChangedHandler(GraphChangedDelegateHandle);
	}

	UE_LOG(LogSolarOrbzTerrainGraph, Log, TEXT("FSolarOrbzTerrainGraphEditorToolkit: closing editor for %s"), EditingStack ? *EditingStack->GetName() : TEXT("(unknown - EditingStack already null)"));
}

void FSolarOrbzTerrainGraphEditorToolkit::InitEditor(const EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& InitToolkitHost, USolarOrbzTerrainLayerStack* StackToEdit)
{
	if (!StackToEdit)
	{
		UE_LOG(LogSolarOrbzTerrainGraph, Error, TEXT("FSolarOrbzTerrainGraphEditorToolkit::InitEditor called with a null StackToEdit - aborting editor open"));
		return;
	}

	EditingStack = StackToEdit;
	UE_LOG(LogSolarOrbzTerrainGraph, Log, TEXT("FSolarOrbzTerrainGraphEditorToolkit::InitEditor: opening editor for %s"), *EditingStack->GetName());

	TerrainGraph = EditingStack->GetOrCreateTerrainGraph();
	// Force a fresh rebuild even if GetOrCreateTerrainGraph() returned an already-built graph from
	// an earlier session on this same object, in case Layers changed since (e.g. undo/redo through
	// the generic Details panel while this graph wasn't open to see it happen).
	TerrainGraph->RebuildFromLayers(EditingStack);

	GraphChangedDelegateHandle = TerrainGraph->AddOnGraphChangedHandler(FOnGraphChanged::FDelegate::CreateSP(this, &FSolarOrbzTerrainGraphEditorToolkit::HandleGraphChanged));

	SGraphEditor::FGraphEditorEvents GraphEvents;
	GraphEvents.OnSelectionChanged = SGraphEditor::FOnSelectionChanged::CreateSP(this, &FSolarOrbzTerrainGraphEditorToolkit::HandleSelectionChanged);

	GraphEditorWidget = SNew(SGraphEditor)
		.AdditionalCommands(MakeShared<FUICommandList>())
		.GraphToEdit(TerrainGraph)
		.GraphEvents(GraphEvents);

	FPropertyEditorModule& PropertyEditorModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs DetailsArgs;
	DetailsArgs.bAllowSearch = true;
	DetailsView = PropertyEditorModule.CreateDetailView(DetailsArgs);

	const TSharedRef<FTabManager::FLayout> Layout = FTabManager::NewLayout("SolarOrbzTerrainGraphEditor_Layout_v1")
		->AddArea
		(
			FTabManager::NewPrimaryArea()
			->SetOrientation(Orient_Horizontal)
			->Split
			(
				FTabManager::NewStack()
				->SetSizeCoefficient(0.75f)
				->AddTab(GraphCanvasTabId, ETabState::OpenedTab)
			)
			->Split
			(
				FTabManager::NewStack()
				->SetSizeCoefficient(0.25f)
				->AddTab(DetailsTabId, ETabState::OpenedTab)
			)
		);

	constexpr bool bCreateDefaultStandaloneMenu = true;
	constexpr bool bCreateDefaultToolbar = true;
	InitAssetEditor(Mode, InitToolkitHost, TEXT("SolarOrbzTerrainGraphEditorApp"), Layout, bCreateDefaultStandaloneMenu, bCreateDefaultToolbar, TArray<UObject*>{ EditingStack });

	UE_LOG(LogSolarOrbzTerrainGraph, Log, TEXT("FSolarOrbzTerrainGraphEditorToolkit::InitEditor: editor ready for %s"), *EditingStack->GetName());
}

void FSolarOrbzTerrainGraphEditorToolkit::RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	InTabManager->RegisterTabSpawner(GraphCanvasTabId, FOnSpawnTab::CreateSP(this, &FSolarOrbzTerrainGraphEditorToolkit::SpawnGraphTab))
		.SetDisplayName(LOCTEXT("GraphTabLabel", "Terrain Graph"))
		.SetGroup(GetWorkspaceMenuCategory());

	InTabManager->RegisterTabSpawner(DetailsTabId, FOnSpawnTab::CreateSP(this, &FSolarOrbzTerrainGraphEditorToolkit::SpawnDetailsTab))
		.SetDisplayName(LOCTEXT("DetailsTabLabel", "Details"))
		.SetGroup(GetWorkspaceMenuCategory());
}

void FSolarOrbzTerrainGraphEditorToolkit::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);

	InTabManager->UnregisterTabSpawner(GraphCanvasTabId);
	InTabManager->UnregisterTabSpawner(DetailsTabId);
}

FName FSolarOrbzTerrainGraphEditorToolkit::GetToolkitFName() const
{
	return FName(TEXT("SolarOrbzTerrainGraphEditor"));
}

FText FSolarOrbzTerrainGraphEditorToolkit::GetBaseToolkitName() const
{
	return LOCTEXT("ToolkitName", "Terrain Graph Editor");
}

FString FSolarOrbzTerrainGraphEditorToolkit::GetWorldCentricTabPrefix() const
{
	return TEXT("SolarOrbzTerrainGraph ");
}

FLinearColor FSolarOrbzTerrainGraphEditorToolkit::GetWorldCentricTabColorScale() const
{
	return FLinearColor(0.059f, 0.478f, 0.549f);
}

TSharedRef<SDockTab> FSolarOrbzTerrainGraphEditorToolkit::SpawnGraphTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("GraphTabLabel", "Terrain Graph"))
		[
			BuildGraphTabContent()
		];
}

TSharedRef<SDockTab> FSolarOrbzTerrainGraphEditorToolkit::SpawnDetailsTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("DetailsTabLabel", "Details"))
		[
			DetailsView.ToSharedRef()
		];
}

TSharedRef<SWidget> FSolarOrbzTerrainGraphEditorToolkit::BuildGraphTabContent()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SComboButton)
				.OnGetMenuContent(this, &FSolarOrbzTerrainGraphEditorToolkit::BuildAddLayerMenu)
				.ButtonContent()
				[
					SNew(STextBlock).Text(LOCTEXT("AddLayerButton", "Add Layer"))
				]
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			GraphEditorWidget.ToSharedRef()
		];
}

TSharedRef<SWidget> FSolarOrbzTerrainGraphEditorToolkit::BuildAddLayerMenu()
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
		UE_LOG(LogSolarOrbzTerrainGraph, Warning, TEXT("BuildAddLayerMenu: GetDerivedClasses found no concrete USolarOrbzTerrainLayer subclasses - the Add Layer menu will be empty"));
	}

	return SNew(SBox)
		.Padding(2.0f)
		[
			MenuBox
		];
}

void FSolarOrbzTerrainGraphEditorToolkit::AddLayerOfClass(UClass* LayerClass)
{
	if (!EditingStack || !TerrainGraph || !LayerClass)
	{
		UE_LOG(LogSolarOrbzTerrainGraph, Warning, TEXT("AddLayerOfClass: called with a null stack (%d)/graph (%d)/class (%d) - ignoring"), EditingStack != nullptr, TerrainGraph != nullptr, LayerClass != nullptr);
		FSlateApplication::Get().DismissAllMenus();
		return;
	}

	EditingStack->Modify();

	USolarOrbzTerrainLayer* NewLayer = NewObject<USolarOrbzTerrainLayer>(EditingStack, LayerClass, NAME_None, RF_Transactional);
	NewLayer->EnsureEditorNodeId();
	EditingStack->Layers.Add(NewLayer);

	UE_LOG(LogSolarOrbzTerrainGraph, Log, TEXT("AddLayerOfClass: added a %s to %s (now %d layer(s))"), *LayerClass->GetName(), *EditingStack->GetName(), EditingStack->Layers.Num());

	TerrainGraph->RebuildFromLayers(EditingStack);
	if (GraphEditorWidget.IsValid())
	{
		GraphEditorWidget->NotifyGraphChanged();
	}

	FSlateApplication::Get().DismissAllMenus();
}

void FSolarOrbzTerrainGraphEditorToolkit::HandleSelectionChanged(const TSet<UObject*>& NewSelection)
{
	TArray<UObject*> SelectedLayers;
	for (UObject* SelectedObject : NewSelection)
	{
		if (USolarOrbzTerrainGraphNode* Node = Cast<USolarOrbzTerrainGraphNode>(SelectedObject))
		{
			if (Node->Layer)
			{
				SelectedLayers.Add(Node->Layer);
			}
		}
	}

	UE_LOG(LogSolarOrbzTerrainGraph, Verbose, TEXT("HandleSelectionChanged: %d node(s) selected, %d editable layer(s)"), NewSelection.Num(), SelectedLayers.Num());

	if (DetailsView.IsValid())
	{
		DetailsView->SetObjects(SelectedLayers);
	}
}

void FSolarOrbzTerrainGraphEditorToolkit::HandleGraphChanged(const FEdGraphEditAction& Action)
{
	if (!EditingStack || !TerrainGraph)
	{
		return;
	}
	if (TerrainGraph->IsRebuilding())
	{
		// RebuildFromLayers() itself adds/removes nodes through the normal API, which broadcasts
		// here too - compiling mid-rebuild would write a half-built graph (e.g. Start/Output present
		// but not yet wired to any layer) back into Layers. See IsRebuilding()'s own comment.
		return;
	}

	UE_LOG(LogSolarOrbzTerrainGraph, Verbose, TEXT("HandleGraphChanged: graph edited, compiling back into %s"), *EditingStack->GetName());
	TerrainGraph->CompileToLayers(EditingStack);
}

#undef LOCTEXT_NAMESPACE
