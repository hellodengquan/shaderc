# 基于编译产物 SPIR-V 的材质参数反射：优化等级对接口的影响

本文所有结论均来自本仓库的实际代码路径，并附带一个可在本机完整复算的
GLSL 材质示例。行号引用对应当前工作区（`task/210`，HEAD `0581495`）。

---

## 1. 背景与关键事实（代码路径）

引擎没有任何“专用反射 API”可用：shaderc 的公开头文件
（`libshaderc/include/shaderc/shaderc.h`、`shaderc.hpp`）只暴露编译/汇编
接口，不含任何反射函数。所谓“用编译出的 SPIR-V 建立材质参数面板”，本质上
就是引擎自行解析模块中的三类指令：

| 反射信息 | SPIR-V 指令 | 性质 |
|---|---|---|
| 名字（资源名、block 名、成员名） | `OpName` / `OpMemberName` | 调试信息（debug 2 段） |
| 绑定（descriptor set / binding） | `OpDecorate ... DescriptorSet/Binding` | 注解（annotation 段），非调试信息 |
| 成员布局 | `OpMemberDecorate ... Offset` 等 | 注解，非调试信息 |

这一区分决定了后面所有行为：**strip 调试信息只删名字，不删绑定/布局注解；
而死代码消除（DCE）可以把整条资源连同它的注解一起删掉。**

### 1.1 编译流水线

`libshaderc_util/src/compiler.cc` 的 `Compiler::Compile()`：

1. glslang 前端 `GlslangToSpv()` 生成**未经优化**的 SPIR-V
   （`options.disableOptimizer = true`，`compiler.cc:368-372`）。
2. 收集优化 pass 列表 `opt_passes`：
   - HLSL 源且开启 legalization（默认开）时先压入 `kLegalizationPasses`
     （`compiler.cc:384-388`）；
   - 再追加 `enabled_opt_passes_`（`compiler.cc:390-391`）。
3. 交给 SPIRV-Tools 优化，且始终带上
   `opt_options.set_preserve_bindings(preserve_bindings_)`
   （`compiler.cc:394-405`），随后还会跑一遍校验器
   （`spirv_tools_wrapper.cc:138-139`，`set_run_validator(true)`）。

### 1.2 三个优化等级各自做什么

`Compiler::SetOptimizationLevel()`（`compiler.cc:483-503`）：

- `Zero`（默认）：清空 pass 列表，**不调用优化器**。
- `Size`：若未请求调试信息，先 `kStripDebugInfo`，再 `kSizePasses`。
- `Performance`：若未请求调试信息，先 `kStripDebugInfo`，再
  `kPerformancePasses`。

`kStripDebugInfo` / `kSizePasses` / `kPerformancePasses` 在
`libshaderc_util/src/spirv_tools_wrapper.cc:148-168` 分别映射到
SPIRV-Tools 的 `CreateStripDebugInfoPass()`、`RegisterSizePasses()`、
`RegisterPerformancePasses()`。两个配方的完整 pass 名单见
`third_party/spirv-tools/source/opt/optimizer.cpp:183-268`，其中都多次
调用 `CreateAggressiveDCEPass(...)`；Size 配方还额外包含
`CreateEliminateDeadMembersPass()`（`optimizer.cpp:260`），Performance
配方**没有**这个 pass——这是两者在反射结果上出现差异的直接原因。

### 1.3 调试信息与 strip 的互斥逻辑

- `SetGenerateDebugInfo()` / `SetGenerateNonSemanticDebugInfo()` /
  `SetGenerateNonSemanticDebugSource()` 都会调用
  `RemoveStripDebugInfoPass()`，把已排队的 `kStripDebugInfo` 中和成
  `kNullPass`（`compiler.cc:459-481`）。因此 `-g` 与 `-Os/-O` 同时给出时，
  名字会保留，优化照常进行。
- 反过来，没有 `-g` 时，Size/Performance 一定会先跑
  `StripDebugInfoPass`，删除全部 `OpName`/`OpMemberName`/`OpString`/
  `OpSource`/`OpLine`（实现见
  `third_party/spirv-tools/source/opt/strip_debug_info_pass.cpp:73-111`）。
- glslang 对每个具名变量都无条件发 `OpName`
  （`third_party/glslang/SPIRV/SpvBuilder.cpp:3149-3150`），与是否 `-g`
  无关；`-g` 决定的是 `OpSource/OpLine` 等额外调试段以及名字能否在 strip
  后存活。

