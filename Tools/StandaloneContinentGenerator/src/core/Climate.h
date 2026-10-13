// A deliberately simple climate pass over an already-baked FHeightField: latitude + elevation
// drive temperature; a single-direction prevailing-wind sweep with orographic lift/rain-shadow
// drives moisture. This is the same "approximate, not a physics simulation" spirit this codebase
// already states for Plate Tectonics, and the same academic lineage this project's own Voronoi
// Growth docstring already cites (Azgaar's Fantasy Map Generator uses an equivalent simplified
// wind-sweep rainfall model) - not a GCM, not derived from any commercial tool.
#pragma once

#include "HeightField.h"
#include <cstdint>
#include <vector>

struct FClimateConfig
{
    float EquatorTempC = 28.0f;
    float PoleTempC = -35.0f;
    float LapseRateCPerKm = 6.5f;         // standard atmospheric lapse rate - cools with altitude
    float OceanMoistureBaseline = 1.0f;   // moisture a parcel of air carries fresh off the ocean
    float OceanEvaporationRate = 0.35f;   // how fast moisture replenishes per cell crossed over ocean
    float RainShadowSensitivity = 1500.0f; // meters of rise needed to wring out RainfallMaxFraction of carried moisture
    float RainfallMaxFraction = 0.6f;     // most moisture a single cell's orographic lift can remove at once
};

struct FClimateField
{
    int32_t Width = 0;
    int32_t Height = 0;
    std::vector<float> TemperatureC;
    std::vector<float> MoistureNormalized; // 0..1 after normalization

    FClimateField() = default;
    FClimateField(int32_t InWidth, int32_t InHeight)
        : Width(InWidth), Height(InHeight)
        , TemperatureC((size_t)InWidth * InHeight, 0.0f)
        , MoistureNormalized((size_t)InWidth * InHeight, 0.0f)
    {
    }
};

// Simulates temperature and (via a two-pass prevailing-wind sweep so the sphere's wraparound
// stabilizes) moisture for every cell of an already-baked height field.
inline FClimateField SimulateClimate(const FHeightField& Terrain, const FClimateConfig& Config)
{
    FClimateField Climate(Terrain.Width, Terrain.Height);

    for (int32_t Y = 0; Y < Terrain.Height; ++Y)
    {
        const double LatitudeDeg = RowLatitudeDegrees(Y, Terrain.Height);
        const double LatitudeFrac = std::abs(LatitudeDeg) / 90.0;

        // Pass 1 (discarded): lets the wind-sweep's carried-moisture value stabilize around the
        // sphere's wraparound seam before Pass 2 actually records it - otherwise cell X=0's result
        // would depend on an arbitrary "moisture at the start of the row" assumption instead of on
        // what's actually upwind of it all the way around.
        float MoistureCarried = Config.OceanMoistureBaseline;
        for (int32_t Pass = 0; Pass < 2; ++Pass)
        {
            for (int32_t X = 0; X < Terrain.Width; ++X)
            {
                const float CellHeight = Terrain.At(X, Y);
                const float TemperatureC = (float)(Config.EquatorTempC + (Config.PoleTempC - Config.EquatorTempC) * LatitudeFrac)
                    - std::max(CellHeight, 0.0f) / 1000.0f * Config.LapseRateCPerKm;

                if (CellHeight <= 0.0f)
                {
                    MoistureCarried = std::min(MoistureCarried + Config.OceanEvaporationRate, Config.OceanMoistureBaseline);
                }
                else
                {
                    const int32_t PrevX = Terrain.WrapX(X - 1);
                    const float Rise = std::max(CellHeight - Terrain.At(PrevX, Y), 0.0f);
                    const float RainFraction = MathUtil::Clamp(Rise / Config.RainShadowSensitivity, 0.0f, Config.RainfallMaxFraction);
                    MoistureCarried -= MoistureCarried * RainFraction;
                }

                if (Pass == 1)
                {
                    const size_t Idx = (size_t)Y * Terrain.Width + X;
                    Climate.TemperatureC[Idx] = TemperatureC;
                    Climate.MoistureNormalized[Idx] = MathUtil::Clamp(MoistureCarried / Config.OceanMoistureBaseline, 0.0f, 1.0f);
                }
            }
        }
    }

    return Climate;
}
