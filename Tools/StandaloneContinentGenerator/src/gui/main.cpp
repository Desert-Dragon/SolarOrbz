// GUI frontend: raylib + raygui interactive real-time 3D orbit-camera preview with live sliders and
// a Run Simulation button, sharing solarorbz_core with the CLI. No UE dependency - see
// src/core/ContinentLayer.h for the full scope-cut/fidelity notes (RNG, noise, mesh).
#include "ContinentLayer.h"
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
    constexpr int32_t ScreenWidth = 1280;
    constexpr int32_t ScreenHeight = 800;
    constexpr int32_t PanelWidth = 300;

    // Visualization-only - NOT part of FContinentConfig. Default height values are a very small
    // percentage of Earth-scale radii (this is exactly the "no mountains visible" issue this tool
    // exists to let you iterate on without rebuilding the UE plugin each time) - the preview needs its
    // own exaggeration knob, independent of the config, to actually show relief at a glance.
    struct FVisualizationSettings
    {
        // Deliberately NOT Earth-scale by default - see HeightExaggeration's doc comment. Must stay
        // comfortably larger than the worst case (most negative Ocean Floor Depth/Trench slider value)
        // times the largest Height Exaggeration the slider allows, or the displaced radius collapses
        // toward zero (SphereMesh.h's MinRadius clamp then catches it, but the result is a degenerate
        // spiky shape, not a dented sphere - the actual symptom seen before this value was raised).
        float PreviewRadiusMeters = 400000.0f;
        float HeightExaggeration = 5.0f;
        int32_t MeshGridWidth = 128;
        int32_t MeshGridHeight = 64;
    };

    Model BuildRaylibModel(const ContinentLayer& Layer, const FVisualizationSettings& Viz)
    {
        const FSphereMesh Src = BuildSphereMesh(Layer, Viz.MeshGridWidth, Viz.MeshGridHeight, Viz.PreviewRadiusMeters, Viz.HeightExaggeration);

        Mesh RMesh = { 0 };
        RMesh.vertexCount = (int)Src.Vertices.size();
        RMesh.triangleCount = (int)Src.Indices.size() / 3;
        RMesh.vertices = (float*)MemAlloc(sizeof(float) * 3 * RMesh.vertexCount);
        RMesh.normals = (float*)MemAlloc(sizeof(float) * 3 * RMesh.vertexCount);
        RMesh.colors = (unsigned char*)MemAlloc(sizeof(unsigned char) * 4 * RMesh.vertexCount);
        RMesh.indices = (unsigned short*)MemAlloc(sizeof(unsigned short) * Src.Indices.size());

        // Color-codes elevation relative to sea level (0m) - blue below, green/brown/white above -
        // since this tool's job is visually verifying generation math, not final-game shading.
        float MinH = 1e9f, MaxH = -1e9f;
        for (const FSphereMeshVertex& V : Src.Vertices) { MinH = std::min(MinH, V.HeightMeters); MaxH = std::max(MaxH, V.HeightMeters); }
        const float Range = std::max(MaxH - MinH, 1.0f);

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
            if (V.HeightMeters <= 0.0f)
            {
                const float T = MathUtil::Clamp((V.HeightMeters - MinH) / std::max(-MinH, 1.0f), 0.0f, 1.0f);
                C = { (unsigned char)(10 + 20 * T), (unsigned char)(40 + 60 * T), (unsigned char)(90 + 120 * T), 255 };
            }
            else
            {
                const float T = MathUtil::Clamp(V.HeightMeters / std::max(MaxH, 1.0f), 0.0f, 1.0f);
                if (T < 0.5f)
                {
                    const float Tt = T / 0.5f;
                    C = { (unsigned char)(60 + Tt * 80), (unsigned char)(110 + Tt * 40), (unsigned char)(50), 255 };
                }
                else
                {
                    const float Tt = (T - 0.5f) / 0.5f;
                    C = { (unsigned char)(140 + Tt * 115), (unsigned char)(150 + Tt * 105), (unsigned char)(90 + Tt * 165), 255 };
                }
            }
            RMesh.colors[i * 4 + 0] = C.r; RMesh.colors[i * 4 + 1] = C.g; RMesh.colors[i * 4 + 2] = C.b; RMesh.colors[i * 4 + 3] = C.a;
        }
        for (size_t i = 0; i < Src.Indices.size(); ++i)
        {
            RMesh.indices[i] = (unsigned short)Src.Indices[i];
        }

        UploadMesh(&RMesh, false);
        Model M = LoadModelFromMesh(RMesh);
        return M;
    }
}

