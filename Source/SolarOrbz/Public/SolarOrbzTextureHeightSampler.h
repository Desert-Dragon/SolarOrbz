// SolarOrbz - Shared CPU-side texture sampler. Decodes a UTexture2D's editor
// source data once and caches it, so multiple layers can sample real pixel
// values without depending on GPU compression settings or a render round trip.
// Not a UObject - just a plain helper struct embedded (usually as a mutable
// member) inside whichever layer needs it.

#pragma once

#include "CoreMinimal.h"

class UTexture2D;

struct SOLARORBZ_API FSolarOrbzTextureHeightSampler
{
	/** Decodes Texture's source data if it isn't already cached for this exact texture. Returns false if decoding failed or Texture is null. */
	bool EnsureDecoded(UTexture2D* Texture);

	/** Bilinear sample, 0..1. U wraps (for equirectangular/longitude use); V clamps (for latitude/local stamp use). */
	float SampleBilinear01(float U, float V) const;

private:
	TArray<float> CachedHeights01; // width*height, row-major, 0..1
	int32 CachedWidth = 0;
	int32 CachedHeight = 0;
	TWeakObjectPtr<UTexture2D> CachedTexture;
};
