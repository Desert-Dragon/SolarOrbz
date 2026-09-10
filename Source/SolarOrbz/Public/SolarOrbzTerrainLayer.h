// SolarOrbz - Terrain layer base class. A layer is one contribution to a
// planet's height (procedural noise or an authored heightmap), combined
// with others in a USolarOrbzTerrainLayerStack.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SolarOrbzTerrainLayer.generated.h"

UENUM(BlueprintType)
enum class ESolarOrbzTerrainBlendMode : uint8
{
	Add,
	Subtract,
	Multiply,
	Max,
	Min,
	Replace,
};

/**
 * One contribution to a planet's terrain height. Mirrors the "layer stack"
 * approach used by tools like World Machine / World Creator: each layer is
 * either procedural (noise) or authored (heightmap), and a stack of them
 * composites top-to-bottom via blend modes.
 */
UCLASS(Abstract, EditInlineNew, DefaultToInstanced, BlueprintType)
class SOLARORBZ_API USolarOrbzTerrainLayer : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Layer")
	bool bEnabled = true;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Layer")
	ESolarOrbzTerrainBlendMode BlendMode = ESolarOrbzTerrainBlendMode::Add;

	/** Multiplies this layer's raw output before blending - the simplest way to fade a layer in/out. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Layer")
	float Weight = 1.0f;

	/**
	 * Returns this layer's height contribution, in UE units (cm), at a point on the unit sphere.
	 * @param UnitDirection  Normalized direction from the planet center (position on a unit sphere).
	 * @param UV             The mesh's spherical UV at this point (matches FSolarOrbzIcoSphereMeshData::UVs).
	 */
	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const { return 0.0f; }
};
