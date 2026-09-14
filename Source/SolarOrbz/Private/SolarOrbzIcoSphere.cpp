// SolarOrbz - IcoSphere subsystem implementation.

#include "SolarOrbzIcoSphere.h"

#include "Engine/Texture2D.h"

#include "SolarOrbzTerrainLayers.h"
#include "SolarOrbzBiomeSystem.h"
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

// ================================================================================================
// FSolarOrbzIcoSphereGenerator
// ================================================================================================
namespace SolarOrbzIcoSphere
{
	// Using an explicit double constant rather than the engine's PI macro to
	// avoid precision loss now that FVector components are double (LWC) by default.
	static constexpr double PI_D = 3.14159265358979323846;

	// Edge length of a regular icosahedron with circumradius 1.0.
	static constexpr double UnitCircumradiusEdgeLength = 1.0514622242382672;
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

int32 FSolarOrbzIcoSphereGenerator::ComputeSubdivisionLevelForEdgeLength(float Radius, float TargetEdgeLength, int32 MaxSubdivisions, int32* OutUnclampedLevel)
{
	using namespace SolarOrbzIcoSphere;

	const double BaseEdgeLength = FMath::Max(Radius, KINDA_SMALL_NUMBER) * UnitCircumradiusEdgeLength;

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

int32 FSolarOrbzIcoSphereGenerator::Generate(float Radius, float VerticesPerMeter, FSolarOrbzIcoSphereMeshData& OutMeshData, int32 MaxSubdivisions, int32* OutUnclampedLevel)
{
	// UE units are centimeters, so "per meter" density -> divide 100 by it to get target edge length in cm.
	const float TargetEdgeLength = VerticesPerMeter > KINDA_SMALL_NUMBER ? (100.0f / VerticesPerMeter) : Radius;

	const int32 Level = ComputeSubdivisionLevelForEdgeLength(Radius, TargetEdgeLength, MaxSubdivisions, OutUnclampedLevel);
	GenerateAtSubdivisionLevel(Radius, Level, OutMeshData);
	return Level;
}

void FSolarOrbzIcoSphereGenerator::GenerateAtSubdivisionLevel(float Radius, int32 SubdivisionLevel, FSolarOrbzIcoSphereMeshData& OutMeshData)
{
	FBuildContext Context;
	BuildBaseIcosahedron(Context);

	SubdivisionLevel = FMath::Max(SubdivisionLevel, 0);
	for (int32 i = 0; i < SubdivisionLevel; ++i)
	{
		SubdivideOnce(Context);
	}

	FixUVSeamsAndFinalize(Context, FMath::Max(Radius, KINDA_SMALL_NUMBER), OutMeshData);
}

void FSolarOrbzIcoSphereGenerator::BuildBaseIcosahedron(FBuildContext& Context)
{
	Context.Positions.Reset(12);
	Context.Indices.Reset(60);
	Context.MidpointCache.Reset();

	const double T = (1.0 + FMath::Sqrt(5.0)) / 2.0;

	// 12 vertices of a regular icosahedron, normalized onto the unit sphere.
	const FVector RawVerts[12] =
	{
		FVector(-1,  T,  0), FVector( 1,  T,  0), FVector(-1, -T,  0), FVector( 1, -T,  0),
		FVector( 0, -1,  T), FVector( 0,  1,  T), FVector( 0, -1, -T), FVector( 0,  1, -T),
		FVector( T,  0, -1), FVector( T,  0,  1), FVector(-T,  0, -1), FVector(-T,  0,  1),
	};

	for (const FVector& V : RawVerts)
	{
		Context.Positions.Add(V.GetSafeNormal());
	}

	// 20 triangular faces, wound so normals point outward (CCW when viewed from outside).
	const int32 RawFaces[20][3] =
	{
		{0, 11, 5}, {0, 5, 1},  {0, 1, 7},  {0, 7, 10}, {0, 10, 11},
		{1, 5, 9},  {5, 11, 4}, {11, 10, 2},{10, 7, 6}, {7, 1, 8},
		{3, 9, 4},  {3, 4, 2},  {3, 2, 6},  {3, 6, 8},  {3, 8, 9},
		{4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},  {9, 8, 1},
	};

	for (const int32 (&Face)[3] : RawFaces)
	{
		Context.Indices.Add(Face[0]);
		Context.Indices.Add(Face[1]);
		Context.Indices.Add(Face[2]);
	}
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

void FSolarOrbzIcoSphereGenerator::FixUVSeamsAndFinalize(const FBuildContext& Context, float Radius, FSolarOrbzIcoSphereMeshData& OutMeshData)
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

	if (RegenTriggers.Contains(PropertyChangedEvent.GetPropertyName()))
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
	// convert once, right here, and every internal calculation below stays in cm.
	const float RadiusCm = RadiusMeters * 100.0f;

	LastSubdivisionLevelUsed = FSolarOrbzIcoSphereGenerator::Generate(RadiusCm, VerticesPerMeter, CachedMeshData, MaxSubdivisions, &LastRequestedSubdivisionLevel);

	if (LastRequestedSubdivisionLevel > LastSubdivisionLevelUsed)
	{
		UE_LOG(LogSolarOrbz, Warning,
			TEXT("SolarOrbz: Vertices Per Meter (%.4f) would need subdivision level %d at this radius, but Max Subdivisions caps it at %d - the density setting is NOT being reached. Raise Max Subdivisions or lower Vertices Per Meter."),
			VerticesPerMeter, LastRequestedSubdivisionLevel, LastSubdivisionLevelUsed);
	}

	// Keep the pristine outward sphere direction for every vertex - both passes displace
	// along this, not along the (changing) recomputed normal, so height stays purely radial.
	TArray<FVector> OriginalUnitDirections = CachedMeshData.Normals;

	// Resolved once, reused below both for Continent-style layers reading landmass counts and for
	// Climate Simulation's atmosphere density - null if no Profile is assigned, or it's a Star/
	// Asteroid Profile instead of a Planet Profile (both valid, just nothing to read here for them).
	const USolarOrbzPlanetProfile* PlanetProfile = Cast<USolarOrbzPlanetProfile>(Profile);

	// --- Pass A: base terrain (procedural noise and/or authored heightmap). ---
	if (TerrainStack)
	{
		// Profile data (e.g. Continent's landmass counts) must reach layers before PrepareLayers()
		// bakes them - Bake() only runs once per regenerate, so this has to happen first, not after.
		TerrainStack->ApplyProfile(PlanetProfile);

		// Whole-surface bake first (e.g. erosion) - must happen before any per-point EvaluateHeight
		// calls below, including the ones the Climate Simulation will make against this same stack,
		// so rain shadows react to eroded terrain rather than the pre-erosion noise.
		TerrainStack->PrepareLayers(RadiusCm);

		float MinHeight = TNumericLimits<float>::Max(), MaxHeight = TNumericLimits<float>::Lowest(), SumHeight = 0.0f;

		for (int32 i = 0; i < CachedMeshData.Vertices.Num(); ++i)
		{
			const FVector& UnitDirection = OriginalUnitDirections[i];
			const float Height = TerrainStack->EvaluateHeight(UnitDirection, CachedMeshData.UVs[i]);
			CachedMeshData.Vertices[i] += UnitDirection * Height;

			MinHeight = FMath::Min(MinHeight, Height);
			MaxHeight = FMath::Max(MaxHeight, Height);
			SumHeight += Height;
		}

		const float AvgHeight = CachedMeshData.Vertices.Num() > 0 ? SumHeight / CachedMeshData.Vertices.Num() : 0.0f;
		const float PeakToPeakCm = MaxHeight - MinHeight;
		UE_LOG(LogSolarOrbz, Log,
			TEXT("SolarOrbz Terrain: %d enabled layer(s), height range %.1fcm..%.1fcm (avg %.1fcm), peak-to-peak %.1fcm = %.4f%% of radius (%.1fcm)"),
			TerrainStack->Layers.Num(), MinHeight, MaxHeight, AvgHeight, PeakToPeakCm,
			RadiusCm > KINDA_SMALL_NUMBER ? (PeakToPeakCm / RadiusCm) * 100.0f : 0.0f, RadiusCm);

		if (PeakToPeakCm < KINDA_SMALL_NUMBER)
		{
			UE_LOG(LogSolarOrbz, Warning,
				TEXT("SolarOrbz Terrain: peak-to-peak height is ~0 - either every layer is disabled/has 0 Weight, or the stack has no layers at all. Check the Layers array on your TerrainLayerStack asset."));
		}
		else if (PeakToPeakCm / RadiusCm < 0.001f) // under 0.1% of radius
		{
			UE_LOG(LogSolarOrbz, Warning,
				TEXT("SolarOrbz Terrain: peak-to-peak height is only %.4f%% of the planet's radius - this is real terrain, not a bug, but at that scale it will look essentially flat when viewing the whole planet (this is also true of real Earth - Everest is only ~0.14%% of Earth's radius). Zoom the camera in close to the surface to confirm the bumps are really there, or temporarily raise elevation values well past realistic for an at-a-glance test."),
				(PeakToPeakCm / RadiusCm) * 100.0f);
		}

		// Recompute now so Pass B has real slope data to mask against, not the pristine sphere's.
		FSolarOrbzIcoSphereGenerator::RecomputeSmoothNormals(CachedMeshData);
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

	// --- Pass B: biome-specific detail, masked by climate/composite conditions and blended on top. ---
	// Rendering has three mutually-exclusive modes sharing the same vertex color + UV1/UV2 channels:
	// debug colors (bShowBiomeDebugColors), texture-array blending (BiomeBlendMaterial assigned),
	// or plain DefaultMaterial with none of this data populated.
	const bool bWantsBlendMaterial = !bShowBiomeDebugColors && BiomeStack && BiomeBlendMaterial;
	constexpr int32 MaxBlendedBiomes = 4; // matches vertex color's 4 channels (RGBA) and UV1/UV2's 2+2 float slots

	TArray<FLinearColor> BiomeDebugColors;
	TArray<FVector2D> BiomeBlendUV1; // (Index0, Index1)
	TArray<FVector2D> BiomeBlendUV2; // (Index2, Index3)
	TArray<FLinearColor> BiomeBlendWeights; // (Weight0, Weight1, Weight2, Weight3)

	if (BiomeStack)
	{
		if (bShowBiomeDebugColors)
		{
			BiomeDebugColors.SetNum(CachedMeshData.Vertices.Num());
		}

		if (bWantsBlendMaterial)
		{
#if WITH_EDITOR
			// Auto-rebuild if the texture array looks out of sync with the current biome list, so
			// first-time setup (and adding/removing a biome) just works without a separate manual
			// step - BuildBiomeTextureArray is still exposed for an explicit rebuild too.
			TArray<USolarOrbzBiome*> UniqueBiomesCheck;
			BiomeStack->GetUniqueBiomes(UniqueBiomesCheck);
			const bool bArrayStale = !BiomeStack->BiomeTextureArray
				|| BiomeStack->BiomeTextureArray->SourceTextures.Num() != UniqueBiomesCheck.Num();
			if (bArrayStale)
			{
				BiomeStack->BuildBiomeTextureArray();
			}
#endif
			BiomeBlendUV1.SetNum(CachedMeshData.Vertices.Num());
			BiomeBlendUV2.SetNum(CachedMeshData.Vertices.Num());
			BiomeBlendWeights.Init(FLinearColor(0, 0, 0, 0), CachedMeshData.Vertices.Num());
		}

		int32 NumWithClimateData = 0;
		int32 NumDominantBiomeHits = 0;

		TArray<int32> TopBiomeIndices;
		TArray<float> TopBiomeWeights;

		for (int32 i = 0; i < CachedMeshData.Vertices.Num(); ++i)
		{
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
				++NumWithClimateData;
			}

			const float BiomeHeight = BiomeStack->EvaluateBiomeTerrainContribution(Context);
			CachedMeshData.Vertices[i] += UnitDirection * BiomeHeight;

			if (bShowBiomeDebugColors)
			{
				if (const USolarOrbzBiome* Dominant = BiomeStack->GetDominantBiome(Context))
				{
					BiomeDebugColors[i] = Dominant->PreviewColor;
					++NumDominantBiomeHits;
				}
				else
				{
					BiomeDebugColors[i] = FLinearColor::Black; // no biome layer applies here
				}
			}
			else if (bWantsBlendMaterial)
			{
				BiomeStack->EvaluateTopWeightedBiomes(Context, MaxBlendedBiomes, TopBiomeIndices, TopBiomeWeights);
				if (TopBiomeIndices.Num() > 0)
				{
					++NumDominantBiomeHits; // reusing the same stat: "at least one biome matched here"
				}

				auto IndexOrPad = [&TopBiomeIndices](int32 Slot) { return TopBiomeIndices.IsValidIndex(Slot) ? (float)TopBiomeIndices[Slot] : -1.0f; };
				auto WeightOrPad = [&TopBiomeWeights](int32 Slot) { return TopBiomeWeights.IsValidIndex(Slot) ? TopBiomeWeights[Slot] : 0.0f; };

				BiomeBlendUV1[i] = FVector2D(IndexOrPad(0), IndexOrPad(1));
				BiomeBlendUV2[i] = FVector2D(IndexOrPad(2), IndexOrPad(3));
				BiomeBlendWeights[i] = FLinearColor(WeightOrPad(0), WeightOrPad(1), WeightOrPad(2), WeightOrPad(3));
			}
		}

		FSolarOrbzIcoSphereGenerator::RecomputeSmoothNormals(CachedMeshData);

		if (bShowBiomeDebugColors)
		{
			UE_LOG(LogSolarOrbz, Log,
				TEXT("SolarOrbz Biome Debug: %d/%d verts had climate data, %d/%d verts matched a biome layer (rest rendered black = no layer applies there)."),
				NumWithClimateData, CachedMeshData.Vertices.Num(), NumDominantBiomeHits, CachedMeshData.Vertices.Num());

			if (NumDominantBiomeHits == 0)
			{
				UE_LOG(LogSolarOrbz, Warning, TEXT("SolarOrbz Biome Debug: not a single vertex matched any biome layer's mask - check each layer's Mask Preset ranges (Min/Max/Falloff) against the Moisture/Temperature/Elevation stats logged above."));
			}
		}
		else if (bWantsBlendMaterial)
		{
			UE_LOG(LogSolarOrbz, Log,
				TEXT("SolarOrbz Biome Blend: %d/%d verts had climate data, %d/%d verts matched at least one biome (rest render with all-zero weights - no biome applies there)."),
				NumWithClimateData, CachedMeshData.Vertices.Num(), NumDominantBiomeHits, CachedMeshData.Vertices.Num());

			if (NumDominantBiomeHits == 0)
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
		VertexColorsToUse = &BiomeBlendWeights;
		UV1ToUse = &BiomeBlendUV1;
		UV2ToUse = &BiomeBlendUV2;
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
	InstanceUVs.SetNumChannels(1);

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
		InstanceColors[InstanceID] = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
		InstanceUVs.Set(InstanceID, 0, FVector2f(CachedMeshData.UVs[i]));

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
