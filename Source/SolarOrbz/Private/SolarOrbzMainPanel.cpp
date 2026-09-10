// SolarOrbz - Main Slate panel implementation

#include "SolarOrbzMainPanel.h"
#include "SolarOrbzIcoSphereActor.h"

#include "Editor.h"
#include "Engine/World.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"

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
				MakeLabeledRow(LOCTEXT("RadiusLabel", "Radius (cm)"),
					SNew(SSpinBox<float>)
					.MinValue(1.0f)
					.MaxValue(10000000.0f)
					.MinSliderValue(1.0f)
					.MaxSliderValue(100000.0f)
					.Delta(10.0f)
					.Value_Lambda([this]() { return Radius; })
					.OnValueChanged_Lambda([this](float NewValue) { Radius = NewValue; })
				)
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
			[
				MakeLabeledRow(LOCTEXT("DensityLabel", "Vertices / Meter"),
					SNew(SSpinBox<float>)
					.MinValue(0.01f)
					.MaxValue(100.0f)
					.MinSliderValue(0.01f)
					.MaxSliderValue(20.0f)
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
					.MaxValue(8)
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
			FActorSpawnParameters SpawnParams;
			SpawnParams.Name = MakeUniqueObjectName(World, ASolarOrbzIcoSphereActor::StaticClass(), TEXT("SolarOrbzIcoSphere"));
			PreviewActor = World->SpawnActor<ASolarOrbzIcoSphereActor>(SpawnParams);
		}
	}

	if (ASolarOrbzIcoSphereActor* Actor = PreviewActor.Get())
	{
		Actor->Radius = Radius;
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
		return FText::Format(
			LOCTEXT("StatsFormat", "Subdivision level {0}   |   {1} verts   |   {2} tris"),
			FText::AsNumber(Actor->GetLastSubdivisionLevelUsed()),
			FText::AsNumber(Actor->GetPreviewVertexCount()),
			FText::AsNumber(Actor->GetPreviewTriangleCount()));
	}
	return LOCTEXT("StatsEmpty", "No preview generated yet.");
}

bool SSolarOrbzMainPanel::IsPreviewValid() const
{
	return PreviewActor.IsValid();
}

#undef LOCTEXT_NAMESPACE
