// See PlanetExport.h - this is the file-writing logic ported verbatim out of the CLI's own
// src/cli/main.cpp (comments on each function kept from there) so both the CLI and the GUI's
// "Export" button go through one implementation instead of two copies that could drift apart.
#include "PlanetExport.h"
#include "LatLongGrid.h"
#include "SphereMesh.h"

// STB_IMAGE_WRITE_STATIC gives every stbi_write_* symbol internal linkage - without it, this
// translation unit's own STB_IMAGE_WRITE_IMPLEMENTATION collides at link time with raylib's own
// (rtextures.c also embeds stb_image_write.h with SUPPORT_IMAGE_EXPORT on by default), the moment
// both land in the same binary - which solarorbz_gui now does, since this file moved here from the
// CLI (which never linked raylib) so the GUI's own Export button could call it too.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>
#include <lodepng.h>

#include <fstream>
#include <iostream>
#include <vector>

namespace
{
    void WriteGrayscalePng(const std::string& Path, const FHeightField& Field, float MinVal, float MaxVal)
    {
        const float Range = std::max(MaxVal - MinVal, 1e-6f);
        std::vector<uint8_t> Pixels(Field.HeightMeters.size());
        for (size_t i = 0; i < Field.HeightMeters.size(); ++i)
        {
            const float Normalized = MathUtil::Clamp((Field.HeightMeters[i] - MinVal) / Range, 0.0f, 1.0f);
            Pixels[i] = (uint8_t)std::lround(Normalized * 255.0f);
        }
        stbi_write_png(Path.c_str(), Field.Width, Field.Height, 1, Pixels.data(), Field.Width);
    }

    // A single linear MinVal..MaxVal grayscale ramp looks nearly flat for a real planet's height
    // field: ocean trenches (thousands of meters deep) and eroded land relief (tens to low
    // thousands of meters) sit on wildly different scales, so land ends up crushed into a sliver
    // near one end of the 0..255 range. Scaling below/above sea level separately (0 maps to a
    // fixed mid-gray) gives both sides real visual contrast regardless of how that magnitude
    // compares - the same split-range idea the GUI's vertex-color ramp already uses.
    void WriteSeaLevelSplitPng(const std::string& Path, const FHeightField& Field, float MinVal, float MaxVal)
    {
        const float OceanRange = std::max(-MinVal, 1e-6f);
        const float LandRange = std::max(MaxVal, 1e-6f);
        std::vector<uint8_t> Pixels(Field.HeightMeters.size());
        for (size_t i = 0; i < Field.HeightMeters.size(); ++i)
        {
            const float H = Field.HeightMeters[i];
            float Normalized;
            if (H <= 0.0f)
            {
                Normalized = 0.5f - 0.5f * MathUtil::Clamp(-H / OceanRange, 0.0f, 1.0f);
            }
            else
            {
                Normalized = 0.5f + 0.5f * MathUtil::Clamp(H / LandRange, 0.0f, 1.0f);
            }
            Pixels[i] = (uint8_t)std::lround(Normalized * 255.0f);
        }
        stbi_write_png(Path.c_str(), Field.Width, Field.Height, 1, Pixels.data(), Field.Width);
    }

