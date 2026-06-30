# DreamShader ChangeLog

## 1.4.1 - 2026-07-01

### Added

- Parameter input pins can be wired from the `Graph` with a call form, e.g. `Mask(Input = ...)` and `Tex(Coordinates = ...)`, for channel/component-mask and texture-sample parameters.
- `Docs/ParameterReference.md`: per-type declaration + metadata reference covering asset slots (`[Prop = Path(...)]`) and input pins for every parameter type.

### Fixed

- Decompiled multi-output Custom nodes emitted both `Output=` and `OutputIndex=` and failed to regenerate; the decompiler now emits a single output selector.
- `DynamicParameter`, `CurveAtlasRowParameter` inline defaults, and the texture-sample parameter family failed to generate or compile; they now produce valid nodes (texture samples seed a default texture).
- Import directives no longer shift diagnostic line/column numbers in multi-file sources.

### Changed

- De-duplicated internal JSON/SQLite editor helpers to remove a unity-build symbol-collision risk.

## 1.4.0 - 2026-06-06

### Compatibility

- Added Unreal Engine `5.3` through `5.7` compatibility coverage.
- Verified single-plugin `RunUAT BuildPlugin` builds for UE `5.3`, `5.4`, `5.5`, `5.6`, and `5.7` on Win64.

## 1.3.9 - 2026-05-29

### Maintenance

- Updated plugin version metadata and documentation references.

## 1.3.8 - 2026-05-25

### Texture Support

- Added `VolumeTexture` property parsing, code generation, and default texture handling.
- Preserved texture object subtypes during code generation so `Texture2D`, `Texture2DArray`, and `VolumeTexture` inputs are passed to generated HLSL with the correct Unreal texture type.

### Plugin Cleanup

- Removed built-in shader library path support from project settings and documentation.

## 1.3.7 - 2026-05-18

### Decompiler

- Added reflected literal property export for generic `UE.Expression(...)` decompilation so unsupported `MaterialExpression` nodes retain more editable state.
- Preserved connected `TextureSampleParameter2D` graph inputs such as UV coordinates by exporting connected sample parameters as graph expressions instead of plain `Properties` declarations.
- Fixed decompiled `MaterialExpressionCustom` imports with dynamic named inputs and custom output type metadata.

### Performance

- Improved import performance for very large decompiled materials by reducing per-node package dirtying, throttling progress text updates, and skipping automatic layout on large generated graphs.

## 1.3.6 - 2026-05-12

### Build Fixes

- Added an explicit `MaterialDomain.h` include to `DreamShaderSettings.h` so projects that include the settings header directly can resolve `EMaterialDomain` reliably.

## 1.3.5 - 2026-05-11

### ShaderFunction Calls

- Added statement-style multi-output `ShaderFunction` and `VirtualFunction` calls in `Graph`, using positional inputs followed by output target variables.

### Dream Shader Function Files

- Added `.dsf` Dream Shader Function files for reusable generated `ShaderFunction` assets.
- Allowed `.dsm` and `.dsf` files to import `.dsf` files so generated functions can be reused across DreamShader sources.
- Added `.dsf` source discovery, dependency tracking, and VSCode workspace file association.

### Decompiler

- Added Content Browser export actions for `UMaterial` -> `.dsm` and `UMaterialFunction` -> `.dsf`.
- Decompiled files are written under `DShader/Decompiled/Materials` or `DShader/Decompiled/Functions` with unique file names.
- Common constants, parameters, arithmetic nodes, swizzles, texture samples, Custom nodes, and MaterialFunction calls are exported to DreamShader graph text; less common reflected nodes fall back to `UE.Expression(...)`.

## 1.3.4 - 2026-05-11

### Output Initializers

- Added support for initialized output declarations such as `vec3 Color = Tint;` inside `Outputs`.
- Allowed `Shader` blocks to use initialized output declarations with an empty `Graph = {}` block.

## 1.3.3 - 2026-05-11

### Graph Swizzles

- Fixed vector property component counts so declared `vec2` / `vec3` properties bind through `RG` / `RGB` instead of always using `RGBA`.
- Fixed non-sequential swizzles such as `.gbr` by generating explicit `ComponentMask` and `AppendVector` nodes.

