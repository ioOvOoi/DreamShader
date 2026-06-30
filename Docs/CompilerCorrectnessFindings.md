# DreamShader 生成器正确性缺陷报告

> 来源：对生成器 HLSL/材质发射逻辑的多视角静态审计 + 对抗式验证（每条都被独立 agent 尝试反驳，驳不倒才保留）。共 19 个候选 → **核实 12 个真实缺陷**。每条都带 `文件:行号`、期望/实际、修复方向、最小复现。复现可直接转为 Generate 层测试夹具。

> **修复进度（2026-06-29）—— A 线全部修复，套件 37/37 green**：
>
> | # | 修复 | 回归测试 |
> |---|---|---|
> | #1 | 禁止把较大的非权威操作数缩小强制 | ✅ `M_Truncate`（generate-error） |
> | #2 | `truthy` 改接 `!=` 语义 | ✅ `Gen.Wiring.TruthyCondition`（节点内省） |
> | #3 | 标量 swizzle 越界改报错 | ✅ `M_ScalarSwizzle`（generate-error） |
> | #4 | 混合位置+命名实参直接报错（stopgap） | ⬜ 需真实 MaterialFunction 资产，测试待定 |
> | #5 | BreakOut 内联按语义名取通道 | ⬜ 需 /Engine BreakOut 资产，测试待定 |
> | #6 | `EnsureTopLevelReturn` 改 token 感知扫描 | ⬜ 失败在 HLSL 编译期，需材质编译/Custom Code 内省，测试待定 |
> | #7 | 分词器消费 `f/h/u/l` 后缀 | ✅ `M_Suffix`（generate-ok） |
> | #8 | 向量默认值 `%f`→`SanitizeFloat` | ⬜ 需 ChannelMaskParameter DefaultValue 内省，测试待定 |
> | #9 | BreakMaterialAttributes 读取改按名解析 | ✅ `M_MaterialAttributeRead`（generate-ok，烟雾）。**注**：SurfaceThickness 在本 UE5.8/Moon 构建不是 Break 输出，无法读取——引擎限制，非 DSL 问题；按名解析对已暴露属性更健壮。 |
> | #10 | int/uint 构造器打整数标记，整数除法报错 | ✅ `M_IntegerDivide`（generate-error） |
> | #11 | if 分支同名局部类型一致性检查 | ✅ `M_IfBranchTypeMismatch`（generate-error） |
> | #12 | uint32 字面量经 int64 解析（全范围） | ⬜ 无 stock uint32 反射属性，测试待定 |
> | #13 | `LayoutGeneratedExpressions` 进度账目重算（修 SlowTask ensure） | 由 if 材质 generate 测试间接覆盖 |
>
> **测试覆盖**：7 个 bug 有自动回归测试转 green（#1/#2/#3/#7/#9/#10/#11）；5 个修复已应用但回归测试受限于"需真实资产 / 需材质编译 / 无对应反射属性"暂缺（#4/#5/#6/#8/#12）；#13 间接覆盖。

## 三个主题

**主题一：静默的类型/语义错配（最危险）。** DSL「宣称支持某语义、却静默生成偏离该语义的图」，无任何诊断。混合尺寸向量运算静默截断丢通道、标量 swizzle 越界静默 splat、`if(x)` 被编成 `x>0` 而非 `x!=0`、BreakOut 内联按声明位置而非语义名取通道、混合「位置+命名」实参接错引脚。不崩溃、不报错，却产出与意图不符的材质，最难察觉，测试应优先覆盖。

**主题二：HLSL 兼容性缺口导致生成失败/硬错误。** `EnsureTopLevelReturn` 裸 substring 匹配 `return`；数字字面量分词器不接受 `f`/`u`/`l` 后缀（`0.55f` 解析中止）；`BreakMaterialAttributes` 读取侧硬编码索引且漏 `SurfaceThickness`。

**主题三：字面量精度/整数语义丢失（影响较窄）。** 向量默认值用 `%f`（6 位小数）→ 小幅值塌缩为 0；`int(7)/int(2)` 走浮点除法得 3.5；`uint32` 经有符号 `int32` 解析，>2^31 的合法值被拒。

**架构性根因：** 多缺陷共享同一模式——「写入侧用引擎权威映射（GUID/名称解析/按值赋值），读取/内联/格式化侧却用手写位置索引或字符串拼接」。建议 Generate 层统一「名称/语义驱动解析优先，位置回退需显式校验」。

