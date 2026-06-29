# DreamShader 测试语料库 (Corpus)

数据驱动单元测试的固定 fixture 树。一个通用 runner（C++）在运行时枚举本目录、对每个源文件跑对应入口点、与同名 `.expected.json` 金样本比对。

> **加一个关键字 = 往对应 `<Layer>/` 目录丢一个 `.dsm/.dsf/.dsh`（可选配一个 `.expected.json`）。不写 C++、不重新编译。**

## 目录约定

```
Corpus/
└── Parse/              # 纯解析层 (FTextShaderParser::Parse), 快, 无资产 I/O
    ├── Lexical/        # 注释 / 字符串转义 / 字面量 / 括号平衡 ...
    ├── TopLevel/       # Shader / ShaderFunction / Namespace / VirtualFunction ...
    ├── Sections/       # Properties / Settings / Outputs / Inputs / Options / Graph
    └── Types/          # float/vec/int/bool 家族 / Texture* / MaterialAttributes / Substrate
```

后续层（暂未接入）会平行新增 `Generate/`、`Diagnostics/`、`Roundtrip/` 等子树，各配自己的 runner。

## 命名

| 形式 | 含义 |
|---|---|
| `<前缀>_<名字>.<ext>` | 正例。前缀编码层级：`L_`/`T_`/`S_`/`Ty_`。 |
| `<名字>.bad.<ext>` | 负例。runner 见 `.bad.` 默认期望 **解析失败**（即使没有 json）。 |
| `<同名>.expected.json` | 可选金样本。缺失时用默认期望（正例=解析成功，`.bad.`=解析失败）。 |

## `.expected.json` 字段（全部可选、声明式）

```json
{
  "entryPoint": "parse",
  "outcome": "ok",                                  // ok | error
  "errorContains": ["Unterminated block"],          // error 用例: 错误串子串(全部需命中, 大小写不敏感)
  "warningsContain": ["deprecated"],                // Definition.Warnings 子串
  "definition": {                                   // outcome=ok 时对 FTextShaderDefinition 的字段断言
    "name": "DreamMaterials/M_X",
    "settings": { "Domain": "UI" },                 // 经 TryGetSetting 比对: 键大小写不敏感, 值精确
    "outputDeclarations": 1,
    "outputs": 1,
    "materialFunctions": 1,
    "materialFunction0Kind": "ShaderFunction",      // ShaderFunction | ShaderLayer | ShaderLayerBlend
    "virtualFunctions": 0,
    "codeNotEmpty": true
  }
}
```

## 运行

编辑器内：`Tools > Test Automation`，筛 `DreamShader.Lang.Parse`。

命令行（headless）：

```
"F:\UnrealEngine\UE_Moon\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" ^
  "I:\UnrealProject_Moon\DEV_58\MoonEngineSample\MoonEngineSample.uproject" ^
  -ExecCmds="Automation RunTests DreamShader.Lang.Parse; Quit" ^
  -nullrhi -unattended -nopause -nosplash -log
```

## 更新金样本

跑测试时加 `-DreamShaderUpdateGolden`，runner 会用**实际解析结果**重写每个 `.expected.json`。
仅人工触发，写回后务必 **review diff** 再提交，避免把回归当成新基线接受。
