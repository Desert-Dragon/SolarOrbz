# Working notes for Claude Code on this repo

## Engine target: UE5.8

The user builds this project against **UE5.8** specifically (not "UE5" generically).
Write all documentation — testing/playtest guides, design docs, Artifacts, editor
instructions — oriented to that exact version:

- State editor steps, node names, and menu paths as fact for UE5.8, not hedged
  ("if your version has this node"). Verify a specific claim (e.g. "this node
  exists," "this API's signature is...") with a web search before writing it as
  fact if not already confirmed earlier in the session, rather than guessing or
  carrying a generic cross-version hedge forward out of caution.
- If something genuinely did change across versions in a way that matters (a node
  renamed, an API deprecated), say so explicitly and say what UE5.8 actually uses
  — don't silently write the old/generic version.
- This doesn't block writing something that's also true of other engine versions;
  it just means don't hedge or genericize when a UE5.8-specific fact is checkable.

## Subsystem documentation gets mirrored as an Artifact

Whenever a substantive subsystem doc gets created or meaningfully updated —
design docs, testing/playtest runbooks, build checklists (the kind of thing
under `Docs/`, not every file in the repo) — also publish or update a
corresponding Artifact version, not just the committed Markdown.

- The Markdown in the repo stays the source of truth (git history, diffs in
  review, other tooling that reads the repo). The Artifact is a secondary,
  properly designed mirror, not a replacement — don't stop committing the
  real doc.
- Load the `artifact-design` skill before writing/updating the page. Convert
  the doc's content into an actual designed page (tables, diagrams, status
  chips, whatever the content calls for) rather than transcribing the
  Markdown one-to-one into an HTML wrapper.
- When updating an already-mirrored doc, republish to the **same** Artifact
  URL (pass `url`) so the link stays stable — don't create a new Artifact
  for the same doc. See the mapping below; keep it current when you publish
  a new one.
- Default scope: this applies to standalone docs meant to be read as a
  whole (design docs, runbooks, checklists). It does NOT automatically apply
  to `ROADMAP.md`, which changes on nearly every commit as a running log —
  mirror it only if asked. If this scoping call is wrong, the user will say
  so; adjust from there.

### Current doc → Artifact mapping

| Doc | Artifact URL |
|---|---|
| `Docs/ChunkedPlanetTerrain.md` (full design doc) | https://claude.ai/artifact/N9fHhKzhCV5ZPbFK6s85G3 (Chunked Planet Terrain Design) |
| `Docs/ChunkedPlanetTerrain.md`'s "Phase 1, continued" checklist (companion interactive view) | https://claude.ai/artifact/WW2MjJu1MVtQFewq4cwbFr (Streaming Manager Checklist) |
| `Docs/ChunkedPlanetTerrain_TestingGuide.md` | https://claude.ai/artifact/26AT6jRtgk3v3o7qa3cZUv (Chunked Terrain Playtest Runbook) |
| `Docs/SolarOrbzTerrainGraphEditor.md` | https://claude.ai/artifact/79qFnMtpN95TsvT9iZxCgj (Terrain Graph Editor Design) |
| `Docs/SolarOrbzTerrainGraphEditor_UsageGuide.md` | https://claude.ai/artifact/BprPEaMNDusrRVAYzhsu27 (Terrain Graph Editor Usage Guide) |

Add a row here whenever a new subsystem doc gets its own Artifact mirror. A doc can have more than
one companion Artifact (e.g. a full design-doc mirror plus a separate interactive checklist built
from one section of it) — list each.