## 缺陷清单（severity → confidence 排序）

| # | 严重度 | 置信 | 标题 | 位置 |
|---|---|---|---|---|
| 1 | major | high | 混合尺寸向量运算静默截断较大操作数（丢分量）而非报错 | CodeExpressions.cpp:1603-1627 → :953-958 |
| 2 | major | high | `if(x)` 真值被编成 `x>0` 而非 `x!=0`（负数条件当 false） | CodeExpressions.cpp:645 |
| 3 | major | high | 标量 swizzle 接受越界通道并静默 splat（`s.yz`→`float2(s,s)`） | CodeExpressions.cpp:2426-2444 |
| 4 | major | high | 混合「位置+命名」MaterialFunctionCall 实参接到错误引脚 | CodeCalls.cpp:1484-1589 |
| 5 | major | high | BreakOut 内联 swizzle 按声明位置而非语义名取通道 | CodeCalls.cpp:1878-1903 |
| 6 | major | high | `EnsureTopLevelReturn` 裸 substring 匹配 `return`（注释/标识符误判）→ Custom 无返回 HLSL 编译失败 | Support.cpp:1745 |
| 7 | major | high | 数字字面量不接受 `f`/`u`/`l` 后缀（`0.55f` 解析中止） | CodeParsing.cpp:784 |
| 8 | major | high | 向量默认值用 `%f`（6 位小数）→ 精度丢失/小幅值归零 | Support.cpp:5221 |
| 9 | major | high | `BreakMaterialAttributes` 读取侧硬编码索引、漏 `SurfaceThickness`、间隙(25)脆弱 | CodeShared.h:553 |
| 10 | minor | med | 整数构造器/字面量发射浮点节点，`int(7)/int(2)`=3.5 丢截断 | CodeExpressions.cpp:1685/2638/2680 |
| 11 | minor | high | if 合并两分支同名局部时按 ThenValue 取分量数，静默类型错配 | CodeExpressions.cpp:506 |
| 12 | minor | high | `FUInt32Property` 经有符号 int32 解析，拒绝 >2^31 的合法无符号值 | Support.cpp:1267 |

## 修复要点与复现

### #2 `if(x)` 真值语义（建议优先修，一处小改）
`truthy` 与 `>` 共用 `ConnectBranches(True, False, False)`，`If` 节点仅 `x>0` 选 then，负数走 else。仓库自身约定（Decompiler:244）与 `!=` 实现都是 `x!=0`。**修复：** 把 `truthy` 从 `>` 分支移到 `!=` 分支（`ConnectBranches(True, False, True)`）。
```c
// M_TruthyBug.dsm —— Sign=-1 非零, C 语义期望 RED(then), 实际产出 GREEN
Shader(Name="Materials/M_TruthyBug", Root="Game") {
    Properties = { ScalarParameter Sign = -1.0; }
    Settings = { Domain="Surface"; ShadingModel="DefaultLit"; BlendMode="Opaque"; }
    Outputs = { float3 Color; Base.BaseColor = Color; }
    Graph = {
        float3 Color = float3(0,0,0);
        if (Sign) { Color = float3(1,0,0); } else { Color = float3(0,1,0); }
    }
}
```

### #7 数字后缀 `f`（HLSL 作者高频踩）
`Tokenize` 的 Number 分支不消费 `f`/`u`/`l`，`0.55f` 在 `0.55` 处断开并发出标识符 `f`，报 `Unexpected token 'f'`。**修复：** 数值主体后可选消费单个后缀（浮点 f/F/h/H，整数 u/U/l/L），token 文本仍取干净数值。
```c
// M_Suffix.dsm —— 当前生成失败 "Unexpected token 'f'"
Shader(Name="Materials/M_Suffix", Root="Game") {
    Settings = { Domain="Surface"; ShadingModel="DefaultLit"; BlendMode="Opaque"; }
    Outputs = { float Rough; Base.Roughness = Rough; }
    Graph = { Rough = 0.55f; }
}
```

### #6 `EnsureTopLevelReturn` substring 误判
裸 `Contains("return")` 会被注释 `// returns...`、字符串、标识符 `returnValue` 误导，跳过合成 `return` → Custom 函数无返回 → HLSL `not all control paths return a value`。**修复：** 用 token 感知扫描（剥离注释/字符串、brace 深度 0、边界检查），复用本文件已有的注释/字符串状态机。

