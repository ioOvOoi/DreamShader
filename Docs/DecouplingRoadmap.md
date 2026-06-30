# DreamShader 解耦路线图（低风险优先）

> 覆盖 10 个超大文件（合计约 2 万行），最紧迫的是 `DreamShaderMaterialGeneratorSupport.cpp`（**6719 行**）。所有目标都具备干净的「接缝」：要么是单一 namespace 下的 static 自由函数集合，要么是已跨多个 `.cpp` 分布的部分类（`FCodeGraphBuilder` 已分布在 `Code{Calls,Expressions,Parsing,UE}.cpp`）。

## 进度（2026-06-29，每刀均 compile+link+测试验证、单独提交）

Support.cpp **6866 → 4246 行**（抽出 2620 行，**~38%**），新增 6 个内聚文件：

| 刀 | 抽出内容 | → 新文件 |
|---|---|---|
| 1 | 5 个字面量解析器 `Parse{Scalar,Boolean,Integer,UnsignedInteger32,Vector}Literal` | `DreamShaderMaterialValueParsing.cpp` |
| 2 | `ResolveMaterialProperty`（材质属性名→枚举，含 Substrate/Moon 守卫） | ↑ |
| 3 | `TryResolveCustomOutputType` | ↑ |
| 4 | `TryResolveWorldPositionShaderOffset` | ↑ |
| 5 | `NormalizeEnumLookupKey` + `TryResolveEnumLiteral` | ↑ |
| 6 | **类型解析簇**（`TryGetComponentCountForOutputType`/`IsMaterialAttributesType`/`IsSubstrateMaterialType(+Supported)`/`TryResolveCodeDeclaredType`×4/`TryResolveOutputVariableComponentCount`×3/`TryResolveMaterialFunctionParameterType`/`ValidateOutputs`，435 行） | `DreamShaderTypeResolution.cpp` |
| 7 | **HLSL 函数 codegen 簇**（标识符分词/调用重写/`CollectEmbeddedFunctionClosure`/`PrepareCustomNodeCode`/`WriteGeneratedInclude` 等，**1234 行**，5 个簇内 static + 3 个 header 入口） | `DreamShaderHlslFunctionCodegen.cpp` |
| 8 | 图拆除（`ClearMaterialExpressions`/`ClearMaterialFunctionExpressions`，126 行；暴露 2 个共享 static） | `DreamShaderMaterialGraphTeardown.cpp` |
| 9 | 资产引用解析（`Path(...)` → object path，`TryResolveDreamShaderAssetReference` 等 6 函数，331 行；零纠缠） | `DreamShaderAssetReferenceResolution.cpp` |
| 10 | 表达式反射（`ResolveMaterialExpressionClass`/`FindMaterialExpressionArgumentProperty`/`IsMaterialExpressionInputProperty`，102 行；零暴露） | `DreamShaderMaterialExpressionReflection.cpp` |

**Support.cpp 6866 → 4246 行（抽出 2620 行，~38%），11 刀，6 个新内聚文件。** Phase 1（纯叶子簇）+ Phase 2 的可干净分离簇全部抽出。

**方法（Phase 2 的关键经验）**：一次 material-settings 的天真尝试因**双向纠缠**（簇既调留下的 static，又含被留下函数调用的共享 static）而编译失败、回退。此后改用**工作流先做完整依赖闭包映射**（双向核验：簇调用的 static 全在簇内或暴露；簇内函数无被簇外调用），再机械应用——graph-teardown / asset-ref / reflection 三刀均一次成功。

**剩余 ~4246 行 = 深度耦合的生成核心**：节点工厂（`CreateOwnedMaterialExpression`/`CreatePropertyExpression`/`CreateUEBuiltinExpression`）、图布局（`LayoutGeneratedExpressions` + 两个匿名命名空间）、材质设置应用、表达式元数据/参数写入、反射属性写入器（`SetMaterialExpressionLiteralProperty`）。这些**相互深度依赖、共享大量 static**——依赖映射工作流已无法找到下一个可干净分离的簇。进一步拆分**不再是字节级搬移，而是内部重构（打破依赖、抽接口）**，须在 golden-output 测试覆盖下有意识进行。其他巨石文件（GraphDecompiler 3972 / CodeExpressions / ParserSections / Workspace）同理。

