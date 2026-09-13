// SolarOrbz - Terrain layer stack implementation.

#include "SolarOrbzTerrainLayerStack.h"
#include "SolarOrbzTerrainLayer.h"

void USolarOrbzTerrainLayerStack::PrepareLayers(float RadiusCm) const
{
	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		USolarOrbzTerrainLayer* Layer = Layers[i];
		if (!Layer || !Layer->bEnabled || !Layer->RequiresWholeSurfaceBake())
		{
			continue;
		}

		// Capture by value (this, i) - safe to call from Bake() as many times as it likes, since it
		// only ever reaches back into layers strictly below index i, never itself or anything above it.
		auto PriorLayersHeight = [this, i](const FVector& UnitDirection, const FVector2D& UV) -> float
		{
			return EvaluateHeightUpTo(i, UnitDirection, UV);
		};

		Layer->Bake(PriorLayersHeight, RadiusCm);
	}
}

float USolarOrbzTerrainLayerStack::EvaluateHeight(const FVector& UnitDirection, const FVector2D& UV) const
{
	return EvaluateHeightUpTo(Layers.Num(), UnitDirection, UV);
}

float USolarOrbzTerrainLayerStack::EvaluateHeightUpTo(int32 EndIndexExclusive, const FVector& UnitDirection, const FVector2D& UV) const
{
	float Accum = 0.0f;

	const int32 Count = FMath::Min(EndIndexExclusive, Layers.Num());
	for (int32 i = 0; i < Count; ++i)
	{
		const TObjectPtr<USolarOrbzTerrainLayer>& Layer = Layers[i];
		if (!Layer || !Layer->bEnabled)
		{
			continue;
		}

		const float LayerHeight = Layer->GetRawHeight(UnitDirection, UV) * Layer->Weight;

		switch (Layer->BlendMode)
		{
		case ESolarOrbzTerrainBlendMode::Add:      Accum += LayerHeight; break;
		case ESolarOrbzTerrainBlendMode::Subtract: Accum -= LayerHeight; break;
		case ESolarOrbzTerrainBlendMode::Multiply: Accum *= LayerHeight; break;
		case ESolarOrbzTerrainBlendMode::Max:      Accum = FMath::Max(Accum, LayerHeight); break;
		case ESolarOrbzTerrainBlendMode::Min:      Accum = FMath::Min(Accum, LayerHeight); break;
		case ESolarOrbzTerrainBlendMode::Replace:  Accum = LayerHeight; break;
		}
	}

	return Accum;
}
