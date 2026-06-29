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

## 巨石文件

| 文件 | 行数 |
|---|---|
| `MaterialAssetGeneration/...GeneratorSupport.cpp` | **6719** |
| `Decompiler/...GraphDecompiler.cpp` | 3972 |
| `MaterialAssetGeneration/...GeneratorCodeExpressions.cpp` | 2969 |
| `MaterialAssetGeneration/...GeneratorCode.cpp`(MaterialGenerator.cpp) | 2820 |
| `MaterialAssetGeneration/...GeneratorCodeCalls.cpp` | 2112 |
| `Parser/...ParserSections.cpp` | 1700 |
| `MaterialAssetGeneration/...GeneratorCodeParsing.cpp` | 1637 |
| `Bridge/...EditorBridge.cpp` | 1475 |
| `MaterialAssetGeneration/...GeneratorCodeUE.cpp` | 1403 |
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