### #1 混合尺寸截断
`float3 a * float4 b`：当唯一权威操作数是较小者时，`b` 被静默掩码为 `.rgb` 丢第 4 分量，无诊断。**修复：** `TryCoerceNonAuthoritativeOperand` 禁止缩小强制（`OtherValue.ComponentCount > Authoritative` → return false，让真正的尺寸不匹配错误触发）。

### #3 标量 swizzle 越界
标量分支只校验字符属于 `{x,r,y,g,z,b,w,a}` 却不查通道是否存在，`s.yz`→`float2(s,s)`、`s.w`→`s`，HLSL 里 `someFloat.y` 本是编译错误。**修复：** 标量分支镜像多分量分支，对越界通道（索引 != 0）报相同错误，保留 `.x/.xx/.xxxx` 合法复制。

### #4 位置+命名实参错配
输入 `[A,B]`、调用 `(posval, A=namedval)`：A 绑 namedval（游标不动），B 消费 posval —— 本该给 A 的 posval 接到 B，两处后置校验都检测不到。**修复：** 位置绑定按声明顺序感知命名占位；强制位置实参在命名实参之前；双重赋值报错。止血方案：同时含位置+命名实参直接报错。

### #5 BreakOut 内联通道错位
`TryInlineBreakOutFunction` 用 `Swizzles[声明位置]` 取通道。Outputs 声明为 `[B,G,R]` 时请求 `Output="R"` 得索引 2 → 发出 `.b`，而真实资产路径按名称返回红通道。内联本应透明，却静默产出不同通道。**修复：** 由语义名（R/G/B/A↔X/Y/Z/W）推导 swizzle 通道，非识别名则回退资产路径。

### #8 向量默认值精度
标量用 `SanitizeFloat`（最短可往返），向量用 `Printf("(R=%f,...)")`（6 位小数）：`0.123456789`→`0.123457`，`1e-8`→`0`。命中非白名单 Vector 参数（ChannelMaskParameter/StaticComponentMaskParameter/DynamicParameter/CurveAtlasRowParameter）。**修复：** 向量分支每分量用 `SanitizeFloat` 或 `%.9g` 或 `FLinearColor::ToString()`，同修 `(R=,G=,B=,A=)` 与 `(X=,Y=,Z=,W=)` 回退。

### #9 BreakMaterialAttributes 读取索引
手写 switch：缺 `MP_SurfaceThickness` case（合法属性被硬报错），且 `MP_PixelDepthOffset=24`→`MP_Displacement=26` 间隙(25)假设第 25 槽不可读，引擎/Substrate/Moon 构建顺序不同则后续索引偏移。写入侧(:334)是 GUID 驱动，二者不对称。**修复：** 读取侧改名称驱动（`FMaterialAttributeDefinitionMap::GetDisplayNameForMaterial` 在 Break 节点 Outputs 按名匹配），镜像写入侧。

### #10/#11/#12（minor）
- **#10** `int/uint` 族构造器走 float 求值、`/` 无条件 `Divide`，`int(7)/int(2)`=3.5。修复：整数除法报错或 `Floor` 近似 + 文档注明。
- **#11** if 合并两分支全新声明同名局部时按 ThenValue 分量数强制，`then=float3/else=float` 被静默广播。修复：合并前加分支类型一致性检查。
- **#12** `FUInt32Property` 用 `ParseIntegerLiteral(int32)`，`3000000000` 溢出为负被拒。修复：用 `int64` lex + 范围检查，或 `LexTryParseString(uint32&)`。

### #14 if 分支内读取参数导致生成失败（已修，2026-06-30）

`if`/`else` 任一分支首次**读取**某 Property/Parameter 时，`TryCreatePropertyValue` 会把它惰性写入该分支的值表（`DreamShaderMaterialGeneratorCodeExpressions.cpp:2906` 的 `Values->Add`）。而 if 合并（`ExecuteIfStatement`）把"分支值表相对 base 的任何新增/变更"一律当作需在两分支间用 If 节点选择的输出，于是只在单边读到的参数（如 `Color = Lit.rgb;` 中的 `Lit`）被误判为"单边条件赋值"，在 `:503` 报 `Graph if statement could not resolve both branch values for '<param>'` —— **任何在 if 分支内读取参数的材质都无法生成**。参数永不可作为赋值目标，真正需在分支间选择的只有被赋值的局部/输出变量。

