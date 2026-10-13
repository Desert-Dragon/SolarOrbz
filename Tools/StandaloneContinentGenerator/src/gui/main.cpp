// GUI frontend: raylib + raygui interactive real-time 3D orbit-camera preview of the full terrain
// pipeline (Continent -> Noise -> Base Layer Adjust, plus on-demand Hydraulic/Thermal erosion and
// a Climate/Biome pass), sharing solarorbz_core with the CLI. No UE dependency - see
// src/core/ContinentLayer.h and src/core/TerrainNode.h for the scope-cut/fidelity notes.
#include "TerrainNode.h"
#include "Nodes/ContinentNode.h"
#include "Nodes/NoiseNode.h"
#include "Nodes/HeightAdjustNode.h"
#include "Nodes/HydraulicErosionNode.h"
#include "Nodes/ThermalErosionNode.h"
#include "Climate.h"
#include "BiomeClassifier.h"
#include "SphereMesh.h"

#include <raylib.h>
#include <rlgl.h>
#define RAYGUI_IMPLEMENTATION
#include <raygui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    constexpr int32_t ScreenWidth = 1360;
    constexpr int32_t ScreenHeight = 980;
    constexpr int32_t PanelWidth = 320;

    enum class EViewMode : int32_t { Height = 0, Biome = 1 };

    // Visualization-only - NOT part of any node's own config. Default height values are a very small
    // percentage of Earth-scale radii (this is exactly the "no mountains visible" issue this tool
    // exists to let you iterate on without rebuilding the UE plugin each time) - the preview needs its
    // own exaggeration knob, independent of the config, to actually show relief at a glance.
    struct FVisualizationSettings
    {
        // Deliberately NOT Earth-scale by default - see HeightExaggeration's doc comment. Must stay
        // comfortably larger than the worst case (most negative Ocean Floor Depth/Trench slider value)
        // times the largest Height Exaggeration the slider allows, or the displaced radius collapses
        // toward zero (SphereMesh.h's MinRadius clamp then catches it, but the result is a degenerate
        // spiky shape, not a dented sphere).
        float PreviewRadiusMeters = 400000.0f;
        float HeightExaggeration = 5.0f;
        int32_t MeshGridWidth = 128;
        int32_t MeshGridHeight = 64;
        EViewMode ViewMode = EViewMode::Height;
    };

    // Grid topology (vertex count, triangle winding) depends only on MeshGridWidth/Height, which never
    // change at runtime - only vertex positions/normals/colors do, every time the terrain changes. The
    // model is therefore allocated and uploaded ONCE (dynamic=true), and later refreshed in place via
    // UpdateMeshBuffer rather than UnloadModel+rebuild every frame a slider is dragged.
    Model CreatePreviewModel(int32_t Width, int32_t Height)
    {
        Mesh RMesh = { 0 };
        RMesh.vertexCount = Width * Height;
        RMesh.triangleCount = Width * (Height - 1) * 2;
        RMesh.vertices = (float*)MemAlloc(sizeof(float) * 3 * RMesh.vertexCount);
        RMesh.normals = (float*)MemAlloc(sizeof(float) * 3 * RMesh.vertexCount);
        RMesh.colors = (unsigned char*)MemAlloc(sizeof(unsigned char) * 4 * RMesh.vertexCount);
        RMesh.indices = (unsigned short*)MemAlloc(sizeof(unsigned short) * RMesh.triangleCount * 3);

        std::fill(RMesh.vertices, RMesh.vertices + 3 * RMesh.vertexCount, 0.0f);
        std::fill(RMesh.normals, RMesh.normals + 3 * RMesh.vertexCount, 0.0f);
        std::fill(RMesh.colors, RMesh.colors + 4 * RMesh.vertexCount, (unsigned char)255);

        // Static topology, filled once - identical winding to BuildSphereMesh's own index generation
        // (longitude wraps, latitude doesn't).
        size_t IndexCursor = 0;
        for (int32_t Y = 0; Y < Height - 1; ++Y)
        {
            for (int32_t X = 0; X < Width; ++X)
            {
                const int32_t XNext = (X + 1) % Width;
                const unsigned short I00 = (unsigned short)(Y * Width + X);
                const unsigned short I10 = (unsigned short)(Y * Width + XNext);
                const unsigned short I01 = (unsigned short)((Y + 1) * Width + X);
                const unsigned short I11 = (unsigned short)((Y + 1) * Width + XNext);
                // Reversed from the "obvious" (I00,I10,I11)/(I00,I11,I01) order - verified empirically
                // against raylib's front-face convention: with backface culling enabled, this order is
                // what survives as the visible outer surface.
                RMesh.indices[IndexCursor++] = I00; RMesh.indices[IndexCursor++] = I11; RMesh.indices[IndexCursor++] = I10;
                RMesh.indices[IndexCursor++] = I00; RMesh.indices[IndexCursor++] = I01; RMesh.indices[IndexCursor++] = I11;
            }
        }

        UploadMesh(&RMesh, true); // dynamic=true - these buffers are rewritten in place every update
        return LoadModelFromMesh(RMesh);
    }

    Color HeightRampColor(float HeightMeters, float MinH, float MaxH)
    {
        if (HeightMeters <= 0.0f)
        {
            const float T = MathUtil::Clamp((HeightMeters - MinH) / std::max(-MinH, 1.0f), 0.0f, 1.0f);
            return { (unsigned char)(10 + 20 * T), (unsigned char)(40 + 60 * T), (unsigned char)(90 + 120 * T), 255 };
        }
        const float T = MathUtil::Clamp(HeightMeters / std::max(MaxH, 1.0f), 0.0f, 1.0f);
        if (T < 0.5f)
        {
            const float Tt = T / 0.5f;
            return { (unsigned char)(60 + Tt * 80), (unsigned char)(110 + Tt * 40), (unsigned char)(50), 255 };
        }
        const float Tt = (T - 0.5f) / 0.5f;
        return { (unsigned char)(140 + Tt * 115), (unsigned char)(150 + Tt * 105), (unsigned char)(90 + Tt * 165), 255 };
    }

    // Overwrites the model's existing vertex/normal/color buffers in place (same vertex count every
    // time - MeshGridWidth/Height are fixed) via UpdateMeshBuffer, instead of reallocating GPU objects
    // (churning GPU mesh buffers every frame a slider is dragged was the cause of an earlier reported
    // rendering glitch - see git history). Colors come from either the elevation ramp or the biome
    // classification, per Viz.ViewMode.
    void UpdatePreviewModel(Model& PreviewModel, const FHeightField& Terrain, const FBiomeField& Biomes, const FVisualizationSettings& Viz)
    {
        const FLatLongGrid TerrainGrid(Terrain.Width, Terrain.Height);
        auto SampleHeight = [&](const Vec3& Dir) { return Terrain.SampleDirection(TerrainGrid, Dir); };
        const FSphereMesh Src = BuildSphereMesh(SampleHeight, Viz.MeshGridWidth, Viz.MeshGridHeight, Viz.PreviewRadiusMeters, Viz.HeightExaggeration);
        Mesh& RMesh = PreviewModel.meshes[0];

        float MinH = 1e9f, MaxH = -1e9f;
        for (const FSphereMeshVertex& V : Src.Vertices) { MinH = std::min(MinH, V.HeightMeters); MaxH = std::max(MaxH, V.HeightMeters); }

        for (size_t i = 0; i < Src.Vertices.size(); ++i)
        {
            const FSphereMeshVertex& V = Src.Vertices[i];
            RMesh.vertices[i * 3 + 0] = (float)V.Position.X;
            RMesh.vertices[i * 3 + 1] = (float)V.Position.Z; // raylib is Y-up; our core math is Z-up (matches the UE plugin's own convention)
            RMesh.vertices[i * 3 + 2] = (float)V.Position.Y;
            RMesh.normals[i * 3 + 0] = (float)V.Normal.X;
            RMesh.normals[i * 3 + 1] = (float)V.Normal.Z;
            RMesh.normals[i * 3 + 2] = (float)V.Normal.Y;

            Color C;
            if (Viz.ViewMode == EViewMode::Biome)
            {
                const Vec3 Dir = V.Position.GetSafeNormal();
                const FLatLongGrid::FBilinearCell Cell = TerrainGrid.ComputeBilinearCell(Dir);
                const int32_t NearestX = Cell.Tx < 0.5 ? Cell.X0 : Cell.X1;
                const int32_t NearestY = Cell.Ty < 0.5 ? Cell.Y0 : Cell.Y1;
                const EBiome Biome = (EBiome)Biomes.BiomeId[(size_t)NearestY * Terrain.Width + NearestX];
                const FBiomeColor BC = GetBiomeColor(Biome);
                C = { BC.R, BC.G, BC.B, 255 };
            }
            else
            {
                C = HeightRampColor(V.HeightMeters, MinH, MaxH);
            }
            RMesh.colors[i * 4 + 0] = C.r; RMesh.colors[i * 4 + 1] = C.g; RMesh.colors[i * 4 + 2] = C.b; RMesh.colors[i * 4 + 3] = C.a;
        }

        UpdateMeshBuffer(RMesh, 0, RMesh.vertices, (int)(sizeof(float) * 3 * RMesh.vertexCount), 0);
        UpdateMeshBuffer(RMesh, 2, RMesh.normals, (int)(sizeof(float) * 3 * RMesh.vertexCount), 0);
        UpdateMeshBuffer(RMesh, 3, RMesh.colors, (int)(sizeof(unsigned char) * 4 * RMesh.vertexCount), 0);
    }
}