int main()
{
    InitWindow(ScreenWidth, ScreenHeight, "SolarOrbz Standalone - Continent Generator Preview");
    SetTargetFPS(60);

    // raylib's default far clip plane (1000 units) assumes small-scene scale - our world coordinates
    // are in meters at planet-preview scale (tens of thousands+), so without widening this the whole
    // model (and the camera's own orbit distance) sits beyond the far plane and nothing renders.
    rlSetClipPlanes(0.1, 10000000.0);

    FContinentConfig Config;
    FVisualizationSettings Viz;
    ContinentLayer Layer;
    Layer.Config = Config;
    Layer.Bake();
    Model PreviewModel = BuildRaylibModel(Layer, Viz);

    // Manual orbit camera - left-drag to orbit, wheel to zoom. Deliberately not raylib's built-in
    // CAMERA_ORBITAL mode, which auto-spins on its own rather than responding to the mouse.
    float Yaw = 0.5f, Pitch = 0.4f, Distance = Viz.PreviewRadiusMeters * 3.5f;
    Camera3D Camera = { 0 };
    Camera.target = { 0.0f, 0.0f, 0.0f };
    Camera.up = { 0.0f, 1.0f, 0.0f };
    Camera.fovy = 45.0f;
    Camera.projection = CAMERA_PERSPECTIVE;

    bool bNeedsRebuild = false;
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
        rlDisableBackfaceCulling(); // mesh winding wasn't rigorously verified against raylib's convention - draw both sides rather than risk missing triangles
        DrawModel(PreviewModel, { 0, 0, 0 }, 1.0f, WHITE);
        rlEnableBackfaceCulling();
        EndMode3D();

        DrawRectangle(0, 0, PanelWidth, ScreenHeight, Color{ 30, 30, 36, 235 });
        int Y = 10;
        GuiLabel(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 20 }, "SolarOrbz Standalone"); Y += 24;

        GuiLabel(Rectangle{ 10, (float)Y, 120, 20 }, "Algorithm"); Y += 20;
        // GuiDropdownBox draws its expanded list right where it's called - every control drawn AFTER
        // it in the same frame paints over that list. So its actual draw+interaction call is deferred
        // to the very end of the frame (see below), on top of everything else; only its layout space
        // is reserved here. Everything else is GuiLock()ed while it's open so a click can't "fall
        // through" the open list onto a slider underneath on the same frame - raygui's own idiom for
        // this (see its controls_test_suite demo).
        const Rectangle AlgorithmBoxRect{ 10, (float)Y, PanelWidth - 20.0f, 24 };
        Y += 32;

        if (bAlgorithmDropdownOpen)
        {
            GuiLock();
        }

        char Buf[128];
        auto SliderInt = [&](const char* Label, int32_t& Value, float Min, float Max)
        {
            float F = (float)Value;
            std::snprintf(Buf, sizeof(Buf), "%s: %d", Label, Value);
            if (GuiSlider(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 20 }, nullptr, Buf, &F, Min, Max))
            {
                Value = (int32_t)std::lround(F);
                bNeedsRebuild = true;
            }
            Y += 26;
        };
        auto SliderFloat = [&](const char* Label, float& Value, float Min, float Max)
        {
            std::snprintf(Buf, sizeof(Buf), "%s: %.2f", Label, Value);
            if (GuiSlider(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 20 }, nullptr, Buf, &Value, Min, Max))
            {
                bNeedsRebuild = true;
            }
            Y += 26;
        };

        SliderInt("Seed", Config.Seed, 0, 9999);
        SliderInt("Num Continents", Config.NumContinents, 0, 20);
        SliderInt("Num Islands", Config.NumIslands, 0, 40);
        SliderFloat("Min Continent Deg", Config.MinContinentRadiusDegrees, 1.0f, 60.0f);
        SliderFloat("Max Continent Deg", Config.MaxContinentRadiusDegrees, 1.0f, 60.0f);
        SliderFloat("Coastline Noise Amp", Config.CoastlineNoiseAmplitude, 0.0f, 1.0f);
        SliderFloat("Ocean Floor Depth (m)", Config.OceanFloorDepthMeters, -8000.0f, 0.0f);
        SliderFloat("Land Plateau Height (m)", Config.LandPlateauHeightMeters, 0.0f, 2000.0f);

        if (Config.Algorithm == EContinentAlgorithm::PlateTectonics)
        {
            SliderInt("Num Plates", Config.NumPlates, 2, 64);
            SliderFloat("Mountain Height (m)", Config.MountainHeightMeters, 0.0f, 10000.0f);
            SliderFloat("Trench Depth (m)", Config.TrenchDepthMeters, -10000.0f, 0.0f);
            SliderInt("Steps Per Run", Config.StepsPerRun, 1, 50);
            SliderFloat("Myr Per Step", Config.MillionYearsPerStep, 0.1f, 50.0f);

            if (GuiButton(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 28 }, "Run Simulation"))
            {
                Layer.Config = Config;
                Layer.AdvanceTectonicSimulation(Config.StepsPerRun, Config.MillionYearsPerStep);
                bNeedsRebuild = true;
            }
            Y += 34;
        }

        Y += 6;
        SliderFloat("Height Exaggeration", Viz.HeightExaggeration, 1.0f, 15.0f);
        if (GuiButton(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 28 }, "Regenerate") || bNeedsRebuild)
        {
            Layer.Config = Config;
            Layer.Bake();
            UnloadModel(PreviewModel);
            PreviewModel = BuildRaylibModel(Layer, Viz);
            bNeedsRebuild = false;
        }
        Y += 36;

        const FContinentStatus Status = Layer.GetStatus();
        std::snprintf(Buf, sizeof(Buf), "Land coverage: %.1f%%", Layer.EstimateLandCoveragePercent());
        GuiLabel(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 20 }, Buf); Y += 20;
        if (Config.Algorithm == EContinentAlgorithm::PlateTectonics)
        {
            std::snprintf(Buf, sizeof(Buf), "Simulated age: %.1f Myr", Status.SimulatedAgeMyr);
            GuiLabel(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 20 }, Buf); Y += 20;
            std::snprintf(Buf, sizeof(Buf), "Plates: %d", Status.CurrentPlateCount);
            GuiLabel(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 20 }, Buf); Y += 20;
        }

        GuiUnlock();

        // Drawn last, on top of every other panel control - see the comment at AlgorithmBoxRect above.
        // Switching algorithms here only takes effect for next frame's layout (which sliders show,
        // Bake()'s rebuild) - an imperceptible one-frame lag, not a correctness issue.
        if (GuiDropdownBox(AlgorithmBoxRect, "Radial Seeds;Voronoi Growth;Plate Tectonics", &AlgorithmIndex, bAlgorithmDropdownOpen))
        {
            bAlgorithmDropdownOpen = !bAlgorithmDropdownOpen;
            Config.Algorithm = (EContinentAlgorithm)AlgorithmIndex;
            bNeedsRebuild = true;
        }

        DrawFPS(ScreenWidth - 90, 10);
        EndDrawing();
    }

    UnloadModel(PreviewModel);
    CloseWindow();
    return 0;
}
