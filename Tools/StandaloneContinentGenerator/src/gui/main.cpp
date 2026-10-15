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
#include "PlanetExport.h"

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
        float PreviewRadiusMeters = 400000.0f; // live-editable via the "Radius (km)" slider, see main()
        float HeightExaggeration = 5.0f;
        // Kept in lockstep with TerrainPipeline's own GridWidth/GridHeight by the "Grid Resolution"
        // slider in main() - this used to be a fixed 2x downsample (128x64) of the baked field, which
        // was fine back when the only terrain was a smooth Continent base shape, but it blurs/aliases
        // away the Secondary Noise layer's hill/mountain-scale detail (its highest fBm octave's
        // wavelength is already only a few baked cells wide; resampling that onto a coarser mesh loses
        // it) - confirmed visually (identical config looked flat in the 3D preview but showed clear
        // mottled relief in the CLI's heightmap PNG, which samples the baked field directly). The
        // slider caps both at 352 so vertex count (max 352x176=61952) stays under CreatePreviewModel's
        // unsigned short index limit (65535).
        int32_t MeshGridWidth = 256;
        int32_t MeshGridHeight = 128;
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
    void UpdatePreviewModel(Model& PreviewModel, const FHeightField& Terrain, const FBiomeField& Biomes, const FVisualizationSettings& Viz, float& OutReferenceLineRadius)
    {
        const FLatLongGrid TerrainGrid(Terrain.Width, Terrain.Height);
        auto SampleHeight = [&](const Vec3& Dir) { return Terrain.SampleDirection(TerrainGrid, Dir); };
        const FSphereMesh Src = BuildSphereMesh(SampleHeight, Viz.MeshGridWidth, Viz.MeshGridHeight, Viz.PreviewRadiusMeters, Viz.HeightExaggeration);
        Mesh& RMesh = PreviewModel.meshes[0];

        float MinH = 1e9f, MaxH = -1e9f;
        for (const FSphereMeshVertex& V : Src.Vertices) { MinH = std::min(MinH, V.HeightMeters); MaxH = std::max(MaxH, V.HeightMeters); }
        // BuildSphereMesh displaces each vertex to RadiusMeters + HeightMeters*HeightExaggeration (floored
        // at MinRadius - see its own doc comment), so this is the single largest distance any vertex ends
        // up from the origin. The pole/prime-meridian reference lines are drawn a touch past this (see
        // their draw call in main()) so they stay a complete, fully visible circle/line regardless of
        // Height Exaggeration or Radius - never clipped under even the tallest mountain.
        OutReferenceLineRadius = Viz.PreviewRadiusMeters + MaxH * Viz.HeightExaggeration;

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

            // raylib's default material/shader is unlit - DrawModel with no custom Shader bound just
            // interpolates these vertex colors directly, so without this the Normals computed above
            // (and uploaded to the mesh buffer) have no visual effect at all. A flat, un-shaded
            // elevation-ramp color is exactly why hills/mountains didn't read as relief even once the
            // Secondary Noise layer started actually producing them - slope was in the data but never
            // reached the screen. Simple fixed-direction Lambertian term, baked straight into the
            // vertex color (cheap, no shader needed): makes slopes/ridges/valleys actually look
            // three-dimensional, and as a side benefit smooths over the lat/long mesh's regular
            // diagonal-split triangulation becoming visible as a crosshatch against sharp flat-color
            // boundaries (confirmed - that pattern was a real, if secondary, artifact of the old
            // unlit/flat-ramp rendering, not of the noise data itself).
            static const Vec3 LightDir = Vec3(0.5, -0.4, 0.75).GetSafeNormal();
            const float NdotL = (float)std::max(Vec3::Dot(V.Normal, LightDir), 0.0);
            const float Shade = 0.45f + 0.65f * NdotL;
            auto Tone = [Shade](unsigned char Channel) { return (unsigned char)MathUtil::Clamp((float)Channel * Shade, 0.0f, 255.0f); };
            C.r = Tone(C.r); C.g = Tone(C.g); C.b = Tone(C.b);

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
    // model (and the camera's own orbit distance) sits beyond the far plane and nothing renders. Set
    // generously past the "Radius (km)" slider's own max (100,000 km) times the camera's initial 3.5x
    // orbit distance, plus headroom for scrolling further out - a too-tight far plane here would clip
    // the whole planet invisible the moment Radius is pushed up near that max.
    rlSetClipPlanes(0.1, 2000000000.0);
    rlEnableBackfaceCulling();

    TerrainPipeline Pipeline;
    Pipeline.GridWidth = 256;
    Pipeline.GridHeight = 128;

    auto ContinentNodePtr = std::make_unique<ContinentNode>();
    ContinentNode* ContinentNodeRaw = ContinentNodePtr.get();
    Pipeline.Nodes.push_back(std::move(ContinentNodePtr));

    auto NoiseNodePtr = std::make_unique<NoiseNode>();
    // On by default at a hill/mountain-scale frequency - the Continent layer alone only shapes
    // coastlines/plate-boundary-scale relief (confirmed by eye: a no-noise bake is smooth/featureless
    // across the interior of any landmass); this is the layer that actually puts hills and mountains
    // on that land. Values picked empirically via CLI (comparing a no-noise vs. with-noise heightmap
    // PNG on both Radial Seeds' small islands and a Plate Tectonics continent) - mottled multi-scale
    // relief without washing out the base Continent shape. Still a live slider, so it can be turned
    // back down to 0 for a smooth/stylized look.
    NoiseNodePtr->Config.AmplitudeMeters = 350.0f;
    NoiseNodePtr->Config.Frequency = 8.0f;
    // Ridged, not FNoiseNodeConfig's own Perlin default - confirmed via CLI heightmap comparison at
    // identical Amplitude/Frequency that Ridged's connected, sharp-crested ridgelines are what reads
    // as "mountains," where Perlin's rounded, mottled bumps read as rolling hills. Still a live toggle
    // in the GUI (Secondary Noise section), so any of the five basis types remain one click away.
    NoiseNodePtr->Config.Type = ENoiseType::Ridged;
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
    float ReferenceLineRadius = Viz.PreviewRadiusMeters;
    UpdatePreviewModel(PreviewModel, BakedTerrain, Biomes, Viz, ReferenceLineRadius);

    auto RebakeBase = [&]()
    {
        BakedTerrain = Pipeline.Bake();
        Climate = SimulateClimate(BakedTerrain, ClimateConfig);
        Biomes = ClassifyBiomes(BakedTerrain, Climate, BiomeConfig);
        UpdatePreviewModel(PreviewModel, BakedTerrain, Biomes, Viz, ReferenceLineRadius);
    };
    auto RecomputeClimateAndRefresh = [&]()
    {
        Climate = SimulateClimate(BakedTerrain, ClimateConfig);
        Biomes = ClassifyBiomes(BakedTerrain, Climate, BiomeConfig);
        UpdatePreviewModel(PreviewModel, BakedTerrain, Biomes, Viz, ReferenceLineRadius);
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

    // "Export" section state (see below, near the Regenerate button) - writes the exact same
    // UE5-import-ready files the CLI's --config run does (ExportPlanetToUE5, shared via
    // src/core/PlanetExport.h), from whatever is currently baked/live-edited in this preview,
    // instead of requiring a drop back to the CLI with a hand-written config to reproduce it.
    char ExportPrefixBuf[128] = "solarorbz_export";
    bool bExportPrefixEditMode = false;
    char ExportStatusBuf[192] = "";

    // The panel's own content (Planet/Algorithm/Base Layer Adjust/.../stats) is taller than
    // ScreenHeight once every section is visible at once (confirmed - the "Simulated age"/"Plates"
    // stats lines render below the bottom of a 980px-tall window with Plate Tectonics selected), so
    // it needs to scroll. PanelScrollY is in the same pixel units as Y below; MeasuredPanelContentHeight
    // is remeasured every frame from this frame's own layout (see the end of the panel block) so the
    // clamp always reflects the CURRENT section visibility (e.g. Plate Tectonics' extra sliders), one
    // frame stale at most - fine since layout height is otherwise static.
    float PanelScrollY = 0.0f;
    float MeasuredPanelContentHeight = 1600.0f;

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
        else
        {
            // Scrolling over the panel used to do nothing at all (the branch above only runs when the
            // mouse is OVER THE 3D VIEW) - repurposed for panel scroll instead of adding a new modifier
            // key or control. Scrolling down (negative wheel delta) increases PanelScrollY, which
            // DECREASES the starting Y below, moving content up the screen to reveal what's beneath.
            PanelScrollY -= GetMouseWheelMove() * 40.0f;
        }
        const float MaxPanelScroll = std::max(0.0f, MeasuredPanelContentHeight - (float)ScreenHeight + 16.0f);
        PanelScrollY = MathUtil::Clamp(PanelScrollY, 0.0f, MaxPanelScroll);

        Camera.position = {
            Distance * std::cos(Pitch) * std::sin(Yaw),
            Distance * std::sin(Pitch),
            Distance * std::cos(Pitch) * std::cos(Yaw)
        };

        BeginDrawing();
        ClearBackground(Color{ 18, 18, 24, 255 });

        BeginMode3D(Camera);
        DrawModel(PreviewModel, { 0, 0, 0 }, 1.0f, WHITE);

        // Visual-reference-only overlay: a line through both poles (the sphere's own rotation axis)
        // and a great circle through the prime meridian (0 deg longitude) AND its antimeridian (180
        // deg) - the same plane covers both, so one DrawCircle3D call draws both for free. Neither
        // line is node config or saved anywhere; they're just an orientation aid.
        //
        // Axis mapping: FLatLongGrid's convention (see its own header comment) has the poles on the
        // core math's +-Z axis and the prime meridian (U=0.5) in the core X-Z plane (core Y=0) - and
        // UpdatePreviewModel's vertex writes remap core (X,Y,Z) -> raylib (X,Z,Y) (raylib is Y-up; the
        // core math is Z-up, matching the UE plugin's own convention). So in raylib space: the poles
        // sit on the Y axis, and the prime-meridian/antimeridian plane is raylib's own XY plane
        // (raylib Z=0) - exactly DrawCircle3D's own default unrotated plane, needing no rotation at all.
        //
        // Drawn at ReferenceLineRadius (a touch past the tallest displaced vertex - see
        // UpdatePreviewModel) so both stay a complete, unbroken circle/line regardless of Height
        // Exaggeration or Radius, never clipped under even the tallest mountain.
        //
        // Built out of DrawCylinderEx segments, NOT DrawLine3D/DrawCircle3D - those draw through
        // rlgl's immediate-mode RL_LINES batch, a separate path from DrawModel's own mesh draw, and
        // confirmed (via screenshot, on both this Xvfb host and the user's own machine) to not
        // reliably depth-test against the sphere: the far-side half of each shape, which should be
        // hidden behind the opaque terrain, was rendering straight through it instead. DrawCylinderEx
        // instead builds real triangles through the exact same mesh-draw path as the sphere itself,
        // so it depth-tests against it correctly, at the cost of being several dozen short draw calls
        // instead of one - cheap at this vertex count.
        const float TubeRadius = std::max(ReferenceLineRadius * 0.0025f, 1.0f);
        DrawCylinderEx(Vector3{ 0, -ReferenceLineRadius, 0 }, Vector3{ 0, ReferenceLineRadius, 0 }, TubeRadius, TubeRadius, 8, Color{ 255, 60, 60, 255 });
        constexpr int32_t MeridianSegments = 72;
        Vector3 PrevPoint{ 0.0f, ReferenceLineRadius, 0.0f };
        for (int32_t i = 1; i <= MeridianSegments; ++i)
        {
            const float Angle = (float)i * (2.0f * (float)MathUtil::PI_D / (float)MeridianSegments);
            const Vector3 NextPoint{ std::sin(Angle) * ReferenceLineRadius, std::cos(Angle) * ReferenceLineRadius, 0.0f };
            DrawCylinderEx(PrevPoint, NextPoint, TubeRadius, TubeRadius, 6, Color{ 255, 230, 60, 255 });
            PrevPoint = NextPoint;
        }

        EndMode3D();

        DrawRectangle(0, 0, PanelWidth, ScreenHeight, Color{ 30, 30, 36, 235 });
        // No scissor/clip region needed around the scrolled content below: the window's own
        // framebuffer is exactly ScreenHeight tall, so anything scrolled above Y=0 or below Y=
        // ScreenHeight is already not rasterized - there's nothing left for a scissor rect to cut off.
        // (A BeginScissorMode(0, 0, PanelWidth, ScreenHeight) was tried here first and reverted: every
        // slider's value label is deliberately drawn past PanelWidth, out into the otherwise-empty
        // margin before the 3D view starts, and clipping at PanelWidth truncated every one of them.)
        int Y = 8 - (int)PanelScrollY;
        GuiLabel(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 20 }, "SolarOrbz Standalone"); Y += 22;

        // Moved above the Algorithm section (which used to define these itself) so the new Planet
        // section below - Radius/Grid Resolution, logically global settings above any Continent
        // config - can use them too.
        char Buf[128];
        // GuiSlider's own int return value ("result") is dead code in this build of raygui -
        // GuiSliderPro declares `int result = 0;` and never reassigns it before returning, so
        // `if (GuiSlider(...))` is ALWAYS false (confirmed by reading build/_deps/raygui-src/src/
        // raygui.h directly). *value itself still gets mutated correctly by pointer while dragging -
        // that part of raygui works fine - but every one of this file's `if (GuiSlider(...)) {
        // *bDirtyFlag = true; }` call sites was silently dead: no slider drag has ever set a dirty
        // flag, so no slider drag has ever triggered a live rebake, in this entire GUI. Confirmed via
        // Xvfb: dragging "Grid Resolution" moved nothing, and "Radius (km)" visibly reset itself back
        // every frame (its SliderFloat binds the real field directly, so GuiSliderPro's pointer write
        // did take effect for that one frame, but main()'s own `if (bRadiusDirty)` write-back into
        // Viz.PreviewRadiusMeters never ran, so next frame's fresh read of Viz.PreviewRadiusMeters
        // stomped it right back). Fixed by detecting the change ourselves (before/after comparison)
        // instead of trusting GuiSlider's return value.
        auto SliderInt = [&](const char* Label, int32_t& Value, float Min, float Max, bool* bDirtyFlag)
        {
            float F = (float)Value;
            std::snprintf(Buf, sizeof(Buf), "%s: %d", Label, Value);
            GuiSlider(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 18 }, nullptr, Buf, &F, Min, Max);
            const int32_t NewValue = (int32_t)std::lround(F);
            if (NewValue != Value)
            {
                Value = NewValue;
                if (bDirtyFlag) *bDirtyFlag = true;
            }
            Y += 23;
        };
        auto SliderFloat = [&](const char* Label, float& Value, float Min, float Max, bool* bDirtyFlag)
        {
            std::snprintf(Buf, sizeof(Buf), "%s: %.2f", Label, Value);
            const float OldValue = Value;
            GuiSlider(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 18 }, nullptr, Buf, &Value, Min, Max);
            if (bDirtyFlag && Value != OldValue) *bDirtyFlag = true;
            Y += 23;
        };
        auto SectionLabel = [&](const char* Label)
        {
            Y += 4;
            GuiLine(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 1 }, nullptr); Y += 4;
            GuiLabel(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 16 }, Label); Y += 18;
        };

        SectionLabel("Planet");
        {
            // Radius is visualization-only in the sense that it's not a node Config field (no
            // generation math reads it), but it IS the actual planet radius the preview sphere is
            // built at - BuildSphereMesh displaces real vertices by HeightMeters against this exact
            // RadiusMeters. Edited in km for a less awkward slider range than raw meters would give
            // (Earth is ~6371 km; range covers small moons through gas-giant-scale bodies).
            float RadiusKm = Viz.PreviewRadiusMeters / 1000.0f;
            bool bRadiusDirty = false;
            SliderFloat("Radius (km)", RadiusKm, 1.0f, 100000.0f, &bRadiusDirty);
            if (bRadiusDirty)
            {
                const float OldRadius = Viz.PreviewRadiusMeters;
                Viz.PreviewRadiusMeters = RadiusKm * 1000.0f;
                // Keep the camera's relative zoom level instead of leaving Distance fixed - a 10x
                // radius change with Distance untouched would otherwise either strand the camera deep
                // inside the new sphere or leave the old one looking like a speck.
                Distance *= Viz.PreviewRadiusMeters / OldRadius;
                // BuildSphereMesh bakes RadiusMeters directly into vertex positions - without also
                // refreshing the mesh buffer, the sphere stays sized at whatever radius it was last
                // baked at (confirmed via Xvfb: the preview shrank to a barely-visible speck after a
                // large Radius change, because only the CAMERA had moved to match the new scale while
                // the actual mesh verts were untouched). bNeedsClimateRefresh is this file's existing
                // "cheap visual-only refresh" flag (see Height Exaggeration below) - reused here since
                // Radius, like exaggeration, needs only UpdatePreviewModel, not a full Pipeline rebake.
                bNeedsClimateRefresh = true;
            }

            // Grid Resolution drives BOTH the pipeline's bake resolution (Pipeline.GridWidth/Height)
            // and the preview mesh resolution (Viz.MeshGridWidth/Height) together - they're kept in
            // lockstep (see MeshGridWidth's own doc comment on why a mismatch blurs/aliases detail
            // away). Always 2:1 (width:height) to match the equirectangular FHeightField/FLatLongGrid
            // convention used everywhere else. Capped at 352 so Width*Height (max 352*176=61952) stays
            // under CreatePreviewModel's unsigned short index limit (65535).
            bool bResolutionDirty = false;
            SliderInt("Grid Resolution", Pipeline.GridWidth, 64, 352, &bResolutionDirty);
            if (bResolutionDirty)
            {
                Pipeline.GridWidth -= Pipeline.GridWidth % 2; // keep it even so Height divides cleanly
                Pipeline.GridHeight = Pipeline.GridWidth / 2;
                Viz.MeshGridWidth = Pipeline.GridWidth;
                Viz.MeshGridHeight = Pipeline.GridHeight;
                UnloadModel(PreviewModel);
                PreviewModel = CreatePreviewModel(Viz.MeshGridWidth, Viz.MeshGridHeight);
                bNeedsRebuild = true; // changing resolution re-bakes everything, same as Regenerate
            }
        }

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
        {
            // Perlin's rounded, mottled "hills" (the previous default) vs. Ridged's connected,
            // sharp-crested ridgelines - confirmed via CLI heightmap comparison at identical Amplitude/
            // Frequency: Ridged is unmistakably what reads as "mountains" rather than rolling terrain,
            // since it takes abs(Perlin) and inverts it, turning the noise field's ZERO-CROSSINGS
            // (naturally line-like, not point-like) into sharp ridge crests instead of smooth hilltops.
            int32_t NoiseTypeIndex = (int32_t)NoiseNodeRaw->Config.Type;
            const int32_t OldNoiseTypeIndex = NoiseTypeIndex;
            GuiToggleGroup(Rectangle{ 10, (float)Y, (PanelWidth - 20.0f) / 5.0f, 20 }, "Perlin;Ridged;Billow;Value;Voronoi", &NoiseTypeIndex);
            if (NoiseTypeIndex != OldNoiseTypeIndex)
            {
                NoiseNodeRaw->Config.Type = (ENoiseType)NoiseTypeIndex;
                bNeedsRebuild = true;
            }
            Y += 26;
        }
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

        SectionLabel("Export");
        {
            // Click toggles edit mode, same pattern GuiTextBox's own raygui examples use - no GuiLock
            // needed around it (unlike the Algorithm dropdown above): a text box doesn't expand over
            // other controls, it just captures keyboard input while editing.
            if (GuiTextBox(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 22 }, ExportPrefixBuf, (int)sizeof(ExportPrefixBuf), bExportPrefixEditMode))
            {
                bExportPrefixEditMode = !bExportPrefixEditMode;
            }
            Y += 26;

            if (GuiButton(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 26 }, "Export to UE5"))
            {
                // Writes from BakedTerrain/Climate/Biomes exactly as currently shown in the 3D
                // preview (erosion included, if "Run Hydraulic/Thermal Erosion" was clicked) - not a
                // fresh bake. RadiusMeters mirrors the "Radius (km)" slider above (Viz.PreviewRadiusMeters)
                // so the PCG points CSV's Slope column means the same thing here as it would reimported
                // into a UE planet actually built at this radius - see WritePcgPointsCsv's own comment.
                FPlanetExportOptions ExportOptions;
                ExportOptions.OutPrefix = ExportPrefixBuf;
                ExportOptions.RadiusMeters = (double)Viz.PreviewRadiusMeters;
                const FPlanetExportResult ExportResult = ExportPlanetToUE5(BakedTerrain, Climate, Biomes, ClimateConfig, ExportOptions);
                std::snprintf(ExportStatusBuf, sizeof(ExportStatusBuf),
                    ExportResult.bOk ? "Wrote %s_* (height %.0fm..%.0fm)" : "Export FAILED - see console (%s_*, %.0fm..%.0fm attempted)",
                    ExportPrefixBuf, ExportResult.MinHeightMeters, ExportResult.MaxHeightMeters);
            }
            Y += 30;

            if (ExportStatusBuf[0] != '\0')
            {
                GuiLabel(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 18 }, ExportStatusBuf); Y += 18;
            }
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

        // Y - (8 - (int)PanelScrollY) is this frame's actual layout span (bottom minus the scrolled
        // starting point); adding 8 back re-references it to the unscrolled Y=8 baseline that
        // MaxPanelScroll (computed at the top of the loop, from LAST frame's measurement) expects.
        MeasuredPanelContentHeight = (float)Y - 8.0f + PanelScrollY;

        // A plain scrollbar track + thumb along the panel's right edge - the only indication a mouse-
        // wheel scroll even does anything here, since nothing else about the panel looks scrollable.
        if (MaxPanelScroll > 0.0f)
        {
            const float TrackX = PanelWidth - 6.0f;
            DrawRectangle((int)TrackX, 0, 4, ScreenHeight, Color{ 20, 20, 26, 255 });
            const float ThumbHeight = std::max(30.0f, (float)ScreenHeight * (float)ScreenHeight / MeasuredPanelContentHeight);
            const float ThumbY = (PanelScrollY / MaxPanelScroll) * ((float)ScreenHeight - ThumbHeight);
            DrawRectangle((int)TrackX, (int)ThumbY, 4, (int)ThumbHeight, Color{ 120, 170, 200, 255 });
        }

        DrawFPS(ScreenWidth - 90, 10);
        EndDrawing();
    }

    UnloadModel(PreviewModel);
    CloseWindow();
    return 0;
}