### 1.4 `-fpreserve-bindings` 到底保护什么

开关经 `shaderc_compile_options_set_preserve_bindings`
（`libshaderc/src/shaderc.cc:548-551`）→ `set_preserve_bindings`
（`compiler.cc:395`）→ `IRContext::set_preserve_bindings`
（`third_party/spirv-tools/source/opt/optimizer.cpp:729`）。

AggressiveDCE 初始化模块级活跃指令时，只对 `OpDecorate` 中的
**`DescriptorSet` 与 `Binding`** 两种装饰做保活
（`third_party/spirv-tools/source/opt/aggressive_dead_code_elim_pass.cpp:726-734`）：

```cpp
if (context()->preserve_bindings()) {
  // Keep all bindings.
  if ((... == spv::Decoration::DescriptorSet) ||
      (... == spv::Decoration::Binding)) {
    AddToWorklist(&anno);
  }
}
```

由此得到两条硬约束：

1. 它只保护带 `Binding` 装饰的描述符资源（UBO/SSBO/sampler/image 等）。
   **push constant 没有 `Binding` 装饰，不在保护范围内。**
2. 它保活的是“装饰 → 变量 → 类型”整条引用链，所以变量本身和
   `OpName`（若未 strip）会一起留下；但它不保护 block 的**成员**
   （成员消除由独立的 `EliminateDeadMembersPass` 完成，见 §4.3）。

---

## 2. 可复算的材质接口示例

### 2.1 着色器源码

`material.frag`：一个 UBO（含一个永不读取的成员 `unusedParam`）、一个参与
最终颜色的纹理、一个不参与的纹理、一个不参与的 storage image、一个不参与
的 push constant。

```glsl
#version 450

layout(set = 0, binding = 0) uniform MaterialParams {
    vec4 baseColor;
    float unusedParam;   // 永不读取
    float roughness;
} params;

layout(set = 0, binding = 1) uniform sampler2D usedTex;    // 参与颜色
layout(set = 0, binding = 2) uniform sampler2D unusedTex;  // 不参与颜色
layout(set = 0, binding = 3, rgba8) uniform readonly image2D unusedImage;

layout(push_constant) uniform PushBlock {
    float unusedPush;
} push;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 texel = texture(usedTex, vec2(0.5));
    outColor = params.baseColor * texel + vec4(params.roughness);
}
```

### 2.2 复算命令

工具用本仓库 `build/` 下编译出的 `glslc`、`spirv-dis`、`spirv-opt`、
`spirv-val`（SPIRV-Tools v2026.4）。

```bash
GLSLC=<repo>/build/glslc/glslc
DIS=<repo>/build/third_party/spirv-tools/tools/spirv-dis
OPT=<repo>/build/third_party/spirv-tools/tools/spirv-opt
VAL=<repo>/build/third_party/spirv-tools/tools/spirv-val

# 六个配置
$GLSLC -g -O0                 material.frag -o debug_O0.spv
$GLSLC       -Os              material.frag -o release_Os.spv
$GLSLC       -O               material.frag -o release_Operf.spv
$GLSLC -g -Os                 material.frag -o debug_Os.spv
$GLSLC -g -Os -fpreserve-bindings material.frag -o debug_Os_preserve.spv
$GLSLC       -Os -fpreserve-bindings material.frag -o release_Os_preserve.spv

# 反汇编 / 校验
for f in debug_O0 release_Os release_Operf debug_Os debug_Os_preserve release_Os_preserve; do
  $DIS --no-indent --raw-id $f.spv -o $f.spvasm
  $VAL $f.spv          # 六个模块全部 VALID
done
```

### 2.3 独立复算优化器配方（证明黑盒可重现）

shaderc 的优化只是 spirv-opt 的一组固定 pass。用未优化模块手工重放，产物与
glslc **逐字节相同**：

```bash
$GLSLC -O0 material.frag -o base_O0_nog.spv
$OPT --strip-debug -Os base_O0_nog.spv -o manual_Os.spv
cmp manual_Os.spv release_Os.spv            # 输出 IDENTICAL

$OPT --preserve-bindings --strip-debug -Os base_O0_nog.spv -o manual_pb.spv
cmp manual_pb.spv release_Os_preserve.spv   # 输出 IDENTICAL
```

---

## 3. 实测结果：六配置下的资源存活矩阵