## 1.3.2 - 2026-05-11

### Material Function Generation

- Preserved generated `ShaderFunction` input and output IDs across regeneration so existing `MaterialFunctionCall` nodes in regular Unreal materials keep their connections.
- Skipped unused generated property nodes in Graph and Custom/HLSL generation paths.
- Improved generated node placement and avoided Unreal's full automatic layout pass for DreamShader-generated material graphs.
- Fixed a crash when regenerating opened material function assets whose expressions were still rooted by the editor.

## 1.3.1 - 2026-05-09

### Function Calls

- Single-output `Function` and `GraphFunction` calls can now be used as value expressions, for example `Color = Texture::Sample2DRGB(BaseTex, UV0);`.
- Multi-output `Function` and `GraphFunction` calls still require explicit out variables, for example `Texture::Sample2D(BaseTex, UV0, Color, Alpha);`.

### Graph Functions

- Added top-level and namespaced `GraphFunction` blocks for reusable HLSL Custom-node logic.
- `GraphFunction` remains HLSL, but `UE.*` calls inside its body are converted into material nodes and passed into the Custom node as generated inputs.
- Added GraphFunction argument validation, recursive call detection, and explicit out-variable writeback.

## 1.3.0 - 2026-05-08

### Shader Layer Functions

- Added top-level `ShaderLayer(Name="...", Root="...")` and `ShaderLayerBlend(Name="...", Root="...")` blocks.
- Generated layer assets now use Unreal's native `UMaterialFunctionMaterialLayer` / `UMaterialFunctionMaterialLayerBlend` classes.
- `MaterialLayer` / `MaterialLayerBlend` remain compatibility aliases and emit warnings; new source should use `ShaderLayer` / `ShaderLayerBlend`.
- `ShaderLayer` / `ShaderLayerBlend` reuse the existing `Properties`, `Inputs`, `Outputs`, `Settings`, and `Graph` sections.
- Added validation that Shader Layer blocks output exactly one `MaterialAttributes` value, and Shader Layer Blend blocks declare at least two `MaterialAttributes` inputs.
- Vector parameter properties now keep their RGBA output available in Graph, so `.a` / `.w` can read alpha and assignments to lower component counts automatically use leading channels.

## 1.2.10 - 2026-05-08

### VSCode MaterialExpression Manifest

- Added editor-side export of reflected `UMaterialExpression` metadata to `Saved/DreamShader/Bridge/material-expressions.json`.
- The manifest is refreshed on editor bridge startup and when opening the DreamShader VSCode workspace.
- Exported metadata includes expression class names, editable reflected properties, expression inputs, output pins, and inferred DreamShader `OutputType` hints.
- Release workflow now downloads the latest `dreamshader-language-support` GitHub Release assets and attaches them to DreamShader releases.

## 1.2.6 - 2026-04-30

### ShaderFunction Properties

- Added `ShaderFunction` `Properties` as material-function-local property nodes.
- Added `const` property declarations for scalar, vector, and texture helper nodes that are not externally adjustable parameters.
- `ShaderFunction` `Inputs` preview defaults can now reference generated `Properties`, including texture object previews such as `opt Texture2D BaseColorTex = Tex;`.

## 1.2.5 - 2026-04-30

### Material Attributes

- Added `MaterialAttributes` as a graph value type for `Shader`, `ShaderFunction`, and `VirtualFunction` signatures.
- Added struct-like member writes such as `Attrs.BaseColor = Color;` and `Attrs.Roughness = Roughness;`.
- Added `Base.MaterialAttributes = Attrs;` output binding support and automatic `Use Material Attributes` enablement on generated materials.
- MaterialAttributes values can be returned from generated or virtual Material Functions and passed through Graph assignments.

## 1.2.4 - 2026-04-30

### Parameter Reflection

- Replaced the documented comma-style metadata suffix with a semicolon-based trailing reflection block for declarations.
- Parameter reflection blocks can now set any reflected `UMaterialExpression` property exposed by the generated parameter node.
- Basic `float` / vector / texture property shorthand declarations use the same reflection path as explicit parameter node declarations.
- Texture sample parameters can now configure reflected properties such as `SamplerType`, `SamplerSource`, `MipValueMode`, `AutomaticViewMipBias`, `ConstCoordinate`, and `ConstMipValue`.

