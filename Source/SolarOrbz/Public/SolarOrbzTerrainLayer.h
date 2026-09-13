// SolarOrbz - Terrain layer base class. A layer is one contribution to a
// planet's height (procedural noise or an authored heightmap), combined
// with others in a USolarOrbzTerrainLayerStack.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Templates/Function.h"
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
	 * Most layers (noise, heightmap, stamp) are pure functions of a single point - GetRawHeight
	 * needs nothing but the point itself. Erosion is different: carving a valley requires knowing
	 * about the terrain around a point, not just the point. Layers that need this override this to
	 * return true and implement Bake(), which the stack calls once per regenerate, before any
	 * per-vertex GetRawHeight calls, giving them a chance to build whatever whole-surface data they need.
	 */
	virtual bool RequiresWholeSurfaceBake() const { return false; }

	/**
	 * Called once per regenerate, before GetRawHeight is ever called for this layer, only when
	 * RequiresWholeSurfaceBake() returns true. PriorLayersHeight evaluates every layer below this
	 * one in the stack (not including this layer) at an arbitrary point - i.e. exactly the terrain
	 * this layer should treat as its starting point to erode, stamp around, etc.
	 * @param PriorLayersHeight  Callable: (UnitDirection, UV) -> combined height of layers below this one, in cm.
	 * @param RadiusCm           The planet's base radius, for layers that need real physical distances (e.g. slope).
	 */
	virtual void Bake(const TFunctionRef<float(const FVector& UnitDirection, const FVector2D& UV)>& PriorLayersHeight, float RadiusCm) {}

	/**
	 * Returns this layer's height contribution, in UE units (cm), at a point on the unit sphere.
	 * @param UnitDirection  Normalized direction from the planet center (position on a unit sphere).
	 * @param UV             The mesh's spherical UV at this point (matches FSolarOrbzIcoSphereMeshData::UVs).
	 */
	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const { return 0.0f; }
};
