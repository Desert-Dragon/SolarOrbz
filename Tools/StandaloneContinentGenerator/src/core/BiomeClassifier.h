// Classifies height+temperature+moisture into a discrete biome per cell - a simplified version of
// Robert H. Whittaker's 1975 biome classification (a public ecology concept keyed on mean annual
// temperature and precipitation, not anyone's proprietary code), with height-based special cases
// (Ocean/Beach/SnowIce/BareMountain) layered on top the way most game-dev biome maps do. This is
// the "build biome materials" piece: GetBiomeColor() is what the GUI paints the preview with and
// what the CLI writes out as a biome-ID PNG, standing in for a real material assignment until this
// feeds back into the UE plugin's own Biome/BiomeStack system.
#pragma once

#include "HeightField.h"
#include "Climate.h"
#include <cstdint>
#include <vector>

enum class EBiome : uint8_t
{
    Ocean,
    Beach,
    SnowIce,
    BareMountain,
    Tundra,
    Taiga,
    Desert,
    Savanna,
    TemperateGrassland,
    TemperateForest,
    TemperateRainforest,
    TropicalRainforest,
    Count
};

struct FBiomeColor { uint8_t R, G, B; };

inline FBiomeColor GetBiomeColor(EBiome Biome)
{
    switch (Biome)
    {
    case EBiome::Ocean:               return {  28,  70, 130 };
    case EBiome::Beach:               return { 230, 215, 170 };
    case EBiome::SnowIce:             return { 245, 245, 250 };
    case EBiome::BareMountain:        return { 120, 115, 110 };
    case EBiome::Tundra:              return { 165, 170, 150 };
    case EBiome::Taiga:               return {  65, 100,  75 };
    case EBiome::Desert:              return { 225, 195, 120 };
    case EBiome::Savanna:             return { 190, 180,  90 };
    case EBiome::TemperateGrassland:  return { 140, 180,  90 };
    case EBiome::TemperateForest:     return {  55, 130,  65 };
    case EBiome::TemperateRainforest: return {  35, 110,  75 };
    case EBiome::TropicalRainforest:  return {  20,  90,  50 };
    default:                         return { 255,   0, 255 }; // unmistakable magenta - a missing case, not a valid biome
    }
}

inline const char* GetBiomeName(EBiome Biome)
{
    switch (Biome)
    {
    case EBiome::Ocean:               return "Ocean";
    case EBiome::Beach:               return "Beach";
    case EBiome::SnowIce:             return "Snow/Ice";
    case EBiome::BareMountain:        return "Bare Mountain";
    case EBiome::Tundra:              return "Tundra";
    case EBiome::Taiga:               return "Taiga";
    case EBiome::Desert:              return "Desert";
    case EBiome::Savanna:             return "Savanna";
    case EBiome::TemperateGrassland:  return "Temperate Grassland";
    case EBiome::TemperateForest:     return "Temperate Forest";
    case EBiome::TemperateRainforest: return "Temperate Rainforest";
    case EBiome::TropicalRainforest:  return "Tropical Rainforest";
    default:                         return "Unknown";
    }
}

struct FBiomeConfig
{
    float BeachThresholdMeters = 30.0f;
    float MountainThresholdMeters = 3500.0f;
    float ColdTempC = 5.0f;
    float HotTempC = 22.0f;
    float AridMoisture = 0.25f;
    float DryMoisture = 0.5f;
    float WetMoisture = 0.75f;
};

inline EBiome ClassifyBiome(float HeightMeters, float TemperatureC, float Moisture, const FBiomeConfig& Config)
{
    if (HeightMeters <= 0.0f)
    {
        return EBiome::Ocean;
    }
    if (HeightMeters <= Config.BeachThresholdMeters)
    {
        return EBiome::Beach;
    }
    if (HeightMeters >= Config.MountainThresholdMeters)
    {
        return (TemperatureC < 0.0f) ? EBiome::SnowIce : EBiome::BareMountain;
    }

    if (TemperatureC < Config.ColdTempC)
    {
        return (Moisture < Config.DryMoisture) ? EBiome::Tundra : EBiome::Taiga;
    }
    if (TemperatureC < Config.HotTempC)
    {
        if (Moisture < Config.AridMoisture) return EBiome::Desert;
        if (Moisture < Config.DryMoisture) return EBiome::TemperateGrassland;
        if (Moisture < Config.WetMoisture) return EBiome::TemperateForest;
        return EBiome::TemperateRainforest;
    }
    // Hot.
    if (Moisture < Config.AridMoisture) return EBiome::Desert;
    if (Moisture < Config.WetMoisture) return EBiome::Savanna;
    return EBiome::TropicalRainforest;
}

struct FBiomeField
{
    int32_t Width = 0;
    int32_t Height = 0;
    std::vector<uint8_t> BiomeId; // index into EBiome

    FBiomeField() = default;
    FBiomeField(int32_t InWidth, int32_t InHeight)
        : Width(InWidth), Height(InHeight), BiomeId((size_t)InWidth * InHeight, 0)
    {
    }
};

inline FBiomeField ClassifyBiomes(const FHeightField& Terrain, const FClimateField& Climate, const FBiomeConfig& Config)
{
    FBiomeField Biomes(Terrain.Width, Terrain.Height);
    for (size_t i = 0; i < Terrain.HeightMeters.size(); ++i)
    {
        Biomes.BiomeId[i] = (uint8_t)ClassifyBiome(Terrain.HeightMeters[i], Climate.TemperatureC[i], Climate.MoistureNormalized[i], Config);
    }
    return Biomes;
}
