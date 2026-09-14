// SolarOrbz - Celestial Body Profiles. A profile is a swappable asset holding the physical
// identity of a body - gravity, atmosphere, landmass counts for planets; luminosity for stars;
// composition for asteroids - kept separate from the Terrain/Climate/Biome pipeline assets so any
// Terrain Layer Stack can be paired with any profile. This is what makes the system modular across
// wildly different body types: a star or an asteroid simply doesn't assign a ClimateSimulation or
// BiomeStack at all, while still going through the same actor and mesh generator.
//
// Two ways to fill in a planet's data, matching two different authoring needs:
//   - USolarOrbzAuthoredPlanetProfile: exact values you type in by hand - this is what a
//     hand-placed, recognizable BattleTech world uses. Turn your existing Earth-like setup into
//     one of these.
//   - USolarOrbzProceduralPlanetProfile: Min/Max ranges plus a seed. Resolves to concrete values
//     the first time they're read, then caches them, so the same seed always produces the same
//     planet. Swap this in wholesale for a fully-procedural world instead of toggling individual
//     fields between "authored" and "auto".
//
// Both derive from USolarOrbzPlanetProfile, which is the actual interface everything else in the
// plugin talks to (ClimateSimulation, the actor, and eventually the Continent Layer) - so none of
// that code needs to know or care which flavor of profile it's holding.
//
// Note on Units meta tags: Unreal's Units= property meta only accepts single, whitelisted units
// (e.g. "Kelvin", "m") - it has no support for composite units like "kg/m^3" or "m/s^2" (compiling
// with one throws "Unrecognized units"). Properties below that are physically composite units say
// so in their doc comment instead of using Units=.

#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "Engine/DataAsset.h"
#include "SolarOrbzProfiles.generated.h"

class UStaticMesh;

// ================================================================================================
// USolarOrbzCelestialBodyProfile - abstract base for every body type (planet, star, asteroid).
// Deliberately minimal - almost nothing is shared between a planet's atmosphere data and a star's
// luminosity, so the real interfaces live on the concrete branches below, not here.
// ================================================================================================
UCLASS(Abstract, BlueprintType)
class SOLARORBZ_API USolarOrbzCelestialBodyProfile : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Optional flavor text / lore blurb - not read by any code, purely for your own reference (e.g. a BattleTech canonical name or description). */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Profile", meta = (MultiLine = true))
	FText Description;
};

// ================================================================================================
// FSolarOrbzAtmosphereGas / USolarOrbzPlanetProfile - the interface every planet profile (authored
// or procedural) implements. Code elsewhere in the plugin holds a USolarOrbzPlanetProfile* and
// calls these accessors without caring which concrete subclass it actually is.
// ================================================================================================

/** One component gas in an atmosphere mixture. */
USTRUCT(BlueprintType)
struct SOLARORBZ_API FSolarOrbzAtmosphereGas
{
	GENERATED_BODY()

	/** e.g. "Nitrogen", "Oxygen", "CarbonDioxide", "Methane", "Hydrogen". Not an enum so you can name anything without waiting on an engineering change. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Atmosphere")
	FName GasName;

	/** Percentage of total atmospheric volume, 0-100. Not enforced to sum to 100 across all entries - under/over just means "trace/unspecified remainder" or "over-specified", your call. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Atmosphere", meta = (ClampMin = "0.0", ClampMax = "100.0"))
	float PercentageByVolume = 0.0f;
};

UCLASS(Abstract, BlueprintType)
class SOLARORBZ_API USolarOrbzPlanetProfile : public USolarOrbzCelestialBodyProfile
{
	GENERATED_BODY()

public:
	/** Surface gravity, m/s^2. Earth is ~9.81. Read this for player movement/physics so gravity is correct the instant someone lands, without a separate per-planet gravity volume to keep in sync. */
	virtual float GetSurfaceGravity() const PURE_VIRTUAL(USolarOrbzPlanetProfile::GetSurfaceGravity, return 9.81f;);

	/** Average planetary density, kg/m^3. Earth is ~5514. Distinct from atmosphere density below - this is the bulk rock/metal/ice density of the body itself. */
	virtual float GetDensity() const PURE_VIRTUAL(USolarOrbzPlanetProfile::GetDensity, return 5514.0f;);

