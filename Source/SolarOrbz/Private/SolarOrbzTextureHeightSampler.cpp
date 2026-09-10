// SolarOrbz - Shared texture height sampler implementation.
// Moved out of SolarOrbzHeightmapTerrainLayer so the stamp layer can reuse it
// verbatim instead of a second, potentially-drifting copy.

#include "SolarOrbzTextureHeightSampler.h"
#include "Engine/Texture2D.h"

bool FSolarOrbzTextureHeightSampler::EnsureDecoded(UTexture2D* Texture)
{
#if WITH_EDITOR
	if (CachedTexture.Get() == Texture && CachedHeights01.Num() > 0)
	{
		return true;
	}

	CachedHeights01.Reset();
	CachedWidth = CachedHeight = 0;
	CachedTexture = Texture;

	if (!Texture)
	{
		return false;
	}

	FTextureSource& Source = Texture->Source;
	if (!Source.IsValid())
	{
		return false;
	}

	CachedWidth = Source.GetSizeX();
	CachedHeight = Source.GetSizeY();
	const ETextureSourceFormat Format = Source.GetFormat();

	TArray64<uint8> RawData;
	// NOTE: the exact FTextureSource::GetMipData overload has shifted across engine versions -
	// this is a MipIndex-based accessor either way, adjust if 5.8's header differs.
	if (!Source.GetMipData(RawData, 0))
	{
		CachedWidth = CachedHeight = 0;
		return false;
	}

	const int64 PixelCount = (int64)CachedWidth * CachedHeight;
	CachedHeights01.SetNumUninitialized(PixelCount);

	switch (Format)
	{
	case TSF_G8:
	{
		for (int64 i = 0; i < PixelCount; ++i)
		{
			CachedHeights01[i] = RawData[i] / 255.0f;
		}
		break;
	}
	case TSF_G16:
	{
		const uint16* Pixels = reinterpret_cast<const uint16*>(RawData.GetData());
		for (int64 i = 0; i < PixelCount; ++i)
		{
			CachedHeights01[i] = Pixels[i] / 65535.0f;
		}
		break;
	}
	case TSF_BGRA8:
	{
		const uint8* Pixels = RawData.GetData();
		for (int64 i = 0; i < PixelCount; ++i)
		{
			CachedHeights01[i] = Pixels[i * 4 + 0] / 255.0f;
		}
		break;
	}
	case TSF_RGBA16F:
	{
		const FFloat16* Pixels = reinterpret_cast<const FFloat16*>(RawData.GetData());
		for (int64 i = 0; i < PixelCount; ++i)
		{
			CachedHeights01[i] = FMath::Clamp((float)Pixels[i * 4 + 0], 0.0f, 1.0f);
		}
		break;
	}
	default:
		UE_LOG(LogTemp, Warning, TEXT("SolarOrbz: texture '%s' uses an unsupported source format - re-import as G8, G16, BGRA8 or RGBA16F."), *Texture->GetName());
		CachedWidth = CachedHeight = 0;
		return false;
	}

	return CachedWidth > 0 && CachedHeight > 0;
#else
	return false;
#endif
}

float FSolarOrbzTextureHeightSampler::SampleBilinear01(float U, float V) const
{
	if (CachedWidth <= 0 || CachedHeight <= 0)
	{
		return 0.0f;
	}

	const float Fx = FMath::Frac(U) * CachedWidth - 0.5f;
	const float Fy = FMath::Clamp(V, 0.0f, 1.0f) * (CachedHeight - 1);

	int32 X0 = FMath::FloorToInt(Fx);
	int32 Y0 = FMath::FloorToInt(Fy);
	const float Tx = Fx - X0;
	const float Ty = Fy - Y0;

	auto WrapX = [this](int32 X) { X %= CachedWidth; return X < 0 ? X + CachedWidth : X; };
	auto ClampY = [this](int32 Y) { return FMath::Clamp(Y, 0, CachedHeight - 1); };

	const int32 X1 = WrapX(X0 + 1);
	X0 = WrapX(X0);
	const int32 Y1 = ClampY(Y0 + 1);
	Y0 = ClampY(Y0);

	auto Sample = [this](int32 X, int32 Y) { return CachedHeights01[Y * CachedWidth + X]; };

	const float Top = FMath::Lerp(Sample(X0, Y0), Sample(X1, Y0), Tx);
	const float Bottom = FMath::Lerp(Sample(X0, Y1), Sample(X1, Y1), Tx);
	return FMath::Lerp(Top, Bottom, Ty);
}