**复现：** `Graph { float2 uv = UE.TexCoord(Index=0); float mask = uv.x; if (mask > Threshold) { Color = Lit.rgb; } else { Color = Dark.rgb; } }`（`Lit`/`Dark` 为 VectorParameter）。对照：现有 `M_IfBranchTypeMismatch` 之所以能跑到类型检查，是因为它分支体只用字面量、参数只在 condition（分支前）读，所以未触发。

**修复：** 合并循环开头跳过 `FindPropertyDefinition(Name)` 命中的名字（声明的属性是读副作用噪声，非分支输出）；附带消除了原先为每个变更名重复构造条件/比较节点的浪费。回归夹具 `Tests/Corpus/Generate/Material/M_IfBranchParamRead.dsm`（`outcome=ok`），套件 51/51 green。根因模式同 #9：惰性物化 + 写入/读取侧语义不对称。**经由"多写样例材质 + 看生成 .ush"练习发现。**

### #15 生成的 HLSL 函数名与引擎内建着色器函数全局命名冲突（已修，2026-06-30）

**只在着色器编译期暴露，资产生成期不报错** —— 这是此前测试网的盲区（corpus Generate 测试只验证 `GenerateMaterialFromFile` 成功，不编译材质着色器）。

`BuildGeneratedFunctionSymbolName`（`DreamShaderHlslFunctionCodegen.cpp`）直接用 `SanitizeIdentifier(Function.Name)` 作为 HLSL 函数名，**裸名进入材质 Custom 节点的全局 HLSL 命名空间**。用户把 `.dsh` 函数命名为 `Luminance` → `.ush` 里 emit `float Luminance(float3)` → 与 `/Engine/Private/Common.ush:743` 的内建 `float Luminance(float3)` **重定义**：
```
M_SinCos_2031e9e7.ush(7,7): error: redefinition of 'Luminance'
```
`Square`/`Pow2`/`Desaturate` 等同理（`Square` 也在 Common.ush）。**叠加 bug B（whole-closure inlining）放大**：`WriteGeneratedInclude` 把 header 内**全部**函数写进 .ush（M_SinCos 只调 SinCos，却也 emit Luminance/Remap），于是 M_SinCos 因一个它根本没用到的 Luminance 而编译失败。

**修复：** `BuildGeneratedFunctionSymbolName` 统一加前缀 `DreamShaderFn_`。该名是函数定义、body 内函数间调用重写（`ReplacementBySpelling`）、与 Custom 节点调用点（CodeCalls.cpp:590/598）的**唯一真源**，一处改全链一致 → `DreamShaderFn_Luminance`，永不撞引擎内建。

**端到端验证（/unreal-bridge + Live Coding + 看 .ush + `get_material_compile_errors`）：** 7 个样例材质（M_Grayscale/Desaturate/Contrast/Posterize/Luma/SinCos + 新增 M_Square）着色器**全部 0 error**。`M_Square` 专测：函数名 `Square`（引擎冲突）经**嵌套 DreamShader 调用** `Brighten→Square` 到达，.ush 正确产出 `DreamShaderFn_Brighten` 调 `DreamShaderFn_Square(k)`（嵌套 out-param→return 降级也对）。

**坑（验证副产物，非 bug）：** 文件监视器 `ProcessSourceFile` 用 `CompileAssets(force=false)`；用相同字节 re-save `.dsm` → 材质资产判为 up-to-date 跳过重生（Custom 节点旧裸名调用），但 .ush 仍重写（新前缀）→ 临时不一致 `undeclared identifier 'Grayscale'`。**真实内容变更**强制全量重生后即一致。

**遗留（bug B，现已无害）：** whole-closure inlining 仍把整个 header 写进每个 .ush（dead code）。加前缀后不再冲突、被 HLSL 编译器 strip，仅属冗余，可后续让 `WriteGeneratedInclude` 只 emit 材质实际调用的传递闭包。

**测试网盲区结论：** asset-gen 绿 ≠ shader-compile 绿。深度正确性须 `UnrealBridgeMaterialLibrary.get_material_compile_errors(path,"SM6","Default")`（或编辑器内打开材质看统计面板），应纳入回归。