	/** Air density at sea level, kg/m^3. Earth ~1.225; Mars ~0.02; Venus ~65; 0 for an airless body (the Moon, most asteroids-as-planets). This is what ClimateSimulation reads instead of owning its own copy - one number, one source of truth. */
	virtual float GetAtmosphereDensityAtSeaLevel() const PURE_VIRTUAL(USolarOrbzPlanetProfile::GetAtmosphereDensityAtSeaLevel, return 1.225f;);

	/** Atmospheric pressure at sea level, kPa. Earth is ~101.325. 0 for airless bodies. */
	virtual float GetAtmospherePressureKPa() const PURE_VIRTUAL(USolarOrbzPlanetProfile::GetAtmospherePressureKPa, return 101.325f;);

	/** Atmosphere composition by gas. Empty for an airless body. */
	virtual TArray<FSolarOrbzAtmosphereGas> GetAtmosphereComposition() const PURE_VIRTUAL(USolarOrbzPlanetProfile::GetAtmosphereComposition, return {};);

	/**
	 * How many continent-scale landmasses (not islands - see GetNumIslands) this planet should
	 * have. Not read by anything yet - this is here for the upcoming Continent Layer to consume,
	 * so profiles can be authored now and the terrain will pick it up once that layer exists.
	 */
	virtual int32 GetNumContinents() const PURE_VIRTUAL(USolarOrbzPlanetProfile::GetNumContinents, return 1;);

	/** Roughly how many smaller, incidental islands beyond the continents above. A minimum/approximate count, not an exact one - not read by anything yet, same status as GetNumContinents. */
	virtual int32 GetNumIslands() const PURE_VIRTUAL(USolarOrbzPlanetProfile::GetNumIslands, return 0;);

	/** Whether this planet has a distinct landmass centered on its north pole (an Antarctica-analogue, just at the other end). Not read by anything yet, same status as GetNumContinents. */
	virtual bool HasNorthPolarContinent() const PURE_VIRTUAL(USolarOrbzPlanetProfile::HasNorthPolarContinent, return false;);

	/** Whether this planet has a distinct landmass centered on its south pole. Not read by anything yet, same status as GetNumContinents. */
	virtual bool HasSouthPolarContinent() const PURE_VIRTUAL(USolarOrbzPlanetProfile::HasSouthPolarContinent, return false;);
};

// ================================================================================================
// USolarOrbzAuthoredPlanetProfile - exact, hand-typed values. Turn an existing hand-authored
// planet (like your Earth setup) into one of these: create an instance, fill in its real numbers,
// and assign it to that planet's actor.
// ================================================================================================
UCLASS(BlueprintType, meta = (DisplayName = "Authored Planet Profile"))
class SOLARORBZ_API USolarOrbzAuthoredPlanetProfile : public USolarOrbzPlanetProfile
{
	GENERATED_BODY()

public:
	/** m/s^2. Earth is ~9.81. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Physical", meta = (ClampMin = "0.0"))
	float SurfaceGravity = 9.81f;

	/** kg/m^3. Earth is ~5514 (bulk planetary density, not atmosphere). */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Physical", meta = (ClampMin = "0.0"))
	float Density = 5514.0f;