| 资源（源码声明） | `-g -O0` | `-Os` | `-O`(perf) | `-g -Os` | `-g -Os` +preserve | `-Os` +preserve |
|---|---|---|---|---|---|---|
| `params` UBO (set0/b0)，整条 block | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `usedTex` (set0/b1) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `unusedTex` (set0/b2) | ✅ | ❌ | ❌ | ❌ | **✅ b2** | **✅ b2** |
| `unusedImage` (set0/b3) | ✅ | ❌ | ❌ | ❌ | **✅ b3** | **✅ b3** |
| push constant `push` | ✅ | ❌ | ❌ | ❌ | **❌** | **❌** |
| UBO 成员 `unusedParam`（offset 16） | ✅ 3 成员 | ❌ 被删 | ✅ 保留 | ❌ 被删 | ❌ 被删 | ❌ 被删 |
| `OpName` 条数（另有 `OpMemberName`） | ✅ 10（+4） | ❌ 0 | ❌ 0 | ✅ 5（+2） | ✅ 7（+2） | ❌ 0 |
| `OpSource/OpLine` 调试段 | ✅ | ❌ | ❌ | ✅ | ✅ | ❌ |
| 文件大小（字节） | 2664 | 812 | 820 | 2220 | 2420 | 972 |

逐配置说明：

- **`-g -O0`（调试包，零优化）**：优化器根本不运行，源码里声明的 4 个
  描述符 + push constant + 3 个 UBO 成员 + 全部名字都在。这是反射信息最
  完整的模块。
- **`-Os` / `-O`（发布包，无 `-g`）**：先 strip 名字，再 DCE。
  `unusedTex`、`unusedImage`、push constant 整体消失；只剩 b0、b1。
- **`-g -Os`（带调试信息的优化包）**：strip 被中和，`OpSource/OpLine` 和
  **存活资源的** `OpName` 保留；但 DCE 照样删掉 `unusedTex`/
  `unusedImage`/push，且它们的 `OpName` 因引用目标死亡而被一并清除
  （`aggressive_dead_code_elim_pass.cpp:894-907`）。实测对
  `unusedTex|unusedImage|push|PushBlock|unusedParam|unusedPush` 的名字
  匹配数为 **0**。
- **加 `-fpreserve-bindings`**：b2、b3 的变量、`Binding/DescriptorSet`
  装饰、（带 `-g` 时的）`OpName` 全部回来，且绑定号仍是 2、3；但 push
  constant 依旧被删，UBO 死成员依旧被 `-Os` 删除。

---

## 4. 名字与绑定各自的保证

### 4.1 绑定号（set/binding）的保证

- **优化器绝不重新编号 `Binding`/`DescriptorSet`。** DCE 只会整条删除或
  整条保留，从不改写装饰的数值。实测 `-Os -fpreserve-bindings` 产物中
  四条装饰为 `Binding 0/1/2/3`，与源码完全一致；未加 preserve 时只是
  少了 2、3 两条，留下的 0、1 数值不变。
- **未加 preserve 时绑定集合会收缩**：反射得到的是“实际存活的绑定”，
  编号可能出现空洞（本例只剩 {0,1}）。引擎不能假设 binding 连续。
- 绑定装饰不是调试信息，**不随 `-g` 有无而增减**，只随 DCE 存亡。
- glslang 对没有显式 set/binding 的描述符默认发 `DescriptorSet 0` /
  `Binding 0`（`third_party/glslang/SPIRV/GlslangToSpv.cpp:12494-12505`），
  自动绑定时还会叠加 `setShift*Binding` 的 base
  （`compiler.cc:297-305`）。因此两版本要拿到同一布局，必须使用完全相同
  的 auto-bind / binding-base / register 映射选项。

### 4.2 名字的保证

- `OpName` 属于调试信息：无 `-g` 的 Size/Performance 一定 strip，发布包
  里反射名字**不可用**，面板只能靠引擎自带的名字表（见 §5）。
- 带 `-g` 时名字保留，但仅对**存活的 id**有效；被 DCE 的资源其
  `OpName` 一起消失。即“调试信息能保留名字”不等于“能保留未使用资源的
  名字”。
- 入口点名字 `"main"` 来自 `OpEntryPoint` 的内联字符串，不是 `OpName`，
  所以即使 strip 后仍然存在（六个模块的 `OpEntryPoint ... "main"` 都在）。

### 4.3 UBO/SSBO 成员布局的保证（最容易踩坑）

实测 UBO `MaterialParams` 在各配置下的结构：