## 1.2.3 - 2026-04-29

### Parameters

- Added declaration metadata `[Group="...", SortPriority=32, Description="..."]` for material `Properties` and function input/output declarations.
- Added explicit Parameter node declarations including `ScalarParameter`, `VectorParameter`, `TextureObjectParameter`, texture sample parameter nodes, `StaticBoolParameter`, and `StaticSwitchParameter`.
- Added `StaticSwitchParameter` graph calls, for example `UseDetail(True=detailColor, False=baseColor)`.
- Added `UE.CollectionParam(Collection=Path(...), Parameter="...")` for Material Parameter Collection reads.

### Function Defaults

- Added `opt` inputs for `ShaderFunction` and `VirtualFunction`.
- Added `default` call arguments for optional material function inputs, preserving Unreal FunctionInput preview defaults.
- Generated `ShaderFunction` assets now write input/output descriptions and sort priorities to FunctionInput / FunctionOutput nodes.
- VirtualFunction copy/create/sync now emits optional inputs, preview defaults, and pin metadata when available.

## 1.2.2 - 2026-04-29

### VirtualFunction Workflow

- `CreateVirtualFunction` now reuses the existing declaration for the selected Material Function instead of creating duplicate `.dsh` files.
- When a matching declaration already exists, the Material Function `DreamShader` menu shows `OpenVirtualFunction` and `Copy Virtual Function Reference` instead of the create/copy-definition actions.
- `OpenVirtualFunction` opens the existing declaration in VSCode and jumps to the declaration location when possible.
- Added startup validation and refresh for `VirtualFunction` declarations under `DShader`, reporting missing source `UMaterialFunction` assets and updating changed signatures.

### Import Compatibility

- `import "File.dsh"` now works with or without a trailing semicolon in the Unreal generator import pass.

## 1.2.1 - 2026-04-29

### Editor Workflow

- Replaced the single Material Function toolbar action with a `DreamShader` dropdown menu.
- Added `CopyVirtualFunction`, `CreateVirtualFunction`, and `CopyVirtualFunctionCall` actions to the Material Function editor toolbar and Material Function asset context menu.
- `CreateVirtualFunction` writes a `.dsh` declaration file under the configured `DShader/VirtualFunctions` directory and opens it in the default external editor.
- `CopyVirtualFunctionCall` copies a ready-to-paste Graph call using the generated input names and first output.
- Added `Open Dream Shader Workspace (VSCode)` to the editor Tools menu and DreamShader toolbar section. It writes `DShader/DreamShader.code-workspace`, opens it in VSCode when available, and falls back to the default editor or Notepad.

### Release

- Added a GitHub Actions release workflow that packages the plugin source and publishes a GitHub Release from version tags or manual workflow dispatch.

## 1.2.0 - 2026-04-28

### VirtualFunction

- Added `VirtualFunction(Name="...")` declarations for existing Unreal `UMaterialFunction` assets.
- `VirtualFunction` calls can be used from `Graph` like `ShaderFunction` calls, without generating or overwriting the referenced asset.
- `Options.Asset` supports `Path(Game, "...")`, `Path(Engine, "...")`, `Path(Plugin.PluginName, "...")` / `Path(Plugins.PluginName, "...")`, and full Unreal object paths.
- Added Material Function context-menu and Material Editor toolbar actions that copy a complete `VirtualFunction` declaration with inputs, outputs, and options.

### Asset Roots

- Kept `Root="Plugin.PluginName"` mapped to the project plugin content root, physically saving generated assets under `[Project]/Plugins/PluginName/Content`.
- `Plugins.PluginName` and `Plugins/PluginName` remain compatibility spellings.

### Tooling

- Updated the VSCode extension language service for `VirtualFunction`, plugin path completion inside `Path(Plugins.)`, snippets, hover text, signatures, and diagnostics.
- Updated plugin documentation for DreamShader `1.2.0`.