    // Writes a genuine LINEAR 16-bit grayscale PNG - black (0) = MinVal, white (65535) = MaxVal -
    // for round-tripping into the real UE plugin's USolarOrbzHeightmapTerrainLayer, NOT a copy of
    // WriteSeaLevelSplitPng's 8-bit, sea-level-split, contrast-enhanced visualization above (that
    // encoding is deliberately nonlinear and lossy - reimporting it would NOT reproduce the actual
    // baked heights). stb_image_write's PNG writer is 8-bit-per-channel only (confirmed by reading
    // its own header - no 16-bit code path exists), nowhere near enough vertical precision for a
    // real heightmap (8 bits over a several-thousand-meter range is tens of meters per step); lodepng
    // is used here instead, purely for this one function, since it supports genuine 16-bit grayscale
    // PNG output. Orientation matches FSolarOrbzLatLongGrid/FLatLongGrid's own shared convention
    // exactly (Y=0 row is the north pole, X increasing is increasing longitude) - the same "north
    // pole at top" equirectangular layout USolarOrbzHeightmapTerrainLayer's own header comment
    // documents, confirmed by comparing both grids' identical V=0-at-north-pole convention, so no
    // flip/rotation is needed on either side of the round trip.
    bool WriteUE5HeightmapPng16(const std::string& Path, const FHeightField& Field, float MinVal, float MaxVal)
    {
        const float Range = std::max(MaxVal - MinVal, 1e-6f);
        std::vector<uint8_t> Bytes((size_t)Field.HeightMeters.size() * 2);
        for (size_t i = 0; i < Field.HeightMeters.size(); ++i)
        {
            const float Normalized = MathUtil::Clamp((Field.HeightMeters[i] - MinVal) / Range, 0.0f, 1.0f);
            const uint16_t Value16 = (uint16_t)std::lround(Normalized * 65535.0f);
            // PNG (and lodepng's raw-buffer convention for bitdepth > 8) stores multi-byte samples
            // big-endian (most significant byte first) regardless of host endianness.
            Bytes[i * 2 + 0] = (uint8_t)(Value16 >> 8);
            Bytes[i * 2 + 1] = (uint8_t)(Value16 & 0xFF);
        }
        const unsigned Error = lodepng::encode(Path, Bytes, (unsigned)Field.Width, (unsigned)Field.Height, LCT_GREY, 16);
        if (Error != 0)
        {
            std::cerr << "Failed to write " << Path << ": " << lodepng_error_text(Error) << "\n";
            return false;
        }
        return true;
    }

    // Raw discrete biome-ID map - pixel value IS the EBiome enum ordinal (0..EBiome::Count-1), NOT a
    // normalized/interpolated quantity like the heightmap above. Biomes are categories, not a
    // continuum - blending two adjacent pixel values during any resampling (including whatever
    // texture filtering a consuming engine applies) would produce a meaningless intermediate "biome"
    // that doesn't exist in the classifier's own enum. Whoever samples this PNG in UE (a PCGEx texture
    // sampling node, most likely - see README.md's "PCG scatter-point dataset" section) needs to use
    // POINT/nearest-neighbor filtering, never bilinear, for exactly that reason. 8-bit via
    // stb_image_write is plenty - EBiome::Count is 12, nowhere near the 256 values a single byte can
    // hold. Same orientation convention as WriteUE5HeightmapPng16 (no flip/rotation needed).
    bool WriteUE5BiomeIdPng(const std::string& Path, const std::vector<uint8_t>& BiomeIds, int32_t Width, int32_t Height)
    {
        return stbi_write_png(Path.c_str(), Width, Height, 1, BiomeIds.data(), Width) != 0;
    }

    // Formats an FVector-shaped value the way Unreal's own struct text import/export does -
    // "(X=...,Y=...,Z=...)" - so a single CSV column maps directly onto one FVector UPROPERTY on a
    // DataTable row struct (FStructProperty::ImportText's parenthesized Key=Value convention, long-
    // standing UE behavior, not something that varies by engine version). Lets WritePcgPointsCsv emit
    // one "Position" and one "Normal" column instead of six separate X/Y/Z columns.
    // Double-quoted (RFC 4180): the value itself contains commas (between X=/Y=/Z=), and an unquoted
    // field is split on every comma by any standards-conforming CSV reader - confirmed the hard way by
    // reading this exact output back with Python's own csv module, which misparsed it identically to
    // how UE's DataTable CSV importer would. No embedded double-quote or newline is possible in this
    // specific format (it's always exactly this shape, never arbitrary text), so no quote-escaping
    // beyond the wrapping pair is needed.
    std::string FormatUEVector(float X, float Y, float Z)
    {
        char Buf[96];
        std::snprintf(Buf, sizeof(Buf), "\"(X=%.3f,Y=%.3f,Z=%.3f)\"", (double)X, (double)Y, (double)Z);
        return Buf;
    }

