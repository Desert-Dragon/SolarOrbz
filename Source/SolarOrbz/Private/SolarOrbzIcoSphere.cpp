// SolarOrbz - IcoSphere subsystem implementation.

#include "SolarOrbzIcoSphere.h"

#include "Engine/Texture2D.h"
#include "ImageCore.h"

#include "SolarOrbzTerrainLayers.h"
#include "SolarOrbzBiomeSystem.h"
#include "SolarOrbzLatLongGrid.h"
#include "SolarOrbzProfiles.h"

#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Async/ParallelFor.h"
#include "HAL/ThreadSafeCounter.h"

DEFINE_LOG_CATEGORY_STATIC(LogSolarOrbz, Log, All);

// ================================================================================================
// FSolarOrbzTextureHeightSampler
// ================================================================================================
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

	// FTextureSource::GetMipImage (BlockIndex, LayerIndex, MipIndex), not the older GetMipData -
	// this is a REAL FIX, not a style preference: a previous version of this function called
	// GetMipData(RawData, 0), which only resolves against an old 3-arg overload
	// (OutMipData, MipIndex, IImageWrapperModule* = nullptr) that does NOT decompress Source data
	// that's stored PNG/JPEG-compressed on disk (a common, often default, state for an imported
	// texture) - it silently returns false for exactly that case, which left CachedWidth/Height at
	// 0 and made every subsequent SampleBilinear01 call return a flat 0.0 - i.e. the WHOLE
	// heightmap evaluated to a single constant height (MinHeightMeters) everywhere, visually
	// identical to no heightmap at all regardless of how extreme Min/MaxHeightMeters were set.
	// GetMipImage decompresses Source data internally and is Epic's own documented replacement for
	// GetMipData going forward - this is the actual, UE5.8-correct fix for that symptom, not a
	// rewrite for its own sake.
	FImage RawImage;
	if (!Source.GetMipImage(RawImage, 0, 0, 0))
	{
		return false;
	}

	// Normalize to one known pixel format/gamma space regardless of the texture's actual source
	// format (G8/G16/BGRA8/RGBA16F/RGBA32F/...) instead of hand-decoding each one - CopyTo is
	// Epic's own tested format-conversion code, so this also stops being limited to only the 4
	// formats the old switch statement happened to handle. DestGammaSpace is RawImage's OWN
	// GammaSpace (not forced to Linear) specifically so a texture already treated as linear data
	// (the normal, correct import setting for a non-color heightmap: sRGB unchecked) round-trips
	// through this conversion with the exact same numeric values as before - this fixes the actual
	// decode bug without silently changing how an existing, correctly-imported heightmap's pixel
	// values map to height.
	FImage LinearImage;
	RawImage.CopyTo(LinearImage, ERawImageFormat::R32F, RawImage.GammaSpace);

	CachedWidth = LinearImage.SizeX;
	CachedHeight = LinearImage.SizeY;

	const int64 PixelCount = (int64)CachedWidth * CachedHeight;
	if (PixelCount <= 0 || LinearImage.RawData.Num() < PixelCount * (int64)sizeof(float))
	{
		CachedWidth = CachedHeight = 0;
		return false;
	}

	CachedHeights01.SetNumUninitialized(PixelCount);
	const float* Pixels = reinterpret_cast<const float*>(LinearImage.RawData.GetData());
	for (int64 i = 0; i < PixelCount; ++i)
	{
		CachedHeights01[i] = Pixels[i];
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

// ================================================================================================
// FSolarOrbzIcoSphereGenerator
// ================================================================================================
namespace SolarOrbzIcoSphere
{
	// Single canonical copy now lives on FSolarOrbzLatLongGrid - this generator is mesh-vertex-
	// driven rather than grid-cell-driven, so it doesn't use that utility directly, but still
	// shares its PI_D constant rather than keeping a second copy of the same literal.
	static constexpr double PI_D = FSolarOrbzLatLongGrid::PI_D;

	// Edge length of a regular icosahedron with circumradius 1.0.
	static constexpr double UnitCircumradiusEdgeLength = 1.0514622242382672;

	// Absolute ceiling on subdivision level, enforced here in code regardless of what
	// MaxSubdivisions value a caller passes in. ASolarOrbzIcoSphereActor::MaxSubdivisions' own
	// UPROPERTY meta (ClampMax=11) only constrains NEW edits made through the Details panel widget -
	// it does NOT retroactively re-clamp a value already saved on an actor from before that meta
	// existed, nor one set via Blueprint, C++, or the property matrix. Without a real code-level
	// ceiling, a stale/out-of-range MaxSubdivisions combined with a high Vertices Per Meter can
	// request a subdivision level whose vertex/index buffers overflow uint32 during mesh section
	// creation - an editor crash (IntFitsIn/IntCastChecked assertion), not just a slow regenerate.
	// Keep in sync with the ClampMax on MaxSubdivisions in SolarOrbzIcoSphere.h.
	constexpr int32 AbsoluteMaxSubdivisionLevel = 11;
}

int64 FSolarOrbzIcoSphereGenerator::EstimateVertexCount(int32 SubdivisionLevel)
{
	// Base icosahedron: 12 verts, 20 tris, 30 edges.
	// Each subdivision: V' = V + E, E' = 2E + 3F, F' = 4F  (Euler-consistent for a closed sphere mesh)
	int64 V = 12, E = 30, F = 20;
	for (int32 i = 0; i < SubdivisionLevel; ++i)
	{
		const int64 NewV = V + E;
		const int64 NewE = 2 * E + 3 * F;
		const int64 NewF = 4 * F;
		V = NewV; E = NewE; F = NewF;
	}
	// Actual output vertex count will be a bit higher due to UV seam/pole duplication.
	return V;
}

void FSolarOrbzIcoSphereGenerator::RecomputeSmoothNormals(FSolarOrbzIcoSphereMeshData& MeshData)
{
	TArray<FVector> AccumNormals;
	AccumNormals.SetNumZeroed(MeshData.Vertices.Num());

	for (int32 TriStart = 0; TriStart < MeshData.Triangles.Num(); TriStart += 3)
	{
		const int32 IA = MeshData.Triangles[TriStart];
		const int32 IB = MeshData.Triangles[TriStart + 1];
		const int32 IC = MeshData.Triangles[TriStart + 2];

		const FVector& A = MeshData.Vertices[IA];
		const FVector& B = MeshData.Vertices[IB];
		const FVector& C = MeshData.Vertices[IC];

		// Un-normalized cross product's magnitude is proportional to triangle area, which
		// naturally area-weights the contribution of each face to its corners' normals.
		const FVector FaceNormal = FVector::CrossProduct(B - A, C - A);

		AccumNormals[IA] += FaceNormal;
		AccumNormals[IB] += FaceNormal;
		AccumNormals[IC] += FaceNormal;
	}

	for (int32 i = 0; i < MeshData.Normals.Num(); ++i)
	{
		const FVector Normalized = AccumNormals[i].GetSafeNormal();
		if (!Normalized.IsNearlyZero())
		{
			MeshData.Normals[i] = Normalized;
		}
	}
}

int32 FSolarOrbzIcoSphereGenerator::ComputeSubdivisionLevelForEdgeLength(double Radius, float TargetEdgeLength, int32 MaxSubdivisions, int32* OutUnclampedLevel)
{
	using namespace SolarOrbzIcoSphere;

	// Clamped once, here, so every return path below (including the early-out just past this line)
	// automatically respects the absolute ceiling regardless of what MaxSubdivisions came in as.
	MaxSubdivisions = FMath::Clamp(MaxSubdivisions, 0, AbsoluteMaxSubdivisionLevel);

	const double BaseEdgeLength = FMath::Max(Radius, (double)KINDA_SMALL_NUMBER) * UnitCircumradiusEdgeLength;

	if (TargetEdgeLength <= KINDA_SMALL_NUMBER)
	{
		if (OutUnclampedLevel)
		{
			*OutUnclampedLevel = MaxSubdivisions;
		}
		return MaxSubdivisions;
	}

	const double Ratio = BaseEdgeLength / (double)TargetEdgeLength;
	const int32 Level = Ratio > 1.0 ? (int32)FMath::CeilToDouble(FMath::Log2(Ratio)) : 0;

	if (OutUnclampedLevel)
	{
		*OutUnclampedLevel = FMath::Max(Level, 0);
	}

	return FMath::Clamp(Level, 0, MaxSubdivisions);
}

int32 FSolarOrbzIcoSphereGenerator::Generate(double Radius, float VerticesPerMeter, FSolarOrbzIcoSphereMeshData& OutMeshData, int32 MaxSubdivisions, int32* OutUnclampedLevel)
{
	// UE units are centimeters, so "per meter" density -> divide 100 by it to get target edge length in cm.
	const float TargetEdgeLength = VerticesPerMeter > KINDA_SMALL_NUMBER ? (100.0f / VerticesPerMeter) : (float)Radius;

	const int32 Level = ComputeSubdivisionLevelForEdgeLength(Radius, TargetEdgeLength, MaxSubdivisions, OutUnclampedLevel);
	GenerateAtSubdivisionLevel(Radius, Level, OutMeshData);
	return Level;
}

void FSolarOrbzIcoSphereGenerator::GenerateAtSubdivisionLevel(double Radius, int32 SubdivisionLevel, FSolarOrbzIcoSphereMeshData& OutMeshData)
{
	FBuildContext Context;
	BuildBaseIcosahedron(Context);

	// Second, independent line of defense (see AbsoluteMaxSubdivisionLevel above) - this is a public
	// entry point callers can reach directly with an explicit level, bypassing
	// ComputeSubdivisionLevelForEdgeLength's own clamp entirely.
	SubdivisionLevel = FMath::Clamp(SubdivisionLevel, 0, SolarOrbzIcoSphere::AbsoluteMaxSubdivisionLevel);
	for (int32 i = 0; i < SubdivisionLevel; ++i)
	{
		SubdivideOnce(Context);
	}

	FixUVSeamsAndFinalize(Context, FMath::Max(Radius, (double)KINDA_SMALL_NUMBER), OutMeshData);
}

void FSolarOrbzIcoSphereGenerator::GetBaseIcosahedron(TArray<FVector>& OutVertices, TArray<FIntVector>& OutFaces)
{
	const double T = (1.0 + FMath::Sqrt(5.0)) / 2.0;

	// 12 vertices of a regular icosahedron, normalized onto the unit sphere.
	const FVector RawVerts[12] =
	{
		FVector(-1,  T,  0), FVector( 1,  T,  0), FVector(-1, -T,  0), FVector( 1, -T,  0),
		FVector( 0, -1,  T), FVector( 0,  1,  T), FVector( 0, -1, -T), FVector( 0,  1, -T),
		FVector( T,  0, -1), FVector( T,  0,  1), FVector(-T,  0, -1), FVector(-T,  0,  1),
	};

	OutVertices.Reset(12);
	for (const FVector& V : RawVerts)
	{
		OutVertices.Add(V.GetSafeNormal());
	}

	// 20 triangular faces, wound so normals point outward (CCW when viewed from outside).
	const int32 RawFaces[20][3] =
	{
		{0, 11, 5}, {0, 5, 1},  {0, 1, 7},  {0, 7, 10}, {0, 10, 11},
		{1, 5, 9},  {5, 11, 4}, {11, 10, 2},{10, 7, 6}, {7, 1, 8},
		{3, 9, 4},  {3, 4, 2},  {3, 2, 6},  {3, 6, 8},  {3, 8, 9},
		{4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},  {9, 8, 1},
	};

	OutFaces.Reset(20);
	for (const int32 (&Face)[3] : RawFaces)
	{
		OutFaces.Add(FIntVector(Face[0], Face[1], Face[2]));
	}
}

void FSolarOrbzIcoSphereGenerator::BuildBaseIcosahedron(FBuildContext& Context)
{
	TArray<FVector> BaseVertices;
	TArray<FIntVector> BaseFaces;
	GetBaseIcosahedron(BaseVertices, BaseFaces);

	Context.Positions = BaseVertices;
	Context.Indices.Reset(BaseFaces.Num() * 3);
	for (const FIntVector& Face : BaseFaces)
	{
		Context.Indices.Add(Face.X);
		Context.Indices.Add(Face.Y);
		Context.Indices.Add(Face.Z);
	}

	Context.MidpointCache.Reset();
}

int32 FSolarOrbzIcoSphereGenerator::GetOrCreateMidpoint(FBuildContext& Context, int32 IndexA, int32 IndexB)
{
	const uint64 Key = ((uint64)FMath::Min(IndexA, IndexB) << 32) | (uint32)FMath::Max(IndexA, IndexB);

	if (const int32* Existing = Context.MidpointCache.Find(Key))
	{
		return *Existing;
	}

	const FVector Mid = ((Context.Positions[IndexA] + Context.Positions[IndexB]) * 0.5).GetSafeNormal();
	const int32 NewIndex = Context.Positions.Add(Mid);
	Context.MidpointCache.Add(Key, NewIndex);
	return NewIndex;
}

void FSolarOrbzIcoSphereGenerator::SubdivideOnce(FBuildContext& Context)
{
	TArray<uint32> NewIndices;
	NewIndices.Reserve(Context.Indices.Num() * 4);

	// Fresh per level: midpoints are only reused *within* a single subdivision pass.
	Context.MidpointCache.Reset();

	for (int32 i = 0; i < Context.Indices.Num(); i += 3)
	{
		const int32 A = Context.Indices[i];
		const int32 B = Context.Indices[i + 1];
		const int32 C = Context.Indices[i + 2];

		const int32 AB = GetOrCreateMidpoint(Context, A, B);
		const int32 BC = GetOrCreateMidpoint(Context, B, C);
		const int32 CA = GetOrCreateMidpoint(Context, C, A);

		NewIndices.Append({ (uint32)A, (uint32)AB, (uint32)CA });
		NewIndices.Append({ (uint32)B, (uint32)BC, (uint32)AB });
		NewIndices.Append({ (uint32)C, (uint32)CA, (uint32)BC });
		NewIndices.Append({ (uint32)AB, (uint32)BC, (uint32)CA });
	}

	Context.Indices = MoveTemp(NewIndices);
}

void FSolarOrbzIcoSphereGenerator::FixUVSeamsAndFinalize(const FBuildContext& Context, double Radius, FSolarOrbzIcoSphereMeshData& OutMeshData)
{
	using namespace SolarOrbzIcoSphere;

	OutMeshData.Reset();

	const int32 SourceVertCount = Context.Positions.Num();
	OutMeshData.Vertices.Reserve(SourceVertCount);
	OutMeshData.Normals.Reserve(SourceVertCount);
	OutMeshData.Tangents.Reserve(SourceVertCount);
	OutMeshData.UVs.Reserve(SourceVertCount);
	OutMeshData.Triangles.Reserve(Context.Indices.Num());

	// --- Pass 1: emit one vertex per unique geometric position, with a baseline UV. ---
	// Longitude wraps around Z (up in UE); latitude runs from +Z (north pole, V=0) to -Z (south pole, V=1).
	auto ComputeUV = [](const FVector& UnitPos) -> FVector2D
	{
		const double Azimuth = FMath::Atan2((double)UnitPos.Y, (double)UnitPos.X); // -PI .. PI
		const double U = 0.5 + Azimuth / (2.0 * PI_D);
		const double Polar = FMath::Acos(FMath::Clamp((double)UnitPos.Z, -1.0, 1.0)); // 0 .. PI
		const double V = Polar / PI_D;
		return FVector2D((float)U, (float)V);
	};

	auto ComputeTangent = [](const FVector& UnitPos, const FVector& Normal) -> FVector
	{
		// Tangent follows the line of latitude (increasing longitude). Degenerates at the poles,
		// where we just fall back to a fixed axis - poles are single points, tangent is irrelevant there.
		FVector Tangent = FVector::CrossProduct(FVector::UpVector, Normal);
		if (!Tangent.Normalize())
		{
			Tangent = FVector::ForwardVector;
		}
		return Tangent;
	};

	OutMeshData.Vertices.SetNum(SourceVertCount);
	OutMeshData.Normals.SetNum(SourceVertCount);
	OutMeshData.Tangents.SetNum(SourceVertCount);
	OutMeshData.UVs.SetNum(SourceVertCount);

	for (int32 i = 0; i < SourceVertCount; ++i)
	{
		const FVector& UnitPos = Context.Positions[i];
		OutMeshData.Vertices[i] = UnitPos * Radius;
		OutMeshData.Normals[i] = UnitPos;
		OutMeshData.Tangents[i] = ComputeTangent(UnitPos, UnitPos);
		OutMeshData.UVs[i] = ComputeUV(UnitPos);
	}

	OutMeshData.Triangles.SetNum(Context.Indices.Num());
	for (int32 TriStart = 0; TriStart < Context.Indices.Num(); TriStart += 3)
	{
		// Swap the last two corners of every triangle: our index generation uses a
		// mathematically "natural" CCW winding, but it's the opposite handedness from
		// what UE treats as front-facing, so left as-is the sphere renders inside-out.
		OutMeshData.Triangles[TriStart + 0] = (int32)Context.Indices[TriStart + 0];
		OutMeshData.Triangles[TriStart + 1] = (int32)Context.Indices[TriStart + 2];
		OutMeshData.Triangles[TriStart + 2] = (int32)Context.Indices[TriStart + 1];
	}

	// --- Pass 2: fix the U seam (where longitude wraps 0 -> 1) and the two poles. ---
	// A triangle straddles the seam when its U values span more than half the UV range;
	// duplicate the "low" side vertices per-triangle so the interpolation goes the short way around.
	// A triangle touches a pole when one vertex sits exactly at Z = +-1; duplicate that pole vertex
	// per-triangle with U averaged from its two neighbors, since a pole has no single "correct" U.
	TMap<int32, int32> SeamDuplicateCache; // original index -> duplicate index with U += 1.0
	auto GetSeamDuplicate = [&](int32 OriginalIndex) -> int32
	{
		if (const int32* Existing = SeamDuplicateCache.Find(OriginalIndex))
		{
			return *Existing;
		}

		// Copy out by value first - Add() can reallocate the array, which would
		// invalidate a reference taken from that same array before the copy happens.
		const FVector PosCopy = OutMeshData.Vertices[OriginalIndex];
		const FVector NormalCopy = OutMeshData.Normals[OriginalIndex];
		const FVector TangentCopy = OutMeshData.Tangents[OriginalIndex];
		FVector2D UVCopy = OutMeshData.UVs[OriginalIndex];
		UVCopy.X += 1.0f;

		const int32 NewIndex = OutMeshData.Vertices.Add(PosCopy);
		OutMeshData.Normals.Add(NormalCopy);
		OutMeshData.Tangents.Add(TangentCopy);
		OutMeshData.UVs.Add(UVCopy);
		SeamDuplicateCache.Add(OriginalIndex, NewIndex);
		return NewIndex;
	};

	const double PoleDotThreshold = 0.9999; // |Z| this close to 1 counts as "at the pole"

	for (int32 TriStart = 0; TriStart < OutMeshData.Triangles.Num(); TriStart += 3)
	{
		int32 TriIdx[3] = { OutMeshData.Triangles[TriStart], OutMeshData.Triangles[TriStart + 1], OutMeshData.Triangles[TriStart + 2] };
		float U[3] = { OutMeshData.UVs[TriIdx[0]].X, OutMeshData.UVs[TriIdx[1]].X, OutMeshData.UVs[TriIdx[2]].X };

		bool bIsPole[3];
		for (int32 k = 0; k < 3; ++k)
		{
			bIsPole[k] = FMath::Abs(OutMeshData.Normals[TriIdx[k]].Z) > PoleDotThreshold;
		}

		// Pole handling first: a pole vertex's U is meaningless on its own, so derive it from
		// the other two (non-pole) corners and give this triangle its own private copy.
		bool bHasPole = bIsPole[0] || bIsPole[1] || bIsPole[2];
		if (bHasPole)
		{
			for (int32 k = 0; k < 3; ++k)
			{
				if (bIsPole[k])
				{
					const int32 OtherA = TriIdx[(k + 1) % 3];
					const int32 OtherB = TriIdx[(k + 2) % 3];
					float AvgU = (OutMeshData.UVs[OtherA].X + OutMeshData.UVs[OtherB].X) * 0.5f;

					// If the two non-pole corners themselves straddle the seam, fix that first
					// so we're averaging the correct (unwrapped) values.
					if (FMath::Abs(OutMeshData.UVs[OtherA].X - OutMeshData.UVs[OtherB].X) > 0.5f)
					{
						AvgU += 0.5f;
					}

					// Copy out by value first, same reason as GetSeamDuplicate above.
					const FVector PosCopy = OutMeshData.Vertices[TriIdx[k]];
					const FVector NormalCopy = OutMeshData.Normals[TriIdx[k]];
					const FVector TangentCopy = OutMeshData.Tangents[TriIdx[k]];
					const float OriginalV = OutMeshData.UVs[TriIdx[k]].Y;

					const int32 DupIndex = OutMeshData.Vertices.Add(PosCopy);
					OutMeshData.Normals.Add(NormalCopy);
					OutMeshData.Tangents.Add(TangentCopy);
					OutMeshData.UVs.Add(FVector2D(AvgU, OriginalV));

					OutMeshData.Triangles[TriStart + k] = DupIndex;
					TriIdx[k] = DupIndex;
					U[k] = AvgU;
				}
			}
		}

		// Seam handling: if this triangle's U values span more than half the UV space,
		// it's wrapping around the back - push the "low" corners into the [1,2] range instead.
		const float MinU = FMath::Min3(U[0], U[1], U[2]);
		const float MaxU = FMath::Max3(U[0], U[1], U[2]);
		if (MaxU - MinU > 0.5f)
		{
			for (int32 k = 0; k < 3; ++k)
			{
				if (!bIsPole[k] && U[k] < 0.5f)
				{
					OutMeshData.Triangles[TriStart + k] = GetSeamDuplicate(TriIdx[k]);
				}
			}
		}
	}
}

// ================================================================================================
// ASolarOrbzIcoSphereActor
// ================================================================================================
ASolarOrbzIcoSphereActor::ASolarOrbzIcoSphereActor()
{
	PrimaryActorTick.bCanEverTick = false;

	ProcMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("ProcMesh"));
	SetRootComponent(ProcMesh);
	ProcMesh->bUseAsyncCooking = true;
}

