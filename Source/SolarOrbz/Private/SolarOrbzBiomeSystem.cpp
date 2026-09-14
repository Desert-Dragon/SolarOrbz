// SolarOrbz - Biome System subsystem implementation.

#include "SolarOrbzBiomeSystem.h"
#include "SolarOrbzTerrainLayers.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"

DEFINE_LOG_CATEGORY_STATIC(LogSolarOrbzBiome, Log, All);

// ================================================================================================
// USolarOrbzBiomeMask - GetWeight has an inline default; nothing else to implement here.
// ================================================================================================

// ================================================================================================
// FSolarOrbzMaskRange / USolarOrbzClimateBiomeMask
// ================================================================================================
float FSolarOrbzMaskRange::Evaluate(float Value) const
{
	if (!bEnabled)
	{
		return 1.0f;
	}

	// Authors can leave Min > Max (e.g. while dragging both fields around) - swap rather than
	// silently returning 0 everywhere, so a momentarily-inverted range still behaves sanely.
	const float EffectiveMin = FMath::Min(Min, Max);
	const float EffectiveMax = FMath::Max(Min, Max);

	if (Falloff <= KINDA_SMALL_NUMBER)
	{
		return (Value >= EffectiveMin && Value <= EffectiveMax) ? 1.0f : 0.0f;
	}

	const float LowEdge = FMath::SmoothStep(EffectiveMin - Falloff, EffectiveMin + Falloff, Value);
	const float HighEdge = 1.0f - FMath::SmoothStep(EffectiveMax - Falloff, EffectiveMax + Falloff, Value);
	return FMath::Clamp(FMath::Min(LowEdge, HighEdge), 0.0f, 1.0f);
}