	/** kg/m^3. Earth is ~1.225; Mars ~0.02; Venus ~65; 0 for airless. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Atmosphere", meta = (ClampMin = "0.0"))
	float AtmosphereDensityAtSeaLevel = 1.225f;

	/** kPa. Earth is ~101.325. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Atmosphere", meta = (ClampMin = "0.0"))
	float AtmospherePressureKPa = 101.325f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Atmosphere")
	TArray<FSolarOrbzAtmosphereGas> AtmosphereComposition;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Landmasses", meta = (ClampMin = "0"))
	int32 NumContinents = 7;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Landmasses", meta = (ClampMin = "0"))
	int32 NumIslands = 0;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Landmasses")
	bool bHasNorthPolarContinent = false;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Landmasses")
	bool bHasSouthPolarContinent = true;

	virtual float GetSurfaceGravity() const override { return SurfaceGravity; }
	virtual float GetDensity() const override { return Density; }
	virtual float GetAtmosphereDensityAtSeaLevel() const override { return AtmosphereDensityAtSeaLevel; }
	virtual float GetAtmospherePressureKPa() const override { return AtmospherePressureKPa; }
	virtual TArray<FSolarOrbzAtmosphereGas> GetAtmosphereComposition() const override { return AtmosphereComposition; }
	virtual int32 GetNumContinents() const override { return NumContinents; }
	virtual int32 GetNumIslands() const override { return NumIslands; }
	virtual bool HasNorthPolarContinent() const override { return bHasNorthPolarContinent; }
	virtual bool HasSouthPolarContinent() const override { return bHasSouthPolarContinent; }
};

// ================================================================================================
// USolarOrbzProceduralPlanetProfile - Min/Max ranges plus a seed, resolved to concrete values on
// first read and cached from then on (same seed always gives the same planet). Swap this in
// wholesale instead of an Authored profile for a fully-procedural world.
//
// Known simplification: atmosphere composition isn't randomized yet (GetAtmosphereComposition
// always returns empty) - only density and pressure are. Gas-mixture randomization is a stub for
// future work rather than something with an obvious "reasonable range" to default to.
// ================================================================================================
UCLASS(BlueprintType, meta = (DisplayName = "Procedural Planet Profile"))
class SOLARORBZ_API USolarOrbzProceduralPlanetProfile : public USolarOrbzPlanetProfile
{
	GENERATED_BODY()

public:
	/** Same seed always resolves to the same values - change this to reroll. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Procedural")
	int32 Seed = 0;

	/** m/s^2 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Physical", meta = (ClampMin = "0.0"))
	float MinSurfaceGravity = 3.0f;
	/** m/s^2 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Physical", meta = (ClampMin = "0.0"))
	float MaxSurfaceGravity = 15.0f;

	/** kg/m^3 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Physical", meta = (ClampMin = "0.0"))
	float MinDensity = 3000.0f;
	/** kg/m^3 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Physical", meta = (ClampMin = "0.0"))
	float MaxDensity = 6000.0f;

	/** Chance (0-1) the planet has any atmosphere at all. Rolled first - if it fails, density/pressure both resolve to 0 regardless of the ranges below. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Atmosphere", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ChanceOfAtmosphere = 0.7f;

	/** kg/m^3 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Atmosphere", meta = (ClampMin = "0.0"))
	float MinAtmosphereDensityAtSeaLevel = 0.02f;
	/** kg/m^3 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Atmosphere", meta = (ClampMin = "0.0"))
	float MaxAtmosphereDensityAtSeaLevel = 5.0f;

	/** kPa */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Atmosphere", meta = (ClampMin = "0.0"))
	float MinAtmospherePressureKPa = 1.0f;
	/** kPa */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Atmosphere", meta = (ClampMin = "0.0"))
	float MaxAtmospherePressureKPa = 300.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Landmasses", meta = (ClampMin = "0"))
	int32 MinContinents = 3;
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Landmasses", meta = (ClampMin = "0"))
	int32 MaxContinents = 9;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Landmasses", meta = (ClampMin = "0"))
	int32 MinIslands = 0;
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Landmasses", meta = (ClampMin = "0"))
	int32 MaxIslands = 20;

	/** Chance (0-1), rolled independently, of a distinct landmass centered on each pole. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Landmasses", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ChanceOfPolarContinent = 0.3f;

	virtual float GetSurfaceGravity() const override { EnsureResolved(); return ResolvedSurfaceGravity; }
	virtual float GetDensity() const override { EnsureResolved(); return ResolvedDensity; }
	virtual float GetAtmosphereDensityAtSeaLevel() const override { EnsureResolved(); return ResolvedAtmosphereDensityAtSeaLevel; }
	virtual float GetAtmospherePressureKPa() const override { EnsureResolved(); return ResolvedAtmospherePressureKPa; }
	virtual TArray<FSolarOrbzAtmosphereGas> GetAtmosphereComposition() const override { return {}; } // see class comment
	virtual int32 GetNumContinents() const override { EnsureResolved(); return ResolvedNumContinents; }
	virtual int32 GetNumIslands() const override { EnsureResolved(); return ResolvedNumIslands; }
	virtual bool HasNorthPolarContinent() const override { EnsureResolved(); return bResolvedHasNorthPolarContinent; }
	virtual bool HasSouthPolarContinent() const override { EnsureResolved(); return bResolvedHasSouthPolarContinent; }

private:
	void EnsureResolved() const;

	mutable bool bResolved = false;
	mutable float ResolvedSurfaceGravity = 0.0f;
	mutable float ResolvedDensity = 0.0f;
	mutable float ResolvedAtmosphereDensityAtSeaLevel = 0.0f;
	mutable float ResolvedAtmospherePressureKPa = 0.0f;
	mutable int32 ResolvedNumContinents = 0;
	mutable int32 ResolvedNumIslands = 0;
	mutable bool bResolvedHasNorthPolarContinent = false;
	mutable bool bResolvedHasSouthPolarContinent = false;
};

// ================================================================================================
// USolarOrbzStarProfile - stub for future star rendering/physics work. Not wired into any actor
// behavior yet; this establishes the modular shape (a body type sitting alongside Planet, with its
// own data) so the plugin's surface generator can eventually drive a star's turbulence/emissive
// look from the same pipeline.
// ================================================================================================
UENUM(BlueprintType)
enum class ESolarOrbzStarSpectralClass : uint8
{
	O, B, A, F, G, K, M
};

UCLASS(BlueprintType, meta = (DisplayName = "Star Profile"))
class SOLARORBZ_API USolarOrbzStarProfile : public USolarOrbzCelestialBodyProfile
{
	GENERATED_BODY()

public:
	/** Relative to the Sun (Sol = 1.0). */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Star", meta = (ClampMin = "0.0"))
	float Luminosity = 1.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Star", meta = (ClampMin = "0.0", Units = "Kelvin"))
	float SurfaceTemperature = 5778.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Star")
	ESolarOrbzStarSpectralClass SpectralClass = ESolarOrbzStarSpectralClass::G;
};