| 配置 | 结构 | 成员 offset |
|---|---|---|
| `-g -O0` / `-O` | `vec4, float, float`（3 成员） | 0, 16, 20 |
| `-Os`（含 `-g -Os`） | `vec4, float`（2 成员，`unusedParam` 被删） | 0, 20 |

要点：

1. **存活成员的字节 offset 保持源码值不变**（`roughness` 始终是 20），
   但**成员序号被压缩**（从成员 2 变成成员 1），`OpMemberName` 也按新
   序号重写（`eliminate_dead_members_pass.cpp:370-386, 388-...`）。
2. 该成员消除只存在于 **Size 配方**（含 `EliminateDeadMembersPass`）；
   Performance 配方没有此 pass，所以 `-O` 下 3 个成员全留。**同为发布
   包，`-Os` 与 `-O` 的反射结构可以不同。**
3. SSBO（Vulkan storage buffer）的结构被该 pass 标记为“完整使用”，不删
   成员（`eliminate_dead_members_pass.cpp:69-72`）。
4. 结论：引擎反射一律以 **`Offset` 字节偏移**为 key，禁止以成员下标为
   key。

### 4.4 结果 id 与入口点接口列表不具稳定性

- 同一资源在不同配置下的 `%N` 结果 id 不同（如 `usedTex` 在 `-O0` 是
  `%14`、在 `-Os` 是 `%13`）。引擎只能以名字/绑定做关联，不能缓存裸 id。
- DCE 还会改写 `OpEntryPoint` 的接口 id 列表，把不活跃的 Input/Output
  接口变量摘掉（`aggressive_dead_code_elim_pass.cpp:1064-1085`）。描述符
  本就不在该列表中；这一条影响的是顶点属性/渲染目标等 `Location` 接口。

### 4.5 特化常量不受 preserve-bindings 保护

DCE 另有独立的 `preserve_spec_constants` 开关
（`aggressive_dead_code_elim_pass.cpp:736-742`），shaderc 没有公开它，
默认 false。实测未使用的 `layout(constant_id=8)` 在 `-Os` 下被删除，
被使用的 `constant_id=7` 保留。引擎若要在面板上暴露特化常量，不能依赖
优化后的模块。

---

## 5. 如何发布共用同一份参数布局的两个版本

目标：调试版（带名字/源码信息）与发布版（strip、优化）在引擎里驱动同一套
参数面板、同一组 descriptor set/binding 与同一份成员 offset。

### 5.1 推荐做法：三份产物、一份“布局事实源”

1. **规范反射模块（layout canonical module）**

   用零优化 + 调试信息 + 绑定保活单独编一份，仅在构建期/工具链使用：

   ```bash
   $GLSLC -g -O0 -fpreserve-bindings material.frag -o material.reflect.spv
   ```

   它包含全部描述符（含未使用的 b2/b3）、全部 `OpName`、完整 UBO 成员与
   offset、push constant 范围。引擎（或离线工具）只解析这一份，生成
   `material.json`/材质 schema：名字 → (set, binding, 类型, offset)。

2. **运行时调试版**

   ```bash
   $GLSLC -g -Os -fpreserve-bindings material.frag -o material.debug.spv
   ```

   保留名字与源码行信息，且描述符集合与反射模块一致。

3. **运行时发布版**

   ```bash
   $GLSLC -Os -fpreserve-bindings material.frag -o material.release.spv
   ```

   strip 掉名字、做尺寸优化，但因 preserve-bindings，描述符集合与绑定号
   与调试版完全相同；运行时按 schema 里的 (set,binding) 绑定资源即可。

### 5.2 必须同时满足的编译条件

- **两个（三个）版本使用同一份 `shaderc_compile_options` 模板**：target
  env/SPIR-V 版本、宏、`SetAutoBindUniforms`、各类 `SetBindingBase*`、
  HLSL 的 `SetHlslRegisterSetAndBinding*`、`SetHlslOffsets` 等必须逐项
  一致。C API 可用 `shaderc_compile_options_clone()`（`shaderc.cc:398-404`）
  克隆后只改优化等级/调试开关，避免漏项。
- **两版本都加 `-fpreserve-bindings`**。这是让未使用描述符在发布版中
  依然可反射、且绑定号不产生空洞的唯一手段。注意它**不覆盖 push
  constant**：push constant 的范围（`OpMemberDecorate Offset` + 类型大小）
  必须从规范反射模块取，不能指望运行时模块。
- **面板成员布局按字节 offset 建立**，不要按成员序号，以兼容 `-Os` 的
  死成员消除（§4.3）。