float ASolarOrbzIcoSphereActor::GetResolvedSurfaceGravity() const
{
	if (const USolarOrbzPlanetProfile* PlanetProfile = Cast<USolarOrbzPlanetProfile>(Profile))
	{
		return PlanetProfile->GetSurfaceGravity();
	}
	return 9.81f; // Earth fallback - matches the same fallback Climate Simulation uses for atmosphere density when no Profile is assigned.
}

void ASolarOrbzIcoSphereActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RegenerateMesh();
}

#if WITH_EDITOR
bool ASolarOrbzIcoSphereActor::TryApplyCosmeticOnlyChange(FName ChangedPropertyName)
{
	if (!ProcMesh || CachedMeshData.Vertices.Num() == 0)
	{
		return false; // nothing generated yet to swap a material on - let the normal path run
	}

	// Blend mode is "live" right now only if every one of its three preconditions holds AND the
	// cached per-vertex arrays it needs still match the current mesh - i.e. a prior regenerate
	// actually populated them for this exact vertex count, not just that the properties are set.
	const bool bBlendModeCurrentlyLive = !bShowBiomeDebugColors && BiomeStack && BiomeBlendMaterial
		&& CachedBiomeBlendWeights.Num() == CachedMeshData.Vertices.Num()
		&& CachedBiomeBlendUV1.Num() == CachedMeshData.Vertices.Num()
		&& CachedBiomeBlendUV2.Num() == CachedMeshData.Vertices.Num();

	if (ChangedPropertyName == GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, DefaultMaterial))
	{
		// Only a true no-op swap if DefaultMaterial is actually what drives material slot 0 right
		// now - neither debug colors nor blend material active. If either of those IS active,
		// DefaultMaterial isn't even visible yet, so there's nothing to swap - but it also doesn't
		// need a regenerate for that same reason; just record it's handled (there's genuinely
		// nothing to do) rather than paying for a full pipeline re-run over an invisible change.
		if (!bShowBiomeDebugColors && !bBlendModeCurrentlyLive)
		{
			ProcMesh->SetMaterial(0, DefaultMaterial);
		}
		return true;
	}

	if (ChangedPropertyName == GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, BiomeBlendMaterial))
	{
		// Only a fast path when blend mode was ALREADY live with matching cached data - i.e. this is
		// "swap to a different blend material", not "turn blend mode on for the first time" (no
		// cached weights/UVs exist yet to reuse in that case, and RegenerateMesh also needs to run
		// once to populate BiomeTextureArray - see its own bArrayStale check).
		if (bBlendModeCurrentlyLive)
		{
			if (!BiomeBlendMID || BiomeBlendMID->Parent != BiomeBlendMaterial)
			{
				BiomeBlendMID = UMaterialInstanceDynamic::Create(BiomeBlendMaterial, this);
			}
			if (BiomeBlendMID)
			{
				BiomeBlendMID->SetTextureParameterValue(FName(TEXT("BiomeTextureArray")), BiomeStack->BiomeTextureArray);
				ProcMesh->SetMaterial(0, BiomeBlendMID);
				return true;
			}
		}
		return false;
	}

	return false;
}

void ASolarOrbzIcoSphereActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	static const TSet<FName> RegenTriggers =
	{
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, RadiusMeters),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, VerticesPerMeter),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, MaxSubdivisions),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, bEnablePreviewCollision),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, TerrainStack),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, BiomeStack),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, ClimateSimulation),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, Profile),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, bShowBiomeDebugColors),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, DebugBiomeMaterial),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, DefaultMaterial),
		GET_MEMBER_NAME_CHECKED(ASolarOrbzIcoSphereActor, BiomeBlendMaterial),
	};

	const FName ChangedPropertyName = PropertyChangedEvent.GetPropertyName();

	// DefaultMaterial/BiomeBlendMaterial are both still in RegenTriggers above (for the cases
	// TryApplyCosmeticOnlyChange declines - first-time blend mode setup, or either changing while not
	// the active rendering mode) - this fast path only short-circuits the ones it positively confirms
	// it fully handled; everything else still falls through to the normal full-pipeline check below.
	if (TryApplyCosmeticOnlyChange(ChangedPropertyName))
	{
		return;
	}

	if (RegenTriggers.Contains(ChangedPropertyName))
	{
		RegenerateMesh();
	}
}
#endif

void ASolarOrbzIcoSphereActor::RegenerateMesh()
{
	if (!ProcMesh)
	{
		return;
	}

	// The generator's own API works in UE units (cm) throughout, matching every other UE
	// system (collision, physics, etc). RadiusMeters is purely a user-facing convenience -
	// convert once, right here, and every internal calculation below stays in cm. Kept as
	// double end-to-end (not float) - at Earth-scale radii, float's ~7 significant digits
	// already eats tens of centimeters of precision before any terrain math even runs.
	const double RadiusCm = RadiusMeters * 100.0;

	// Max Subdivisions' own ClampMax=11 only constrains new edits through the Details panel - it
	// doesn't retroactively re-clamp a value already saved on this actor from before that limit
	// existed (or one set via Blueprint/C++). The generator itself now hard-clamps to the same
	// ceiling regardless (see FSolarOrbzIcoSphereGenerator), so this can no longer crash - but if
	// this actor's own authored value is stale/out of range, say so rather than silently ignoring it.
	if (MaxSubdivisions > 11)
	{
		UE_LOG(LogSolarOrbz, Warning,
			TEXT("SolarOrbz: Max Subdivisions is set to %d, above the safe ceiling (11) - this was likely saved before that ceiling existed. Generation is being capped to 11 regardless; open Max Subdivisions in the Details panel and re-enter a value (even the same one) to update the stored property."),
			MaxSubdivisions);
	}

	LastSubdivisionLevelUsed = FSolarOrbzIcoSphereGenerator::Generate(RadiusCm, VerticesPerMeter, CachedMeshData, MaxSubdivisions, &LastRequestedSubdivisionLevel);

	if (LastRequestedSubdivisionLevel > LastSubdivisionLevelUsed)
	{
		// Above this, no single mesh could reach the requested density regardless of Max
		// Subdivisions (EstimateVertexCount(14) is already ~2.7 billion vertices) - a fundamentally
		// different situation from "the cap is binding but raising it would help", which the plain
		// warning below covers. Callers hitting this need a chunked/streaming LOD terrain system,
		// not a higher Max Subdivisions.
		constexpr int32 InfeasibleLevelThreshold = 14;
		if (LastRequestedSubdivisionLevel >= InfeasibleLevelThreshold)
		{
			UE_LOG(LogSolarOrbz, Warning,
				TEXT("SolarOrbz: at this Radius/Vertices Per Meter, subdivision level %d (~%lld vertices) would be required - this is not achievable in any single mesh, regardless of Max Subdivisions. A chunked/streaming LOD terrain system (planned separately) is required for ground-level detail at this scale; this actor is intended for preview/bake at a bounded radius or vertex budget instead."),
				LastRequestedSubdivisionLevel, FSolarOrbzIcoSphereGenerator::EstimateVertexCount(LastRequestedSubdivisionLevel));
		}
		else
		{
			UE_LOG(LogSolarOrbz, Warning,
				TEXT("SolarOrbz: Vertices Per Meter (%.4f) would need subdivision level %d at this radius, but Max Subdivisions caps it at %d - the density setting is NOT being reached. Raise Max Subdivisions or lower Vertices Per Meter."),
				VerticesPerMeter, LastRequestedSubdivisionLevel, LastSubdivisionLevelUsed);
		}
	}

	// Keep the pristine outward sphere direction for every vertex - both passes displace
	// along this, not along the (changing) recomputed normal, so height stays purely radial.
	TArray<FVector> OriginalUnitDirections = CachedMeshData.Normals;

	// Resolved once, reused below both for Continent-style layers reading landmass counts and for
	// Climate Simulation's atmosphere density - null if no Profile is assigned, or it's a Star/
	// Asteroid Profile instead of a Planet Profile (both valid, just nothing to read here for them).
	const USolarOrbzPlanetProfile* PlanetProfile = Cast<USolarOrbzPlanetProfile>(Profile);

	// Also resolved once, reused both for terrain layers that need an absolute "above/below sea
	// level" reference (Noise, Planetary Noise, Continent) and for Climate Simulation's ocean
	// threshold below - a plain property read, doesn't require the simulation to have actually run.
	const float SeaLevelCm = ClimateSimulation ? ClimateSimulation->SeaLevel * 100.0f : 0.0f;

	// --- Pass A: base terrain (procedural noise and/or authored heightmap). ---
	// Runs once unconditionally (the "seed" pass, no Climate Simulation data yet - every layer's
	// own Mask sees bHasClimateData=false and falls back to its own no-simulation behavior, same as
	// today). If any layer's Mask actually needs Temperature/Moisture (AnyLayerNeedsClimateData),
	// it runs a second time below, after Climate Simulation has produced a real grid, so those masks
	// see real data instead of their fallback. Assigns rather than accumulates each time (from the
	// pristine sphere position), so it's safe to call more than once - never double-displaces.
	auto RunTerrainPassA = [this, &OriginalUnitDirections, RadiusCm](const FSolarOrbzClimateGrid* ClimateGridForMasking)
	{
		const int32 NumVerts = CachedMeshData.Vertices.Num();

		// Heights scratch, parallel to CachedMeshData.Vertices - every USolarOrbzTerrainLayer/
		// USolarOrbzBiomeMask in the per-vertex evaluation below is already written to be safe under
		// concurrent calls (thread_local recursion guards throughout SolarOrbzBiomeSystem.cpp/
		// SolarOrbzTerrainLayers.cpp, and USolarOrbzTerrainLayerStack::PrepareLayers() now warms every
		// layer's own lazy caches - e.g. Heightmap/Stamp's texture decode - single-threaded before this
		// runs), so the actual per-vertex height evaluation + displacement below runs under ParallelFor.
		// The Min/Max/Sum stats further down are log-only, not correctness-critical, so they're reduced
		// in a cheap sequential pass over this scratch array afterward rather than accumulated from
		// multiple threads directly into shared locals - ParallelFor has no built-in reduction, and a
		// naive shared float accumulator here would be a real data race (lost updates), not just slow.
		TArray<float> Heights;
		Heights.SetNumUninitialized(NumVerts);

		ParallelFor(NumVerts, [this, &OriginalUnitDirections, RadiusCm, ClimateGridForMasking, &Heights](int32 i)
		{
			const FVector& UnitDirection = OriginalUnitDirections[i];
			const float Height = TerrainStack->EvaluateHeight(UnitDirection, CachedMeshData.UVs[i], ClimateGridForMasking);
			CachedMeshData.Vertices[i] = UnitDirection * RadiusCm + UnitDirection * Height;
			Heights[i] = Height;
		});

		float MinHeight = TNumericLimits<float>::Max(), MaxHeight = TNumericLimits<float>::Lowest(), SumHeight = 0.0f;
		for (int32 i = 0; i < NumVerts; ++i)
		{
			MinHeight = FMath::Min(MinHeight, Heights[i]);
			MaxHeight = FMath::Max(MaxHeight, Heights[i]);
			SumHeight += Heights[i];
		}

		const float AvgHeight = NumVerts > 0 ? SumHeight / NumVerts : 0.0f;
		const float PeakToPeakCm = MaxHeight - MinHeight;
		UE_LOG(LogSolarOrbz, Log,
			TEXT("SolarOrbz Terrain: %d enabled layer(s), height range %.1fcm..%.1fcm (avg %.1fcm), peak-to-peak %.1fcm = %.4f%% of radius (%.1fcm)"),
			TerrainStack->Layers.Num(), MinHeight, MaxHeight, AvgHeight, PeakToPeakCm,
			RadiusCm > KINDA_SMALL_NUMBER ? (PeakToPeakCm / RadiusCm) * 100.0f : 0.0f, RadiusCm);

		if (PeakToPeakCm < KINDA_SMALL_NUMBER)
		{
			UE_LOG(LogSolarOrbz, Warning,
				TEXT("SolarOrbz Terrain: peak-to-peak height is ~0 - either every layer is disabled/has 0 Strength, or the stack has no layers at all. Check the Layers array on your TerrainLayerStack asset."));
		}
		else if (PeakToPeakCm / RadiusCm < 0.001f) // under 0.1% of radius
		{
			UE_LOG(LogSolarOrbz, Warning,
				TEXT("SolarOrbz Terrain: peak-to-peak height is only %.4f%% of the planet's radius - this is real terrain, not a bug, but at that scale it will look essentially flat when viewing the whole planet (this is also true of real Earth - Everest is only ~0.14%% of Earth's radius). Zoom the camera in close to the surface to confirm the bumps are really there, or temporarily raise elevation values well past realistic for an at-a-glance test."),
				(PeakToPeakCm / RadiusCm) * 100.0f);
		}

		// Recompute now so Pass B (and, if this runs again below, slope-masked layer evaluation) has
		// real slope data to work with, not the pristine sphere's.
		FSolarOrbzIcoSphereGenerator::RecomputeSmoothNormals(CachedMeshData);
	};

	bool bTerrainNeedsClimateForMasking = false;
	if (TerrainStack)
	{
		// Profile/Sea Level data (e.g. Continent's landmass counts, Noise/Planetary Noise's
		// sea-level offset) must reach layers before PrepareLayers() bakes them - Bake() only runs
		// once per regenerate, so this has to happen first, not after.
		TerrainStack->ApplyPlanetaryContext(PlanetProfile, SeaLevelCm);

		// Whole-surface bake first (e.g. erosion) - must happen before any per-point EvaluateHeight
		// calls below, including the ones the Climate Simulation will make against this same stack,
		// so rain shadows react to eroded terrain rather than the pre-erosion noise.
		TerrainStack->PrepareLayers(RadiusCm);

		bTerrainNeedsClimateForMasking = TerrainStack->AnyLayerNeedsClimateData();

		if (TerrainStack->AnyLayerNeedsSlope())
		{
			UE_LOG(LogSolarOrbz, Log, TEXT("SolarOrbz Terrain: at least one layer's Mask reads Slope - those layers pay for a small finite-difference height sample per vertex to estimate it."));
		}

		RunTerrainPassA(nullptr);
	}
	else
	{
		UE_LOG(LogSolarOrbz, Warning, TEXT("SolarOrbz Terrain: no TerrainStack assigned on the actor - the mesh is a perfect sphere."));
	}

	// --- Climate simulation: runs once on its own lat/long grid (not per-vertex), sampling elevation ---
	// from TerrainStack the same way the mesh does. Feeds Pass B's sample context below.
	CachedClimateGrid.Reset();
	if (ClimateSimulation)
	{
		// Atmosphere density is Profile's data now, not Climate Simulation's own - falls back to
		// Earth's 1.225 kg/m^3 if no Planet Profile is assigned, matching the old hardcoded default.
		float AtmosphereDensityAtSeaLevel = 1.225f;
		if (PlanetProfile)
		{
			AtmosphereDensityAtSeaLevel = PlanetProfile->GetAtmosphereDensityAtSeaLevel();
		}
		else if (Profile)
		{
			UE_LOG(LogSolarOrbz, Warning, TEXT("SolarOrbz Climate: Profile is assigned but isn't a Planet Profile (e.g. it's a Star or Asteroid Profile) - falling back to Earth's atmosphere density (1.225 kg/m^3) for Climate Simulation."));
		}

		ClimateSimulation->Simulate(TerrainStack, RadiusCm, AtmosphereDensityAtSeaLevel, CachedClimateGrid);

		if (CachedClimateGrid.IsValid())
		{
			float MinTemp = TNumericLimits<float>::Max(), MaxTemp = TNumericLimits<float>::Lowest(), SumTemp = 0.0f;
			float MinMoist = TNumericLimits<float>::Max(), MaxMoist = TNumericLimits<float>::Lowest(), SumMoist = 0.0f;
			for (int32 Idx = 0; Idx < CachedClimateGrid.TemperatureKelvin.Num(); ++Idx)
			{
				const float T = CachedClimateGrid.TemperatureKelvin[Idx];
				const float M = CachedClimateGrid.Moisture01[Idx];
				MinTemp = FMath::Min(MinTemp, T); MaxTemp = FMath::Max(MaxTemp, T); SumTemp += T;
				MinMoist = FMath::Min(MinMoist, M); MaxMoist = FMath::Max(MaxMoist, M); SumMoist += M;
			}
			const int32 CellCount = CachedClimateGrid.TemperatureKelvin.Num();
			UE_LOG(LogSolarOrbz, Log,
				TEXT("SolarOrbz Climate: grid %dx%d - Temperature %.1fK..%.1fK (avg %.1fK), Moisture %.3f..%.3f (avg %.3f)"),
				CachedClimateGrid.Width, CachedClimateGrid.Height, MinTemp, MaxTemp, SumTemp / CellCount, MinMoist, MaxMoist, SumMoist / CellCount);

			if (MaxMoist - MinMoist < KINDA_SMALL_NUMBER)
			{
				UE_LOG(LogSolarOrbz, Warning,
					TEXT("SolarOrbz Climate: Moisture is completely flat (%.3f everywhere) - either every cell is below Sea Level (all ocean) or TerrainStack has no real elevation variation, so wind/atmosphere settings have nothing to act on."),
					MinMoist);
			}
		}
		else
		{
			UE_LOG(LogSolarOrbz, Warning, TEXT("SolarOrbz Climate: ClimateSimulation is assigned but produced an invalid grid (check Grid Width/Height)."));
		}
	}
	else if (bShowBiomeDebugColors)
	{
		UE_LOG(LogSolarOrbz, Warning, TEXT("SolarOrbz Climate: no ClimateSimulation assigned on the actor - any Climate Biome Mask using Moisture/Temperature is running on its no-simulation fallback, not real simulated data."));
	}

	// --- Pass A, final re-run: only when a layer's Mask actually needs Temperature/Moisture and a ---
	// real grid is now available - re-evaluates the whole base terrain pass so those masks see real
	// climate data instead of the seed pass's no-simulation fallback. Free (skipped entirely) for
	// every planet that doesn't use a climate-gated per-layer mask, which is every planet authored
	// before this existed.
	if (TerrainStack && bTerrainNeedsClimateForMasking && CachedClimateGrid.IsValid())
	{
		UE_LOG(LogSolarOrbz, Log, TEXT("SolarOrbz Terrain: re-running the base terrain pass (final) now that Climate Simulation has run, so climate-gated layer masks see real Temperature/Moisture."));
		RunTerrainPassA(&CachedClimateGrid);
	}

	// --- Pass B: biome-specific detail, masked by climate/composite conditions and blended on top. ---
	// Rendering has three mutually-exclusive modes sharing the same vertex color + UV1/UV2 channels:
	// debug colors (bShowBiomeDebugColors), texture-array blending (BiomeBlendMaterial assigned),
	// or plain DefaultMaterial with none of this data populated.
	const bool bWantsBlendMaterial = !bShowBiomeDebugColors && BiomeStack && BiomeBlendMaterial;
	constexpr int32 MaxBlendedBiomes = 4; // matches vertex color's 4 channels (RGBA) and UV1/UV2's 2+2 float slots

	TArray<FLinearColor> BiomeDebugColors;
	CachedBiomeBlendUV1.Reset();
	CachedBiomeBlendUV2.Reset();
	CachedBiomeBlendWeights.Reset();

	if (BiomeStack)
	{
		if (bShowBiomeDebugColors)
		{
			BiomeDebugColors.SetNum(CachedMeshData.Vertices.Num());
		}

		// Invariant for the whole regenerate - computed once here rather than rebuilt inside
		// EvaluateTopWeightedBiomes on every single vertex.
		TArray<USolarOrbzBiome*> UniqueBiomes;
		BiomeStack->GetUniqueBiomes(UniqueBiomes);

		// Each biome's own TerrainDetail stack is a full USolarOrbzTerrainLayerStack too, so it needs
		// the same once-per-regenerate setup the main TerrainStack gets above - without this, any
		// whole-surface-baked layer in it (e.g. Erosion) would silently contribute nothing (never
		// baked), Sea-Level-relative layers would measure from 0 instead of the real Sea Level, and a
		// per-layer Mask reading Slope would always see 0.
		for (USolarOrbzBiome* Biome : UniqueBiomes)
		{
			if (Biome && Biome->TerrainDetail)
			{
				Biome->TerrainDetail->ApplyPlanetaryContext(PlanetProfile, SeaLevelCm);
				Biome->TerrainDetail->PrepareLayers(RadiusCm);
			}
		}

		if (bWantsBlendMaterial)
		{
#if WITH_EDITOR
			// Auto-rebuild if the texture array looks out of sync with the current biome list, so
			// first-time setup (and adding/removing a biome) just works without a separate manual
			// step - BuildBiomeTextureArray is still exposed for an explicit rebuild too.
			const bool bArrayStale = !BiomeStack->BiomeTextureArray
				|| BiomeStack->BiomeTextureArray->SourceTextures.Num() != UniqueBiomes.Num();
			if (bArrayStale)
			{
				BiomeStack->BuildBiomeTextureArray();
			}
#endif
			CachedBiomeBlendUV1.SetNum(CachedMeshData.Vertices.Num());
			CachedBiomeBlendUV2.SetNum(CachedMeshData.Vertices.Num());
			CachedBiomeBlendWeights.Init(FLinearColor(0, 0, 0, 0), CachedMeshData.Vertices.Num());
		}

		// FThreadSafeCounter rather than plain int32 - these two are incremented from inside the
		// ParallelFor below, and a bare `int32 += ` from multiple worker threads at once is a real
		// data race (lost updates), not just a style nit.
		FThreadSafeCounter NumWithClimateData;
		FThreadSafeCounter NumDominantBiomeHits;

		ParallelFor(CachedMeshData.Vertices.Num(), [&](int32 i)
		{
			// Declared fresh per vertex rather than hoisted out of the loop (as a pre-ParallelFor pass
			// over this same code did) - hoisted TArrays were reused/resized in place across
			// iterations specifically to dodge a per-vertex heap allocation, but that only works when
			// iterations run strictly one after another. Under ParallelFor, concurrent tasks would be
			// writing into the SAME three arrays at once and corrupt each other. These are small,
			// bounded-size arrays (one entry per biome layer, at most MaxBlendedBiomes for the other
			// two) - the reintroduced small per-vertex allocation is an acceptable trade for correct
			// concurrent execution, and is dwarfed by the actual mask/noise evaluation cost this
			// parallelizes.
			TArray<float> LayerWeights;
			TArray<int32> TopBiomeIndices;
			TArray<float> TopBiomeWeights;

			const FVector& UnitDirection = OriginalUnitDirections[i];

			FSolarOrbzBiomeSampleContext Context;
			Context.UnitDirection = UnitDirection;
			Context.UV = CachedMeshData.UVs[i];
			Context.Elevation = FVector::DotProduct(CachedMeshData.Vertices[i], UnitDirection) - RadiusCm;
			Context.Slope = FMath::Clamp(1.0f - FVector::DotProduct(CachedMeshData.Normals[i], UnitDirection), 0.0f, 1.0f);
			Context.SeaLevel = ClimateSimulation ? ClimateSimulation->SeaLevel : 0.0f; // authored value, valid even if the simulation itself hasn't produced a grid

			if (CachedClimateGrid.IsValid())
			{
				Context.bHasClimateData = true;
				CachedClimateGrid.Sample(UnitDirection, Context.Temperature, Context.Moisture);
				NumWithClimateData.Increment();
			}

			// Evaluated once per vertex and reused below - every biome's mask used to be evaluated
			// twice per vertex here (once via EvaluateBiomeTerrainContribution, once more via
			// GetDominantBiome/EvaluateTopWeightedBiomes), worse for composite/recursive masks.
			BiomeStack->EvaluateLayerWeights(Context, LayerWeights);

			const float BiomeHeight = BiomeStack->EvaluateBiomeTerrainContribution(Context, LayerWeights);
			CachedMeshData.Vertices[i] += UnitDirection * BiomeHeight;

			if (bShowBiomeDebugColors)
			{
				if (const USolarOrbzBiome* Dominant = BiomeStack->GetDominantBiome(Context, LayerWeights))
				{
					BiomeDebugColors[i] = Dominant->PreviewColor;
					NumDominantBiomeHits.Increment();
				}
				else
				{
					BiomeDebugColors[i] = FLinearColor::Black; // no biome layer applies here
				}
			}
			else if (bWantsBlendMaterial)
			{
				BiomeStack->EvaluateTopWeightedBiomes(Context, LayerWeights, UniqueBiomes, MaxBlendedBiomes, TopBiomeIndices, TopBiomeWeights);
				if (TopBiomeIndices.Num() > 0)
				{
					NumDominantBiomeHits.Increment(); // reusing the same stat: "at least one biome matched here"
				}

				auto IndexOrPad = [&TopBiomeIndices](int32 Slot) { return TopBiomeIndices.IsValidIndex(Slot) ? (float)TopBiomeIndices[Slot] : -1.0f; };
				auto WeightOrPad = [&TopBiomeWeights](int32 Slot) { return TopBiomeWeights.IsValidIndex(Slot) ? TopBiomeWeights[Slot] : 0.0f; };

				CachedBiomeBlendUV1[i] = FVector2D(IndexOrPad(0), IndexOrPad(1));
				CachedBiomeBlendUV2[i] = FVector2D(IndexOrPad(2), IndexOrPad(3));
				CachedBiomeBlendWeights[i] = FLinearColor(WeightOrPad(0), WeightOrPad(1), WeightOrPad(2), WeightOrPad(3));
			}
		});

		FSolarOrbzIcoSphereGenerator::RecomputeSmoothNormals(CachedMeshData);

		if (bShowBiomeDebugColors)
		{
			UE_LOG(LogSolarOrbz, Log,
				TEXT("SolarOrbz Biome Debug: %d/%d verts had climate data, %d/%d verts matched a biome layer (rest rendered black = no layer applies there)."),
				NumWithClimateData.GetValue(), CachedMeshData.Vertices.Num(), NumDominantBiomeHits.GetValue(), CachedMeshData.Vertices.Num());

			if (NumDominantBiomeHits.GetValue() == 0)
			{
				UE_LOG(LogSolarOrbz, Warning, TEXT("SolarOrbz Biome Debug: not a single vertex matched any biome layer's mask - check each layer's Mask Preset ranges (Min/Max/Falloff) against the Moisture/Temperature/Elevation stats logged above."));
			}
		}
		else if (bWantsBlendMaterial)
		{
			UE_LOG(LogSolarOrbz, Log,
				TEXT("SolarOrbz Biome Blend: %d/%d verts had climate data, %d/%d verts matched at least one biome (rest render with all-zero weights - no biome applies there)."),
				NumWithClimateData.GetValue(), CachedMeshData.Vertices.Num(), NumDominantBiomeHits.GetValue(), CachedMeshData.Vertices.Num());

			if (NumDominantBiomeHits.GetValue() == 0)
			{
				UE_LOG(LogSolarOrbz, Warning, TEXT("SolarOrbz Biome Blend: not a single vertex matched any biome - check each biome's Mask ranges against the Moisture/Temperature/Elevation stats logged above."));
			}
		}
	}
	else if (bShowBiomeDebugColors)
	{
		UE_LOG(LogSolarOrbz, Warning, TEXT("SolarOrbz Biome Debug: Show Biome Debug Colors is on but no BiomeStack is assigned - there's nothing to color, the mesh will render fully black (or white, if the material has no vertex colors at all)."));
	}

	if (bShowBiomeDebugColors && !DebugBiomeMaterial)
	{
		UE_LOG(LogSolarOrbz, Warning, TEXT("SolarOrbz Biome Debug: Show Biome Debug Colors is on but Debug Biome Material is not assigned - slot 0 will get a null material (default checker/gray) regardless of the vertex colors computed above."));
	}

	// --- Resolve which material actually goes in slot 0, and which vertex color / UV1 / UV2 data ---
	// accompanies it - the three rendering modes above populated at most one of BiomeDebugColors /
	// BiomeBlendWeights, so this just routes whichever one is live.
	UMaterialInterface* MaterialToUse = DefaultMaterial;
	const TArray<FLinearColor>* VertexColorsToUse = &BiomeDebugColors; // empty unless debug mode populated it
	const TArray<FVector2D>* UV1ToUse = nullptr;
	const TArray<FVector2D>* UV2ToUse = nullptr;

	if (bShowBiomeDebugColors)
	{
		MaterialToUse = DebugBiomeMaterial;
	}
	else if (bWantsBlendMaterial)
	{
		if (!BiomeBlendMID || BiomeBlendMID->Parent != BiomeBlendMaterial)
		{
			BiomeBlendMID = UMaterialInstanceDynamic::Create(BiomeBlendMaterial, this);
		}
		if (BiomeBlendMID)
		{
			BiomeBlendMID->SetTextureParameterValue(FName(TEXT("BiomeTextureArray")), BiomeStack->BiomeTextureArray);
		}
		MaterialToUse = BiomeBlendMID;
		VertexColorsToUse = &CachedBiomeBlendWeights;
		UV1ToUse = &CachedBiomeBlendUV1;
		UV2ToUse = &CachedBiomeBlendUV2;
	}

	ProcMesh->SetMaterial(0, MaterialToUse);
	UE_LOG(LogSolarOrbz, Log, TEXT("SolarOrbz: material slot 0 set to '%s' (bShowBiomeDebugColors=%s, blendMaterial=%s)"),
		*GetNameSafe(MaterialToUse), bShowBiomeDebugColors ? TEXT("true") : TEXT("false"), bWantsBlendMaterial ? TEXT("true") : TEXT("false"));

	TArray<FProcMeshTangent> ProcTangents;
	ProcTangents.Reserve(CachedMeshData.Tangents.Num());
	for (const FVector& T : CachedMeshData.Tangents)
	{
		ProcTangents.Add(FProcMeshTangent(T, false));
	}

	const TArray<FVector2D> EmptyUVChannel;

	ProcMesh->ClearAllMeshSections();
	ProcMesh->CreateMeshSection_LinearColor(
		0,
		CachedMeshData.Vertices,
		CachedMeshData.Triangles,
		CachedMeshData.Normals,
		CachedMeshData.UVs,
		UV1ToUse ? *UV1ToUse : EmptyUVChannel,
		UV2ToUse ? *UV2ToUse : EmptyUVChannel,
		EmptyUVChannel,
		*VertexColorsToUse,
		ProcTangents,
		bEnablePreviewCollision);

	UE_LOG(LogSolarOrbz, Log, TEXT("SolarOrbz: generated icosphere at subdivision level %d (%d verts, %d tris)"),
		LastSubdivisionLevelUsed, CachedMeshData.Vertices.Num(), CachedMeshData.Triangles.Num() / 3);
}