// ================================================================================================
// USolarOrbzAsteroidProfile - stub for future irregular-shape/composition work. Same modularity
// point as the star profile above - small scale is already supported today via a raw (any-scale)
// Noise Layer on the Terrain Stack; this asset is for the physical-identity data that goes with it.
// ================================================================================================
UENUM(BlueprintType)
enum class ESolarOrbzAsteroidComposition : uint8
{
	Rocky,
	Metallic,
	Icy,
	Carbonaceous,
};

UCLASS(BlueprintType, meta = (DisplayName = "Asteroid Profile"))
class SOLARORBZ_API USolarOrbzAsteroidProfile : public USolarOrbzCelestialBodyProfile
{
	GENERATED_BODY()

public:
	/** kg/m^3 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Asteroid", meta = (ClampMin = "0.0"))
	float Density = 2000.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Asteroid")
	ESolarOrbzAsteroidComposition Composition = ESolarOrbzAsteroidComposition::Rocky;

	/**
	 * 0-1, not yet wired into mesh generation - the current generator always displaces a true
	 * sphere, so today "irregular" is approximated by using Noise Layer's raw amplitude at a size
	 * comparable to the asteroid's own radius, not a genuine non-spherical base shape. This field
	 * is a placeholder for when non-spherical base shapes are supported.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Asteroid", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Irregularity = 0.5f;
};

// ================================================================================================
// USolarOrbzCelestialBodyMeshUserData family - baking a body's Profile into its actor is only
// half the story: once RegenerateMesh's preview gets baked out via BakeToStaticMeshAsset, the
// resulting UStaticMesh has no actor, no Profile reference, nothing - just geometry and a
// material. UAssetUserData is Unreal's built-in mechanism for attaching arbitrary metadata to an
// asset permanently (saved as part of the asset itself), which is exactly what's needed here.
//
// Two layers of fidelity, matching different failure modes:
//   - SourceProfile (a soft reference) gives full fidelity - every field the Profile has, not just
//     the ones copied below - as long as that Profile asset still exists in the project.
//   - The scalar fields below are copied in directly at bake time, so gravity/atmosphere/etc are
//     still readable even if the source Profile later gets deleted, renamed, or the shipped game
//     doesn't package editor-only Profile assets at all.
//
// One MeshUserData subclass per body type (Planet/Star/Asteroid) rather than one class with every
// possible field, mirroring how the Profile classes themselves are split - BodyType is on the
// shared base purely so a caller holding a bare UStaticMesh* can tell which concrete type (if any)
// is attached without trying each GetAssetUserData<T>() in turn itself.
// ================================================================================================

UENUM(BlueprintType)
enum class ESolarOrbzCelestialBodyType : uint8
{
	None,
	Planet,
	Star,
	Asteroid,
};

UCLASS(Abstract)
class SOLARORBZ_API USolarOrbzCelestialBodyMeshUserData : public UAssetUserData
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SolarOrbz|Baked")
	ESolarOrbzCelestialBodyType BodyType = ESolarOrbzCelestialBodyType::None;

	/** Radius at bake time, meters - matches the actor's Radius Meters, regardless of body type. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SolarOrbz|Baked")
	double RadiusMeters = 0.0;

	/** Returns None if Mesh is null or has no SolarOrbz body metadata attached (e.g. it was baked with no Profile assigned, or wasn't baked by SolarOrbz at all). Cheap - doesn't load anything, just checks which MeshUserData class (if any) is attached. */
	UFUNCTION(BlueprintCallable, Category = "SolarOrbz|Baked")
	static ESolarOrbzCelestialBodyType GetCelestialBodyType(const UStaticMesh* Mesh);
};

