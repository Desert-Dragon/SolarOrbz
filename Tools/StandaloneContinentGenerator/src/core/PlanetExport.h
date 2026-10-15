// Shared "write a baked planet out to UE5-ready files" logic - originally lived only in the CLI's
// main.cpp; factored out here so the GUI's own "Export" button (see src/gui/main.cpp) can write
// the exact same files from whatever is currently baked/live-edited in the preview, instead of
// requiring a drop back to the CLI with a hand-written config to reproduce it. No UE dependency -
// see ContinentLayer.h/TerrainNode.h for this project's general scope-cut notes.
#pragma once

#include "HeightField.h"
#include "Climate.h"
#include "BiomeClassifier.h"

#include <cstdint>
#include <string>

struct FPlanetExportOptions
{
    std::string OutPrefix = "out";
    int32_t ImageWidth = 1024;
    int32_t ImageHeight = 512;
    // Only affects the PCG points CSV's Slope column - match whatever the real UE planet's actual
    // base radius is for a correct value (see WritePcgPointsCsv's own comment in the .cpp).
    double RadiusMeters = 6378137.0; // Earth's own mean radius
    bool bWriteUE5Heightmap = true;
    bool bWriteUE5BiomeId = true;
    bool bWritePcgPoints = true;
};

// Everything a caller (CLI console output, GUI on-screen status) needs to report what happened,
// without re-deriving it - MinHeightMeters/MaxHeightMeters in particular are the exact values to
// put in the UE plugin's Heightmap Layer so a reimport reproduces this bake's real heights.
struct FPlanetExportResult
{
    bool bOk = false; // false only if a file failed to open/write - see individual b*Written flags for which one
    float MinHeightMeters = 0.0f;
    float MaxHeightMeters = 0.0f;

    std::string HeightPath;
    std::string BiomePath;
    std::string TempPath;
    std::string MoisturePath;

    std::string UE5HeightmapPath;
    bool bUE5HeightmapWritten = false;
    std::string UE5BiomeIdPath;
    bool bUE5BiomeIdWritten = false;
    std::string PcgPointsPath;
    bool bPcgPointsWritten = false;
};

// Resamples Terrain/Climate/Biomes onto Options.ImageWidth x Options.ImageHeight (independent of
// whatever resolution they were actually baked/simulated at) and writes every file under
// Options.OutPrefix: always the four visualization PNGs (_height/_biome/_temperature/_moisture),
// plus the three UE5-import-ready exports (_heightmap_ue5.png, _biomeid_ue5.png, _pcg_points.csv)
// per the corresponding Options.bWrite* flags. ClimateConfig is needed only to normalize the
// temperature visualization PNG against its own Pole/Equator range, same as the CLI always did.
FPlanetExportResult ExportPlanetToUE5(const FHeightField& Terrain, const FClimateField& Climate,
    const FBiomeField& Biomes, const FClimateConfig& ClimateConfig, const FPlanetExportOptions& Options);
