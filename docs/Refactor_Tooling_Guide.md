# AI 辅助重构工具使用指南

> 本文档记录为 Halcyon 项目本地安装/确认可用的静态分析与重构辅助工具，
> 以及在本项目（`D:\projects\XX\Halcyon`，CMake + Vulkan + C++）中的具体
> 使用方式。所有工具均为本地免安装云端依赖的开源工具，不涉及代码上传。

## 工具清单与安装状态

| 工具 | 状态 | 版本 | 用途 |
|---|---|---|---|
| clangd | 已预装（随 LLVM） | 22.1.8 | 精确引用查找、安全重命名（配合编辑器） |
| clang-tidy | 已预装（随 LLVM） | 22.1.8 | 静态检查：未使用参数、现代化建议、bug 模式 |
| clang-query | 已预装（随 LLVM） | 22.1.8 | 基于 AST 的自定义代码模式查询 |
| Graphviz (dot) | 已预装 | 12.2.0 | 配合 Doxygen 生成调用图/依赖图 |
| Doxygen | 新安装 | 1.18.0 | 生成类图、调用图、collaboration graph |
| lizard | 新安装（pip） | 1.24.0 | 函数级复杂度/行数统计与排行 |

未安装：`clang-rename`、`include-what-you-use`（Windows 上均需源码编译，
与本机 LLVM 22.1.8 版本匹配风险较高，暂用 clangd rename / clang-tidy
include-cleaner 替代）。

---

## 1. lizard —— 函数复杂度/行数排行（最简单，立即可用）

不依赖编译，直接扫描源码：

```powershell
# 全项目按圈复杂度排序，找出最该拆分的函数
lizard Source/ -l cpp --sort cyclomatic_complexity

# 只看排名前20，且过滤掉太短的函数
lizard Source/ -l cpp --sort cyclomatic_complexity -T nloc=30 | Select-Object -First 30

# 导出为 CSV 方便存档/对比重构前后差异
lizard Source/ -l cpp --csv > .tygpt\lizard_baseline.csv

# 只看某个高危文件
lizard Source/Renderer/Vulkan/HalcyonVulkanRenderer.cpp -l cpp --sort cyclomatic_complexity
```

输出每行含：`NLOC`（代码行数）、`CCN`（圈复杂度）、`token`（token数）、
`PARAM`（参数个数）、函数名。

**建议**：重构前先跑一次存档（如上 CSV），每个阶段结束后再跑一次，直接对比
数字下降，作为量化验证证据，比人工判断更客观。

---

## 2. compile_commands.json —— clangd / clang-tidy 的地基（必须先生成）

项目当前尚未生成过。用如下命令让 CMake 导出（只需跑一次配置，不需要跑完整
build）：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
# 如果没装 Ninja，用默认生成器也可以（VS 生成器同样支持这个开关）：
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

生成后会在 `build/compile_commands.json` 出现。之后 clangd/clang-tidy
就能拿到每个 `.cpp` 的真实编译参数（include 路径、宏定义等），分析结果才
准确。

---

## 3. clangd —— 精确引用查找 / 安全重命名（配合编辑器使用）

clangd 本身是 LSP server，不是命令行直接跑分析的工具，用法是配置编辑器：

- **VS Code**：装 `clangd` 插件，禁用内置 C++ 智能感知（避免冲突），插件
  会自动找 `build/compile_commands.json`。
- 之后可以直接在编辑器里：
  - 右键 "Find All References" 精确找到某个方法真实被谁调用（比文本搜索
    准确，能排除注释/字符串里的同名文本，能跨重载区分）。
  - 用 "Rename Symbol" 做安全重命名（例如给 `phase1IndirectDrawCountBuffer`
    改名，一键改完所有真实引用点）。

命令行方式验证单文件诊断：

```powershell
clangd --check=Source/Renderer/Vulkan/VulkanGpuSceneBuffers.cpp --compile-commands-dir=build
```

---

## 4. clang-tidy —— 自动检测未使用参数等问题

```powershell
# 检测"未使用参数"问题
clang-tidy -p build Source/Renderer/Vulkan/VulkanGpuSceneBuffers.cpp -checks="-*,misc-unused-parameters,bugprone-*"

# 更全面的检查（现代化建议 + 可读性 + 常见bug模式）
clang-tidy -p build Source/Renderer/Vulkan/HalcyonVulkanRenderer.cpp -checks="-*,modernize-*,readability-*,bugprone-*"
```

`-p build` 指向含 `compile_commands.json` 的目录。**注意**：不建议直接加
`-fix` 自动改代码，先看诊断列表人工确认，避免误改。

---

## 5. clang-query —— 自定义 AST 查询（定位特定代码模式）

适合"找出所有满足某个结构特征的代码"，比文本搜索更可靠（理解语法而非
纯文本）：

```powershell
clang-query -p build Source/Renderer/Vulkan/HalcyonVulkanRenderer.cpp
```

进入交互模式后，例如要找"所有含至少一个参数的函数声明"：

```
match functionDecl(has(parmVarDecl()))
```

学习曲线较高（需要写 AST matcher 语法），日常审查用 `rg`/`clang-tidy`
通常已经够用，clang-query 适合精确定位复杂结构（例如"所有直接调用某
Vulkan API 且不在特定条件块内的位置"）。

---

## 6. Doxygen + Graphviz —— 生成调用图/依赖图（可视化架构）

新开一个终端窗口（首次安装后当前会话 PATH 未刷新），或直接用全路径
`C:\Program Files\doxygen\bin\doxygen.exe`。

生成配置文件：

```powershell
doxygen -g Doxyfile
```

编辑 `Doxyfile` 关键项：

```
INPUT                  = Source
RECURSIVE              = YES
EXTRACT_ALL             = YES
EXTRACT_PRIVATE         = YES
EXTRACT_STATIC          = YES
HAVE_DOT                = YES
CALL_GRAPH              = YES
CALLER_GRAPH           = YES
CLASS_GRAPH             = YES
COLLABORATION_GRAPH     = YES
DOT_GRAPH_MAX_NODES     = 100
GENERATE_HTML           = YES
GENERATE_LATEX          = NO
OUTPUT_DIRECTORY        = docs_generated
```

生成文档：

```powershell
doxygen Doxyfile
```

生成完打开 `docs_generated/html/index.html`，可以看到：

- `Renderer::Impl` 的 include graph / collaboration graph —— 直观看到它
  和多少个类耦合。
- `recordFrame` 之类函数的 caller/callee graph —— 看清调用链深度。
- 全局 Class Hierarchy —— 确认继承关系是否合理。

**建议**：重构前跑一次存档截图，重构后（例如完成 `VulkanGpuSceneBuffers`
拆分）再跑一次对比，直观展示耦合是否确实降低。

---

## 推荐工作流顺序

1. `lizard Source/ -l cpp --csv > baseline.csv` —— 先留一份量化基线。
2. `cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON` —— 生成
   compile_commands.json。
3. 编辑器装 clangd 插件，开始日常引用查找/重命名。
4. 每个重构阶段提交前跑一次
   `clang-tidy -p build <改动文件> -checks="-*,bugprone-*,misc-*"`。
5. 阶段性里程碑（例如完成阶段4）跑一次 Doxygen 生成图，对比架构变化。