### #16 DynamicParameter 无法生成（已修，2026-06-30）

**由新增的参数生成测试 `DreamShader.Gen.Parameters.NodeCreation` 发现。** `DynamicParameter` 能正常解析（`ParameterExpressions.ParseAll` 早已覆盖），但生成时整材质中止：

```
M_ParamGen.dsm(12,9): Failed to evaluate Graph assignment for 'Color'. Property 'Dyn':
  'MaterialExpressionDynamicParameter' does not expose a ParameterName property.
（修第一层后）Metadata property 'group' is not a reflected property on 'MaterialExpressionDynamicParameter'.
```

**根因：** `UMaterialExpressionDynamicParameter` 是调色板里**唯一不继承** `UMaterialExpressionParameter` 的参数节点。它既没有单一 `ParameterName`（改用 `ParamNames[]`，每个输出一个名），也没有 `Group`/`SortPriority` 组织字段。生成器对所有参数节点统一反射设这两组属性，两处都 hard-fail。

**修复（`DreamShaderExpressionFactory.cpp`）：**
1. `SetExpressionParameterName` 特判 `Cast<UMaterialExpressionDynamicParameter>`：把声明标识符写进 `ParamNames[0]`（主输出名），其余输出保留引擎默认。
2. `ApplyExpressionMetadata`：`Group`/`SortPriority`/`Desc` 是咨询性的参数面板组织字段；目标节点不暴露时**告警跳过**而非中止生成。作者手写的其他反射属性仍 hard-fail（保留 typo 检测）。
3. `DefaultValue`（FLinearColor）走既有 vector 默认路径，无需改动。

**端到端验证：** ParamNames[0]=="Dyn"、DefaultValue 透传 `(0.15,0.2,0.3,1.0)`、M_ParamGen 着色器 CLEAN(SM6)。

**附带核实 3 个存疑参数类型：** SpriteTextureSampler 生成+编译均 OK；**FontSampleParameter 生成 OK 但 shader 报 `(Node FontSampleParameter) Missing input Font`** —— 字体采样节点固有需绑定 Font 资产，非编译器缺陷（DSL 暂无 Font 默认注入路径，记为已知局限）。

**再次印证 #15 教训：** 节点数对 + `get_material_compile_errors` CLEAN **双绿仍可能掩盖生成期 error**——失败的 Graph 赋值回退到可编译默认值，首轮 live 探针因此误判 DynamicParameter 无恙。深度验证须额外 grep 生成日志 `LogDreamShader: Error`。

## 建议落地顺序

1. 先建 **Generate 层测试网**（`bTransient=true`，断言生成成功/失败 + 错误串 + 节点形态）。
2. 把上述复现编成 Generate 夹具——现在它们应为 **RED**（复现 bug），形成回归基线。
3. 逐个修复 → RED 转 green。优先 **#2 / #7 / #6 / #1 / #3**（高频、高置信、修复局部）。
4. 修复在测试保护下进行，避免越改越乱。

## 全面开放问题审计（2026-06-30，34 条确认为真，多 agent 对抗式 + 人工 live 复核）

7 维度并行审计（参数生成 / 反编译器 / HLSL codegen / 测试覆盖 / 构建结构 / 写法健壮性 / 样例端到端），每条对当前代码核验 + 部分 live 复现。**核心结论**：happy path（Scalar/Vector 参数、核心 codegen、parse 层）健康，但测试网止步于"生成器返回 true"——**无 shader-compile 闸门、无渲染/像素闸门**，故 ~80% 问题为 latent（编译/渲染才暴露）。

### 本轮已修并验证（提交 e708bc5 + dedup 提交）