- **不要在接口 block 里留“仅调试用”的死成员**：`-Os` 会删掉它，导致两
  版结构不一致；需要稳定 A/B 布局时让成员保持被引用，或显式填充。
- 运行时模块仍经过 SPIRV-Tools 校验器（`spirv_tools_wrapper.cc:139`），
  构建期应让校验失败直接中断发布。

### 5.3 为什么不能只发 `-g -Os` 一份

`-g` 能保住名字和 `OpLine`，但如 §3 所示，它**不阻止 DCE 删除未使用
资源**，也不阻止 Size 配方删除 UBO 死成员与 push constant。想让调试信息
同时承担“完整布局事实源”的职责，必须叠加 `-fpreserve-bindings`，并且
push constant/特化常量仍需另想办法（规范模块或引擎侧 schema）。

---

## 6. 即使开启调试信息，引擎仍须自行处理的差异

1. **未使用描述符的存亡**：`-g` 不保留 `unusedTex`/`unusedImage`；需要
   `-fpreserve-bindings`。引擎不能假设“带调试信息 ⇒ 接口完整”。
2. **push constant 永远可能被 DCE 删除**：没有任何 shaderc 开关能保活
   未使用的 push constant（preserve-bindings 不覆盖它）。其范围只能来自
   规范反射模块/外部 schema。
3. **UBO 成员序号 vs 字节 offset**：`-Os` 删死成员后序号重排、offset
   保留；引擎必须按 offset 反射。`-O` 与 `-Os` 在成员层面结果不同。
4. **名字仅对存活 id 存在**：带 `-g` 的优化模块里，被删资源的 `OpName`
   也没了；发布版（无 `-g`）则完全没有名字，必须用外部名字表。
5. **结果 id 跨配置不稳定**：禁止持久化或硬编码 `%N`。
6. **入口点接口列表会被重写**：Input/Output 的 `Location` 接口可能被
   摘掉，顶点属性/渲染目标反射要以规范模块为准。
7. **未使用特化常量被删**：`SpecId` 面板不能从优化模块反推。
8. **HLSL 与 GLSL 的 `-O0` 语义不同**：HLSL 默认开启 legalization，其
   pass 列表中包含多轮 `AggressiveDCEPass`（`optimizer.cpp:123-177`），
   即使 `-O0` 也会删除未使用资源。实测 HLSL 示例在 `-O0` 下
   `register(t1)` 的 `unusedTex` 已消失，加 `-fpreserve-bindings` 后才
   恢复为 `Binding 1`。而 GLSL `-O0` 不跑任何 pass，资源全留。跨源语言
   统一反射时，`-fpreserve-bindings` 对 HLSL 是必需品而非可选项。

---

## 附：关键代码索引

| 主题 | 位置 |
|---|---|
| 优化等级 → pass 映射 | `libshaderc_util/src/compiler.cc:483-503` |
| 调试信息中和 strip pass | `libshaderc_util/src/compiler.cc:459-481` |
| 优化器调用与 preserve_bindings 透传 | `libshaderc_util/src/compiler.cc:382-406` |
| PassId → SPIRV-Tools pass | `libshaderc_util/src/spirv_tools_wrapper.cc:148-169` |
| Size/Performance/Legalization 配方 | `third_party/spirv-tools/source/opt/optimizer.cpp:123-270` |
| preserve_bindings 仅保活 Binding/DescriptorSet | `third_party/spirv-tools/source/opt/aggressive_dead_code_elim_pass.cpp:726-734` |
| DCE 摘除入口点接口变量 | `third_party/spirv-tools/source/opt/aggressive_dead_code_elim_pass.cpp:1064-1085` |
| DCE 清除死亡 id 的 OpName | `third_party/spirv-tools/source/opt/aggressive_dead_code_elim_pass.cpp:894-907` |
| StripDebugInfo 删除范围 | `third_party/spirv-tools/source/opt/strip_debug_info_pass.cpp:73-111` |
| UBO 死成员消除与 offset 保留 | `third_party/spirv-tools/source/opt/eliminate_dead_members_pass.cpp:284-386` |
| glslang 默认 set/binding 装饰 | `third_party/glslang/SPIRV/GlslangToSpv.cpp:12494-12505` |
| glslang 无条件发 OpName | `third_party/glslang/SPIRV/SpvBuilder.cpp:3149-3150` |
| glslc 命令行 `-O/-Os/-O0/-g/-fpreserve-bindings` | `glslc/src/main.cc:345-346, 650-671` |