    // Writes one row per cell of the baked (not resampled - this is a coarse attribute-sample grid for
    // PCG/PCGEx to project onto its OWN, much denser point cloud, not a literal "one spawned object
    // per row" list) Terrain/Climate/Biome fields, matching the UE plugin's own pre-designed
    // FSolarOrbzBiomeSampleContext fields (Elevation/Slope/Temperature/Moisture) and
    // FSolarOrbzBiomeScatterEntry's "matched by Tag" comment (here, by BiomeName) - see
    // SolarOrbzBiomeSystem.h's own PCG/PCGEx hookups, which this is built to feed directly via the
    // FSolarOrbzPCGScatterPointRow DataTable row struct added alongside it. Position/Normal are read
    // from a real displaced-sphere mesh (SphereMesh.h - the same pole-smoothed, centered-difference-
    // normal code this project's GUI preview already uses), not recomputed ad hoc, so Slope benefits
    // from both fixes rather than reintroducing either bug. Slope's own convention - 0 flat, 1
    // vertical, derived from how far the normal has tilted from the radial direction - mirrors
    // FSolarOrbzBiomeSampleContext::Slope's own documented definition exactly; RadiusMeters must match
    // the real planet's actual base radius for this to mean the same thing in UE, since the physical
    // arc-length a grid cell spans (and so how steep a given height difference across it actually is)
    // scales with radius.
    bool WritePcgPointsCsv(const std::string& Path, const FHeightField& Terrain, const FClimateField& Climate,
        const FBiomeField& Biomes, double RadiusMeters)
    {
        std::ofstream Out(Path);
        if (!Out) return false;

        auto SampleHeight = [&](const Vec3& Dir) -> float
        {
            return Terrain.SampleDirection(FLatLongGrid(Terrain.Width, Terrain.Height), Dir);
        };
        const FSphereMesh Mesh = BuildSphereMesh(SampleHeight, Terrain.Width, Terrain.Height, RadiusMeters, 1.0);

        Out << "Name,Position,Normal,ElevationCm,Slope,TemperatureKelvin,Moisture,BiomeName\n";
        for (int32_t Y = 0; Y < Terrain.Height; ++Y)
        {
            for (int32_t X = 0; X < Terrain.Width; ++X)
            {
                const size_t Idx = (size_t)Y * Terrain.Width + X;
                const FSphereMeshVertex& V = Mesh.Vertices[Idx];
                const Vec3 RadialDir = V.Position.GetSafeNormal();
                const double CosAngle = MathUtil::Clamp(Vec3::Dot(V.Normal, RadialDir), -1.0, 1.0);
                const double Slope = MathUtil::Clamp(std::acos(CosAngle) / (MathUtil::PI_D * 0.5), 0.0, 1.0);
                const float TemperatureKelvin = Climate.TemperatureC[Idx] + 273.15f;
                const EBiome Biome = (EBiome)Biomes.BiomeId[Idx];

                Out << "Point_" << Idx << ","
                    << FormatUEVector((float)(V.Position.X * 100.0), (float)(V.Position.Y * 100.0), (float)(V.Position.Z * 100.0)) << ","
                    << FormatUEVector((float)V.Normal.X, (float)V.Normal.Y, (float)V.Normal.Z) << ","
                    << (V.HeightMeters * 100.0f) << ","
                    << Slope << ","
                    << TemperatureKelvin << ","
                    << Climate.MoistureNormalized[Idx] << ","
                    << GetBiomeName(Biome) << "\n";
            }
        }
        return true;
    }
}