- **#1（blocker，每会话复现）反编译器多输出 Custom**：`BuildCustomExpressionCall` 对 OutputIndex>0 同时发 `Output="name"` + `OutputIndex=N`，生成器拒绝（互斥），`MF_MooaEncodeAttributes` 每次加载生成失败。修复：`BuildUEExpressionCallWithOutputType` 检测到 Arguments 已含 Output/OutputName 选择器时抑制自动 OutputIndex（OutputType 仍按真实 index 解析，不受影响）。验证：重反编译 .dsf 的 OutputIndex 5→0、Output 保持 5、重生成 exit 0 零错误。
- **#4-a CurveAtlasRowParameter 默认值中止**：它按输出分量数被归为 Vector，但派生自 ScalarParameter（DefaultValue 是 float），写 `(R=,G=,B=,A=)` 失败→整材质中止。修复：`SetExpressionDefaultValue` 检测反射 DefaultValue 为 FFloatProperty/FDoubleProperty 时写单通道。
- **#4-b TextureSample 族空贴图**：无默认值声明的采样参数 Texture==null → shader `Missing input Texture`。修复：`CreateGenericParameterNodeExpression` 在 AutoSetSampleType 前对 null-texture 的 TextureSampleParameter 调引擎 `SetDefaultTexture()`。验证：M_ParamFix 的 TextureSampleParameter2D 编译 CLEAN。
- **#9 import 行号偏移**：被剥离的 import 行无占位 → 诊断 mapper 按文件块计行，报错行号偏移 = 上方 import 数。修复：每个被剥离 import 补一空占位行。验证：顶层 parse error 现已 import-count 无关（修前 2 import 偏 2 行）。
- **#23 Settings 解析**：用朴素 `Split("=")` 而非其余 6 处的 quote/paren-aware `SplitTopLevelAssignment`；换用以保持一致。
- **#15 unity-build 重名隐患**：`SerializeJsonObject`/`BindAndExecute` 在 DiagnosticsStore.cpp 与 WorkspaceService.cpp 各有字节相同的匿名 ns 定义，同 blob 即重定义。修复：抽到共享 `DreamShaderEditorPersistenceUtils.h`（inline，单一真源）。

### 剩余高优先（按 reproducing > latent 正确性 > 覆盖/健壮 > 卫生）

