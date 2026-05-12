# DreamShader

[English](README.md) | [语法参考](Docs/LanguageReference.md) | [示例](Docs/Examples.md) | [Package](Docs/Packages.md) | [VSCode](Docs/VSCode.md)

DreamShader 是一个 Unreal Engine 材质生成插件。它提供 `DreamShaderLang` 文本语言，让你用 `.dsm` / `.dsf` / `.dsh` 源文件描述材质、材质函数、Material Layer 和 Material Layer Blend，并自动生成对应的 Unreal 资产。

> 当前版本：`1.3.6`。
>
> DreamShader 仍在持续开发中，核心工作流已经可用。建议把所有 `.dsm` / `.dsf` / `.dsh` 源文件纳入版本管理。
>
> 本插件目前基于 Unreal Engine `5.7` 开发，其他版本尚未完整测试。

问题和 Bug 可以提交到 [Issues](https://github.com/TypeDreamMoon/DreamShader/issues/new)。需要更快的中文支持，也可以加入 [QQ群 466585194](https://qm.qq.com/q/X9uCLjVcY)。

## 核心能力

- 用文本源文件维护 Unreal 材质逻辑，减少重复手动连节点。
- 通过 `Shader(Name="...")` 生成 `UMaterial`。
- 通过 `ShaderFunction(Name="...")` 生成 `UMaterialFunction`。
- 通过 `ShaderLayer(...)` / `ShaderLayerBlend(...)` 生成 Unreal 原生 Material Layer / Layer Blend 函数资产。
- 通过 `VirtualFunction(...)` 声明已有 Unreal 材质函数，并在 DreamShader 图中调用，不生成、不保存、不覆盖原资产。
- 使用 `Graph = { ... }` 编写图逻辑，支持变量、赋值、构造、`UE.*` 材质节点、函数调用和基础 `if` / `else` 图分支。
- 使用 `Function`、`GraphFunction`、`Namespace` 编写可复用 HLSL 风格 helper。
- 支持 `MaterialAttributes` 聚合值，以及 `Attrs.BaseColor = ...` 形式的成员写入。
- 支持 typed `Properties`，包括参数节点、常量 helper、Texture Object、Static Switch、Material Parameter Collection 和 Unreal 反射属性块。
- 支持 `.dsh` header、`.dsf` 函数文件 import graph 和 `DShader/Packages` 可复用包。
- 配套 VSCode 扩展提供语法高亮、补全、跳转、Hover、Signature Help、诊断、Package 命令和 Unreal 桥接诊断。

## 文件模型

| 项目 | 用途 |
| --- | --- |
| `.dsm` | 材质实现文件，通常包含 `Shader`、`ShaderFunction`、`ShaderLayer`、`ShaderLayerBlend` 或 `VirtualFunction`。 |
| `.dsf` | Dream Shader Function 文件，用于生成可复用 `ShaderFunction` 资产，并可被 `.dsm` 导入。 |
| `.dsh` | 共享头文件，通常包含 `import`、`Function`、`GraphFunction`、`Namespace` 和 `VirtualFunction` 声明。 |
| `Shader` | 生成 Unreal `UMaterial`。 |
| `ShaderFunction` | 生成 Unreal `UMaterialFunction`。 |
| `ShaderLayer` | 生成原生 `UMaterialFunctionMaterialLayer`。 |
| `ShaderLayerBlend` | 生成原生 `UMaterialFunctionMaterialLayerBlend`。 |
| `VirtualFunction` | 描述已有 Unreal `UMaterialFunction`，供 `Graph` 调用。 |
| `Graph` | 用于生成材质节点的图 DSL。 |
| `Function` | 可复用 HLSL 风格 helper。 |
| `GraphFunction` | 可复用 Custom 节点 helper，可把 body 中的 `UE.*` 材质节点自动转成 Custom 输入。 |
| `Namespace` | 对 helper 分组，例如 `Texture::Sample2DRGB(...)`。 |
| `Path(...)` | 声明纹理、对象设置或虚拟函数使用的 Unreal 资产路径。 |

推荐项目结构：

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

## 快速开始

1. 把插件复制到 Unreal 项目的 `Plugins/DreamShader`。
2. 在 Unreal Editor 中启用插件并重启编辑器。
3. 在项目根目录创建 `DShader` 目录。
4. 新建 `.dsm` 文件，例如 `DShader/Materials/M_Minimal.dsm`。
5. 保存文件后，DreamShader 会解析源文件并生成或更新目标 Unreal 资产。

配置位于 `Project Settings > DreamPlugin > Dream Shader`。

| 设置 | 默认值 | 说明 |
| --- | --- | --- |
| `SourceDirectory` | `DShader` | DreamShader 源文件根目录。 |
| `GeneratedShaderDirectory` | `Intermediate/DreamShader/GeneratedShaders` | 生成 `.ush` helper 文件的目录。 |
| `AutoCompileOnSave` | `true` | 保存 `.dsm` / `.dsf` / `.dsh` 时刷新受影响资产。 |
| `SaveDebounceSeconds` | `0.25` | 文件保存防抖时间。 |
| `VerboseLogs` | `false` | 输出更详细日志。 |
| `OpenInNewWindow` | `true` | 打开 DreamShader VSCode workspace 时默认使用新窗口。 |

## 最小材质

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

`Root` 可以省略，默认保存到 `Game`。如果需要生成到项目内容插件，可以使用 `Root="Plugin.MyPlugin"`：

```c
Shader(Name="DreamMaterials/M_Minimal", Root="Plugin.MyPlugin")
{
    // ...
}
```

这会生成 Unreal object path `/MyPlugin/DreamMaterials/M_Minimal.M_Minimal`，并保存到 `[Project]/Plugins/MyPlugin/Content/DreamMaterials/M_Minimal.uasset`。

## Properties

`Properties` 可以使用 `float`、`float3`、`Texture2D` 等简写，也可以显式声明 Unreal 参数节点。声明尾部的 `[...]` 块会通过反射写入 Unreal 表达式属性。

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

Material Parameter Collection 可以在图代码中直接读取：

```c
Graph = {
    float wind = UE.CollectionParam(
        Collection=Path(Game, "MaterialParameterCollections/MPC_Global"),
        Parameter="WindStrength");
}
```

## Graph 与 Helper

`Graph = { ... }` 是材质节点 DSL。适合变量声明、赋值、构造、`UE.*` 材质节点、DreamShader helper 调用、生成或虚拟材质函数调用，以及简单图分支。

```c
Graph = {
    float2 uv = UE.TexCoord(Index=0);
    float pulse = UE.Expression(Class="Sine", OutputType="float1", Input=UE.Time());
    Color = vec3(pulse, pulse, pulse);
}
```

`Function` 是 HLSL 风格可复用代码，适合复杂计算、循环，以及应该放进 Custom 节点的逻辑。

```c
Namespace(Name="Color")
{
    Function ApplyTint(in vec3 color, in vec3 tint, out vec3 result) {
        result = color * tint;
    }
}
```

导入后即可调用：

```c
import "Shared/Color.dsh";

Graph = {
    Color::ApplyTint(BaseColor, Tint, Color);
}
```

`GraphFunction` 同样会生成 Custom 节点，但它的 body 中可以直接写 `UE.*` 调用。DreamShader 会先创建对应 Unreal 材质节点，再把节点输出作为自动输入引脚接入 Custom 节点。

## MaterialAttributes

`MaterialAttributes` 可以作为 Graph 值、函数输出、虚拟函数输出和材质输出绑定使用。当 `Shader` 绑定到 `Base.MaterialAttributes` 时，DreamShader 会自动启用 Unreal 材质的 `Use Material Attributes`。

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

## Material Layer

`ShaderLayer` 和 `ShaderLayerBlend` 会生成 Unreal 原生 Material Layer 函数资产。

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

当前规则：

- `ShaderLayer` 创建 `UMaterialFunctionMaterialLayer`。
- `ShaderLayerBlend` 创建 `UMaterialFunctionMaterialLayerBlend`。
- 两者复用 `ShaderFunction` 的 `Properties`、`Inputs`、`Outputs`、`Settings` 和 `Graph` sections。
- 两者必须声明且只声明一个 `MaterialAttributes` 输出。
- `ShaderLayerBlend` 必须至少声明两个 `MaterialAttributes` 输入。
- 旧语法 `MaterialLayer(...)` 和 `MaterialLayerBlend(...)` 仍可解析，但会产生废弃警告。

当前支持的是原生 Layer Function 资产生成；完整 Material Layer Stack / Layer Instance 工作流仍在 Roadmap 中。

## VirtualFunction

`VirtualFunction` 用于把已有 Unreal `UMaterialFunction` 暴露给 DreamShader，不生成、不保存、不覆盖对应资产。

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

Unreal Material Function 资产右键菜单和编辑器工具栏中会提供 DreamShader 操作，用于复制虚拟函数定义、创建 `.dsh` 声明文件、打开已有声明，以及复制调用示例。

## Package

DreamShader Package 是可复用 `.dsh` 函数库，安装位置为：

```text
DShader/Packages/@scope/package-name/
```

导入示例：

```c
import "@typedreammoon/dream-noise/Library/Noise.dsh";
```

Package 结构、锁文件、安装命令和包开发方式见 [Docs/Packages.md](Docs/Packages.md)。

## VSCode 扩展

DreamShaderLang VSCode 扩展提供：

- `.dsm` / `.dsh` 语法高亮和 snippets。
- block、section、helper、`UE.*`、`Path(...)`、Package import 和 Unreal 桥接数据补全。
- Go to Definition、Find References、Hover、Signature Help。
- 本地诊断和 Unreal 桥接诊断。
- Package 安装、更新、移除和浏览命令。
- 快速创建材质、头文件、纹理采样和噪声材质模板。

Unreal 编辑器的 `Tools > DreamShader` 菜单和 DreamShader 工具栏可以打开生成的 `DShader/DreamShader.code-workspace`。

扩展发布地址：[dreamshader-language-support](https://github.com/TypeDreamMoon/dreamshader-language-support/releases)。

## 文档

- [文档总览](Docs/README.md)
- [语法参考](Docs/LanguageReference.md)
- [示例与模式](Docs/Examples.md)
- [Package 系统](Docs/Packages.md)
- [VSCode 支持](Docs/VSCode.md)

## 发布

仓库包含 GitHub Actions 自动发布流程。推送与 `DreamShader.uplugin` 中 `VersionName` 一致的 tag 即可：

```powershell
git tag v1.3.6
git push origin v1.3.6
```

发布包名为 `DreamShader-<Version>.zip`，包含插件源码、资源、内置库、文档、README、CHANGELOG 和 LICENSE，不包含 `Binaries` / `Intermediate`。Release workflow 还会附带 `TypeDreamMoon/dreamshader-language-support` 的最新 VSCode 扩展资产。

## 项目信息

| 项目 | 内容 |
| --- | --- |
| Version | `1.3.6` |
| Language | `DreamShaderLang` |
| Author | TypeDreamMoon |
| GitHub | <https://github.com/TypeDreamMoon> |
| Docs | <https://lang.64hz.cn/> |
| Web | <https://dev.64hz.cn> |
| Copyright | Copyright (c) 2026 TypeDreamMoon. All rights reserved. |

## Roadmap

- Custom full-screen render pass 支持。
- 更完整的 VSCode 语义诊断。
- 完整 Substrate 支持。
- 更深入的 Material Layer 工作流支持。
- 更深入的 Moon Engine 集成。参考：<https://zhuanlan.zhihu.com/p/21979494450>
