# DreamShader 参数（Parameter）声明 + metadata 速查

每个参数在 `Properties` 块里声明：`<类型> <名字> [= <默认值>] [<metadata>];`。
配置分三类：
1. **通用 metadata**（所有参数）：`Group` / `SortPriority` / `Desc` / `ParameterName`。
2. **资源/属性槽** → 写在 `[...]` metadata 里，反射到节点属性。资产用 `Path(Root, "相对路径")`（Root = `Game` / `Engine` / 插件名）。
3. **输入引脚** → 在 `Graph` 里用调用形式 `名字(引脚 = 表达式)` 接线（仿 `StaticSwitch`）。

> 属性名 = UE 引擎类上的反射名（区分语义不区分大小写）。资产绑定经字面量写入器解析 `Path(...)` 到 `FObjectProperty`；输入引脚经 `EvaluateConfigurableParameterCall` 按名接线。

通用示例：`ScalarParameter Rough = 0.5 [Group="Surface"; SortPriority=10; Desc="粗糙度"; ParameterName="Roughness";];`

---

## 1. 值参数（无资源/输入）

| 类型 | 声明示例 | 额外 metadata |
|---|---|---|
| `ScalarParameter` | `ScalarParameter Rough = 0.5 [Group="S"; SliderMin=0; SliderMax=1;];` | `SliderMin` `SliderMax` |
| `StaticBoolParameter` | `StaticBoolParameter Flag = true [Group="S";];` | — |
| `StaticSwitchParameter` | `StaticSwitchParameter Sw = false [Group="S";];`<br>用：`Sw(True = A, False = B)` | — |
| `VectorParameter` | `VectorParameter Tint = float4(0.8, 0.4, 0.2, 1) [Group="S";];` | — |
| `DoubleVectorParameter` | `DoubleVectorParameter DV = float4(1, 2, 3, 4) [Group="S";];` | — |
| `DynamicParameter` | `DynamicParameter Dyn = float4(0, 0, 0, 1) [Group="S";];` | 名字 → `ParamNames[0]` |

## 2. 遮罩参数（输入引脚走调用形式）

| 类型 | 声明 + 用法 | metadata |
|---|---|---|
| `ChannelMaskParameter` | `ChannelMaskParameter M [Group="M"; MaskChannel="Green";];`<br>用：`M(Input = Tint)` | `MaskChannel` = `Red`/`Green`/`Blue`/`Alpha` |
| `StaticComponentMaskParameter` | `StaticComponentMaskParameter SCM [Group="M"; DefaultR=true; DefaultG=true;];`<br>用：`SCM(Input = Tint)` | `DefaultR/G/B/A`（bool） |

## 3. 贴图采样参数（`Texture` 资源 + `Coordinates` 引脚）

声明时绑贴图：`[Texture = Path(Game, "Tex/T_MyTex");]`；高维采样在 Graph 里给坐标。

| 类型 | 用法（坐标维度） | metadata |
|---|---|---|
| `TextureSampleParameter2D` | `Tex2D(Coordinates = uv)` 或裸用 `Tex2D.rgb`（默认 UV） | `Texture` `SamplerSource` `SamplerType` |
| `TextureSampleParameter2DArray` | `T(Coordinates = uvw)`（float3：uv+层） | `Texture`（Texture2DArray） |
| `TextureSampleParameterCube` | `T(Coordinates = dir)`（float3 方向） | `Texture`（TextureCube） |
| `TextureSampleParameterCubeArray` | `T(Coordinates = dir4)`（float4：方向+层） | `Texture`（TextureCubeArray） |
| `TextureSampleParameterVolume` | `T(Coordinates = uvw)`（float3） | `Texture`（VolumeTexture） |
| `TextureSampleParameterSubUV` | `T.rgb`（+ SubUV 帧动画输入） | `Texture` |

示例：`TextureSampleParameterCube Sky [Group="T"; Texture = Path(Game, "Tex/T_SkyCube");];` → `Sky(Coordinates = dir).rgb`

## 4. 贴图对象参数（输出贴图对象，喂给采样器）

| 类型 | 声明 + 用法 | metadata |
|---|---|---|
| `TextureObjectParameter` | `TextureObjectParameter TO [Group="O"; Texture = Path(Game, "Tex/T_MyTex");];`<br>采样：`UE.Expression(Class="TextureSample", OutputType="float4", TextureObject = TO).rgb` | `Texture` |
| `TextureCollectionParameter` | `TextureCollectionParameter TC [Group="O"; TextureCollection = Path(Game, "Tex/TC_MyColl");];`<br>（按索引取，不能直接当采样器） | `TextureCollection` |
| `SparseVolumeTextureObjectParameter` | `SparseVolumeTextureObjectParameter SO [Group="O"; SparseVolumeTexture = Path(Game, "Tex/SVT_My");];`<br>（喂给 SparseVolumeTextureSample） | `SparseVolumeTexture` |

## 5. 资产驱动的采样参数

| 类型 | 声明示例 | metadata / 引脚 |
|---|---|---|
| `CurveAtlasRowParameter` | `CurveAtlasRowParameter C [Group="A"; Curve = Path(Game, "Curve/MyCurve"); Atlas = Path(Game, "Curve/MyAtlas");];`<br>用：`C(InputTime = t)` | `Curve` `Atlas`；`InputTime` 引脚 |
| `FontSampleParameter` | `FontSampleParameter F [Group="A"; Font = Path(Game, "Fonts/MyFont"); FontTexturePage=0;];`<br>（须离线缓存字体，非 runtime 缓存） | `Font` `FontTexturePage` |
| `SpriteTextureSampler` | `SpriteTextureSampler Sp [Group="A";];`（Paper2D sprite 上下文提供贴图） | — |
| `RuntimeVirtualTextureSampleParameter` | `RuntimeVirtualTextureSampleParameter R [Group="A"; VirtualTexture = Path(Game, "RVT/MyRVT");];` | `VirtualTexture` |
| `SparseVolumeTextureSampleParameter` | `SparseVolumeTextureSampleParameter S [Group="A"; SparseVolumeTexture = Path(Game, "Tex/SVT_My");];` | `SparseVolumeTexture` |

## 6. 非 Properties 声明 —— Graph 内建调用

| 名字 | 用法 |
|---|---|
| `CollectionParameter`（MPC） | `UE.CollectionParameter(Collection = Path(Game, "MPC_My"), Parameter = "Tint")` —— 读 Material Parameter Collection 资产里的标量/向量参数 |

---

**要点**：未绑资产的采样/字体/曲线/RVT/SVT 参数会"生成成功但 shader 报 Missing/NULL"——这是 UE 节点固有需求，不是缺陷；按上表用 `[资源槽 = Path(...)]` 绑真实资产即编译。遮罩与高维采样的输入引脚用 `名字(引脚 = ...)` 调用形式接线。