1. **无 shader-compile 闸门（最高杠杆）**：`Tests/` 全树零 `GetCompileErrors`/`GetMaterialResource`/`ShaderMap`；`RecompileMaterial` 结果被吞（MaterialGenerator.cpp ~2523）。这是 #15/#16 及下列一打 latent 全部不可见的根因。建议：generate 层 helper 强制编译 + 迭代 `GetMaterialResource(SP_PCD3D_SM6,High)->GetCompileErrors()`，复用 bridge 已有回路（DreamShaderEditorBridge.cpp:586-615），挂 nightly `DreamShader.Gen.ShaderCompile.*`。修它一举解锁 ~10 条回归保护。
2. **无渲染/像素闸门**：`int(2.7)` 整数构造器是恒等空操作（CodeConstructors.cpp:187-197，无 Floor/Trunc，应走 `UMaterialExpressionTruncate`）；GLSL `mod(x,y)` 被词法替换为 HLSL `fmod`（DreamShaderParser.cpp:162 + MaterialGenerator.cpp:635 两份副本），负操作数符号不同（`mod(-0.3,1)` 给 -0.3 而非 0.7，应用 `x-y*floor(x/y)` 或注入 dreamMod helper）。二者均编译通过，只有 golden-image 像素比对能抓。
3. **参数簇剩余项**（接 #4，本轮只修了中止类的 2 个）：ChannelMaskParameter 必需 Input 悬空（`Missing mask input`，live 复现）；CurveAtlasRow/RVT/SVT/TextureCollection 缺必需资产 → 静默不可编译；TextureCollection/SVT 内联默认值中止并报误导信息；StaticComponentMask 静默丢弃 float4 默认值。建议：per-type 处理（缺资产时清晰报错而非静默；ChannelMask 拒绝为叶或自动接 Constant4Vector）。CurveAtlasRow/FontSampleParameter 须绑曲线/字体资产才能 shader-compile，属节点固有需求。
4. **参数 GUID 稳定性（推测、高影响）**：重生成是 clear-and-rebuild（MaterialGenerator.cpp:2063-2085），仅验证过 MaterialFunction Input/Output id 恢复，**未验证参数 ExpressionGUID**。若变 → Material Instance 覆盖静默断链（数据丢失）。建议回归测试：import → 记 GUID → 重 import → 断言不变 + MIC 覆盖仍绑定。
5. **stale-import 缓存（推测）**：跳过重生只哈希顶层 .dsm（GeneratedAssetMetadata.cpp:40-45），改 imported .dsh 而 .dsm 没动 → 旧材质被报 current。建议哈希传递闭包。
6. **诊断行号其余项**：本轮修了 import 偏移；残留 (a) mapper 常量 off-by-1（`TryMapPreparedSourceIndexToLocation` 边界 `SourceIndex <= LineEnd+1` 使 token 落行首时匹配上一行）；(b) 嵌套 body 'near index' 为 body-local 却按全局映射；(c) ~60 条 statement-level 错误无 'near index' 回退到 .dsm 路径无行列。
7. **EnsureTopLevelReturn 忽略 OutputType**（HlslFunctionCodegen.cpp:149）：无 top-level return 时注 `return 0.0;`，对 MaterialAttributes 类型 Custom 节点 HLSL 无法把标量 0 转 FMaterialAttributes → 硬编译错。应按 OutputType 注类型正确默认或抛清晰诊断。
8. **多 out Custom/Graph 函数调用**：次级 out-target 名可与 input 名冲突 → HLSL 重复形参（CodeCalls.cpp:444-450 仅查 out-target 间唯一）；应对照 Function.Inputs 名拒绝。
9. **WriteGeneratedInclude 全闭包转储**（HlslFunctionCodegen.cpp:1195，reproducing）：把整个 .dsh 所有函数写进每个材质 .ush（M_Luma 只用 Luminance 却 emit Remap/SinCos）。靠前缀+DCE 现无害，但故障耦合。应只发实际调用的传递闭包（复用 CollectDreamShaderFunctionCalls + CollectEmbeddedFunctionClosure）。
10. **off-thread 编辑器服务层未审计**：bridge(TCP)/websocket/file-watcher/SQLite 的 game-thread 亲和性与重入；生成走 Modify/PostEditChange/RecompileMaterial（仅 game-thread）；file-watcher 自触发循环；分别需聚焦审查。
11. **打包/cook 正确性未验证**；**native function 的 opt/默认参数被静默忽略**（解析了 bOptional/DefaultValueText，CodeCalls 执行器强制精确实参从不读）；**MaterialGraphLayout.cpp(1993) 等 6 文件 >1500 行**（卫生债）；**Roundtrip 测试仅重解析不重生成/比结构**；**多个已修 bug(#4/5/6/8/12) 及大量关键字/builtin 无回归夹具**。详见会话审计产物。

**落地建议**：先 #1 止血（已做）→ 建 compile 闸门（解锁一打回归）→ golden-image 闸门 → 参数簇 + 诊断清理。

### 参数输入/资源配置语法（2026-07-01，接审计 #4/#3 参数簇）

审计指出 Scalar/Vector 以外的参数无法从 DSL 配置其输入引脚/资源槽。现两条机制覆盖：
- **输入引脚** → Graph 调用形式 `Param(PinName = expr)`。`EvaluateConfigurableParameterCall`（CodeProperties.cpp）按名找输入引脚 + `ConnectCodeValueToInput` 接线；dispatch（CodeExpressions.cpp:1517）经 `ParameterTypeAcceptsInputArguments` 网关只对有输入引脚的参数类型（ChannelMask/StaticComponentMask/各 TextureSample）放行，value 参数与函数名查找不受影响（仿 StaticSwitch 调用路径）。例：`Msk(Input = Vec)`、`TexCube(Coordinates = dir)`、`TexVolume(Coordinates = uvw)`。
- **资源槽** → `[Prop = Path(...)]` 元数据（既有能力）。字面量写入器（MaterialLiteralPropertyWriter.cpp:165）已解析 `Path(...)` 到 FObjectProperty，`ApplyExpressionMetadata` 把 `[...]` 反射属性应用到节点。例：`FontSampleParameter F [Font = Path(...)]`、`TextureSampleParameter2DArray T [Texture = Path(...)]`、`CurveAtlasRowParameter C [Curve=Path(...); Atlas=Path(...)]`。
- **验证**：`M_ParamInputs`（cube/volume 坐标 + mask 输入接线）shader CLEAN（headless RHI + recompile_material + get_material_compile_errors 直读，0 error）。`M_AllParameters` 全 23 类型 demo 经接线后错误 12→8，残留 8 个纯属缺绑定资产（曲线图集/字体/数组贴图/RVT/SVT/纹理集），用上述元数据绑定真实资产即编译。回归测试 `DreamShader.Gen.Parameters.InputWiring`（断言引脚已连）+ `OtherNodeCreation`。套件 55 green。
