#include "catalyst/common.hpp"
namespace catalyst {
namespace {
constexpr char official[] = "https://docs.fivem.net/docs/catalyst/";
struct Topic {
  const char *name, *summary, *text;
};
// The guide covers only what is specific to driving Catalyst through these tools. The editor itself is
// documented by Cfx.re, and every topic sends the reader there.
constexpr Topic topics[] = {
    {"overview", "official documentation links, the recommended workflow and the topic list",
     R"md(# Catalyst MCP

This server drives Catalyst, the FiveM particle effect editor, from inside the running editor.

## Official documentation

Catalyst is documented by Cfx.re. Read these pages for the editor, the effect hierarchy and FiveM integration; this guide only adds what is specific to the MCP tools.

- Introduction and setup: https://docs.fivem.net/docs/catalyst/
- Editor overview and how effects are built: https://docs.fivem.net/docs/catalyst/overview/
- Example project, export and use in FiveM: https://docs.fivem.net/docs/catalyst/example-project/

## Workflow

1. `catalyst_status`: `editor_ready` must be true. `ui_notices` lists startup dialogs only the user can close; rendering needs `preview_ready`.
2. `catalyst_new_project` or `catalyst_open_project`. Both replace the open project, so save anything worth keeping first.
3. `catalyst_create_collection`, then `catalyst_create_particle`: a starting effect of eight connected nodes. It returns the `view_id` and the node ids.
4. `catalyst_add_texture` with a DDS file, then set the Sprite Shader's `DiffuseTex2` to the texture name, which is the file name without its extension.
5. `catalyst_get_node_template` for every node type you will use: exact property names, defaults, curves and ports.
6. Build the graph with one `catalyst_edit_graph` call. Every single-edit tool reloads the whole project.
7. `catalyst_set_viewport_mode` with `preview`, then `catalyst_render_particle` to look at the result; `catalyst_get_project` reads the graph back.
8. `catalyst_save_project`, then `catalyst_export_ypt`.
)md"},
    {"graph", "node hierarchy, port names, node ids and batch edits",
     R"md(# Graph

One effect is one view. A connection runs from a child's output port, always named `output`, to a named input of its parent:

```
ParticleEffectRule                    the effect: name, durationMin/durationMax in seconds, numLoops
  eventEmitter <- ParticleEventEmitter    when a layer plays: startRatio, endRatio, colourTintMin/Max
    emitterRule <- ParticleEmitterRule      how it spawns: spawnRateOverTime, particleLife, speedScalar, isOneShot
      "Creation Domain" <- ParticleDomain     where particles appear
      "Target Domain"   <- ParticleDomain     where they are aimed
    particleRule <- ParticleRule            what a particle is: blendSet, depthTest, cullMode
      draw <- ParticleDrawSprite
        shader <- SpriteShader                technique, DiffuseTex2
      colourBehaviour <- ParticleBehaviorColor
      sizeBehaviour   <- ParticleBehaviorSize
      accelerationBehaviour, dampeningBehaviour, noiseBehaviour, lightBehaviour, ...
```

- The effect accepts many event emitters, and one Particle Rule can feed many event emitters. Sharing looks this way keeps large effects small.
- `startRatio` and `endRatio` are fractions of the effect duration, not seconds.
- `catalyst_list_node_types` lists every node type; `catalyst_get_node_template` gives a type's inputs (with `multiple`), outputs, properties (`v`) and curves (`k`).
- A property whose template value is null is an input or a curve: connect a node or use `catalyst_set_curve`, not `catalyst_set_node_properties`.
- Property types are checked: integers stay integers, numbers must be finite, unknown names are rejected.
- A connection needs matching port types. A single-value input rejects a second connection, and cycles are rejected.
- Node ids are numbers assigned by the server. Node display names may not contain `/`, `\` or `:`.

## Evolutions

An evolution is a named slider from 0 to 1 that a game script drives. At 1 it replaces chosen animated properties with alternate curves; in between it blends towards them.

- Place a `ParticleEvolution` node, give it a `name`, and connect its output to the Effect Rule's `evolution` input (a slot of the whole effect) or to an Event Emitter's `evolutions` input (a slot of that event). Catalyst's documentation allows five custom slots besides the built-in LOD one.
- `catalyst_set_evolution_properties` chooses what the slot overrides. A property is named by its RAGE keyframe id: the class, a colon, and the member ending in `KFP`, for example `ptxEffectRule:m_colourTintMinKFP`, `ptxu_Colour:m_rgbaMinKFP`, `ptxu_Size:m_whdMinKFP` or `ptxEmitterRule:m_spawnRateOverTimeKFP`.
- The call reloads the project and returns the node, which now has one curve per property under Catalyst's display name, such as `EffectRule: Color Tint Min`. Fill those with `catalyst_set_curve` in a following call; they do not exist earlier in the same batch.
- `previewValue` on the node sets the slider in the preview.
- To recolour a whole effect, override the Effect Rule's colour tint (both `m_colourTintMinKFP` and `m_colourTintMaxKFP`) and set `rgbCanTint` on every Color behaviour. A tint curve runs over the effect's life, so one slot can also change colour over time.
- With `blend_mode` 0, every slot whose slider is above zero takes an equal share of the result, however small the value. Two slots at 1 give the average of their curves; cross-fading two sliders goes through a washed-out middle. Other blend modes have not been tried.
- Evolutions survive YPT export and import.
- A script can set an evolution only on a looped effect: `StartParticleFxLoopedAtCoord(...)` returns a handle, `SetParticleFxLoopedEvolution(handle, name, amount, false)` sets the slider, `StopParticleFxLooped(handle, false)` ends it. These come from the FiveM native reference; we have not run them.

## Batch edits

`catalyst_edit_graph` takes `operations`, each `{"op": ..., ...}` with the arguments of the matching single tool: `place_nodes`, `connect_nodes`, `disconnect_nodes`, `set_node_properties`, `move_nodes`, `delete_nodes`, `set_curve`, `set_sprite_animation`, `set_evolution_properties`. Give a new node a `ref` in `place_nodes` and use that string wherever a later operation expects a node id. Operations run in order; if one fails nothing is changed and the error names it.
)md"},
    {"properties", "what the main properties and curves mean",
     R"md(# Properties and curves

These are serialized names from the node templates; labels in the editor can differ. The behaviour below was observed in Catalyst 1.0.0.30 and 1.0.0.31 while authoring the example effects. Check a template's defaults when in doubt.

## Curves

- `catalyst_set_curve` replaces a whole curve. Keyframe times are 0..1 and strictly increasing; a value has one to four components. One keyframe at time 0 is a constant.
- Behaviour curves (colour, size, acceleration, ...) run over the particle's life.
- Particle Domain curves (`position`, `sizeOuter`, `sizeInner`, `rotation`) run over the event emitter's own window, `startRatio`..`endRatio`. Keyframing `position` moves an emitter while it spawns.
- Min/Max pairs such as `rgbaMin`/`rgbaMax` and `whdMin`/`whdMax` give each particle its own random blend between the two curves.

## Emitter Rule

- `spawnRateOverTime`: particles per second while the event plays. With `isOneShot` true the event emits that many particles once.
- `particleLife`: minimum and maximum life in seconds.
- `speedScalar`: minimum and maximum multiplier of the launch vector.
- `sizeScalar`: percent; 100 leaves the size unchanged.

## Launch direction and speed

A particle starts at a random point of the Creation Domain and is launched along (a random point of the Target Domain minus its start point), multiplied by `speedScalar`. The vector is not normalised.

- A thin spherical target shell of radius 1 around the origin: every direction, speed equal to `speedScalar`.
- A target domain offset from the origin: a cone towards it.
- A flat cylinder as the target: a disc.
- `speedScalar` 0: particles stay where they spawn.

## Particle Domain

- `shape`: 0 box, 1 sphere, 2 cylinder.
- `position` is relative to the effect origin; +Z is up.
- `sizeOuter` holds half-extents for a box and radii otherwise. `sizeInner` hollows the shape: an inner size of 0.9 of the outer one leaves a thin shell.

## Look

- Sprite Shader: `technique` and `DiffuseTex2`, the texture name. The default technique `RGBA_lit_soft` is lit by the scene and fades where a sprite meets geometry; `RGBA_lit` is the same without the fade.
- Particle Rule `blendSet`: 0 normal blending, 1 additive.
- Sprite Draw `alignmentMode`: 0 faces the camera, 1 aligns the sprite with its velocity, the top of the texture leading. Velocity-aligned sprites are back-face culled about half of the time unless the Particle Rule's `cullMode` is 0.
- Color behaviour: `rgbaMin`/`rgbaMax` are RGBA 0..1; `rgbaMax` applies only with `rgbaMaxEnable`. `emissiveIntensity` brightens.
- `rgbCanTint` on the Color behaviour lets the event emitter's `colourTintMin`/`colourTintMax` (r, g, b, a as 0..255) recolour the particle, so one Particle Rule can serve many differently coloured events.
- Size behaviour: `whdMin`/`whdMax` are width, height and depth.
- Acceleration behaviour: `accelMin`/`accelMax` are x, y, z over the particle's life; (0, 0, -9.8) is gravity. Set `enableGravity` false when you supply it yourself.
- Dampening behaviour: `dampeningMin`/`dampeningMax` are per-axis drag.

## Normal maps

- Put a normal map in the Sprite Shader's `NormalSpecMap` and set `technique` to `RGBA_lit_soft_normalspec`. With plain `RGBA_lit_soft` the map is ignored and the sprite is shaded as a fake sphere.
- In a test under the preview light, the half of a map tilted up in the green channel was the brighter one: green up, as in OpenGL-style maps.
- `WrapLightingTerm` above 0 keeps the shaded side from going black.
- The technique name is written to the exported YPT. Catalyst's own YPT import reads it back as `RGBA_lit_soft`, so a reimported effect looks flatter and greyer than the project it came from. Which look the game gives has not been checked.

## Texture atlases

- `catalyst_create_clip_regions` splits a texture into columns and rows of frames.
- Sprite Draw `texFrameIdMin`/`texFrameIdMax` give each particle a random frame in that range; `flipChanceU` 0.5 mirrors half of them. `catalyst_set_sprite_animation` plays frames as a flipbook instead.
- Frames are not part of the YPT. See the `export` topic before relying on an atlas.
)md"},
    {"preview", "viewport modes, rendering, the camera and what the preview cannot show",
     R"md(# Preview and rendering

## Viewport mode

- After a start the viewport is in `editor` mode: a grid and the outlines of every domain drawn over the effect.
- `catalyst_set_viewport_mode` with `preview` shows the effect as the game draws it, over a sky and a ground plane at z = 0. Switch before rendering. `catalyst_status` reports `viewport_mode`.
- `wireframe` shows only the domain outlines; `simulation` shows the outlines and each particle as a dot.
- The dropdown in the corner of the viewport keeps its old label after the tool switches the mode.

## Rendering

- `catalyst_render_particle` seeks the preview and returns a PNG of the whole Catalyst window, editor chrome included. Crop the viewport yourself. The status bar at the bottom right of the capture shows the number of live particles.
- The window must not be minimised. A startup or error dialog makes the call fail; `catalyst_status` lists them in `ui_notices`, and only the user can close them.
- `position` is 0..1. With `duration` in seconds the tool seeks in simulation mode to `position * duration`, so pass the effect's duration to address a moment in seconds. `image: false` seeks without capturing.
- Rendering pauses playback. Graph edits reload the project and reset selection, playback and undo history.
- The preview backdrop we saw is a daytime sky, which washes out additive effects. To inspect one, put a large dark sprite with normal blending behind it as the first event emitter, and keep that out of the project you export.
- Effects spawned by another effect (Particle Effect Spawner) did not appear in the preview in our tests.
- Particles are spawned once per simulation step, so a fast-moving emitter leaves gaps at a low frame rate.

## Camera

- The camera is stored per view and returned by `catalyst_get_project`: `cy` yaw and `cp` pitch in radians, `cd` distance, `tx`/`ty`/`tz` target. With `cy` at -pi/2 the camera looks along +Y with +X to the right.
- No tool sets it. Ask the user to frame the effect.
- Catalyst reads these fields when it first shows a tab and keeps the camera of a tab that is already open, even across a project reload. A project file written with new camera values therefore changes nothing until the effect is loaded once under another name and then under its own.
)md"},
    {"export", "saving projects, YPT export and using the result in FiveM",
     R"md(# Saving, export and FiveM

- Paths are absolute and the destination directory must already exist.
- `catalyst_save_project` writes a `.ctlst` and makes that path the current project. The file is a magic, two length-prefixed resource blobs and the document; Catalyst 1.0.0.31 saves the document in a binary form (`CTLSTB`) and also loads the JSON form (`CTLSTJ`).
- Once Catalyst has been pointed at the game, a saved project also carries the editor's library textures: ours grew from 0.5 MB to over 13 MB. Do not publish such a file.
- `catalyst_export_ypt` writes every collection checked for export as `<collection>_legacy.ypt` and/or `<collection>_enhanced.ypt`. Only the collection's own textures go into it.
- `catalyst_import_ypt` reads either format back into the current project, which makes a quick round-trip check of an export. Two things do not survive it: atlas frames and the normal-map technique (see `properties`).

## Atlas frames and ptxclipregions.dat

- A YPT does not hold atlas frames. The game reads them for every particle texture from one global `ptxclipregions.dat`, which is replaced as a whole. Without an entry each sprite shows the entire atlas; a reimported YPT looks like that until `catalyst_import_clip_regions` is called.
- `catalyst_export_clip_regions` writes that file. With Catalyst pointed at the game it holds the game's entries plus the project's, which is what a replacement must contain.
- A FiveM resource ships it with `files { 'ptxclipregions.dat' }` and `data_file 'PTXCLIPREGIONS_FILE' 'ptxclipregions.dat'`. That data file type comes from pull request 4220 of citizenfx/fivem, which was still open when this was written, and its code is for the legacy game only. Check that the target client has it.
- An effect that must work everywhere should use one texture per frame instead of an atlas.

## In FiveM

The official walkthrough is https://docs.fivem.net/docs/catalyst/example-project/#4-implementing-in-fivem

- Put the Legacy file in a resource's `stream/` folder and the Enhanced file in `stream_enhanced/`. The file name without `.ypt` is the particle dictionary name.
- The effect name is the name of the effect in Catalyst.
- To play it from a client script: `RequestNamedPtfxAsset(dictionary)`, wait for `HasNamedPtfxAssetLoaded(dictionary)`, `UseParticleFxAsset(dictionary)`, then `StartParticleFxNonLoopedAtCoord(effect, x, y, z, rotX, rotY, rotZ, scale, false, false, false)`. Only the calling client sees the effect.
- FiveM documents `StartNetworkedParticleFxNonLoopedAtCoord` for an effect every nearby player sees, and `SetParticleFxNonLoopedColour(r, g, b)` to tint the next effect, which needs `rgbCanTint` on its Color behaviours. Both are from the native reference; we have not run them.
)md"},
};
} // namespace
Json documentation_topics() {
  Json names = Json::array();
  for (const auto &topic : topics)
    names.push_back(topic.name);
  return names;
}
Json documentation(const Json &args) {
  const auto wanted = args.value("topic", std::string("overview"));
  std::string text, others;
  for (const auto &topic : topics) {
    if (wanted == topic.name)
      text = topic.text;
    else
      others += std::string("- `") + topic.name + "`: " + topic.summary + "\n";
  }
  if (text.empty())
    throw ToolError("Unknown topic. Available topics: " + documentation_topics().dump());
  text += "\n## More\n\nOther topics, as `catalyst_get_docs {\"topic\": ...}`:\n\n" + others +
          "\nOfficial Catalyst documentation: " + official + "\n";
  return {{"_mcp_content", Json::array({{{"type", "text"}, {"text", text}}})}};
}
} // namespace catalyst
