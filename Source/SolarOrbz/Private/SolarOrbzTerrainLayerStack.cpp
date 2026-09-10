// SolarOrbz - Terrain layer stack implementation.

#include "SolarOrbzTerrainLayerStack.h"
#include "SolarOrbzTerrainLayer.h"

float USolarOrbzTerrainLayerStack::EvaluateHeight(const FVector& UnitDirection, const FVector2D& UV) const
{
	float Accum = 0.0f;

	for (const TObjectPtr<USolarOrbzTerrainLayer>& Layer : Layers)
	{
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
