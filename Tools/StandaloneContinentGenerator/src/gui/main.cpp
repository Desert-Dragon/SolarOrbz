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
#include "Nodes/ShapeStampNode.h"
#include "Climate.h"
#include "BiomeClassifier.h"
#include "SphereMesh.h"
#include "GroundPatchMesh.h"
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

    // Ocean overlay mesh resolution - a plain constant-radius sphere, so far fewer vertices are
    // needed than the terrain preview's own (height-bearing) mesh to still look smooth at this scale.
    constexpr int32_t OceanGridWidth = 64;
    constexpr int32_t OceanGridHeight = 32;

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

        // On by default - the whole point of this overlay is to make "where is sea level" obvious at
        // a glance rather than something only readable from the Height view's blue/green color ramp.
        // See its draw call in main() for why it's drawn at exactly PreviewRadiusMeters, un-exaggerated.
        bool bShowOceanOverlay = true;
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

    // Ocean overlay - same topology/winding as CreatePreviewModel (built via that exact function, at
    // OceanGridWidth/Height), just a plain constant-RadiusMeters sphere with no height displacement
    // and no per-vertex color variation (DrawModel's own tint parameter supplies the single translucent
    // color - see its call site's own comment for why this mirrors PreviewModel's pattern instead of a
    // raylib GenMeshSphere scaled via DrawModel's `scale` parameter). Colors are left at
    // CreatePreviewModel's own default fill (opaque white) permanently - never touched again after the
    // one-time allocation, since DrawModel's tint already does the only coloring this overlay needs.
    void UpdateOceanModel(Model& OceanModel, int32_t Width, int32_t Height, float RadiusMeters)
    {
        const FLatLongGrid Grid(Width, Height);
        Mesh& RMesh = OceanModel.meshes[0];
        Grid.ForEachCell([&](int32_t Idx, const Vec3& Dir, double, double)
        {
            const Vec3 Pos = Dir * (double)RadiusMeters;
            RMesh.vertices[Idx * 3 + 0] = (float)Pos.X;
            RMesh.vertices[Idx * 3 + 1] = (float)Pos.Z; // raylib Y-up remap, same convention as UpdatePreviewModel
            RMesh.vertices[Idx * 3 + 2] = (float)Pos.Y;
            RMesh.normals[Idx * 3 + 0] = (float)Dir.X;
            RMesh.normals[Idx * 3 + 1] = (float)Dir.Z;
            RMesh.normals[Idx * 3 + 2] = (float)Dir.Y;
        });
        UpdateMeshBuffer(RMesh, 0, RMesh.vertices, (int)(sizeof(float) * 3 * RMesh.vertexCount), 0);
        UpdateMeshBuffer(RMesh, 2, RMesh.normals, (int)(sizeof(float) * 3 * RMesh.vertexCount), 0);
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

    // Ground View - "stand on the ground and look around" counterpart to PreviewModel's whole-planet
    // orbit view (see GroundPatchMesh.h's own top comment for why a separate flat local mesh, not a
    // finer BuildSphereMesh bake, is what actually makes ground-scale noise visible at all). Same
    // static-topology-once/UpdateMeshBuffer-per-update pattern as CreatePreviewModel/UpdatePreviewModel
    // above - GroundPatchResolution never changes at runtime, so only the Resolution the caller first
    // creates this with matters; later terrain/location changes go through UpdateGroundPatchModel.
    Model CreateGroundPatchModel(int32_t Resolution)
    {
        Resolution = std::max(Resolution, 2);
        Mesh RMesh = { 0 };
        RMesh.vertexCount = Resolution * Resolution;
        RMesh.triangleCount = (Resolution - 1) * (Resolution - 1) * 2;
        RMesh.vertices = (float*)MemAlloc(sizeof(float) * 3 * RMesh.vertexCount);
        RMesh.normals = (float*)MemAlloc(sizeof(float) * 3 * RMesh.vertexCount);
        RMesh.colors = (unsigned char*)MemAlloc(sizeof(unsigned char) * 4 * RMesh.vertexCount);
        RMesh.indices = (unsigned short*)MemAlloc(sizeof(unsigned short) * RMesh.triangleCount * 3);

        std::fill(RMesh.vertices, RMesh.vertices + 3 * RMesh.vertexCount, 0.0f);
        std::fill(RMesh.normals, RMesh.normals + 3 * RMesh.vertexCount, 0.0f);
        std::fill(RMesh.colors, RMesh.colors + 4 * RMesh.vertexCount, (unsigned char)255);

        size_t IndexCursor = 0;
        for (int32_t Row = 0; Row < Resolution - 1; ++Row)
        {
            for (int32_t Col = 0; Col < Resolution - 1; ++Col)
            {
                const unsigned short I00 = (unsigned short)(Row * Resolution + Col);
                const unsigned short I10 = (unsigned short)(Row * Resolution + Col + 1);
                const unsigned short I01 = (unsigned short)((Row + 1) * Resolution + Col);
                const unsigned short I11 = (unsigned short)((Row + 1) * Resolution + Col + 1);
                // Same verified front-facing order as CreatePreviewModel's own index generation.
                RMesh.indices[IndexCursor++] = I00; RMesh.indices[IndexCursor++] = I11; RMesh.indices[IndexCursor++] = I10;
                RMesh.indices[IndexCursor++] = I00; RMesh.indices[IndexCursor++] = I01; RMesh.indices[IndexCursor++] = I11;
            }
        }

        UploadMesh(&RMesh, true);
        return LoadModelFromMesh(RMesh);
    }

    void UpdateGroundPatchModel(Model& PatchModel, const FGroundPatchMesh& Src)
    {
        Mesh& RMesh = PatchModel.meshes[0];
        for (size_t i = 0; i < Src.Vertices.size(); ++i)
        {
            const FGroundPatchVertex& V = Src.Vertices[i];
            RMesh.vertices[i * 3 + 0] = V.X;
            RMesh.vertices[i * 3 + 1] = V.Y;
            RMesh.vertices[i * 3 + 2] = V.Z;
            RMesh.normals[i * 3 + 0] = (float)V.Normal.X;
            RMesh.normals[i * 3 + 1] = (float)V.Normal.Y;
            RMesh.normals[i * 3 + 2] = (float)V.Normal.Z;

            // Same cheap fixed-direction Lambertian trick UpdatePreviewModel's own comment explains in
            // full (raylib's default material is unlit - DrawModel just interpolates vertex colors
            // verbatim, so without this, flat per-vertex color would make the patch's own relief
            // completely invisible from any angle steep enough that its silhouette against the sky
            // doesn't give it away - confirmed via screenshot: looking straight down at Ground Detail's
            // 0.4m bumps showed nothing at all under a flat tint, only the horizon-silhouette view
            // revealed them, and a ground-level preview whose detail is only visible from ONE specific
            // viewing angle isn't a useful preview). Same LightDir, same base-color-times-Shade shape -
            // deliberately NOT factored into a shared helper with UpdatePreviewModel's copy, since one
            // takes a biome-aware base color per vertex and this one has a single constant base color;
            // a shared function would need to take that color as a parameter either way, which is the
            // entire body of this one.
            static const Vec3 LightDir = Vec3(0.5, 0.75, -0.4).GetSafeNormal();
            const float NdotL = (float)std::max(Vec3::Dot(V.Normal, LightDir), 0.0);
            const float Shade = 0.45f + 0.65f * NdotL;
            auto Tone = [Shade](unsigned char Channel) { return (unsigned char)MathUtil::Clamp((float)Channel * Shade, 0.0f, 255.0f); };
            RMesh.colors[i * 4 + 0] = Tone(150);
            RMesh.colors[i * 4 + 1] = Tone(130);
            RMesh.colors[i * 4 + 2] = Tone(100);
            RMesh.colors[i * 4 + 3] = 255;
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

    // Clip planes are recomputed every frame instead of set once here - see the per-frame call right
    // before BeginMode3D below for why a single fixed value (this used to be a constant 0.1..2e9,
    // sized to cover the "Radius (km)" slider's max of 100,000 km regardless of its CURRENT value)
    // caused visible z-fighting once the Ocean Overlay was added.
    rlEnableBackfaceCulling();

    TerrainPipeline Pipeline;
    Pipeline.GridWidth = 256;
    Pipeline.GridHeight = 128;

    auto ContinentNodePtr = std::make_unique<ContinentNode>();
    ContinentNode* ContinentNodeRaw = ContinentNodePtr.get();
    Pipeline.Nodes.push_back(std::move(ContinentNodePtr));

    // GUI edits Stamps[0] only (same "CLI supports an array, GUI exposes one" split this file
    // already uses for Noise) - pre-seeded with one entry, disabled by default, so the panel always
    // has a live-bound stamp to show without silently placing an unexpected mountain on a fresh planet.
    auto ShapeStampNodePtr = std::make_unique<ShapeStampNode>();
    ShapeStampNodePtr->Stamps.push_back(FShapeStamp{});
    ShapeStampNodePtr->Stamps[0].bEnabled = false;
    ShapeStampNode* ShapeStampNodeRaw = ShapeStampNodePtr.get();
    Pipeline.Nodes.push_back(std::move(ShapeStampNodePtr));

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

    // Ground Detail - a SECOND, independent Noise node, specifically for the scale a standing/walking
    // viewer actually sees underfoot, not the hill/mountain-scale relief NoiseNodeRaw above already
    // covers. Real gap this closes: the chunked terrain streaming system (SolarOrbzChunkLODPolicy.h's
    // TargetQuadEdgeMeters) can mesh a planet down to a 1-4m quad, and the UE plugin's own Noise Layer
    // can now author a real-world WavelengthMeters directly (same fix, ported here) - but neither one
    // puts anything THERE unless a layer actually exists at that wavelength. Without this second node,
    // a fresh planet has zero ground-level variation: the mesh would get finer, but finer sampling of
    // the SAME hill-scale noise field just means smoother-looking hills, not new small-scale texture -
    // confirmed by eye, the exact complaint this node exists to fix. On by default (not an opt-in
    // slider at 0) specifically because the ask was for this to be procedurally there, not something a
    // designer has to remember to add. WavelengthMeters uses this tool's own new FPipelineContext::
    // RadiusMeters conversion (see FNoiseNodeConfig::WavelengthMeters' own comment) - 3m default
    // wavelength, Octaves=4 reaches down to roughly 3m/2^3 = 0.375m at the finest octave, a plausible
    // "uneven ground, loose rock" scale without the single-rock blockiness a too-low Octave count or
    // too-coarse a base wavelength would give. Kept deliberately subtle (AmplitudeMeters 0.4 - ankle-
    // height variation) rather than dramatic: this rides OVER whatever hills/mountains/coastline
    // terrain the rest of the stack already shaped, so it needs to read as ground TEXTURE, not fight
    // the large-scale landform it's sitting on. Perlin, not Ridged like the hills layer above - Ridged
    // here would read as a field of small sharp blades, not the duller all-over unevenness real ground
    // actually has at this scale.
    auto DetailNoiseNodePtr = std::make_unique<NoiseNode>();
    DetailNoiseNodePtr->Config.Type = ENoiseType::Perlin;
    DetailNoiseNodePtr->Config.Octaves = 4;
    DetailNoiseNodePtr->Config.Persistence = 0.5f;
    DetailNoiseNodePtr->Config.Lacunarity = 2.0f;
    DetailNoiseNodePtr->Config.AmplitudeMeters = 0.4f;
    DetailNoiseNodePtr->Config.bWavelengthInMeters = true;
    DetailNoiseNodePtr->Config.WavelengthMeters = 3.0f;
    DetailNoiseNodePtr->Config.SeedOffset = 7919; // decorrelate from NoiseNodeRaw's own seed offset (0) - an arbitrary prime, same role as every other SeedOffset in this file
    NoiseNode* DetailNoiseNodeRaw = DetailNoiseNodePtr.get();
    Pipeline.Nodes.push_back(std::move(DetailNoiseNodePtr));

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

    // Declared before the first Bake() (moved up from right after it) specifically so
    // Pipeline.Context.RadiusMeters can be set from Viz.PreviewRadiusMeters below BEFORE that first
    // bake runs - otherwise the very first bake would use FPipelineContext::RadiusMeters' own default
    // (Earth's 6378137m) regardless of Viz's own default (400,000m), and any WavelengthMeters-authored
    // noise node (the Ground Detail node above) would look wrong until the next Regenerate/Radius
    // change happened to resync them.
    FVisualizationSettings Viz;
    Pipeline.Context.RadiusMeters = (double)Viz.PreviewRadiusMeters;

    FHeightField BakedTerrain = Pipeline.Bake();
    FClimateField Climate = SimulateClimate(BakedTerrain, ClimateConfig);
    FBiomeField Biomes = ClassifyBiomes(BakedTerrain, Climate, BiomeConfig);

    Model PreviewModel = CreatePreviewModel(Viz.MeshGridWidth, Viz.MeshGridHeight);
    float ReferenceLineRadius = Viz.PreviewRadiusMeters;
    UpdatePreviewModel(PreviewModel, BakedTerrain, Biomes, Viz, ReferenceLineRadius);

    // Ocean overlay - built via CreatePreviewModel itself (same static-topology-once/UpdateMeshBuffer-
    // per-frame pattern as PreviewModel - see UpdateOceanModel's own comment for why NOT a raylib
    // GenMeshSphere scaled via DrawModel's `scale` parameter, which was tried first and didn't reliably
    // depth-test against the terrain on this host). World-space positions baked in immediately so the
    // very first frame (before any toggle/radius change calls UpdateOceanModel again) is already correct.
    Model OceanModel = CreatePreviewModel(OceanGridWidth, OceanGridHeight);
    UpdateOceanModel(OceanModel, OceanGridWidth, OceanGridHeight, Viz.PreviewRadiusMeters);

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

    // Ground View - see GroundPatchMesh.h's own top comment for why this exists (the orbit preview's
    // whole-planet bake grid is many kilometers between samples, nowhere near fine enough to show a
    // meter-scale Ground Detail layer's actual shape). GroundViewLatitude/LongitudeDegrees pick WHERE
    // on the planet to stand, reusing the same Lat/Long convention Shape Stamps already use.
    float GroundViewLatitudeDegrees = 0.0f;
    float GroundViewLongitudeDegrees = 0.0f;
    constexpr float GroundPatchSizeMeters = 40.0f; // a modest back-yard-sized patch - see this file's own README note on why not bigger
    constexpr int32_t GroundPatchResolution = 128;
    constexpr float GroundEyeHeightMeters = 1.8f;
    bool bGroundViewActive = false;
    // Starts true so the very first frame Ground View is ever entered already has a real patch built
    // (CreateGroundPatchModel below only allocates the topology/buffers, every vertex starts at the
    // origin) rather than a flat degenerate slab until something else happens to mark it dirty.
    bool bGroundPatchDirty = true;
    // Separate from the orbit camera's own Yaw/Pitch/Distance - a first-person look-around (mouse
    // drag rotates the view direction in place) rather than an orbit around a fixed target.
    float GroundViewYaw = 0.0f, GroundViewPitch = 0.0f;
    Model GroundPatchModel = CreateGroundPatchModel(GroundPatchResolution);

    auto RebuildGroundPatch = [&]()
    {
        const Vec3 CenterDir = Vec3(
            std::cos(MathUtil::DegreesToRadians(GroundViewLatitudeDegrees)) * std::cos(MathUtil::DegreesToRadians(GroundViewLongitudeDegrees)),
            std::cos(MathUtil::DegreesToRadians(GroundViewLatitudeDegrees)) * std::sin(MathUtil::DegreesToRadians(GroundViewLongitudeDegrees)),
            std::sin(MathUtil::DegreesToRadians(GroundViewLatitudeDegrees))
        ).GetSafeNormal();
        Vec3 Tangent, Bitangent;
        ComputeTangentBasis(CenterDir, Tangent, Bitangent);

        // The coarse-grid's own (aliased/meaningless-shape, but still nonzero) sample of
        // DetailNoiseNodeRaw's contribution AT EXACTLY the center point - subtracted back out below so
        // the patch shows its real, continuously-evaluated detail noise as a pure DELTA from the
        // center, not an absolute elevation. The planet's actual baked elevation at this spot
        // (Continent shape, Shape Stamps, the hill-scale Noise layer) is deliberately NOT added back
        // in - this patch exists to show ground-level TEXTURE, and centering it at Y=0 regardless of
        // whether this spot is a mountainside or a beach keeps the camera/eye-height math below simple
        // (stand at GroundEyeHeightMeters above local Y=0, full stop) instead of needing to know and
        // offset by the planet's actual elevation here too.
        const float DetailAtCenter = DetailNoiseNodeRaw->EvaluateAt(CenterDir, Pipeline.Context);

        const FGroundPatchMesh Patch = BuildGroundPatchMesh(
            [&](float LocalX, float LocalZ) -> float
            {
                // Tangent-plane direction for this sample - see ComputeTangentBasis' own comment for
                // why this flat approximation is exact enough at this patch size.
                const Vec3 Dir = (CenterDir + (Tangent * (double)LocalX + Bitangent * (double)LocalZ) * (1.0 / Pipeline.Context.RadiusMeters)).GetSafeNormal();
                const float DetailHere = DetailNoiseNodeRaw->EvaluateAt(Dir, Pipeline.Context);
                return DetailHere - DetailAtCenter;
            },
            GroundPatchSizeMeters, GroundPatchResolution);

        UpdateGroundPatchModel(GroundPatchModel, Patch);
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
                if (bGroundViewActive)
                {
                    // First-person look-around (rotates the VIEW DIRECTION in place) rather than the
                    // orbit camera's own "rotate the camera around a fixed target" - see Camera.target's
                    // computation below for where GroundViewYaw/Pitch actually get used.
                    GroundViewYaw -= Delta.x * 0.005f;
                    GroundViewPitch = MathUtil::Clamp(GroundViewPitch - Delta.y * 0.005f, -1.5f, 1.5f);
                }
                else
                {
                    Yaw -= Delta.x * 0.005f;
                    Pitch = MathUtil::Clamp(Pitch - Delta.y * 0.005f, -1.5f, 1.5f);
                }
            }
            if (!bGroundViewActive)
            {
                Distance = std::max(Distance - GetMouseWheelMove() * Viz.PreviewRadiusMeters * 0.1f, Viz.PreviewRadiusMeters * 1.05f);
            }
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

        if (bGroundViewActive && bGroundPatchDirty)
        {
            RebuildGroundPatch();
            bGroundPatchDirty = false;
        }

        if (bGroundViewActive)
        {
            // Standing at the patch center (local origin, Y=0 - see RebuildGroundPatch's own comment
            // on why the patch is centered there regardless of this spot's actual planetary elevation)
            // at human eye height, looking in whatever direction GroundViewYaw/Pitch currently point.
            const Vector3 EyePos{ 0.0f, GroundEyeHeightMeters, 0.0f };
            const Vector3 LookDir{
                std::cos(GroundViewPitch) * std::sin(GroundViewYaw),
                std::sin(GroundViewPitch),
                std::cos(GroundViewPitch) * std::cos(GroundViewYaw)
            };
            Camera.position = EyePos;
            Camera.target = Vector3{ EyePos.x + LookDir.x, EyePos.y + LookDir.y, EyePos.z + LookDir.z };
        }
        else
        {
            Camera.target = { 0.0f, 0.0f, 0.0f };
            Camera.position = {
                Distance * std::cos(Pitch) * std::sin(Yaw),
                Distance * std::sin(Pitch),
                Distance * std::cos(Pitch) * std::cos(Yaw)
            };
        }

        // Clip planes sized to the CURRENT scene scale (camera Distance/ReferenceLineRadius), not a
        // single fixed value wide enough to cover every radius the "Radius (km)" slider allows
        // regardless of its actual current setting (the old 0.1..2e9 constant, sized for a 100,000 km
        // planet, was ~7 orders of magnitude wider than this tool's own 400 km default ever needs).
        // Standard (non-reversed) depth buffers pack almost all their usable precision close to the
        // near plane and starve it near the far plane - a near/far ratio that huge left nowhere near
        // enough precision at the planet's actual surface to tell the Ocean Overlay's sea-level shell
        // apart from land only a few hundred meters above/below it, which read as per-pixel flickering
        // speckle (confirmed via screenshot the moment Ocean Overlay was added - every single previous
        // feature in this file only ever needed ONE surface visible at a given pixel, so this exact
        // failure mode had nothing to expose it before now). Recomputed every frame (not just once)
        // since Distance changes continuously while orbiting/zooming and ReferenceLineRadius changes
        // with Radius/Height Exaggeration/the baked terrain itself. Both bounds keep generous (2x-10x)
        // margin past what's actually needed so normal zooming/panel use never clips the sphere away.
        // Ground View's whole scene is a flat few-tens-of-meters patch around the origin - the orbit
        // camera's own Distance-scaled clip planes above would be wildly wrong at this scale (sized
        // for a planet-radius Distance, they'd either clip the entire patch away as "too close" or
        // waste all the depth buffer's precision on a range the patch never approaches). Fixed, small,
        // human-scale planes instead - 0.05m near (closer than that is "inside" the camera's own head)
        // out to 4x the patch size (comfortably past its far edge regardless of look direction).
        if (bGroundViewActive)
        {
            rlSetClipPlanes(0.05f, GroundPatchSizeMeters * 4.0f);
        }
        else
        {
            // Clip planes sized to the CURRENT scene scale (camera Distance/ReferenceLineRadius), not a
            // single fixed value wide enough to cover every radius the "Radius (km)" slider allows
            // regardless of its actual current setting (the old 0.1..2e9 constant, sized for a 100,000 km
            // planet, was ~7 orders of magnitude wider than this tool's own 400 km default ever needs).
            // Standard (non-reversed) depth buffers pack almost all their usable precision close to the
            // near plane and starve it near the far plane - a near/far ratio that huge left nowhere near
            // enough precision at the planet's actual surface to tell the Ocean Overlay's sea-level shell
            // apart from land only a few hundred meters above/below it, which read as per-pixel flickering
            // speckle (confirmed via screenshot the moment Ocean Overlay was added - every single previous
            // feature in this file only ever needed ONE surface visible at a given pixel, so this exact
            // failure mode had nothing to expose it before now). Recomputed every frame (not just once)
            // since Distance changes continuously while orbiting/zooming and ReferenceLineRadius changes
            // with Radius/Height Exaggeration/the baked terrain itself. Both bounds keep generous (2x-10x)
            // margin past what's actually needed so normal zooming/panel use never clips the sphere away.
            const float NearPlane = std::max(Distance * 0.0005f, 0.05f);
            const float FarPlane = std::max((Distance + ReferenceLineRadius) * 2.0f, NearPlane * 10.0f);
            rlSetClipPlanes(NearPlane, FarPlane);
        }

        BeginDrawing();
        ClearBackground(Color{ 18, 18, 24, 255 });

        BeginMode3D(Camera);
        if (bGroundViewActive)
        {
            // WHITE tint - the per-vertex colors UpdateGroundPatchModel writes (base color already
            // Lambertian-shaded) carry the actual look; a non-white tint here would multiply on top
            // of that a second time.
            DrawModel(GroundPatchModel, { 0, 0, 0 }, 1.0f, WHITE);
            DrawGrid(40, GroundPatchSizeMeters / 40.0f); // a visible scale reference underfoot - "how big is a meter here"
        }
        else
        {
        DrawModel(PreviewModel, { 0, 0, 0 }, 1.0f, WHITE);

        // Ocean overlay - a translucent shell at exactly sea level (Viz.PreviewRadiusMeters, the same
        // "Radius (km)" slider value BuildSphereMesh itself adds Height to - see SphereMesh.h). NOT
        // multiplied by Height Exaggeration: sea level is always height=0 by this tool's own
        // convention (ContinentLayer.h: "Height in meters above/below sea level"), and 0 exaggerated
        // by any factor is still 0 - so the overlay's radius has to stay exactly PreviewRadiusMeters
        // regardless of the Exaggeration slider, or it would drift away from the terrain mesh's own
        // true waterline the moment Exaggeration != 1.
        //
        // Built the SAME way PreviewModel itself is (CreatePreviewModel's own static-topology-once/
        // UpdateMeshBuffer-per-frame pattern, world-space positions baked directly into the vertex
        // buffer) rather than a raylib GenMeshSphere scaled via DrawModel's own `scale` parameter - an
        // earlier version tried that and, even fully opaque, it rendered in front of the terrain
        // UNCONDITIONALLY regardless of draw order or actual radius (confirmed via screenshot: a
        // sphere drawn at HALF PreviewRadiusMeters, entirely inside the terrain and so geometrically
        // impossible to see from outside it, still rendered as a solid circle covering everything).
        // Never fully root-caused (something about this environment's software GL rasterizer and a
        // DrawModel `scale` factor applied via the transform matrix to a tiny 1.0-radius mesh, rather
        // than real-world-scale positions baked into the vertex buffer itself, the way every other
        // mesh in this file already works) - side-stepped by reusing the one pattern already proven
        // reliable here instead of chasing the exact mechanism further.
        if (Viz.bShowOceanOverlay)
        {
            UpdateOceanModel(OceanModel, OceanGridWidth, OceanGridHeight, Viz.PreviewRadiusMeters);
            DrawModel(OceanModel, { 0, 0, 0 }, 1.0f, Color{ 50, 110, 190, 110 });
        }

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
        } // !bGroundViewActive

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
            // Radius USED to be visualization-only (no generation math read it) - no longer true since
            // Pipeline.Context.RadiusMeters feeds any WavelengthMeters-authored noise node's actual
            // sampling Frequency (see FPipelineContext::RadiusMeters' own comment and the Ground Detail
            // node this file adds by default). It's still ALSO the actual planet radius the preview
            // sphere is built at - BuildSphereMesh displaces real vertices by HeightMeters against this
            // exact RadiusMeters. Edited in km for a less awkward slider range than raw meters would
            // give (Earth is ~6371 km; range covers small moons through gas-giant-scale bodies).
            float RadiusKm = Viz.PreviewRadiusMeters / 1000.0f;
            bool bRadiusDirty = false;
            SliderFloat("Radius (km)", RadiusKm, 1.0f, 100000.0f, &bRadiusDirty);
            if (bRadiusDirty)
            {
                const float OldRadius = Viz.PreviewRadiusMeters;
                Viz.PreviewRadiusMeters = RadiusKm * 1000.0f;
                Pipeline.Context.RadiusMeters = (double)Viz.PreviewRadiusMeters;
                // Keep the camera's relative zoom level instead of leaving Distance fixed - a 10x
                // radius change with Distance untouched would otherwise either strand the camera deep
                // inside the new sphere or leave the old one looking like a speck.
                Distance *= Viz.PreviewRadiusMeters / OldRadius;
                // BuildSphereMesh bakes RadiusMeters directly into vertex positions - without also
                // refreshing the mesh buffer, the sphere stays sized at whatever radius it was last
                // baked at (confirmed via Xvfb: the preview shrank to a barely-visible speck after a
                // large Radius change, because only the CAMERA had moved to match the new scale while
                // the actual mesh verts were untouched). Now ALSO needs a full Pipeline rebake (not
                // just UpdatePreviewModel) - Pipeline.Context.RadiusMeters just changed above, and any
                // WavelengthMeters-authored noise node's actual sampling Frequency depends on it, so
                // the height field itself is stale until RebakeBase() re-runs the whole pipeline.
                bNeedsRebuild = true;
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
            // Plate boundaries are otherwise an exact spherical-Voronoi nearest-seed comparison - dead
            // straight/polygonal-looking at a glance (confirmed via this same GUI - see ROADMAP.md's
            // Mars-test entry). These warp the direction used for that comparison only, same domain-
            // warp technique as Radial Seeds' own (CLI-only) CoastlineWarpStrength/Frequency - see
            // PlateBoundaryWarpStrength's own header comment in ContinentLayer.h.
            SliderFloat("Boundary Warp Strength", ContinentNodeRaw->Config.PlateBoundaryWarpStrength, 0.0f, 2.0f, &bNeedsRebuild);
            SliderFloat("Boundary Warp Freq", ContinentNodeRaw->Config.PlateBoundaryWarpFrequency, 0.5f, 20.0f, &bNeedsRebuild);
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

        SectionLabel("Shape Stamp");
        {
            // GUI only ever edits Stamps[0] - see its construction comment above for why that's
            // consistent with how this file already treats Noise (CLI array, GUI exposes one).
            FShapeStamp& Stamp = ShapeStampNodeRaw->Stamps[0];

            // GuiCheckBox's own int return is dead code too (same family of bug as GuiSlider/
            // GuiToggleGroup documented below) - compare *bound value* before/after instead.
            const bool bWasEnabled = Stamp.bEnabled;
            GuiCheckBox(Rectangle{ 10, (float)Y, 20, 20 }, "Enabled", &Stamp.bEnabled);
            if (Stamp.bEnabled != bWasEnabled) bNeedsRebuild = true;
            Y += 26;

            int32_t ArchetypeIndex = (int32_t)Stamp.Archetype;
            const int32_t OldArchetypeIndex = ArchetypeIndex;
            GuiToggleGroup(Rectangle{ 10, (float)Y, (PanelWidth - 20.0f) / 4.0f, 20 }, "Mountain;Volcano;Crater;Canyon", &ArchetypeIndex);
            if (ArchetypeIndex != OldArchetypeIndex)
            {
                Stamp.Archetype = (EShapeStampArchetype)ArchetypeIndex;
                bNeedsRebuild = true;
            }
            Y += 26;

            SliderFloat("Latitude (deg)", Stamp.LatitudeDegrees, -90.0f, 90.0f, &bNeedsRebuild);
            SliderFloat("Longitude (deg)", Stamp.LongitudeDegrees, -180.0f, 180.0f, &bNeedsRebuild);
            SliderFloat("Radius (deg)", Stamp.RadiusDegrees, 1.0f, 60.0f, &bNeedsRebuild);
            SliderFloat("Height (m)", Stamp.HeightMeters, 0.0f, 5000.0f, &bNeedsRebuild);

            if (Stamp.Archetype == EShapeStampArchetype::Volcano || Stamp.Archetype == EShapeStampArchetype::Crater)
            {
                SliderFloat("Crater Depth Frac", Stamp.CraterDepthFraction, 0.0f, 1.0f, &bNeedsRebuild);
            }
            if (Stamp.Archetype == EShapeStampArchetype::Canyon)
            {
                SliderFloat("Length (deg)", Stamp.LengthDegrees, 1.0f, 90.0f, &bNeedsRebuild);
                SliderFloat("Rotation (deg)", Stamp.RotationDegrees, 0.0f, 360.0f, &bNeedsRebuild);
            }

            int32_t StampNoiseTypeIndex = (int32_t)Stamp.NoiseType;
            const int32_t OldStampNoiseTypeIndex = StampNoiseTypeIndex;
            GuiToggleGroup(Rectangle{ 10, (float)Y, (PanelWidth - 20.0f) / 5.0f, 20 }, "Perlin;Ridged;Billow;Value;Voronoi", &StampNoiseTypeIndex);
            if (StampNoiseTypeIndex != OldStampNoiseTypeIndex)
            {
                Stamp.NoiseType = (ENoiseType)StampNoiseTypeIndex;
                bNeedsRebuild = true;
            }
            Y += 26;
            SliderFloat("Stamp Noise (m)", Stamp.NoiseStrengthMeters, 0.0f, 1000.0f, &bNeedsRebuild);
            SliderFloat("Stamp Noise Freq", Stamp.NoiseFrequency, 0.5f, 20.0f, &bNeedsRebuild);
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

        // Ground Detail - DetailNoiseNodeRaw, a separate Noise node from NoiseNodeRaw above (see that
        // node's own construction comment for why: hill/mountain-scale relief and walking-scale ground
        // texture are different concerns that would fight each other sharing one noise field). Always
        // authored in real-world WavelengthMeters, never exposed as a raw Frequency slider the way
        // Secondary Noise above is - a 0.5-20 "cycles around the sphere" range can't reach a few-meter
        // wavelength on a planet-sized body anyway (see FNoiseNodeConfig::WavelengthMeters' own
        // comment), so there is no sane bWavelengthInMeters=false mode to expose here.
        SectionLabel("Ground Detail");
        {
            int32_t DetailNoiseTypeIndex = (int32_t)DetailNoiseNodeRaw->Config.Type;
            const int32_t OldDetailNoiseTypeIndex = DetailNoiseTypeIndex;
            GuiToggleGroup(Rectangle{ 10, (float)Y, (PanelWidth - 20.0f) / 5.0f, 20 }, "Perlin;Ridged;Billow;Value;Voronoi", &DetailNoiseTypeIndex);
            if (DetailNoiseTypeIndex != OldDetailNoiseTypeIndex)
            {
                DetailNoiseNodeRaw->Config.Type = (ENoiseType)DetailNoiseTypeIndex;
                bNeedsRebuild = true;
            }
            Y += 26;
        }
        SliderFloat("Detail Amplitude (m)", DetailNoiseNodeRaw->Config.AmplitudeMeters, 0.0f, 5.0f, &bNeedsRebuild);
        SliderFloat("Detail Wavelength (m)", DetailNoiseNodeRaw->Config.WavelengthMeters, 0.1f, 50.0f, &bNeedsRebuild);
        SliderInt("Detail Octaves", DetailNoiseNodeRaw->Config.Octaves, 1, 6, &bNeedsRebuild);

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
        // Pure draw-time visualization flag read directly every frame (see its draw call above) - no
        // rebake/refresh needed, unlike most other controls here, so this just binds straight to it.
        GuiCheckBox(Rectangle{ 10, (float)Y, 20, 20 }, "Ocean Overlay (sea level)", &Viz.bShowOceanOverlay);
        Y += 26;

        // Ground View - see GroundPatchMesh.h's own top comment and RebuildGroundPatch's construction
        // comment for why this exists as a wholly separate camera/mesh/clip-plane path rather than
        // just zooming the orbit camera all the way in: the whole-planet bake grid behind PreviewModel
        // is many kilometers between samples, so there is nothing to zoom INTO - the mesh itself has
        // no vertices closer together than that, regardless of camera distance.
        SectionLabel("Ground View");
        SliderFloat("Stand At Lat (deg)", GroundViewLatitudeDegrees, -90.0f, 90.0f, &bGroundPatchDirty);
        SliderFloat("Stand At Long (deg)", GroundViewLongitudeDegrees, -180.0f, 180.0f, &bGroundPatchDirty);
        if (GuiButton(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 26 }, bGroundViewActive ? "Back To Orbit View" : "Enter Ground View"))
        {
            bGroundViewActive = !bGroundViewActive;
            if (bGroundViewActive)
            {
                // Always face back toward the planet's center on entry - an arbitrary but
                // deterministic starting look direction, rather than whatever stale Yaw/Pitch was
                // left over from the last time Ground View was active (or 0/0 looking at the
                // horizon, equally arbitrary but less immediately orienting).
                GroundViewYaw = 0.0f;
                GroundViewPitch = -0.3f;
                bGroundPatchDirty = true;
            }
        }
        Y += 30;

        if (GuiButton(Rectangle{ 10, (float)Y, PanelWidth - 20.0f, 26 }, "Regenerate (discards erosion)") || bNeedsRebuild)
        {
            RebakeBase();
            bNeedsRebuild = false;
            bNeedsClimateRefresh = false;
            bGroundPatchDirty = true; // the coarse baseline Ground View reads (BakedTerrain) just changed
        }
        Y += 30;

        if (bNeedsClimateRefresh)
        {
            RecomputeClimateAndRefresh();
            bNeedsClimateRefresh = false;
            bGroundPatchDirty = true; // covers Radius/Height Exaggeration/erosion, which also land here
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
    UnloadModel(OceanModel);
    CloseWindow();
    return 0;
}
