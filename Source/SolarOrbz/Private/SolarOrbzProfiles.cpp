// SolarOrbz - Celestial Body Profiles implementation.
// Only USolarOrbzProceduralPlanetProfile needs real logic - every other class in this subsystem
// is either a pure-virtual interface or plain data with inline accessors.

#include "SolarOrbzProfiles.h"
#include "Math/RandomStream.h"

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
