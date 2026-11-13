# Catalyst MCP

A Windows x64 C++ MCP server embedded in a `version.dll` proxy plugin for [Catalyst](https://docs.fivem.net/docs/catalyst/), the FiveM particle effect editor. It lets an MCP client, such as an AI agent, create, edit, preview and export particle effects in the running editor. The plugin forwards the Win32 version APIs to the real System32 DLL and runs an authenticated, loopback-only Streamable HTTP endpoint inside Catalyst.


This is an unofficial project; see the [disclaimer](#disclaimer).

Reference executable: **Catalyst 1.0.0.30**, SHA-256 `76b89d30a8874ca3c82543a9c6f0bf347ab56e1aa34606cf910bce4359afa1f7`.

The bridge discovers 35 native symbols with wildcard byte patterns and validates 38 object-layout/vtable operands before installing hooks. SHA-256 is diagnostic metadata, not a compatibility gate. A different executable can run the bridge when every signature and layout check passes; with a missing or ambiguous signature the MCP server does not start. Actual future Catalyst releases still require validation because matching machine code cannot prove every ABI or behavior contract.

Verified with Catalyst **1.0.0.30 and 1.0.0.31**, including 49 native checks in an isolated 1.0.0.31 process.

## Build

Requirements: Visual Studio 2022 with Desktop development with C++, a Windows 10/11 SDK containing C++/WinRT, and CMake 3.24 or later. Dependencies are vendored; building requires no network access.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release -j 6
```

Output: `build/Release/version.dll`. Use Release builds with the supported release Catalyst executable. The plugin uses the MSVC release ABI at the native boundary.

## Install and connect

1. Close Catalyst and copy `version.dll` beside `Catalyst.exe`. Keep any existing proxy DLL separately; do not overwrite another plugin.
2. Start Catalyst. Complete Catalyst's first-run dialogs yourself, including the telemetry choice and license agreement. The MCP server stays off until you have accepted the license agreement and starts once you do. Locate the game or dismiss the game-location dialog if you will use your own assets.
3. The plugin creates `catalyst-mcp.json`, `catalyst-mcp.log`, and `catalyst-mcp-patterns.json` beside its DLL. The configuration contains a port and a randomly generated bearer token. Keep the token private. The pattern report contains resolution timings, symbols, layout checks, and rejection diagnostics.
4. Connect an MCP client to `http://127.0.0.1:13338/mcp` and supply `Authorization: Bearer <token from catalyst-mcp.json>`.
5. Call `catalyst_status`, then create or open a project. `catalyst_get_docs` returns the workflow and what the properties mean; Catalyst itself is documented at [docs.fivem.net/docs/catalyst](https://docs.fivem.net/docs/catalyst/).

The port can be changed in `catalyst-mcp.json` while Catalyst is closed. Set the environment variable `CATALYST_MCP_DISABLE=1` to disable server startup while retaining version-API forwarding. Remove `version.dll` while Catalyst is closed to uninstall the plugin. No injector, administrator account, IDA installation, or external server is required at runtime.

The endpoint supports MCP protocol versions `2025-03-26`, `2025-06-18`, and `2025-11-25`. It implements the JSON response form of Streamable HTTP. GET and DELETE return 405; there is no legacy `/sse` endpoint. POST must include both `application/json` and `text/event-stream` in `Accept`. Sessions are stateless. This follows the [MCP transport specification](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports).

## Tools

| Tools | Purpose |
| --- | --- |
| `catalyst_status` | Executable identity, pattern-resolution report, bridge readiness, and startup dialogs that cover the preview |
| `catalyst_get_docs` | Built-in authoring guide by topic (`overview`, `graph`, `properties`, `preview`, `export`), with links to the official Catalyst documentation |
| `catalyst_new_project`, `catalyst_open_project`, `catalyst_save_project` | Project lifecycle and native `.ctlst` serialization |
| `catalyst_create_collection` | Create a named collection |
| `catalyst_create_particle` | Build an eight-node sprite effect with seven typed connections |
| `catalyst_get_project` | Inspect views, nodes, properties, curves, connections, asset ownership, and clip regions |
| `catalyst_list_node_types`, `catalyst_get_node_template` | Discover all 32 native factories, defaults, named ports, and port types |
| `catalyst_place_nodes`, `catalyst_move_nodes`, `catalyst_delete_nodes` | Edit graph layout and contents |
| `catalyst_connect_nodes`, `catalyst_disconnect_nodes` | Edit typed connections; reject self-links, cycles, and occupied single inputs |
| `catalyst_set_node_properties` | Merge typed scalar, enum, string, boolean, and vector properties |
| `catalyst_set_curve` | Replace a normalized animation curve with ordered keyframes |
| `catalyst_edit_graph` | Apply many of the graph edits above as one transaction with a single project reload |
| `catalyst_add_texture`, `catalyst_add_drawable` | Import DDS textures and YDR drawables into a collection |
| `catalyst_import_ypt`, `catalyst_export_ypt` | Native Legacy/Enhanced import and export |
| `catalyst_create_clip_regions`, `catalyst_import_clip_regions`, `catalyst_export_clip_regions` | Sprite atlas grids and `ptxclipregions.dat` |
| `catalyst_set_sprite_animation` | Flipbook frame range, FPS, blending, hold, and lifetime matching |
| `catalyst_set_evolution_properties` | Choose the animated properties a Particle Evolution node overrides; Catalyst then gives the node one curve per property |
| `catalyst_set_viewport_mode` | Switch the viewport between `preview` (the effect as the game draws it), `editor`, `wireframe` and `simulation` |
| `catalyst_render_particle` | Seek to a normalized position and return a PNG of the Catalyst window containing the preview |

Paths must be absolute. Create destination directories before saving or exporting. Export uses collections checked for export and writes `<collection>_legacy.ypt` and/or `<collection>_enhanced.ypt`.

`catalyst_create_particle` creates an Effect Rule, Event Emitter, Emitter Rule, Particle Rule, Sprite Draw, Sprite Shader, Color Behavior, and Size Behavior. Assign a texture before expecting visible particles. Use node templates to discover exact property names and ports; UI labels can differ from serialized names.

## Example workflow

Calls below show tool arguments, not raw JSON-RPC envelopes:

```json
{"tool":"catalyst_new_project","arguments":{}}
{"tool":"catalyst_create_collection","arguments":{"name":"my_effects.ypt"}}
{"tool":"catalyst_create_particle","arguments":{"name":"soft_smoke","collection_id":1}}
{"tool":"catalyst_add_texture","arguments":{"path":"C:/Effects/soft.dds","collection_id":1}}
{"tool":"catalyst_set_node_properties","arguments":{"view_id":1,"node_id":6,"properties":{"DiffuseTex2":"soft","technique":"RGBA_lit"}}}
{"tool":"catalyst_set_curve","arguments":{"view_id":1,"node_id":3,"property":"spawnRateOverTime","keyframes":[{"time":0,"value":[25,25,0]}]}}
{"tool":"catalyst_render_particle","arguments":{"view_id":1,"position":0.5}}
{"tool":"catalyst_save_project","arguments":{"path":"C:/Effects/soft_smoke.ctlst"}}
{"tool":"catalyst_export_ypt","arguments":{"directory":"C:/Effects/Export","legacy":true,"enhanced":true}}
```

Use IDs returned by creation tools rather than assuming the example IDs in an existing project. Curves use normalized times in `[0,1]`; each value has one to four components. Inspect the corresponding native default curve to learn component meaning and units. Many Catalyst size/scalar values use percentages.

### Batch edits

Every graph tool call serializes the project, changes it and reloads it, about 0.2 s on a 671-node effect. `catalyst_edit_graph` runs a list of operations against one copy of the project and reloads once; rebuilding that whole effect, 2,875 operations, took 0.4 s in one call:

```json
{"tool":"catalyst_edit_graph","arguments":{"view_id":1,"operations":[
  {"op":"place_nodes","nodes":[{"type":"ParticleBehaviorAcceleration","ref":"gravity","x":1050,"y":900}]},
  {"op":"set_curve","node_id":"gravity","property":"accelMin","keyframes":[{"time":0,"value":[0,0,-9.8]}]},
  {"op":"set_curve","node_id":"gravity","property":"accelMax","keyframes":[{"time":0,"value":[0,0,-9.8]}]},
  {"op":"connect_nodes","from_node":"gravity","from_port":"output","to_node":4,"to_port":"accelerationBehaviour"}
]}}
```

- `op` is `place_nodes`, `connect_nodes`, `disconnect_nodes`, `set_node_properties`, `move_nodes`, `delete_nodes`, `set_curve` or `set_sprite_animation`. The other keys are the arguments of the `catalyst_` tool of that name and pass the same validation. `view_id` defaults to the call's.
- A node in `place_nodes` may carry a `ref`, a name of your choice. Later operations in the same call accept it wherever a node id is expected. Existing nodes are addressed by number.
- Operations run in order. If one fails, the error names it (`operations[3] (connect_nodes): ...`) and the project is left unchanged.
- The result lists the new node ids in placement order (`placed`), the id behind every ref (`refs`) and the view's node and connection counts. A call takes up to 16,384 operations.

For atlas creation, columns/rows select the tiles. Each frame defaults to the native full-tile crop `(u_min,u_max,v_min,v_max) = (0,1,0,1)`. Optional `frames` entries can crop individual tiles, for example `{"index":0,"u_min":0.1,"u_max":0.9,"v_min":0.05,"v_max":0.95}`. These bounds are relative to a tile, not absolute atlas coordinates.

Rendering pauses playback. Without `duration`, the position is in Catalyst's normalized timeline mode. With a positive `duration` in seconds, the tool uses simulation mode and seeks to `position * duration`. `image:false` performs the seek without capturing. Windows Graphics Capture works with an occluded window, but Catalyst must be running and restored from the minimized state. Startup/error dialogs cause a tool error instead of returning an obscured preview as a successful particle render.

## Data and runtime behavior

- Editor operations are queued and executed on Catalyst's editor thread. The HTTP listener never directly edits the graph.
- Graph edits use Catalyst's own project serializer and loader, preserving embedded texture and drawable resource blobs. They rebuild the native objects and invalidate timeline caches. A recovery project is written to `mcp-cache/recovery.ctlst` before each graph reload.
- Reloads reset selection, playback state, and native undo history. A temporary working project may appear in Catalyst's recent-files list. Save useful milestones explicitly. Separate tool calls are separate transactions; the operations of one `catalyst_edit_graph` call are a single transaction.
- The queue is bounded. An unstarted operation is cancelled after a 60-second wait. If a native operation has already started at timeout, the tool reports an unknown ongoing outcome; inspect the project before retrying it.
- The DLL stays mapped for the process lifetime. Shutdown occurs with Catalyst; hot unloading is unsupported.
- The HTTP server binds only to IPv4 loopback, checks Host and Origin, requires a bearer token, and limits payload and worker-queue sizes. It does not expose arbitrary memory access or arbitrary function calls.
- The HTTP server is not started until the running editor reports that Catalyst's license agreement is accepted, and tool calls are refused while that dialog is open. The plugin reads the dialog state and never changes it. When the native signatures do not match, the state cannot be read and the server does not start; `catalyst-mcp.log` and `catalyst-mcp-patterns.json` say why.

## Native binding

Native binding uses a bounded SSE2 pattern search over executable PE sections. Relative calls and RIP-relative data references are decoded, and AMD64 unwind metadata identifies function starts. All hooks are created before they are enabled; a hook failure rolls them back. Every Catalyst process performs a fresh scan at startup. Resolved addresses remain in an in-memory binding table for native calls; there is no persistent signature cache. The diagnostic JSON report is output only and is never loaded as an address source.

Drawable import has not been exercised with a real YDR asset.


## Dependencies

- [nlohmann/json 3.12.0](https://github.com/nlohmann/json/tree/v3.12.0), MIT.
- [cpp-httplib 0.28.0](https://github.com/yhirose/cpp-httplib/tree/v0.28.0), MIT.
- [MinHook 1.3.4](https://github.com/TsudaKageyu/minhook/tree/v1.3.4), BSD 2-Clause.
- Microsoft Windows SDK, including C++/WinRT, Direct3D 11, WIC, and Windows Graphics Capture.

## License

Catalyst MCP is released under the [MIT License](LICENSE). The vendored dependencies keep their own licenses; the texts are in `third_party/`.

## Disclaimer

This is an unofficial community project. It is not affiliated with, endorsed by, or supported by Cfx.re or Rockstar Games. Catalyst, FiveM and Grand Theft Auto are trademarks of their respective owners. The repository contains no Catalyst binaries and no game assets: you need your own Catalyst installation, and the example effects use only textures generated for this project.