## 进度（2026-06-30，扩展到 Support.cpp 之外的巨石文件，每刀 compile+link+51 测试验证）

把 Phase-1 的可干净分离簇推进到**全部五个可字节搬移的巨石文件**；新增 16 个内聚文件（14 cpp + 2 h），共抽出 **5787 行**（16 刀，全程 51/51 green）。**Support.cpp 6866→926（-86.5%）、CodeExpressions 3041→1807、CodeCalls 2162→1312、MaterialGenerator 2820→2547、CodeUE 1403→1277**——五个可搬移巨石全部下降，三个最大者已降为普通文件。**仅剩 GraphDecompiler 4134** 为结构性硬骨头（文件局部 inline 类，须先把类声明 hoist 进头 + 把 3000+ 行 inline 成员改 out-of-line 再拆——内部重构、非字节搬移，须 round-trip golden 覆盖下专门做）；MaterialGenerator/CodeUE 余下少量在大匿名 ns 或单个巨型函数内，收益渐小。**Support.cpp 6866→4246→2435→1123→926（累计 -86.5%）**——此前判为"不可字节级搬移的深度耦合生成核心"的论断，被图布局(#15)与表达式工厂(#16)两刀证伪：两者都是 needs-expose（暴露少量共享 static + 链接器迭代揪漏）即可干净分离。Support.cpp 已从 #1 万行巨石降到 926 行普通文件。

**本轮映射工作流已核实但未做的候选**（详见各自 verify）：
- **CodeExpressions swizzle 簇**（`TryResolveSwizzleChannelIndex`/`TryBuildOrderedSwizzleMask` 两 static + `CreateSingleChannelMask`/`CreateSwizzleExpression` 两成员，~220L）= **leaf-clean 但非连续**（散在 333-419 与 2335-2540，中间夹着不搬的成员），须 4 段分别剪切，留作下一刀。
- **GraphDecompiler metadata static 簇**（1796-1942，11 个格式化 static）= **entangled**：`BuildLiteralEnumArgument`/`AddParameterMetadata`/`AddTextureParameterMetadata`/`AddTextureSampleMetadata` 被簇外成员函数调用（1994-3110），搬出会留下未定义 static。
- **GraphDecompiler 整体 = 字节级搬移不可行（结构性）**：`class FDreamShaderGraphDecompiler`（:1377）是定义在 .cpp 匿名命名空间内的**文件局部类，所有成员 inline 写在类体里**（公开接口是薄包装 `FBridgeGraphDecompiler` :4101，持有一个 `FDreamShaderGraphDecompiler` 成员）。**类体无法跨 TU 拆分**——所谓"LayoutMetadata 成员簇"(2262-2497) 只是类体内一段 inline 成员/静态定义，搬不走。映射工作流 verify 把它误判为可搬的 member-clean（根本没识别出文件局部 inline 类）；人工 grep `class FDreamShaderGraphDecompiler` 揪出。**前置条件**：要解耦 GraphDecompiler，须先把类声明 hoist 进头文件、把 3000+ 行 inline 成员改成 out-of-line `Class::method` 定义，再拆——这是**内部重构（非字节搬移）**，须在 round-trip golden 测试覆盖下专门进行，不属本路线的低风险搬迁阶段。CodeExpressions/CodeCalls/CodeUE/MaterialGenerator 的 `FCodeGraphBuilder`/`FMaterialGenerator` 则是头文件声明的正常类，成员可自由跨 TU 安放（本轮已多次验证）。
- **Support 设置簇**（596-1122）= **needs-expose-large**：`ValidateSettings`/`ApplySettings` 调 `TryResolve{Domain,BlendMode,ShadingModel}`（63-79 留守 static）等多个，暴露面大；前序也因此回退过。
剩余 Support.cpp 926 行 = 设置/校验簇 + 字面量创建 + 注释/reroute/默认值，更内聚。

| 刀 | 文件 | 抽出内容 | → 新文件 | 类型 |
|---|---|---|---|---|
| 12 | CodeCalls 2162→2082 | 4 个 `FCodeGraphBuilder::Find{,Graph,Material,Virtual}FunctionDefinition`（名称→定义只读查找，79L） | `...GeneratorFunctionLookups.cpp` | 成员函数纯搬（类头提供链接，零暴露） |
| 13 | MaterialGenerator 2820→2654 | 诊断格式化簇（prepared-source index→file/line/col 映射 + parse/generate/code-block 错误格式化，7 函数 165L） | `...GeneratorDiagnostics.cpp` + `.h` | **needs-expose**：匿名 ns static 被同文件多处调用，搬到新 TU 后同文件调用变跨 TU →去 static、提升到 `Editor` 具名作用域、加头声明 |
| 14 | CodeUE 1403→1277 | 3 个 `FCodeGraphBuilder::TryResolve{Vector,Position}Transform{Basis,Target}`（字符串→引擎枚举，125L） | `...GeneratorTransformBasis.cpp` | 成员函数纯搬（零暴露） |
| 15 | **Support 4246→2435** | **图布局簇**（Support.cpp 990-2799，两个匿名 ns + `LayoutGeneratedExpressions`×2：收集表达式/依赖图/owner block/跨块 NamedReroute/region+explicit 布局注释/根节点定位，**1810L**） | `...MaterialGraphLayout.cpp`（1886L） | **needs-expose-small**：簇仅外依赖 1 个 static `CreateOwnedMaterialExpression`（去 static + 进私有头）+ 1 个常量 `FastLayoutExpressionThreshold`（随簇搬，原处删） |
| 16 | **Support 2435→1123** | **表达式工厂簇**（Support.cpp 1123-2428：纹理类型校验/元数据写入/`Create{Const,Parameter,GenericUE,UEBuiltin}Expression`/`CreatePropertyExpression`，**1306L**） | `...ExpressionFactory.cpp`（1379L） | **needs-expose**：去 static + 进私有头共 8 个共享符号——`TryResolvePropertyReference`/`CreateScalarLiteralExpressionEx`/`CreateVectorLiteralExpression`/`ResolveExpressionInputValue` + 5-arg `SetMaterialExpressionLiteralProperty` + `TryGetUEBuiltinArgument`/`ValidateUEBuiltinArgumentNames`/`TryResolvePositionOrigin`。后 3 个靠**链接器迭代揪出**（grep 漏了 UE-builtin 实参解析路径）。 |
| 17 | **Support 1123→926** | **反射字面量写入器**（`SetMaterialExpressionLiteralProperty` 两个重载，399-594，**196L**：按反射类型 bool/int/uint/float/enum/struct/object/asset 解析并写入 FProperty） | `...MaterialLiteralPropertyWriter.cpp`（267L） | **leaf-clean 零暴露**：已 header-declared，全部出依赖（`Parse*Literal`/`TryResolveEnumLiteral`/`TryResolveDreamShaderAssetReference`）皆在兄弟 TU 外部链接，无 file-local static 调用——映射工作流对抗 verify 直接 confirmed。 |
| 18 | **CodeExpressions 3041→2817** | **swizzle 簇**（2 个 static `TryResolveSwizzleChannelIndex`/`TryBuildOrderedSwizzleMask` + 2 个成员 `CreateSingleChannelMask`/`CreateSwizzleExpression`，**221L**） | `...CodeSwizzle.cpp` | **leaf-clean 但非连续**：3 段（333-419 / 2335-2380 / 2453-2540）分别剪切，中间夹 `MakeCodeValueReuseToken`/`AppendValues` 留守。2 个 static 仅被搬走的成员调用；成员调用方（`CoerceValueToType`/`EvaluateMemberAccess`/CodeCalls）经类头跨 TU 解析。零暴露。 |
| 19 | **CodeExpressions 2817→2609** | **字面量/限定名提取簇**（8 个成员 `TryFlattenQualifiedName`/`TryExtract{Text,Literal,AssetReference,Scalar,Integer,Boolean}*`/`IsDefaultArgument`，1118-1324，**207L**） | `...CodeLiterals.cpp` | **member-clean 零暴露**：全成员函数，出依赖仅 header-declared `Parse*Literal`/`TryResolveDreamShaderAssetReference`（兄弟 TU）；调用方（CodeCalls/CodeUE）经类头透明。 |
| 20 | **CodeExpressions 2609→2244** | **数学内建求值** `EvaluateMathBuiltinCall`（1449-1812，**364L**：abs/floor/frac/saturate/sin/cos/sqrt/normalize/lerp/clamp/min/max/pow/dot…→ UE 节点） | `...CodeMathBuiltins.cpp` | **needs-expose-small**：成员函数，唯一 file-local static 出依赖 `MakeCodeValueReuseToken`（去 static + 进私有头；多消费者，原则 #4 已预期）。 |
| 21 | **CodeExpressions 2244→2075** | **向量构造器簇**（4 个成员 `IsIntegerConstructorName`/`IsVectorConstructorName`/`GetConstructorComponentCount`/`EvaluateVectorConstructor`，1840-2007，**168L**） | `...CodeConstructors.cpp` | **member-clean 零暴露**：全成员，出依赖皆成员/inline-shared。 |
| 22 | **CodeExpressions 2075→1934** | **可复用表达式缓存簇**（5 个成员 `TryBuildReusableCallKey`×2/`BuildReusableExpressionToken`/`TryFindReusableExpressionValue`/`AddReusableExpressionValue`，**140L**） | `...CodeReuse.cpp` | **member-clean**：出依赖仅已暴露的 `MakeCodeValueReuseToken` + 成员变量 `ReusableExpressionValues`。 |
| 23 | **CodeExpressions 1934→1807** | **类型强制簇** `CoerceValueToType`×3 重载（**127L**：截断禁止掩码/标量 splat/AppendVector 增长） | `...CodeCoercion.cpp` | **member-clean 零暴露**：全成员，出依赖皆成员/inline-shared。 |
| 24 | **CodeCalls 2082→1312** | **材质/虚函数调用簇**（7 个成员 `Evaluate/Execute{Material,Virtual}FunctionCall`/`CreateAndConnectMaterialFunctionCallAsset`/`Execute/EvaluateMaterialFunctionCallAsset`，**767L**） | `...CodeFunctionCalls.cpp` | **needs-expose**：把 CodeCalls 顶部匿名 ns 三个共享 helper（`ApplyFunctionCallOutputType`/`BuildFunctionSourceArgumentList`/`IsSubstrateTypeUnsupportedForEngine`）整体提升到 Private 作用域 + 私有头（去匿名 ns + 去缩进），簇即可干净搬出。 |
| 25 | **MaterialGenerator 2654→2547** | **源加载簇**（`ResolveDreamShaderImportPath`/`LoadPreparedDreamShaderSourceRecursive`/`LoadPreparedDreamShaderSource`，**109L**：递归 import 内联 + 环检测 + header/function 文件规则） | `...SourceLoading.cpp` + `.h` | **needs-expose-small**：仅 `LoadPreparedDreamShaderSource` 被成员调用（提升到 Editor 作用域 + 新头）；另两个随簇留 static；出依赖皆外部（`FDreamShaderDependencyGraphService`/模块函数/FFileHelper）。 |

**#15 是迄今最大一刀**：此前被判为"深度耦合生成核心、不可字节级搬移"的 4246 行里，**图布局子系统（1810 行，最大单块）实为可干净分离**——只需暴露 1 个共享 static。经映射工作流（incoming caller 核验=0）+ **人工补查 outgoing static 依赖**（工作流 verify agent 漏报了 `CreateOwnedMaterialExpression`，靠手工 grep 1495/1558 抓到）。**unity-build 陷阱**：把 `FastLayoutExpressionThreshold` 同时留在 Support 又复制进新 TU → 同一 unity blob 内匿名 ns 合并、`constexpr` 重定义；原处删除即解（其唯一使用者已随簇迁走）。Support.cpp 总历程 **6866→4246（前序-38%）→2435（本轮再-43%，累计-65%）**。

**对抗验证教训**：映射工作流的 verify agent 把 MaterialGenerator 诊断簇误判为 "clean-byte-move"（它只查"外部 TU 调用者"=0），但**匿名 ns/static 函数的同文件调用者在搬出后即变成跨 TU 未定义符号**——这正是内部链接陷阱（指导原则 #4）。人工复核改为 needs-expose（去 static + 头声明）才正确。**成员函数（外部链接，类头声明）搬迁永远安全；匿名 ns/static 自由函数搬迁必须先查同文件调用者。**

**已核实但本轮未做的候选**：
- **CodeUE Substrate 簇**（`FSubstrateBuiltinDescriptor` 结构 + 表 + `FindSubstrateBuiltinDescriptor` + `IsSubstrateExpressionClass`，~61L）= needs-expose（`FindSubstrateBuiltinDescriptor` 被留在 CodeUE 的 `EvaluateUEBuiltinCall` 调用；且结构/表带 `#if DREAMSHADER_WITH_SUBSTRATE_BUILTINS` 守卫，须连结构一起进头）。
- **ParserSections trim 簇**（`GetTrimStartDelta`/`CountLinesBeforeIndex`/`AdjustRegionsForTrim`）= **entangled**：被 `ParseShaderBody`/`ParseMaterialFunctionBody` 调用且彼此交织，非干净叶子。
- **CodeExpressions swizzle/equivalence/reuse-token 簇**：`MakeCodeValueReuseToken`/`AreCodeValuesEquivalent` 是已知多消费者 static（见原则 #4 名单），须先暴露；adversarial verify 未跑完，暂不动。

## 巨石文件

| 文件 | 行数 |
|---|---|
| `MaterialAssetGeneration/...GeneratorSupport.cpp` | ~~6719~~ ~~4246~~ ~~2435~~ ~~1123~~ **926** |
| `MaterialAssetGeneration/...MaterialGraphLayout.cpp`(新, 图布局) | 1886 |
| `MaterialAssetGeneration/...ExpressionFactory.cpp`(新, 表达式工厂) | 1379 |
| `MaterialAssetGeneration/...MaterialLiteralPropertyWriter.cpp`(新, 反射写入) | 267 |
| `Decompiler/...GraphDecompiler.cpp` | 3972 |
| `MaterialAssetGeneration/...GeneratorCodeExpressions.cpp` | ~~2969~~ ~~2817~~ ~~2609~~ ~~2075~~ **1807** |
| `MaterialAssetGeneration/...GeneratorCode.cpp`(MaterialGenerator.cpp) | ~~2820~~ ~~2654~~ **2547** |
| `MaterialAssetGeneration/...GeneratorCodeCalls.cpp` | ~~2162~~ ~~2082~~ **1312** |
| `Parser/...ParserSections.cpp` | 1700 |
| `MaterialAssetGeneration/...GeneratorCodeParsing.cpp` | 1637 |
| `Bridge/...EditorBridge.cpp` | 1475 |
| `MaterialAssetGeneration/...GeneratorCodeUE.cpp` | ~~1403~~ **1277** |
| `Workspace/...WorkspaceService.cpp` | 1121 |

## 指导原则

1. **始终保持可编译。** UBT 自动 glob `Private/` 下所有 `.cpp`，新增翻译单元无需改 `Build.cs`。每一步都是「剪切函数体 + 私有头加声明」，由编译器与链接器验证。绝不改任何已导出符号签名。
2. **纯函数先行，图操作殿后。** 无 UObject/图状态依赖的纯逻辑立即拆（最低风险 + 新测试最易覆盖，可「先拆后测」）。创建/删除真实 `UMaterialExpression`、写资产、定位节点的「重逻辑」放最后，且**要求先有 golden-output 测试覆盖**。
3. **纯搬迁 vs 逻辑重构分离。** 本路线只做逐字节搬迁（behavior-preserving），god-function 内部的去重/表驱动改写一律标注「需先有测试」，留作后续，不在搬迁步骤里顺手做。
4. **内部链接陷阱。** static 自由函数有内部链接，被搬到别的 `.cpp` 会变未定义符号。规则：每个 static 与其调用者同文件搬迁，或提升为共享头 inline（已知多消费者：`MakeCodeValueReuseToken`、`IsIdentifierToken`、`IsSubstrateTypeUnsupportedForEngine`、`ApplyFunctionCallOutputType`、`CreateOwnedMaterialExpression`、`FindSubstrateBuiltinDescriptor`）。
5. **宏/版本守卫一致性。** `MOON_ENGINE`、`DREAMSHADER_WITH_SUBSTRATE_BUILTINS`、`DREAMSHADER_UE_VERSION_AT_LEAST` 散布多处，每个新文件保留相同 include（`DreamShaderVersionCompat.h`、`DreamShaderSettings.h`）。

## 三阶段与测试保护

- **阶段 1（纯函数切片，步骤 1-15 中的纯切片）：** 正确性由「编译+链接成功」基本证明，无运行期风险。可「先拆后测」，不阻塞。
- **阶段 2（反射/设置/数据库/清单）：** 构造瞬态 UMaterial 探针或写 SQLite/JSON，「编译过 ≠ 行为对」。需在搬迁前/同时落地特征化测试。
- **阶段 3（图构建/工厂/布局/反编译核心）：** 创建删除真实节点、产出 HLSL/图。**必须先有 golden-output 快照测试**再搬。

## 24 步执行序（低风险纯切片在前）

| 步 | 文件 | 动作（缩减行数） | 风险 | 需先有测试 |
|---|---|---|---|---|
| 1 | Support | 拆 `DreamShaderMaterialValueParsing`（枚举/字面量解析, ~430） | 极低 | 否 |
| 2 | Support | 拆 `DreamShaderAssetReference`（Path() 解析, ~340） | 极低 | 否 |
| 3 | Support | 拆 `DreamShaderHlslFunctionCodegen`（分词/调用重写/include, ~1110） | 低 | 否 |
| 4 | Support | 拆 `DreamShaderTypeResolution`（类型查询 + `ValidateOutputs`, ~430） | 低 | 否 |
| 5 | CodeExpressions | 拆 `...CodeLiterals`（AST 字面量/参数提取, ~240） | 极低 | 否 |
| 6 | CodeParsing | 拆 4 个解析叶子 + `FCodeExpressionParser` 整体（~660） | 低 | 否 |
| 7 | CodeUE | 拆 transform-basis + Substrate 描述符表（~210） | 低 | 否 |
| 8 | CodeCalls | 拆 `FindXxxFunctionDefinition` 查找簇（~90） | 低 | 否 |
| 9 | MaterialGenerator | 拆 `DreamShaderShaderTextUtils` + `...Diagnostics`（~605） | 低 | 否 |
| 10 | ParserSections | 拆 4 个叶子簇（类型 token/区域/布局/通用, ~940） | 低-中 | 否 |
| 11 | GraphDecompiler | 拆类型格式化 + 文本工具（~610） | 中 | 否 |
| 12 | CodeExpressions | 搬 `EvaluateMathBuiltinCall`（~365） | 低 | 仅搬 |
| 13 | CodeExpressions | 搬 reuse/swizzle/coercion（~810） | 低-中 | coercion 改动需测 |
| 14 | MaterialGenerator | 搬源加载/导入簇（~290） | 低 | 否 |
| 15 | Workspace | 拆启动器/Substrate 目录/反射纯函数（~525） | 低 | 否 |
| 16 | Workspace | 拆 SQLite 桥数据库 + 清单导出器（~510） | 低-中 | **是**（往返 + golden-JSON） |
| 17 | Bridge | partial-class TU 切分：路径/菜单/命令（~700） | 低 | 否 |
| 18 | Bridge | 抽编译诊断 + 源变更分类器为纯函数 | 中 | **是** |
| 19 | GraphDecompiler | 抽组件推断（228 行 god 函数, ~430） | 中 | **是**（golden） |
| 20 | GraphDecompiler | 抽材质设置/反射字面量/布局（~585） | 中 | **是** |
| 21 | CodeCalls | 搬 Custom 调用 + 资产调用簇（~1300） | 中 | **是**（golden-output） |
| 22 | Support | 搬反射属性写入器 + 材质设置（~760） | 中 | **是**（UMaterial 探针） |
| 23 | Support | 搬表达式工厂 + 图布局 + 图拆除（~3320） | 中 | **是**（golden） |
| 24 | CodeExpressions | 搬控制流/语句层（`ExecuteStatement`/`ExecuteIfStatement`, ~640） | 中 | **是** |

**步骤 1-4** 即把 Support.cpp 减约 2.3k 行且零风险（compile+link 即证明）。**步骤 23** 完成 Support.cpp 全拆（6719 → 约九个 280-1560 行文件）。

> 注：本路线只搬不改。god-function 的内部表驱动/去重重构（`EvaluateMathBuiltinCall` if 链、`CreateUEBuiltinExpression`、`GenerateMaterialFromFile` 重复输出绑定循环、`CompileExpressionValue` 705 行分发器等）留作搬迁完成后的下一阶段，且都需测试先行。
