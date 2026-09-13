// SolarOrbz - Editor module & UI subsystem implementation.

#include "SolarOrbzEditor.h"

#include "Styling/SlateStyleRegistry.h"
#include "Framework/Application/SlateApplication.h"
#include "Slate/SlateGameResources.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyleMacros.h"

#include "SolarOrbzIcoSphere.h"

#include "LevelEditor.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "ToolMenus.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"

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

	Style->Set("SolarOrbz.OpenPluginWindow", new IMAGE_BRUSH_SVG(TEXT("PlaceholderButtonIcon"), Icon20x20));

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

	ChildSlot
	[
		SNew(SBox)
		.Padding(FMargin(12.0f))
		[
			SNew(SVerticalBox)

			// --- IcoSphere parameters ---
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeSectionHeader(LOCTEXT("ParamsHeader", "IcoSphere Parameters"))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
			[
				MakeLabeledRow(LOCTEXT("RadiusLabel", "Radius (m)"),
					SNew(SSpinBox<float>)
					.MinValue(0.01f)
					.MinSliderValue(1.0f)
					.MaxSliderValue(1000000.0f) // slider convenience range (1,000 km) - typing goes further, no hard ceiling
					.Delta(1.0f)
					.Value_Lambda([this]() { return RadiusMeters; })
					.OnValueChanged_Lambda([this](float NewValue) { RadiusMeters = NewValue; })
				)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
			[
				MakeLabeledRow(LOCTEXT("DensityLabel", "Vertices / Meter"),
					SNew(SSpinBox<float>)
					.MinValue(0.001f)
					.MinSliderValue(0.01f)
					.MaxSliderValue(20.0f) // slider convenience range - typing goes further, no hard ceiling
					.Delta(0.05f)
					.Value_Lambda([this]() { return VerticesPerMeter; })
					.OnValueChanged_Lambda([this](float NewValue) { VerticesPerMeter = NewValue; })
				)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
			[
				MakeLabeledRow(LOCTEXT("MaxSubdivLabel", "Max Subdivisions"),
					SNew(SSpinBox<int32>)
					.MinValue(0)
					.MinSliderValue(0)
					.MaxSliderValue(10) // slider convenience range - typing goes further, no hard ceiling (watch the stats line!)
					.Value_Lambda([this]() { return MaxSubdivisions; })
					.OnValueChanged_Lambda([this](int32 NewValue) { MaxSubdivisions = NewValue; })
				)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
			[
				MakeLabeledRow(LOCTEXT("CollisionLabel", "Preview Collision"),
					SNew(SCheckBox)
					.IsChecked_Lambda([this]() { return bEnablePreviewCollision ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) { bEnablePreviewCollision = (NewState == ECheckBoxState::Checked); })
				)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 4.0f)
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

FReply SSolarOrbzMainPanel::OnGenerateClicked()
{
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
		Actor->RadiusMeters = RadiusMeters;
		Actor->VerticesPerMeter = VerticesPerMeter;
		Actor->MaxSubdivisions = MaxSubdivisions;
		Actor->bEnablePreviewCollision = bEnablePreviewCollision;
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