int main()
{
    InitWindow(ScreenWidth, ScreenHeight, "SolarOrbz Standalone - Terrain Pipeline Preview");
    SetTargetFPS(60);

    // raylib's default far clip plane (1000 units) assumes small-scene scale - our world coordinates
    // are in meters at planet-preview scale (tens of thousands+), so without widening this the whole
    // model (and the camera's own orbit distance) sits beyond the far plane and nothing renders.
    rlSetClipPlanes(0.1, 10000000.0);
    rlEnableBackfaceCulling();

    TerrainPipeline Pipeline;
    Pipeline.GridWidth = 256;
    Pipeline.GridHeight = 128;

    auto ContinentNodePtr = std::make_unique<ContinentNode>();
    ContinentNode* ContinentNodeRaw = ContinentNodePtr.get();
    Pipeline.Nodes.push_back(std::move(ContinentNodePtr));

    auto NoiseNodePtr = std::make_unique<NoiseNode>();
    NoiseNodePtr->Config.AmplitudeMeters = 0.0f; // off by default - purely additive detail on top of Continent
    NoiseNode* NoiseNodeRaw = NoiseNodePtr.get();
    Pipeline.Nodes.push_back(std::move(NoiseNodePtr));

    auto HeightAdjustNodePtr = std::make_unique<HeightAdjustNode>();
    HeightAdjustNode* HeightAdjustNodeRaw = HeightAdjustNodePtr.get();
    Pipeline.Nodes.push_back(std::move(HeightAdjustNodePtr));

    // Erosion is deliberately NOT part of Pipeline.Nodes - it's expensive enough (tens of thousands
    // of droplets) that re-running it every frame a slider is dragged would stall the UI. It's an
    // explicit "Run Erosion" step applied directly onto the current baked terrain, the same pattern
    // already established by Plate Tectonics' own "Run Simulation" button.
    HydraulicErosionNode HydraulicErosion;
    ThermalErosionNode ThermalErosion;

    FClimateConfig ClimateConfig;
    FBiomeConfig BiomeConfig;

    FHeightField BakedTerrain = Pipeline.Bake();
    FClimateField Climate = SimulateClimate(BakedTerrain, ClimateConfig);
    FBiomeField Biomes = ClassifyBiomes(BakedTerrain, Climate, BiomeConfig);

    FVisualizationSettings Viz;
    Model PreviewModel = CreatePreviewModel(Viz.MeshGridWidth, Viz.MeshGridHeight);
    UpdatePreviewModel(PreviewModel, BakedTerrain, Biomes, Viz);

    auto RebakeBase = [&]()
    {
        BakedTerrain = Pipeline.Bake();
        Climate = SimulateClimate(BakedTerrain, ClimateConfig);
        Biomes = ClassifyBiomes(BakedTerrain, Climate, BiomeConfig);
        UpdatePreviewModel(PreviewModel, BakedTerrain, Biomes, Viz);
    };
    auto RecomputeClimateAndRefresh = [&]()
    {
        Climate = SimulateClimate(BakedTerrain, ClimateConfig);
        Biomes = ClassifyBiomes(BakedTerrain, Climate, BiomeConfig);
        UpdatePreviewModel(PreviewModel, BakedTerrain, Biomes, Viz);
    };

    // Manual orbit camera - left-drag to orbit, wheel to zoom. Deliberately not raylib's built-in
    // CAMERA_ORBITAL mode, which auto-spins on its own rather than responding to the mouse.
    float Yaw = 0.5f, Pitch = 0.4f, Distance = Viz.PreviewRadiusMeters * 3.5f;
    Camera3D Camera = { 0 };
    Camera.target = { 0.0f, 0.0f, 0.0f };
    Camera.up = { 0.0f, 1.0f, 0.0f };
    Camera.fovy = 45.0f;
    Camera.projection = CAMERA_PERSPECTIVE;

    bool bNeedsRebuild = false;
    bool bNeedsClimateRefresh = false;
    bool bAlgorithmDropdownOpen = false;
    int32_t AlgorithmIndex = 0; // RadialSeeds=0, VoronoiGrowth=1, PlateTectonics=2

    while (!WindowShouldClose())
    {
        const bool bMouseOverPanel = GetMouseX() < PanelWidth;
        if (!bMouseOverPanel)
        {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
            {
                const Vector2 Delta = GetMouseDelta();
                Yaw -= Delta.x * 0.005f;
                Pitch = MathUtil::Clamp(Pitch - Delta.y * 0.005f, -1.5f, 1.5f);
            }
            Distance = std::max(Distance - GetMouseWheelMove() * Viz.PreviewRadiusMeters * 0.1f, Viz.PreviewRadiusMeters * 1.05f);
        }
        Camera.position = {
            Distance * std::cos(Pitch) * std::sin(Yaw),
            Distance * std::sin(Pitch),
            Distance * std::cos(Pitch) * std::cos(Yaw)
        };

        BeginDrawing();
        ClearBackground(Color{ 18, 18, 24, 255 });

        BeginMode3D(Camera);
        DrawModel(PreviewModel, { 0, 0, 0 }, 1.0f, WHITE);
        EndMode3D();

        DrawRectangle(0, 0, PanelWidth, ScreenHeight, Color{ 30, 30, 36, 235 });
        int Y = 8;
        GuiLabel(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 20 }, "SolarOrbz Standalone"); Y += 22;

        GuiLabel(Rectangle{ 10, (float)Y, 120, 18 }, "Algorithm"); Y += 18;
        // GuiDropdownBox draws its expanded list right where it's called - every control drawn AFTER
        // it in the same frame paints over that list. So its actual draw+interaction call is deferred
        // to the very end of the frame (see below), on top of everything else; only its layout space
        // is reserved here. Everything else is GuiLock()ed while it's open so a click can't "fall
        // through" the open list onto a slider underneath on the same frame.
        const Rectangle AlgorithmBoxRect{ 10, (float)Y, PanelWidth - 20.0f, 22 };
        Y += 28;

        if (bAlgorithmDropdownOpen)
        {
            GuiLock();
        }

        char Buf[128];
        auto SliderInt = [&](const char* Label, int32_t& Value, float Min, float Max, bool* bDirtyFlag)
        {
            float F = (float)Value;
            std::snprintf(Buf, sizeof(Buf), "%s: %d", Label, Value);
            if (GuiSlider(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 18 }, nullptr, Buf, &F, Min, Max))
            {
                Value = (int32_t)std::lround(F);
                if (bDirtyFlag) *bDirtyFlag = true;
            }
            Y += 23;
        };
        auto SliderFloat = [&](const char* Label, float& Value, float Min, float Max, bool* bDirtyFlag)
        {
            std::snprintf(Buf, sizeof(Buf), "%s: %.2f", Label, Value);
            if (GuiSlider(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 18 }, nullptr, Buf, &Value, Min, Max))
            {
                if (bDirtyFlag) *bDirtyFlag = true;
            }
            Y += 23;
        };
        auto SectionLabel = [&](const char* Label)
        {
            Y += 4;
            GuiLine(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 1 }, nullptr); Y += 4;
            GuiLabel(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 16 }, Label); Y += 18;
        };

        // Binds Pipeline.Context.Seed, not ContinentNodeRaw->Config.Seed - every node's Apply() takes
        // its seed from the shared pipeline context (ContinentNode::Apply() overwrites its own Config
        // copy's Seed with Ctx.Seed before baking), so this is the one slider that actually reseeds
        // Continent AND Noise together.
        SliderInt("Seed", Pipeline.Context.Seed, 0, 9999, &bNeedsRebuild);
        SliderInt("Num Continents", ContinentNodeRaw->Config.NumContinents, 0, 20, &bNeedsRebuild);
        SliderInt("Num Islands", ContinentNodeRaw->Config.NumIslands, 0, 40, &bNeedsRebuild);
        SliderFloat("Min Continent Deg", ContinentNodeRaw->Config.MinContinentRadiusDegrees, 1.0f, 60.0f, &bNeedsRebuild);
        SliderFloat("Max Continent Deg", ContinentNodeRaw->Config.MaxContinentRadiusDegrees, 1.0f, 60.0f, &bNeedsRebuild);
        SliderFloat("Coastline Noise Amp", ContinentNodeRaw->Config.CoastlineNoiseAmplitude, 0.0f, 1.0f, &bNeedsRebuild);
        SliderFloat("Ocean Floor Depth (m)", ContinentNodeRaw->Config.OceanFloorDepthMeters, -8000.0f, 0.0f, &bNeedsRebuild);
        SliderFloat("Land Plateau Height (m)", ContinentNodeRaw->Config.LandPlateauHeightMeters, 0.0f, 2000.0f, &bNeedsRebuild);

        if (ContinentNodeRaw->Config.Algorithm == EContinentAlgorithm::PlateTectonics)
        {
            SliderInt("Num Plates", ContinentNodeRaw->Config.NumPlates, 2, 64, &bNeedsRebuild);
            SliderFloat("Mountain Height (m)", ContinentNodeRaw->Config.MountainHeightMeters, 0.0f, 10000.0f, &bNeedsRebuild);
            SliderFloat("Trench Depth (m)", ContinentNodeRaw->Config.TrenchDepthMeters, -10000.0f, 0.0f, &bNeedsRebuild);
            SliderInt("Steps Per Run", ContinentNodeRaw->Config.StepsPerRun, 1, 50, nullptr);
            SliderFloat("Myr Per Step", ContinentNodeRaw->Config.MillionYearsPerStep, 0.1f, 50.0f, nullptr);

            if (GuiButton(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 26 }, "Run Simulation"))
            {
                ContinentNodeRaw->Layer.Config = ContinentNodeRaw->Config;
                ContinentNodeRaw->Layer.Config.Seed = Pipeline.Context.Seed;
                ContinentNodeRaw->Layer.AdvanceTectonicSimulation(ContinentNodeRaw->Config.StepsPerRun, ContinentNodeRaw->Config.MillionYearsPerStep);
                bNeedsRebuild = true;
            }
            Y += 30;
        }

        SectionLabel("Base Layer Adjust");
        SliderFloat("Scale", HeightAdjustNodeRaw->Config.ScaleMultiplier, 0.1f, 3.0f, &bNeedsRebuild);
        SliderFloat("Offset (m)", HeightAdjustNodeRaw->Config.OffsetMeters, -2000.0f, 2000.0f, &bNeedsRebuild);
        SliderFloat("Power Curve", HeightAdjustNodeRaw->Config.PowerCurve, 0.3f, 3.0f, &bNeedsRebuild);

        SectionLabel("Secondary Noise");
        SliderFloat("Amplitude (m)", NoiseNodeRaw->Config.AmplitudeMeters, 0.0f, 1000.0f, &bNeedsRebuild);
        SliderFloat("Frequency", NoiseNodeRaw->Config.Frequency, 0.5f, 20.0f, &bNeedsRebuild);

        SectionLabel("Erosion (explicit - click to apply)");
        SliderInt("Droplets", HydraulicErosion.Config.NumDroplets, 1000, 100000, nullptr);
        SliderFloat("Hydraulic Rate", HydraulicErosion.Config.ErosionRate, 0.05f, 1.0f, nullptr);
        if (GuiButton(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 24 }, "Run Hydraulic Erosion"))
        {
            HydraulicErosion.Apply(BakedTerrain, Pipeline.Context);
            bNeedsClimateRefresh = true;
        }
        Y += 28;
        SliderInt("Thermal Iterations", ThermalErosion.Config.Iterations, 1, 60, nullptr);
        SliderFloat("Talus Angle (deg)", ThermalErosion.Config.TalusAngleDegrees, 10.0f, 60.0f, nullptr);
        if (GuiButton(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 24 }, "Run Thermal Erosion"))
        {
            ThermalErosion.Apply(BakedTerrain, Pipeline.Context);
            bNeedsClimateRefresh = true;
        }
        Y += 28;

        SectionLabel("Climate");
        SliderFloat("Equator Temp (C)", ClimateConfig.EquatorTempC, 0.0f, 45.0f, &bNeedsClimateRefresh);
        SliderFloat("Pole Temp (C)", ClimateConfig.PoleTempC, -60.0f, 10.0f, &bNeedsClimateRefresh);
        SliderFloat("Ocean Evaporation", ClimateConfig.OceanEvaporationRate, 0.05f, 1.0f, &bNeedsClimateRefresh);
        SliderFloat("Rain Shadow Sensitivity", ClimateConfig.RainShadowSensitivity, 200.0f, 4000.0f, &bNeedsClimateRefresh);

        SectionLabel("View");
        {
            const int32_t PrevMode = (int32_t)Viz.ViewMode;
            if (GuiButton(Rectangle{ 10, (float)Y, (PanelWidth - 26.0f) / 2.0f, 24 }, "Height"))
            {
                Viz.ViewMode = EViewMode::Height;
            }
            if (GuiButton(Rectangle{ 10 + (PanelWidth - 26.0f) / 2.0f + 6, (float)Y, (PanelWidth - 26.0f) / 2.0f, 24 }, "Biome"))
            {
                Viz.ViewMode = EViewMode::Biome;
            }
            if ((int32_t)Viz.ViewMode != PrevMode)
            {
                bNeedsClimateRefresh = true;
            }
        }
        Y += 28;
        SliderFloat("Height Exaggeration", Viz.HeightExaggeration, 1.0f, 15.0f, &bNeedsClimateRefresh);

        if (GuiButton(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 26 }, "Regenerate (discards erosion)") || bNeedsRebuild)
        {
            RebakeBase();
            bNeedsRebuild = false;
            bNeedsClimateRefresh = false;
        }
        Y += 30;

        if (bNeedsClimateRefresh)
        {
            RecomputeClimateAndRefresh();
            bNeedsClimateRefresh = false;
        }

        const FContinentStatus Status = ContinentNodeRaw->Layer.GetStatus();
        // Computed from the actual current BakedTerrain, not ContinentNodeRaw->Layer's own
        // EstimateLandCoveragePercent() - that only reflects the raw Continent layer in isolation,
        // which goes stale (and visibly wrong) the moment Noise/Base Layer Adjust/erosion change
        // what's actually baked.
        int32_t LandCells = 0;
        for (float H : BakedTerrain.HeightMeters) { if (H > 0.0f) ++LandCells; }
        const float LandCoveragePercent = 100.0f * LandCells / (float)BakedTerrain.HeightMeters.size();
        std::snprintf(Buf, sizeof(Buf), "Land coverage: %.1f%%", LandCoveragePercent);
        GuiLabel(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 18 }, Buf); Y += 18;
        if (ContinentNodeRaw->Config.Algorithm == EContinentAlgorithm::PlateTectonics)
        {
            std::snprintf(Buf, sizeof(Buf), "Simulated age: %.1f Myr", Status.SimulatedAgeMyr);
            GuiLabel(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 18 }, Buf); Y += 18;
            std::snprintf(Buf, sizeof(Buf), "Plates: %d", Status.CurrentPlateCount);
            GuiLabel(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 18 }, Buf); Y += 18;
        }

        GuiUnlock();

        // Drawn last, on top of every other panel control - see the comment at AlgorithmBoxRect above.
        if (GuiDropdownBox(AlgorithmBoxRect, "Radial Seeds;Voronoi Growth;Plate Tectonics", &AlgorithmIndex, bAlgorithmDropdownOpen))
        {
            bAlgorithmDropdownOpen = !bAlgorithmDropdownOpen;
            ContinentNodeRaw->Config.Algorithm = (EContinentAlgorithm)AlgorithmIndex;
            bNeedsRebuild = true;
        }

        DrawFPS(ScreenWidth - 90, 10);
        EndDrawing();
    }

    UnloadModel(PreviewModel);
    CloseWindow();
    return 0;
}
