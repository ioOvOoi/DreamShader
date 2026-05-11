# DreamShader

[中文文档](README.zh-CN.md) | [Language Reference](Docs/LanguageReference.md) | [Examples](Docs/Examples.md) | [Packages](Docs/Packages.md) | [VSCode](Docs/VSCode.md)

DreamShader is an Unreal Engine plugin for authoring materials and material functions with text source files. It introduces `DreamShaderLang`, a compact DSL that turns `.dsm` and `.dsh` files into Unreal `UMaterial`, `UMaterialFunction`, Material Layer, and Material Layer Blend assets.

> Current version: `1.3.3`.
>
> DreamShader is actively developed. The core authoring workflow is usable, but you should keep all `.dsm` / `.dsh` files in source control.
>
> The plugin is currently developed against Unreal Engine `5.7`. Other engine versions have not been fully tested.

For bug reports and feature requests, open an [issue](https://github.com/TypeDreamMoon/DreamShader/issues/new). For faster Chinese-language support, you can also join the [QQ group 466585194](https://qm.qq.com/q/X9uCLjVcY).

## Highlights

- Author Unreal materials as readable text files instead of repeatedly wiring node graphs by hand.
- Generate `UMaterial` assets from `Shader(Name="...")`.
- Generate `UMaterialFunction` assets from `ShaderFunction(Name="...")`.
- Generate native Unreal Material Layer and Layer Blend function assets from `ShaderLayer(...)` and `ShaderLayerBlend(...)`.
- Declare existing Unreal material functions with `VirtualFunction(...)` and call them from DreamShader graphs without overwriting the original assets.
- Build graph logic with `Graph = { ... }`, including variables, assignments, constructors, `UE.*` material nodes, function calls, and basic `if` / `else` graph branches.
- Write reusable HLSL-style helpers with `Function`, `GraphFunction`, and `Namespace`.
- Use `MaterialAttributes` values, including `Attrs.BaseColor = ...` style member writes.
- Use typed `Properties` for parameters, constants, texture objects, static switches, Material Parameter Collections, and reflected Unreal expression properties.
- Import shared `.dsh` headers and install reusable DreamShader packages under `DShader/Packages`.
- Work with a VSCode extension that provides syntax highlighting, completion, hover, signature help, diagnostics, package commands, and Unreal bridge diagnostics.

## Source Model

| Item | Purpose |
| --- | --- |
| `.dsm` | Material implementation file. Usually contains `Shader`, `ShaderFunction`, `ShaderLayer`, `ShaderLayerBlend`, or `VirtualFunction` blocks. |
| `.dsh` | Shared header file. Usually contains `import`, `Function`, `GraphFunction`, `Namespace`, and `VirtualFunction` declarations. |
| `Shader` | Generates an Unreal `UMaterial`. |
| `ShaderFunction` | Generates an Unreal `UMaterialFunction`. |
| `ShaderLayer` | Generates a native `UMaterialFunctionMaterialLayer`. |
| `ShaderLayerBlend` | Generates a native `UMaterialFunctionMaterialLayerBlend`. |
| `VirtualFunction` | Describes an existing Unreal `UMaterialFunction` so it can be called from `Graph`. |
| `Graph` | Node-oriented DSL used inside generated materials and functions. |
| `Function` | Reusable HLSL-style helper. |
| `GraphFunction` | Reusable Custom-node helper that can pull `UE.*` material nodes into generated Custom inputs. |
| `Namespace` | Groups helpers, for example `Texture::Sample2DRGB(...)`. |
| `Path(...)` | Declares Unreal asset paths for textures, object settings, or virtual functions. |

Recommended project layout:

```text
MyProject/
├─ DShader/
│  ├─ Materials/
│  │  └─ M_Sample.dsm
│  ├─ Shared/
│  │  └─ Common.dsh
│  └─ Packages/
└─ Plugins/
   └─ DreamShader/
```

## Quick Start

1. Copy the plugin into your Unreal project at `Plugins/DreamShader`.
2. Enable the plugin in Unreal Editor and restart the editor.
3. Create a `DShader` directory in the project root.
4. Create a `.dsm` file, for example `DShader/Materials/M_Minimal.dsm`.
5. Save the file. DreamShader will parse the source and generate or update the target Unreal asset.

Project settings are available under `Project Settings > DreamPlugin > Dream Shader`.

| Setting | Default | Description |
| --- | --- | --- |
| `SourceDirectory` | `DShader` | Root directory for DreamShader source files. |
| `GeneratedShaderDirectory` | `Intermediate/DreamShader/GeneratedShaders` | Output directory for generated `.ush` helper files. |
| `AutoCompileOnSave` | `true` | Rebuild affected assets when `.dsm` / `.dsh` files are saved. |
| `SaveDebounceSeconds` | `0.25` | File-save debounce time. |
| `VerboseLogs` | `false` | Print more detailed logs. |
| `OpenInNewWindow` | `true` | Open the generated VSCode workspace in a new window by default. |

## Minimal Material

```c
Shader(Name="DreamMaterials/M_Minimal")
{
    Properties = {
        vec3 Tint = vec3(1.0, 0.2, 0.2);
    }

    Settings = {
        Domain = "UI";
        ShadingModel = "Unlit";
    }

    Outputs = {
        vec3 Color;
        Base.EmissiveColor = Color;
    }

    Graph = {
        Color = Tint;
    }
}
```

`Root` is optional and defaults to `Game`. To generate into a project content plugin, use `Root="Plugin.MyPlugin"`:

```c
Shader(Name="DreamMaterials/M_Minimal", Root="Plugin.MyPlugin")
{
    // ...
}
```

This produces the Unreal object path `/MyPlugin/DreamMaterials/M_Minimal.M_Minimal` and saves the asset under `[Project]/Plugins/MyPlugin/Content/DreamMaterials/M_Minimal.uasset`.

## Properties

`Properties` can use compact types such as `float`, `float3`, and `Texture2D`, or explicit Unreal parameter node types. Reflected Unreal expression properties can be written in a trailing `[...]` block.

```c
Properties = {
    ScalarParameter Roughness = 0.35 [
        Group="Surface";
        SortPriority=10;
        Description="Material roughness";
    ];

    VectorParameter Tint = float4(1.0, 0.9, 0.8, 1.0) [
        Group="Surface";
        SortPriority=20;
    ];

    StaticSwitchParameter UseDetail = true [
        Group="Switches";
        SortPriority=30;
    ];

    TextureSampleParameter2D Albedo = Path(Game, "Textures/T_Albedo") [
        Group="Textures";
        SamplerType="Color";
    ];
}
```

Material Parameter Collections can be read directly from graph code:

```c
Graph = {
    float wind = UE.CollectionParam(
        Collection=Path(Game, "MaterialParameterCollections/MPC_Global"),
        Parameter="WindStrength");
}
```

## Graph And Helpers

`Graph = { ... }` is the material-node DSL. Use it for variable declarations, assignments, constructors, `UE.*` material nodes, DreamShader helper calls, generated or virtual material function calls, and simple graph branches.

```c
Graph = {
    float2 uv = UE.TexCoord(Index=0);
    float pulse = UE.Expression(Class="Sine", OutputType="float1", Input=UE.Time());
    Color = vec3(pulse, pulse, pulse);
}
```

`Function` is HLSL-style reusable code. It is better for complex calculations, loops, and logic that should live inside a Custom node.

```c
Namespace(Name="Color")
{
    Function ApplyTint(in vec3 color, in vec3 tint, out vec3 result) {
        result = color * tint;
    }
}
```

Then import and call it:

```c
import "Shared/Color.dsh";

Graph = {
    Color::ApplyTint(BaseColor, Tint, Color);
}
```

`GraphFunction` is also generated as a Custom node, but it can reference `UE.*` calls in the body. DreamShader creates those Unreal material nodes and wires their outputs into the Custom node as generated inputs.

## MaterialAttributes

`MaterialAttributes` can be used as a graph value, a function output, a virtual function output, and a material output binding. When a `Shader` binds to `Base.MaterialAttributes`, DreamShader automatically enables Unreal's `Use Material Attributes` option on the generated material.

```c
Outputs = {
    MaterialAttributes Attrs;
    Base.MaterialAttributes = Attrs;
}

Graph = {
    Attrs.BaseColor = Color;
    Attrs.Roughness = Roughness;
}
```

## Material Layers

`ShaderLayer` and `ShaderLayerBlend` generate native Unreal Material Layer function assets.

```c
ShaderLayer(Name="Layers/L_SimpleSurface")
{
    Outputs = {
        MaterialAttributes Attrs;
    }

    Graph = {
        Attrs.BaseColor = vec3(0.8, 0.2, 0.1);
        Attrs.Roughness = 0.5;
    }
}
```

Current rules:

- `ShaderLayer` creates `UMaterialFunctionMaterialLayer`.
- `ShaderLayerBlend` creates `UMaterialFunctionMaterialLayerBlend`.
- Both reuse the `ShaderFunction` sections: `Properties`, `Inputs`, `Outputs`, `Settings`, and `Graph`.
- Both must declare exactly one `MaterialAttributes` output.
- `ShaderLayerBlend` must declare at least two `MaterialAttributes` inputs.
- Legacy `MaterialLayer(...)` and `MaterialLayerBlend(...)` syntax still parses, but emits deprecation warnings.

This is native layer-function generation. Full Material Layer Stack / Layer Instance workflow support is still on the roadmap.

## VirtualFunction

Use `VirtualFunction` to expose an existing Unreal `UMaterialFunction` to DreamShader without generating, saving, or overwriting that asset.

```c
VirtualFunction(Name="BufferWriter")
{
    Options = {
        Asset = Path(Plugins.MoonToon, "MaterialFunctions/Buffer/Writer");
    }

    Inputs = {
        float3 Color;
        float Alpha;
    }

    Outputs = {
        float3 Result;
    }
}

Graph = {
    Result = BufferWriter(Tint, 1.0, Output="Result");
}
```

The Unreal Material Function asset context menu and editor toolbar include DreamShader actions for copying virtual function definitions, creating `.dsh` declarations, opening existing declarations, and copying call examples.

## Packages

DreamShader packages are reusable `.dsh` libraries installed under:

```text
DShader/Packages/@scope/package-name/
```

Import example:

```c
import "@typedreammoon/dream-noise/Library/Noise.dsh";
```

See [Docs/Packages.md](Docs/Packages.md) for package structure, lock files, install commands, and package authoring.

## VSCode Extension

The DreamShaderLang VSCode extension provides:

- Syntax highlighting and snippets for `.dsm` / `.dsh`.
- Completion for blocks, sections, helpers, `UE.*`, `Path(...)`, package imports, and Unreal bridge data.
- Go to Definition, Find References, Hover, and Signature Help.
- Local diagnostics and Unreal bridge diagnostics.
- Package install, update, remove, and browse commands.
- Quick templates for materials, headers, texture sampling, and noise materials.

The Unreal editor menu `Tools > DreamShader` and the DreamShader toolbar can open the generated `DShader/DreamShader.code-workspace` in VSCode.

The extension releases are available from [dreamshader-language-support](https://github.com/TypeDreamMoon/dreamshader-language-support/releases).

## Documentation

- [Documentation Overview](Docs/README.md)
- [Language Reference](Docs/LanguageReference.md)
- [Examples And Patterns](Docs/Examples.md)
- [Package System](Docs/Packages.md)
- [VSCode Support](Docs/VSCode.md)

## Release

The repository includes a GitHub Actions release workflow. Push a tag that matches `VersionName` in `DreamShader.uplugin`:

```powershell
git tag v1.3.3
git push origin v1.3.3
```

The release archive is named `DreamShader-<Version>.zip` and contains the plugin source, resources, built-in libraries, documentation, README, CHANGELOG, and LICENSE. It excludes `Binaries` and `Intermediate`. The release workflow also attaches the latest VSCode extension assets from `TypeDreamMoon/dreamshader-language-support`.

## Project Info

| Item | Value |
| --- | --- |
| Version | `1.3.3` |
| Language | `DreamShaderLang` |
| Author | TypeDreamMoon |
| GitHub | <https://github.com/TypeDreamMoon> |
| Docs | <https://lang.64hz.cn/> |
| Web | <https://dev.64hz.cn> |
| Copyright | Copyright (c) 2026 TypeDreamMoon. All rights reserved. |

## Roadmap

- Custom full-screen render pass support.
- More complete VSCode semantic diagnostics.
- Full Substrate support.
- Deeper Material Layer workflow support.
- Deeper Moon Engine integration. Reference: <https://zhuanlan.zhihu.com/p/21979494450>