UCLASS(meta = (DisplayName = "SolarOrbz Planet Data"))
class SOLARORBZ_API USolarOrbzPlanetMeshUserData : public USolarOrbzCelestialBodyMeshUserData
{
	GENERATED_BODY()

public:
	USolarOrbzPlanetMeshUserData() { BodyType = ESolarOrbzCelestialBodyType::Planet; }

	/** The Profile this was baked from - full fidelity (atmosphere composition, landmass counts, etc) as long as this asset still exists. Null if the mesh was baked with no Profile assigned. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SolarOrbz|Baked")
	TSoftObjectPtr<USolarOrbzPlanetProfile> SourceProfile;

	/** m/s^2 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SolarOrbz|Baked")
	float SurfaceGravity = 9.81f;

	/** kg/m^3 - bulk planetary density, not atmosphere. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SolarOrbz|Baked")
	float Density = 5514.0f;

	/** kg/m^3 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SolarOrbz|Baked")
	float AtmosphereDensityAtSeaLevel = 1.225f;

	/** kPa */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SolarOrbz|Baked")
	float AtmospherePressureKPa = 101.325f;

	/** Returns null if Mesh has no planet data attached (not baked, baked with no Profile, or baked as a Star/Asteroid instead). */
	UFUNCTION(BlueprintCallable, Category = "SolarOrbz|Baked")
	static USolarOrbzPlanetMeshUserData* GetPlanetData(const UStaticMesh* Mesh);
};

UCLASS(meta = (DisplayName = "SolarOrbz Star Data"))
class SOLARORBZ_API USolarOrbzStarMeshUserData : public USolarOrbzCelestialBodyMeshUserData
{
	GENERATED_BODY()

public:
	USolarOrbzStarMeshUserData() { BodyType = ESolarOrbzCelestialBodyType::Star; }

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SolarOrbz|Baked")
	TSoftObjectPtr<USolarOrbzStarProfile> SourceProfile;

	/** Relative to the Sun (Sol = 1.0). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SolarOrbz|Baked")
	float Luminosity = 1.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SolarOrbz|Baked", meta = (Units = "Kelvin"))
	float SurfaceTemperature = 5778.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SolarOrbz|Baked")
	ESolarOrbzStarSpectralClass SpectralClass = ESolarOrbzStarSpectralClass::G;

	/** Returns null if Mesh has no star data attached (not baked, baked with no Profile, or baked as a Planet/Asteroid instead). */
	UFUNCTION(BlueprintCallable, Category = "SolarOrbz|Baked")
	static USolarOrbzStarMeshUserData* GetStarData(const UStaticMesh* Mesh);
};

UCLASS(meta = (DisplayName = "SolarOrbz Asteroid Data"))
class SOLARORBZ_API USolarOrbzAsteroidMeshUserData : public USolarOrbzCelestialBodyMeshUserData
{
	GENERATED_BODY()

public:
	USolarOrbzAsteroidMeshUserData() { BodyType = ESolarOrbzCelestialBodyType::Asteroid; }

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SolarOrbz|Baked")
	TSoftObjectPtr<USolarOrbzAsteroidProfile> SourceProfile;

	/** kg/m^3 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SolarOrbz|Baked")
	float Density = 2000.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SolarOrbz|Baked")
	ESolarOrbzAsteroidComposition Composition = ESolarOrbzAsteroidComposition::Rocky;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SolarOrbz|Baked", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Irregularity = 0.5f;

	/** Returns null if Mesh has no asteroid data attached (not baked, baked with no Profile, or baked as a Planet/Star instead). */
	UFUNCTION(BlueprintCallable, Category = "SolarOrbz|Baked")
	static USolarOrbzAsteroidMeshUserData* GetAsteroidData(const UStaticMesh* Mesh);
};