float USolarOrbzClimateBiomeMask::GetWeight(const FSolarOrbzBiomeSampleContext& Context) const
{
	float Weight = 1.0f;

	Weight *= Elevation.Evaluate((Context.Elevation / 100.0f) - Context.SeaLevel); // cm -> meters, then offset so the range tracks Sea Level rather than the raw base radius
	if (Weight <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	Weight *= Latitude.Evaluate(FMath::Abs(Context.UnitDirection.Z));
	if (Weight <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	Weight *= Slope.Evaluate(Context.Slope);
	if (Weight <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	if (Temperature.bEnabled)
	{
		// Simulated (Kelvin) when available; otherwise a generic Earth-like Lerp by latitude, in the
		// same Kelvin units, so the Min/Max above stay meaningful whether or not a simulation exists.
		const float TemperatureValue = Context.bHasClimateData
			? Context.Temperature
			: FMath::Lerp(288.0f, 255.0f, FMath::Abs(Context.UnitDirection.Z));
		Weight *= Temperature.Evaluate(TemperatureValue);
		if (Weight <= KINDA_SMALL_NUMBER)
		{
			return 0.0f;
		}
	}

	if (Moisture.bEnabled)
	{
		float MoistureValue;
		if (Context.bHasClimateData)
		{
			MoistureValue = Context.Moisture;
		}
		else
		{
			const FVector SeedOffset(
				FMath::Frac(FMath::Sin((float)MoistureSeed * 91.345f) * 47453.7f) * 1000.0f,
				FMath::Frac(FMath::Sin((float)MoistureSeed * 13.71f) * 47453.7f) * 1000.0f,
				FMath::Frac(FMath::Sin((float)MoistureSeed * 58.92f) * 47453.7f) * 1000.0f);

			MoistureValue = FMath::PerlinNoise3D(Context.UnitDirection * MoistureFrequency + SeedOffset) * 0.5f + 0.5f; // 0..1
		}
		Weight *= Moisture.Evaluate(MoistureValue);
	}

	return FMath::Clamp(Weight, 0.0f, 1.0f);
}

// ================================================================================================
// USolarOrbzCompositeBiomeMask
// ================================================================================================
float USolarOrbzCompositeBiomeMask::GetWeight(const FSolarOrbzBiomeSampleContext& Context) const
{
	if (Biomes.Num() == 0)
	{
		return 1.0f; // no children = no restriction, matching an "empty AND is true" convention
	}

	switch (CombineMode)
	{
	case ESolarOrbzMaskCombineMode::Multiply:
	{
		float Result = 1.0f;
		for (const TObjectPtr<USolarOrbzBiome>& Biome : Biomes)
		{
			if (Biome)
			{
				Result *= Biome->GetWeight(Context);
				if (Result <= KINDA_SMALL_NUMBER)
				{
					return 0.0f;
				}
			}
		}
		return Result;
	}
	case ESolarOrbzMaskCombineMode::Min:
	{
		float Result = 1.0f;
		for (const TObjectPtr<USolarOrbzBiome>& Biome : Biomes)
		{
			if (Biome)
			{
				Result = FMath::Min(Result, Biome->GetWeight(Context));
			}
		}
		return Result;
	}
	case ESolarOrbzMaskCombineMode::Max:
	{
		float Result = 0.0f;
		for (const TObjectPtr<USolarOrbzBiome>& Biome : Biomes)
		{
			if (Biome)
			{
				Result = FMath::Max(Result, Biome->GetWeight(Context));
			}
		}
		return Result;
	}
	case ESolarOrbzMaskCombineMode::Average:
	{
		float Sum = 0.0f;
		int32 Count = 0;
		for (const TObjectPtr<USolarOrbzBiome>& Biome : Biomes)
		{
			if (Biome)
			{
				Sum += Biome->GetWeight(Context);
				++Count;
			}
		}
		return Count > 0 ? (Sum / Count) : 1.0f;
	}
	}

	return 1.0f;
}

bool USolarOrbzCompositeBiomeMask::NeedsClimateData() const
{
	// Same reference-cycle guard as USolarOrbzBiome::GetWeight, scoped to this traversal - a cycle
	// here already gets a loud warning from GetWeight itself once this mask is actually evaluated.
	static thread_local int32 RecursionDepth = 0;
	constexpr int32 MaxRecursionDepth = 16;
	if (RecursionDepth >= MaxRecursionDepth)
	{
		return false;
	}

	++RecursionDepth;
	bool bNeeds = false;
	for (const TObjectPtr<USolarOrbzBiome>& Biome : Biomes)
	{
		if (Biome && Biome->Mask && Biome->Mask->NeedsClimateData())
		{
			bNeeds = true;
			break;
		}
	}
	--RecursionDepth;
	return bNeeds;
}

bool USolarOrbzCompositeBiomeMask::NeedsSlope() const
{
	static thread_local int32 RecursionDepth = 0;
	constexpr int32 MaxRecursionDepth = 16;
	if (RecursionDepth >= MaxRecursionDepth)
	{
		return false;
	}

	++RecursionDepth;
	bool bNeeds = false;
	for (const TObjectPtr<USolarOrbzBiome>& Biome : Biomes)
	{
		if (Biome && Biome->Mask && Biome->Mask->NeedsSlope())
		{
			bNeeds = true;
			break;
		}
	}
	--RecursionDepth;
	return bNeeds;
}

// ================================================================================================
// USolarOrbzBiome
// ================================================================================================
float USolarOrbzBiome::GetWeight(const FSolarOrbzBiomeSampleContext& Context) const
{
	if (!Mask)
	{
		return 1.0f;
	}

	// A Composite Mask can reference other Biomes, whose own Mask can itself be a Composite Mask
	// referencing back - an accidental cycle here would recurse forever and hang/crash the editor.
	// This depth cap turns that into a loud, recoverable warning instead. thread_local so this is
	// safe even if biome evaluation is ever parallelized across vertices later.
	static thread_local int32 RecursionDepth = 0;
	constexpr int32 MaxRecursionDepth = 16;

	if (RecursionDepth >= MaxRecursionDepth)
	{
		UE_LOG(LogTemp, Warning, TEXT("SolarOrbz Biome: '%s' hit max mask recursion depth (%d) - you likely have a Composite Mask reference cycle (Biome A's mask includes Biome B, whose mask includes Biome A, directly or through more steps). Returning 0 for this branch."), *GetName(), MaxRecursionDepth);
		return 0.0f;
	}

	++RecursionDepth;
	const float Weight = Mask->GetWeight(Context);
	--RecursionDepth;

	return Weight;
}

// ================================================================================================
// USolarOrbzBiomeStack
// ================================================================================================
void USolarOrbzBiomeStack::EvaluateLayerWeights(const FSolarOrbzBiomeSampleContext& Context, TArray<float>& OutWeights) const
{
	OutWeights.Reset(Layers.Num());
	for (const FSolarOrbzBiomeLayerEntry& Entry : Layers)
	{
		const float MaskWeight = Entry.Biome ? Entry.Biome->GetWeight(Context) : 0.0f;
		OutWeights.Add(FMath::Clamp(MaskWeight * Entry.Opacity, 0.0f, 1.0f));
	}
}

USolarOrbzBiome* USolarOrbzBiomeStack::GetDominantBiome(const FSolarOrbzBiomeSampleContext& Context) const
{
	TArray<float> LayerWeights;
	EvaluateLayerWeights(Context, LayerWeights);
	return GetDominantBiome(Context, LayerWeights);
}

USolarOrbzBiome* USolarOrbzBiomeStack::GetDominantBiome(const FSolarOrbzBiomeSampleContext& Context, const TArray<float>& LayerWeights) const
{
	USolarOrbzBiome* Best = nullptr;
	float BestWeight = 0.0f;

	// Iterate forward and use >= so later (topmost) entries win ties, matching the Photoshop-style ordering.
	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		const FSolarOrbzBiomeLayerEntry& Entry = Layers[i];
		if (!Entry.Biome)
		{
			continue;
		}

		const float Weight = LayerWeights.IsValidIndex(i) ? LayerWeights[i] : 0.0f;
		if (Weight >= BestWeight)
		{
			BestWeight = Weight;
			Best = Entry.Biome;
		}
	}

	return BestWeight > KINDA_SMALL_NUMBER ? Best : nullptr;
}

float USolarOrbzBiomeStack::EvaluateBiomeTerrainContribution(const FSolarOrbzBiomeSampleContext& Context) const
{
	TArray<float> LayerWeights;
	EvaluateLayerWeights(Context, LayerWeights);
	return EvaluateBiomeTerrainContribution(Context, LayerWeights);
}

float USolarOrbzBiomeStack::EvaluateBiomeTerrainContribution(const FSolarOrbzBiomeSampleContext& Context, const TArray<float>& LayerWeights) const
{
	float Accum = 0.0f;

	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		const FSolarOrbzBiomeLayerEntry& Entry = Layers[i];
		if (!Entry.Biome || !Entry.Biome->TerrainDetail)
		{
			continue;
		}

		const float Weight = LayerWeights.IsValidIndex(i) ? LayerWeights[i] : 0.0f;
		if (Weight <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		const float DetailHeight = Entry.Biome->TerrainDetail->EvaluateHeight(Context.UnitDirection, Context.UV);
		Accum += DetailHeight * Weight;
	}

	return Accum;
}

void USolarOrbzBiomeStack::GetUniqueBiomes(TArray<USolarOrbzBiome*>& OutBiomes) const
{
	OutBiomes.Reset();
	for (const FSolarOrbzBiomeLayerEntry& Entry : Layers)
	{
		if (Entry.Biome && !OutBiomes.Contains(Entry.Biome))
		{
			OutBiomes.Add(Entry.Biome);
		}
	}
}

void USolarOrbzBiomeStack::EvaluateTopWeightedBiomes(const FSolarOrbzBiomeSampleContext& Context, int32 MaxBiomes, TArray<int32>& OutBiomeIndices, TArray<float>& OutWeights) const
{
	TArray<float> LayerWeights;
	EvaluateLayerWeights(Context, LayerWeights);

	TArray<USolarOrbzBiome*> UniqueBiomes;
	GetUniqueBiomes(UniqueBiomes);

	EvaluateTopWeightedBiomes(Context, LayerWeights, UniqueBiomes, MaxBiomes, OutBiomeIndices, OutWeights);
}

void USolarOrbzBiomeStack::EvaluateTopWeightedBiomes(const FSolarOrbzBiomeSampleContext& Context, const TArray<float>& LayerWeights, const TArray<USolarOrbzBiome*>& UniqueBiomes, int32 MaxBiomes, TArray<int32>& OutBiomeIndices, TArray<float>& OutWeights) const
{
	OutBiomeIndices.Reset();
	OutWeights.Reset();

	if (UniqueBiomes.Num() == 0)
	{
		return;
	}

	// Weight per UNIQUE biome - if the same biome appears in more than one layer entry, its
	// strongest entry wins rather than double-counting.
	TArray<float> WeightPerUniqueBiome;
	WeightPerUniqueBiome.Init(0.0f, UniqueBiomes.Num());

	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		const FSolarOrbzBiomeLayerEntry& Entry = Layers[i];
		if (!Entry.Biome)
		{
			continue;
		}
		const int32 Index = UniqueBiomes.IndexOfByKey(Entry.Biome);
		if (Index == INDEX_NONE)
		{
			continue;
		}
		const float Weight = LayerWeights.IsValidIndex(i) ? LayerWeights[i] : 0.0f;
		WeightPerUniqueBiome[Index] = FMath::Max(WeightPerUniqueBiome[Index], Weight);
	}

	// Sort unique-biome indices by weight, strongest first.
	TArray<int32> SortedIndices;
	SortedIndices.Reserve(UniqueBiomes.Num());
	for (int32 i = 0; i < UniqueBiomes.Num(); ++i)
	{
		SortedIndices.Add(i);
	}
	SortedIndices.Sort([&WeightPerUniqueBiome](int32 A, int32 B) { return WeightPerUniqueBiome[A] > WeightPerUniqueBiome[B]; });

	float TotalWeight = 0.0f;
	for (int32 i = 0; i < SortedIndices.Num() && OutBiomeIndices.Num() < MaxBiomes; ++i)
	{
		const int32 Index = SortedIndices[i];
		const float Weight = WeightPerUniqueBiome[Index];
		if (Weight <= KINDA_SMALL_NUMBER)
		{
			break; // sorted descending - nothing further is nonzero either
		}
		OutBiomeIndices.Add(Index);
		OutWeights.Add(Weight);
		TotalWeight += Weight;
	}

	// Renormalize so the returned weights sum to 1 - ready to feed straight into a material blend.
	if (TotalWeight > KINDA_SMALL_NUMBER)
	{
		for (float& Weight : OutWeights)
		{
			Weight /= TotalWeight;
		}
	}
}

void USolarOrbzBiomeStack::BuildBiomeTextureArray()
{
#if WITH_EDITOR
	TArray<USolarOrbzBiome*> UniqueBiomes;
	GetUniqueBiomes(UniqueBiomes);

	if (UniqueBiomes.Num() == 0)
	{
		UE_LOG(LogSolarOrbzBiome, Warning, TEXT("SolarOrbz Biome Texture Array: no biomes in Layers - nothing to build."));
		return;
	}

	// Every unique biome needs a texture, or the array's slice indices would no longer line up
	// with GetUniqueBiomes() order (what EvaluateTopWeightedBiomes' OutBiomeIndices addresses) -
	// refuse to build rather than silently produce a misaligned array.
	TArray<TObjectPtr<UTexture2D>> SourceTextures;
	SourceTextures.Reserve(UniqueBiomes.Num());
	bool bAllTexturesPresent = true;

	for (USolarOrbzBiome* Biome : UniqueBiomes)
	{
		if (Biome && Biome->BaseColorTexture)
		{
			SourceTextures.Add(Biome->BaseColorTexture);
		}
		else
		{
			bAllTexturesPresent = false;
			UE_LOG(LogSolarOrbzBiome, Error, TEXT("SolarOrbz Biome Texture Array: biome '%s' has no Base Color Texture assigned - every unique biome needs one for indices to stay in sync. Not rebuilding; any existing array is unchanged."), Biome ? *Biome->GetName() : TEXT("<null>"));
		}
	}

	if (!bAllTexturesPresent)
	{
		return;
	}

	if (!BiomeTextureArray)
	{
		BiomeTextureArray = NewObject<UTexture2DArray>(this, NAME_None, RF_Public);
	}

	// NOTE: this is the same editor-only API the Content Browser's own "Create Texture Array"
	// context menu action uses internally under the hood. This has shifted across engine versions
	// before (same caveat as FSolarOrbzTextureHeightSampler's GetMipData use elsewhere in this
	// plugin) - adjust here if 5.8's UTexture2DArray header differs from this.
	BiomeTextureArray->SourceTextures = SourceTextures;
	BiomeTextureArray->UpdateSourceFromSourceTextures();
	BiomeTextureArray->PostEditChange();

	MarkPackageDirty();

	UE_LOG(LogSolarOrbzBiome, Log, TEXT("SolarOrbz Biome Texture Array: built from %d biome(s)."), SourceTextures.Num());
#endif
}
