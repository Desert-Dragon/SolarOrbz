// SolarOrbz - Main Slate panel: parametric icosphere controls, live preview
// spawning, and bake-to-static-mesh. This is the content dropped into the
// plugin's docking tab from FSolarOrbzModule::OnSpawnPluginTab.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class ASolarOrbzIcoSphereActor;

class SOLARORBZ_API SSolarOrbzMainPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSolarOrbzMainPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	// --- Staged parameters, pushed onto the preview actor when Generate is pressed. ---
	float RadiusMeters = 1000.0f;
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