FPlanetExportResult ExportPlanetToUE5(const FHeightField& Terrain, const FClimateField& Climate,
    const FBiomeField& Biomes, const FClimateConfig& ClimateConfig, const FPlanetExportOptions& Options)
{
    FPlanetExportResult Result;

    // Resample everything onto the requested output image resolution (the internal bake grid and
    // the output image resolution are independent - GridWidth/Height is how finely erosion/climate
    // actually simulate, ImageWidth/Height is just the PNG's pixel size).
    const FLatLongGrid ImageGrid(Options.ImageWidth, Options.ImageHeight);
    const FLatLongGrid BakeGrid(Terrain.Width, Terrain.Height);

    FHeightField HeightImage(Options.ImageWidth, Options.ImageHeight);
    std::vector<uint8_t> BiomePixels((size_t)Options.ImageWidth * Options.ImageHeight * 3);
    std::vector<uint8_t> BiomeIdPixels((size_t)Options.ImageWidth * Options.ImageHeight);
    FHeightField TempImage(Options.ImageWidth, Options.ImageHeight);
    FHeightField MoistureImage(Options.ImageWidth, Options.ImageHeight);

    float MinHeight = 1e9f, MaxHeight = -1e9f;
    ImageGrid.ForEachCell([&](int32_t Idx, const Vec3& Dir, double, double)
    {
        const FLatLongGrid::FBilinearCell Cell = BakeGrid.ComputeBilinearCell(Dir);
        const float H = FLatLongGrid::SampleAtCell(Terrain.HeightMeters, Terrain.Width, Cell);
        HeightImage.HeightMeters[Idx] = H;
        MinHeight = std::min(MinHeight, H);
        MaxHeight = std::max(MaxHeight, H);

        // Nearest-neighbor for biome ID (it's a discrete category, not something to blend) and
        // bilinear for the continuous climate fields.
        const int32_t NearestX = Cell.Tx < 0.5 ? Cell.X0 : Cell.X1;
        const int32_t NearestY = Cell.Ty < 0.5 ? Cell.Y0 : Cell.Y1;
        const uint8_t BiomeIdByte = Biomes.BiomeId[(size_t)NearestY * Terrain.Width + NearestX];
        const EBiome Biome = (EBiome)BiomeIdByte;
        const FBiomeColor Color = GetBiomeColor(Biome);
        BiomePixels[(size_t)Idx * 3 + 0] = Color.R;
        BiomePixels[(size_t)Idx * 3 + 1] = Color.G;
        BiomePixels[(size_t)Idx * 3 + 2] = Color.B;
        BiomeIdPixels[(size_t)Idx] = BiomeIdByte;

        TempImage.HeightMeters[Idx] = FLatLongGrid::SampleAtCell(Climate.TemperatureC, Terrain.Width, Cell);
        MoistureImage.HeightMeters[Idx] = FLatLongGrid::SampleAtCell(Climate.MoistureNormalized, Terrain.Width, Cell);
    });

    Result.MinHeightMeters = MinHeight;
    Result.MaxHeightMeters = MaxHeight;
    Result.HeightPath = Options.OutPrefix + "_height.png";
    Result.BiomePath = Options.OutPrefix + "_biome.png";
    Result.TempPath = Options.OutPrefix + "_temperature.png";
    Result.MoisturePath = Options.OutPrefix + "_moisture.png";
    Result.UE5HeightmapPath = Options.OutPrefix + "_heightmap_ue5.png";
    Result.UE5BiomeIdPath = Options.OutPrefix + "_biomeid_ue5.png";
    Result.PcgPointsPath = Options.OutPrefix + "_pcg_points.csv";

    WriteSeaLevelSplitPng(Result.HeightPath, HeightImage, MinHeight, MaxHeight);
    stbi_write_png(Result.BiomePath.c_str(), Options.ImageWidth, Options.ImageHeight, 3, BiomePixels.data(), Options.ImageWidth * 3);
    WriteGrayscalePng(Result.TempPath, TempImage, ClimateConfig.PoleTempC, ClimateConfig.EquatorTempC);
    WriteGrayscalePng(Result.MoisturePath, MoistureImage, 0.0f, 1.0f);

    if (Options.bWriteUE5Heightmap)
    {
        Result.bUE5HeightmapWritten = WriteUE5HeightmapPng16(Result.UE5HeightmapPath, HeightImage, MinHeight, MaxHeight);
    }
    if (Options.bWriteUE5BiomeId)
    {
        Result.bUE5BiomeIdWritten = WriteUE5BiomeIdPng(Result.UE5BiomeIdPath, BiomeIdPixels, Options.ImageWidth, Options.ImageHeight);
    }
    if (Options.bWritePcgPoints)
    {
        Result.bPcgPointsWritten = WritePcgPointsCsv(Result.PcgPointsPath, Terrain, Climate, Biomes, Options.RadiusMeters);
    }

    Result.bOk = (!Options.bWriteUE5Heightmap || Result.bUE5HeightmapWritten)
        && (!Options.bWriteUE5BiomeId || Result.bUE5BiomeIdWritten)
        && (!Options.bWritePcgPoints || Result.bPcgPointsWritten);
    return Result;
}