void ASolarOrbzIcoSphereActor::BakeToStaticMeshAsset()
{
#if WITH_EDITOR
	if (CachedMeshData.Vertices.Num() == 0)
	{
		RegenerateMesh();
	}

	if (CachedMeshData.Triangles.Num() == 0)
	{
		UE_LOG(LogSolarOrbz, Warning, TEXT("SolarOrbz: nothing to bake, mesh data is empty."));
		return;
	}

	const FString CleanAssetName = BakeAssetName.IsEmpty() ? TEXT("SM_IcoSphere") : BakeAssetName;
	const FString ObjectPath = FPaths::Combine(BakePackagePath, CleanAssetName);
	const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		UE_LOG(LogSolarOrbz, Error, TEXT("SolarOrbz: failed to create package '%s'"), *PackageName);
		return;
	}
	Package->FullyLoad();

	UStaticMesh* NewStaticMesh = NewObject<UStaticMesh>(Package, FName(*CleanAssetName), RF_Public | RF_Standalone);
	if (!NewStaticMesh)
	{
		UE_LOG(LogSolarOrbz, Error, TEXT("SolarOrbz: failed to create UStaticMesh object."));
		return;
	}

	// --- Build a MeshDescription from our generator output. ---
	// Vertices sharing an exact position get a single FVertexID (so normal/tangent-generation and
	// any future LOD reduction see correct topology); each array entry still gets its own
	// FVertexInstanceID, which is exactly what lets the UV-seam and pole duplicates carry different UVs.
	FMeshDescription MeshDescription;
	FStaticMeshAttributes Attributes(MeshDescription);
	Attributes.Register();

	TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> InstanceNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> InstanceTangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> InstanceBinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector4f> InstanceColors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> InstanceUVs = Attributes.GetVertexInstanceUVs();

	// The live preview's Biome Blend Material reads weights from vertex color and biome indices from
	// UV1/UV2 (see RegenerateMesh) - carry that same data into the baked mesh so the material keeps
	// working once it's applied here instead of driving the ProcMeshComponent.
	const bool bHasBiomeBlendData = CachedBiomeBlendWeights.Num() == CachedMeshData.Vertices.Num()
		&& CachedBiomeBlendUV1.Num() == CachedMeshData.Vertices.Num()
		&& CachedBiomeBlendUV2.Num() == CachedMeshData.Vertices.Num();
	InstanceUVs.SetNumChannels(bHasBiomeBlendData ? 3 : 1);

	if (BiomeStack && BiomeBlendMaterial && !bHasBiomeBlendData)
	{
		UE_LOG(LogSolarOrbz, Warning, TEXT("SolarOrbz Bake: Biome Stack and Biome Blend Material are both assigned, but no cached blend data matches the current vertex count (last Regenerate had Show Biome Debug Colors on, or hasn't run since a parameter changed) - baked vertex colors will be flat white and UV1/UV2 empty. Regenerate Mesh with Show Biome Debug Colors off, then Bake again."));
	}

	const FPolygonGroupID PolygonGroupID = MeshDescription.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames()[PolygonGroupID] = FName(TEXT("Default"));

	TMap<FVector, FVertexID> PositionToVertexID;
	PositionToVertexID.Reserve(CachedMeshData.Vertices.Num());

	TArray<FVertexInstanceID> InstanceIDs;
	InstanceIDs.SetNum(CachedMeshData.Vertices.Num());

	for (int32 i = 0; i < CachedMeshData.Vertices.Num(); ++i)
	{
		const FVector& Pos = CachedMeshData.Vertices[i];

		FVertexID VertexID;
		if (const FVertexID* Existing = PositionToVertexID.Find(Pos))
		{
			VertexID = *Existing;
		}
		else
		{
			VertexID = MeshDescription.CreateVertex();
			VertexPositions[VertexID] = FVector3f(Pos);
			PositionToVertexID.Add(Pos, VertexID);
		}

		const FVertexInstanceID InstanceID = MeshDescription.CreateVertexInstance(VertexID);
		InstanceNormals[InstanceID] = FVector3f(CachedMeshData.Normals[i]);
		InstanceTangents[InstanceID] = FVector3f(CachedMeshData.Tangents[i]);
		InstanceBinormalSigns[InstanceID] = 1.0f;
		InstanceColors[InstanceID] = bHasBiomeBlendData
			? FVector4f(CachedBiomeBlendWeights[i].R, CachedBiomeBlendWeights[i].G, CachedBiomeBlendWeights[i].B, CachedBiomeBlendWeights[i].A)
			: FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
		InstanceUVs.Set(InstanceID, 0, FVector2f(CachedMeshData.UVs[i]));
		if (bHasBiomeBlendData)
		{
			InstanceUVs.Set(InstanceID, 1, FVector2f(CachedBiomeBlendUV1[i]));
			InstanceUVs.Set(InstanceID, 2, FVector2f(CachedBiomeBlendUV2[i]));
		}

		InstanceIDs[i] = InstanceID;
	}

	for (int32 TriStart = 0; TriStart < CachedMeshData.Triangles.Num(); TriStart += 3)
	{
		const FVertexInstanceID Corners[3] =
		{
			InstanceIDs[CachedMeshData.Triangles[TriStart]],
			InstanceIDs[CachedMeshData.Triangles[TriStart + 1]],
			InstanceIDs[CachedMeshData.Triangles[TriStart + 2]],
		};
		MeshDescription.CreateTriangle(PolygonGroupID, Corners);
	}

	UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
	BuildParams.bBuildSimpleCollision = true;
	BuildParams.bFastBuild = false;
	BuildParams.bCommitMeshDescription = true;

	FMeshNaniteSettings NewNaniteSettings = NewStaticMesh->GetNaniteSettings();
	NewNaniteSettings.bEnabled = false; // flip on later once you're baking at final terrain density.
	NewStaticMesh->SetNaniteSettings(NewNaniteSettings);

	NewStaticMesh->BuildFromMeshDescriptions({ &MeshDescription }, BuildParams);

	NewStaticMesh->GetStaticMaterials().Empty();
	NewStaticMesh->GetStaticMaterials().Add(FStaticMaterial());

	// --- Attach celestial body metadata so it survives the bake - a plain UStaticMesh otherwise ---
	// has no actor, no Profile reference, nothing at all once this function returns. One
	// UAssetUserData subclass per body type; whichever matches Profile's actual class gets attached.
	// Nothing is attached if Profile is unassigned - there's no meaningful body type to record.
	// NOTE: intentionally non-const locals below - TSoftObjectPtr<T>::operator=(T*) wants an exact,
	// non-const T* match; a const T* here previously triggered UE 5.8's deprecated "incompatible
	// pointer type" implicit-conversion path (a real compiler warning, not a false alarm - that
	// path is slated for removal in a future engine version).
	if (USolarOrbzPlanetProfile* PlanetProfileForBake = Cast<USolarOrbzPlanetProfile>(Profile))
	{
		USolarOrbzPlanetMeshUserData* BodyData = NewObject<USolarOrbzPlanetMeshUserData>(NewStaticMesh);
		BodyData->RadiusMeters = RadiusMeters;
		BodyData->SourceProfile = PlanetProfileForBake;
		BodyData->SurfaceGravity = PlanetProfileForBake->GetSurfaceGravity();
		BodyData->Density = PlanetProfileForBake->GetDensity();
		BodyData->AtmosphereDensityAtSeaLevel = PlanetProfileForBake->GetAtmosphereDensityAtSeaLevel();
		BodyData->AtmospherePressureKPa = PlanetProfileForBake->GetAtmospherePressureKPa();
		NewStaticMesh->AddAssetUserData(BodyData);
		UE_LOG(LogSolarOrbz, Log, TEXT("SolarOrbz: baked Planet metadata (gravity %.2f m/s^2, atmosphere %.3f kg/m^3) into the static mesh."), BodyData->SurfaceGravity, BodyData->AtmosphereDensityAtSeaLevel);
	}
	else if (USolarOrbzStarProfile* StarProfileForBake = Cast<USolarOrbzStarProfile>(Profile))
	{
		USolarOrbzStarMeshUserData* BodyData = NewObject<USolarOrbzStarMeshUserData>(NewStaticMesh);
		BodyData->RadiusMeters = RadiusMeters;
		BodyData->SourceProfile = StarProfileForBake;
		BodyData->Luminosity = StarProfileForBake->Luminosity;
		BodyData->SurfaceTemperature = StarProfileForBake->SurfaceTemperature;
		BodyData->SpectralClass = StarProfileForBake->SpectralClass;
		NewStaticMesh->AddAssetUserData(BodyData);
		UE_LOG(LogSolarOrbz, Log, TEXT("SolarOrbz: baked Star metadata (luminosity %.2f, %.0fK) into the static mesh."), BodyData->Luminosity, BodyData->SurfaceTemperature);
	}
	else if (USolarOrbzAsteroidProfile* AsteroidProfileForBake = Cast<USolarOrbzAsteroidProfile>(Profile))
	{
		USolarOrbzAsteroidMeshUserData* BodyData = NewObject<USolarOrbzAsteroidMeshUserData>(NewStaticMesh);
		BodyData->RadiusMeters = RadiusMeters;
		BodyData->SourceProfile = AsteroidProfileForBake;
		BodyData->Density = AsteroidProfileForBake->Density;
		BodyData->Composition = AsteroidProfileForBake->Composition;
		BodyData->Irregularity = AsteroidProfileForBake->Irregularity;
		NewStaticMesh->AddAssetUserData(BodyData);
		UE_LOG(LogSolarOrbz, Log, TEXT("SolarOrbz: baked Asteroid metadata (density %.0f kg/m^3) into the static mesh."), BodyData->Density);
	}
	else if (Profile)
	{
		UE_LOG(LogSolarOrbz, Warning, TEXT("SolarOrbz: Profile is assigned but isn't a recognized Planet/Star/Asteroid Profile subclass - no metadata baked into the static mesh."));
	}
	else
	{
		UE_LOG(LogSolarOrbz, Log, TEXT("SolarOrbz: no Profile assigned - baking geometry only, no celestial body metadata."));
	}

	NewStaticMesh->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewStaticMesh);
	Package->SetDirtyFlag(true);

	const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(Package, NewStaticMesh, *PackageFileName, SaveArgs);

	UE_LOG(LogSolarOrbz, Log, TEXT("SolarOrbz: baked '%s' -> %s"), *CleanAssetName, bSaved ? TEXT("saved to disk") : TEXT("created in memory but NOT saved"));
#endif
}
