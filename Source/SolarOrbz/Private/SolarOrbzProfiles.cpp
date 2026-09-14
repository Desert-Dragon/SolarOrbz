// SolarOrbz - Celestial Body Profiles implementation.
// Only USolarOrbzProceduralPlanetProfile needs real logic beyond the MeshUserData accessors below -
// every other Profile class in this subsystem is either a pure-virtual interface or plain data
// with inline accessors.

#include "SolarOrbzProfiles.h"
#include "Math/RandomStream.h"
#include "Engine/StaticMesh.h"

void USolarOrbzProceduralPlanetProfile::EnsureResolved() const
{
	if (bResolved)
	{
		return;
	}

	FRandomStream Stream(Seed);

	ResolvedSurfaceGravity = Stream.FRandRange(MinSurfaceGravity, MaxSurfaceGravity);
	ResolvedDensity = Stream.FRandRange(MinDensity, MaxDensity);

	if (Stream.FRand() <= ChanceOfAtmosphere)
	{
		ResolvedAtmosphereDensityAtSeaLevel = Stream.FRandRange(MinAtmosphereDensityAtSeaLevel, MaxAtmosphereDensityAtSeaLevel);
		ResolvedAtmospherePressureKPa = Stream.FRandRange(MinAtmospherePressureKPa, MaxAtmospherePressureKPa);
	}
	else
	{
		ResolvedAtmosphereDensityAtSeaLevel = 0.0f;
		ResolvedAtmospherePressureKPa = 0.0f;
	}

	ResolvedNumContinents = Stream.RandRange(MinContinents, MaxContinents);
	ResolvedNumIslands = Stream.RandRange(MinIslands, MaxIslands);
	bResolvedHasNorthPolarContinent = Stream.FRand() <= ChanceOfPolarContinent;
	bResolvedHasSouthPolarContinent = Stream.FRand() <= ChanceOfPolarContinent;

	bResolved = true;
}

// ================================================================================================
// USolarOrbzCelestialBodyMeshUserData family
// ================================================================================================
ESolarOrbzCelestialBodyType USolarOrbzCelestialBodyMeshUserData::GetCelestialBodyType(const UStaticMesh* Mesh)
{
	if (!Mesh)
	{
		return ESolarOrbzCelestialBodyType::None;
	}

	// const_cast is safe here - GetAssetUserData<T>() only reads the existing user data array, it
	// doesn't mutate the mesh. The template just isn't const-qualified on the engine side.
	if (const USolarOrbzCelestialBodyMeshUserData* Data = const_cast<UStaticMesh*>(Mesh)->GetAssetUserData<USolarOrbzCelestialBodyMeshUserData>())
	{
		return Data->BodyType;
	}

	return ESolarOrbzCelestialBodyType::None;
}

USolarOrbzPlanetMeshUserData* USolarOrbzPlanetMeshUserData::GetPlanetData(const UStaticMesh* Mesh)
{
	return Mesh ? const_cast<UStaticMesh*>(Mesh)->GetAssetUserData<USolarOrbzPlanetMeshUserData>() : nullptr;
}

USolarOrbzStarMeshUserData* USolarOrbzStarMeshUserData::GetStarData(const UStaticMesh* Mesh)
{
	return Mesh ? const_cast<UStaticMesh*>(Mesh)->GetAssetUserData<USolarOrbzStarMeshUserData>() : nullptr;
}

USolarOrbzAsteroidMeshUserData* USolarOrbzAsteroidMeshUserData::GetAsteroidData(const UStaticMesh* Mesh)
{
	return Mesh ? const_cast<UStaticMesh*>(Mesh)->GetAssetUserData<USolarOrbzAsteroidMeshUserData>() : nullptr;
}
