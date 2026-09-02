# arm64 归档重建(90f42140)— 交接文档

> 目的: 从 LiteRT-LM@90f42140(bf6197c9 移植)用 NDK 重建 arm64 预编译归档。
> 交接给他人/新会话接手。最后更新: 2026-09-01 · 接手人应通读本文件 + 背景文档
> `docs/90f42140-host-archive-rebuild.md`(host 侧,已完成,含链接验证问题链)。

## 0. 一句话现状

**fork 修复已落定并推送(`a6e69711`,见 §10),正在 §7 fresh 重建。** 历史实验提交
(4490dabe..bc8ac35b)已全部被 `2231992a` 回退;**正确解法 = `protobuf_BUILD_PROTOC_BINARIES=OFF`
+ patcher 注释掉 `upb_generators.cmake` include**(见 §10 对 P7/P8 的修正——本章旧文
P7 的 "PROTOBUF_BINARIES=OFF + 注释 protobuf_INSTALL" 修法**已被证伪**,不要采用)。

## 1. 目标与环境

| 项 | 值 |
|---|---|
| 模型/引擎 | LiteRT-LM `bf6197c9`(90f42140 + LLGuidance 移植,fork PR #1 已合) |
| 构建树 | `LiteRT-LM/cmake/build/android-arm64`(子模块内,preset 默认路径) |
| NDK | **r28b** = `$HOME/android-ndk/android-ndk-r28b`(clang 19.0.0,已下载解压;官方推荐版本,上游 CI/文档均 r28b) |
| 外部源 | tensorflow@`c143796c`、litert@`dc32e93f`(与 host 90f 构建同源,4 处放置: prebuild/main × tensorflow/litert) |
| 代理 | 下载必须 `http://127.0.0.1:10808`(直连 github 不稳,status 56) |
| 构建环境必需 | `ANDROID_NDK_HOME/ROOT=$HOME/android-ndk/android-ndk-r28b` + 代理 + **`CFLAGS/CXXFLAGS/CARGO_TARGET_AARCH64_LINUX_ANDROID_{C,XX}FLAGS=-D__ANDROID_API__=28`**(见 P4) |
| 日志 | `/tmp/opencode/arm64_90f_build.log` |

## 2. 任务链(当前进度)

1. ✅ 引擎移植合并(PR #1)+ 子模块 bump(bf6197c9)
2. ✅ host 归档重建 + 发布 `litert-lm-prebuilt-20260831`+ 链接验证(见 host 文档)
3. 🔄 **arm64 重建(本任务)**: 构建进行中 → split → 发布 → 重指 MODULE.bazel arm64 条目
4. ⏳ 开启 `AGENTFLOW_ENABLE_LITERTLM_TOKENIZER_API` → repro 套件 → 本地 deep-search 4 会话 teardown 验证(原始目标)

## 3. Fork 已提交修复(ZenWayne/LiteRT-LM,分支 `fix/ndk-r28b-preset` → **PR #2**)

**最终有效提交**(后续 protobuf 实验提交均已被 `2231992a` 回退,详见 §4 P6-P8):

| commit | 内容 | 解决的问题 |
|---|---|---|
| `ac6ab8e6` | CMakePresets: NDK r26d → **r28b**(3 处) | P3: NDK clang-17 对 absl 20260526 ICE |
| `fc885143` | protobuf_shim.cmake: `CMAKE_CXX_STANDARD "17"→"20"` | P2: absl C++20-only,protobuf 被强制 17 → `no type named weak_ordering` |
| `cf424b41` | sentencepiece.cmake: `Protobuf_PROTOC_EXECUTABLE=${LITERTLM_HOST_PROTOC}` | P5(部分): 生成用 host protoc |
| `2231992a` | protobuf 默认 knobs 恢复 + 完整问题链说明 | P6-P8 待 fork 重设计(见 §4) |

## 4. 问题链 & 本地 workaround(需要保留的)

### P1. 工具链参数被覆盖(fork 设计,待改进)
`litertlm_android.toolchain.cmake` 每次 configure **重建** `LITERTLM_TOOLCHAIN_ARGS`(CACHE INTERNAL,
仅含 android ABI/RUST/硬编码 `PROTOBUF_CXX_FLAGS_RELEASE=-O2 -DNDEBUG`),命令行
`-DLITERTLM_TOOLCHAIN_ARGS=...`(miniaudio 标志)被冲掉。
**本地绕行**: 内层重配注入(见 §5)。
**待改进**: fork 应合并用户提供的额外 toolchain args(如 `LITERTLM_EXTRA_TOOLCHAIN_ARGS`)。

### P2. absl 20260526 = C++20-only(已修,fork)
`absl/types/compare.h` 无条件用 `std::strong_ordering`;旧 absl 20260107 兼容 C++17。
host 用 GCC 16.1.1(支持)没事;arm64 曾用 NDK clang-17 + protobuf 被 FORCE 到 C++17 → 编译错+ICE。

### P3. NDK clang-17 vs absl ICE(已修,fork→r28b/clang 19)
NDK r26d 的 clang 17.0.2 对 absl 20260526 的 C++20 比较代码段错误(exit 139,即使 -std=c++20 也崩)。
r28b(clang 19)可正常编译(已验证单文件)。

### P4. Rust cxx cc-rs 缺 `__ANDROID_API__`(未修,env 绕过)
cxx crate 的 cc-rs 用 `--target=aarch64-linux-android`(无 API 级),clang 19 不定义
`__ANDROID_API__` → r28b libc++ `condition_variable.h:226` 的 `pthread_cond_clockwait` 与
bionic 声明守卫不一致 → `use of undeclared identifier`。
**必须**: 构建环境带 `CFLAGS/CXXFLAGS/CARGO_TARGET_..._CFLAGS=-D__ANDROID_API__=28`
(cc-rs 读 CFLAGS/CXXFLAGS;cargo 会缓存失败状态 → env 必须在**首次**编译前设或清 cargo 目录
`cmake/build/android-arm64/litert_lm/build/cargo`)。
**待改进**: fork 的 corrosion/rust 配置应显式传 API 级(不用 env 魔法)。

### P5. sentencepiece 用 arm protoc(半修,验证中)
sentencepiece 上游 `find_package(Protobuf)` 会把 `Protobuf_PROTOC_EXECUTABLE` 覆盖为
交叉编译的 arm protoc(cf424b41 的 -D 可能被覆盖)。配合 P6(binaries=OFF,arm protoc 根本
不存在)后若无 arm protoc 它只能 fallback 到 -D 的 host 值——**构建后验证 sentencepiece
生成是否通过**;若不通过需在 fork 的 sentencepiece.cmake 找 find_package 之后的注入点。

### P6. protobuf 反复重装 arm protoc(已灭,fork)
之前手工把 install 内 protoc 换成 host 版 → **每次构建 protobuf 的 install 步骤重装回 arm 版**
(install stamp 失效导致每轮重跑)。binaries=OFF 后不再产生/安装 arm protoc,根本解决。

### P7. protobuf install 目标丢失(根因已定位,fork 侧需要 patch 机制)
**根因(P6→P7 因果链闭环)**:
- protobuf 上游 `CMakeLists.txt:102-104`:`if (NOT protobuf_BUILD_PROTOBUF_BINARIES) set(protobuf_INSTALL OFF) endif()`
- 我(P6)fork 设 `protobuf_BUILD_PROTOBUF_BINARIES=OFF` → `protobuf_INSTALL` 被上游强制 OFF → **不生成 install 目标** → EP install 步骤报 `没有规则可制作目标 install`
- 若恢复 binaries=ON → protobuf 编译进到 **protoc-gen-upb 链接失败**(P8),也过不去

> **⚠️ 修正(2026-09-01,接手验证)**: P7 段落里 "正确修法" 提到的
> `PROTOBUF_BINARIES=OFF + 注释掉 set(protobuf_INSTALL OFF)` **不成立**——上游
> `CMakeLists.txt:277` 的 `if (protobuf_BUILD_PROTOBUF_BINARIES)` 门控的是**全部库**
> (utf8_range/libprotobuf-lite/libprotobuf/libprotoc/libupb),OFF 时进入 `else` 分支
> `find_package(Protobuf)`,什么都不会编。**正确解法见 §10 已实现提交 `a6e69711`**。

### P8. protobuf 二进制链接失败(arm64,待 fork 修)
binaries=ON 路径下,ARM 端 `protoc-gen-upb` 链接报
`undefined symbol: absl::lts_20260526::hash_internal::CombineLargeContiguousImplOn64BitLengthGt32`
等一批 absl::hash 符号——`libabsl_hash.a` 在 install 目录存在、fork 的
`absl_target_map.cmake` 也映射了 `absl::hash`,但 protobuf 的 upb 目标链接行没带上——fork 的
absl 链接 shim 对 protobuf-upb 的依赖注入不完整。**反正这些生成器二进制在交叉场景下
host 端不可运行、也不需要**,所以正确路线仍是 binaries=OFF(P7 的 PATCH_COMMAND 修法),
不必修 P8。

### P6-P8. protobuf 交叉生成器问题族(完整因果链,待 fork 侧重新设计)

**问题的本质**: protobuf 上游在 cross 构建下会编 **protoc + protoc-gen-upb**(纯 host 用途的
生成器)——它们(1)无法在 host 运行(交叉产物),(2)`protoc-gen-upb*` 在本 fork 的 absl 交付链下
**链接失败**(P8: `undefined absl::lts_20260526::hash_internal::Combine...` 等一批 absl::hash 符号;
libabsl_hash.a 存在、absl_target_map 有映射,但 libprotobuf/libupb 的传递 absl 集不含 hash)。
而"不编它们"的所有尝试都撞上上游耦合:

| 尝试 | 结果 |
|---|---|
| `protobuf_BUILD_PROTOBUF_BINARIES=OFF`(4490dabe) | ❌ 错选项!它门控**库**并让上游把 `protobuf_INSTALL` 置 OFF → install 目标全无(P7) |
| `protobuf_BUILD_PROTOC_BINARIES=OFF` | ✅ 正确的二进制门(protoc 不编、install 不受影响)——但 **upb 生成器不受它门控** |
| `protobuf_BUILD_LIBUPB=OFF` | ❌ libprotoc 的源列表**恒含** upb_generator/*.cc 且 `PUBLIC libupb`;OFF → 缺
  `descriptor.upb.h`(bootstrap 生成物)编译失败;且 sentencepiece 的 find_package 需要 libprotoc.a |
| patcher 注释 upb_generators 的 include | ❌ 破坏 libupb 的 export 集(`install(EXPORT)` 找不到 libupb → generate 失败) |
| patcher 把 upb 生成器 foreach 置空 + install 块置 FALSE | ⚠️ 接近正解,但 libupb 仍未进 export 集(上游
  `cmake/install.cmake` 的 libs 列表/export 组与生成器耦合,精确关系未穷尽) |

**结论/建议**: 这是 protobuf 上游 cross-build 的已知缺口 + fork 的 absl 交付不完整,需要
**fork 侧整体重设计**(不是补丁能修的): 候选方向
(a) 修 fork 的 absl 注入,让生成器能链接(补 hash 等缺的 absl 到 protobuf 的链接线);
(b) 让生成器**用 host 工具链编**(host-only 工具,prebuild 树已有完整 host protobuf——可考虑
    ARM protobuf 构建直接 SKIP 二进制、生成跑 prebuild 的 host protoc+host 生成器);
(c) 彻底检查上游 export 集与 upb 的精确耦合后,用 patcher 正确裁剪(仅删 add_executable,
    export 保留 libupb)。
其余部件已验证可用: 默认状态下 **libprotobuf/libprotoc/libupb 全部能编**(clang 19 + C++20),
仅生成器二进制链接失败——若能修好(fork 层),整链即通。

### P9. 每次 EP 会重置外部源(教训)
1. 对 `protobuf_external`/`tflite_external`/`litert_external` **源码的直接修改**会在 EP 的
   update/patch 步骤被重置(`git checkout/reset`),必须走 fork 的 `*_patcher.cmake` 机制。
2. 本树做过大量手工(swAP 二进制、touch stamp、手工重配)——**推荐 fresh 重建**(§7)规避所有残留。

## 5. 构建命令(恢复/重跑)

```bash
cd /home/wayne/tools/zen/LiteRT-LM
export ANDROID_NDK_HOME=$HOME/android-ndk/android-ndk-r28b
export ANDROID_NDK_ROOT=$HOME/android-ndk/android-ndk-r28b
export https_proxy=http://127.0.0.1:10808 http_proxy=http://127.0.0.1:10808
export CFLAGS="-D__ANDROID_API__=28"
export CXXFLAGS="-D__ANDROID_API__=28"
export CARGO_TARGET_AARCH64_LINUX_ANDROID_CFLAGS="-D__ANDROID_API__=28"
export CARGO_TARGET_AARCH64_LINUX_ANDROID_CXXFLAGS="-D__ANDROID_API__=28"

# configure(已是 r28b preset)
cmake --preset android-arm64

# [P1 绕行] miniaudio: 内层重配注入标志(每次 fresh configure 后要做一次)
cmake -S cmake/packages/litert_lm -B cmake/build/android-arm64/litert_lm/build \
  -DMINIAUDIO_NO_LIBVORBIS=ON -DMINIAUDIO_NO_LIBOPUS=ON

# build(长)
cmake --build cmake/build/android-arm64 -j20
```

## 6. 构建成功后(剩余步骤)

1. **split**(与 host 相同流程):
   ```bash
   AR=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar \
     third_party/litert_lm/scripts/split_archives.sh \
     LiteRT-LM/cmake/build/android-arm64/litert_lm/build \
     /tmp/opencode/arm64-dist          # 输出到临时目录(dist-host-90f 同款布局)
   ```
   (split_archives.sh 已有改进: staging `../tmp/lib`、绝对 BUILD_DIR、kissfft 移除——host 侧已验证 byte-identical)
2. **校验**: 成员 aarch64;`kLiteRtRuntimeBuiltin` 单 D 定义无 R/B;`litert_lm_engine_tokenize` 导出
   (90f 归档应导出,host 已确认)
3. **发布**: release tag 待定(参照命名 `litert-lm-prebuilt-arm64-...`),上传两个 .a
4. **重指**: MODULE.bazel arm64 http_file 条目 → 新 release URL + sha256(host 侧同款)
5. **宏 + 验证**: 置 `AGENTFLOW_ENABLE_LITERTLM_TOKENIZER_API=1` → repro 套件 → 本地
   deep-search 4 会话(原始 teardown bug 验证)

## 7. 若再失败: fresh 重建(推荐)

```bash
cd /home/wayne/tools/zen/LiteRT-LM
rm -rf cmake/build/android-arm64
# 重建 4 处外部源(本地克隆源在 host 树,快):
#   cmake/build/android-arm64/{prebuild/build,litert_lm/build}/external/{tensorflow/src/tflite_external,litert/src/litert_external}
#   git clone --local ../..host树的对应目录
# 然后 §5 的 configure + miniaudio 重配 + build
```
fresh 树 + 全部 fork 修复后,理论上只需 P4 的 env + P1 的 miniaudio 重配两个本地步骤。

## 8. 相关产物/链接- fork PR #2(NDK r28b + protobuf C++20 + sentencepiece host protoc + binaries OFF):
  https://github.com/ZenWayne/LiteRT-LM/pull/2
- host 侧背景: `docs/90f42140-host-archive-rebuild.md`
- host 归档发布: `litert-lm-prebuilt-20260831`;arm64(26df)旧发布: `litert-lm-prebuilt-arm64-20260831`
- ZenAgent 升级 PR #40(待 arm64 归档后补 repoint 提交): https://github.com/ZenWayne/ZenAgent/pull/40

## 9. fork 可执行 patch 草案(方向 a/b 混合: 让 upb 生成器正常链接)

**思路**: 不裁剪任何东西(避免 export 集耦合),只解决唯一失败点——protoc-gen-upb* 的
链接缺 `absl::hash`。fork 的 absl 交付(`absl_target_map.cmake` 有 `absl::hash` 映射、
`libabsl_hash.a` 在 install 目录)已提供 `absl::hash` IMPORTED target;
上游 `upb_generators.cmake` 的链接行只写了 `libprotobuf/utf8_validity/${protobuf_LIB_UPB}`,
从不显示列出 absl——传递链在本定制构建里断了,显式补上即可。

**改动 1 — `cmake/packages/protobuf/protobuf.cmake`**(一行,已验证过的正确旋钮):
把 CMAKE_ARGS 里的 `"-Dprotobuf_BUILD_LIBPROTOC=ON"` 之后加(或替换现有)一行:
```cmake
"-Dprotobuf_BUILD_PROTOC_BINARIES=OFF"   # 不编 protoc 二进制(host 运行不可 + 无用);
                                          # install.cmake 的 protoc 安装段以本旋钮为门,自动屏蔽
```
(注意: 不是 `PROTOBUF_BINARIES`——那是库门,OFF 会连带 `protobuf_INSTALL=OFF`;换 `PROTOC_BINARIES`。)

**改动 2 — `cmake/packages/protobuf/protobuf_shim.cmake`(或 patcher)**,给生成器链接行补 `absl::hash`。
在 `protobuf_patcher.cmake` 追加:
```cmake
# [LiteRTLM] P8: protoc-gen-upb* 链接失败 —— fork 的 absl 交付链未把 hash 传进它们
# (上游该链接行只写 libprotobuf/utf8_validity/libupb)。导入目标 absl::hash 已由
# fork 的 absl_config 提供,显式补进链接行。
# patch_file_content 签名: (file, match, replace, IS_REGEX) — 这里用字面量模式。
patch_file_content("${LITERTLM_PROTOBUF_SRC_DIR}/cmake/upb_generators.cmake"
    "utf8_validity"
    "utf8_validity\n    absl::hash"
    FALSE
)
```
> 逃生口: 若 `absl::hash` 名解析不到(命名空间/find_package 差异),备选=也加
> `absl::flat_hash_map`、`absl::hash_internal::low_level_hash`,或改用
> `string(REGEX REPLACE)` 的 `TRUE` 模式锚定 `utf8_validity[ \t\r\n]+` 后再插。

**改动 3(可选,防 sentencepiece 再取到 ARM protoc)** — 保持已有 `cf424b41`
(`Protobuf_PROTOC_EXECUTABLE=${LITERTLM_HOST_PROTOC}`);若构建后 sentencepiece 的
find_package(Protobuf) 仍覆盖(ProtobufConfig 带 protoc 路径时),追加传
`-Dprotobuf_generate_PROTOC_EXE=${LITERTLM_HOST_PROTOC}` 于 sentencepiece.cmake 的
CMAKE_ARGS(与 91/92 行同列表)。

**验证清单**(在 fresh 树上执行):
1. `cmake --preset android-arm64`(r28b)→ §5 miniaudio 内层重配
2. `cmake --build` → 观察 protobuf 段: libprotobuf/libprotoc/libupb 编过、
   protoc-gen-upb* **链接通过**(不再报 absl::hash 未定义)、install 完成
   (ProtobufConfig.cmake 出现)
3. sentencepiece 段: 生成用 host protoc(无 exit 126)
4. cxx crate: 需 P4 env(`-D__ANDROID_API__=28`)
5. 后续 litert/tflite/链接 → split/发布/重指(§6)

**若改动 2 仍链接失败**: 直接方向 (b) 硬解——运行生成器相关目标不编,改为
`PATCH_COMMAND` 仅把 `upb_generators.cmake` 顶部的 `foreach (generator upb upbdefs upb_minitable)`
改为 `foreach (generator NONE)`(只删可执行目标,**保留** include/export——此前失败是因为
错删了 include 与 install.cmake 的 libs 列表,本次只动 foreach 行),并把
`cmake/install.cmake` 里 61-67 行的生成器安装段(`foreach (generator upb ...)` 整块)
同样改空;不改 38-41 行的 libs 列表(`list(APPEND _protobuf_libraries libupb)` 必须保留,
否则 export 集错误)。

## 10. 已实现修复(2026-09-01,接手提交 `a6e69711`,PR #2)

**结论**: P7 的 "PROTOBUF_BINARIES=OFF + 注释 protobuf_INSTALL" 修法被证伪(它门控
全部库,见 §4 P7 修正注)。§9 的改动 2 草案(`absl::hash`)也**无效**——`absl::hash` 在
`absl_aggregate.cmake:44` 是 `LiteRTLM::absl::shim` 的 ALIAS,而 shim 是**空链接接口**
(只有 include 目录,无 INTERFACE_LINK_LIBRARIES);真正带 absl 库的是
`LiteRTLM::absl::absl`(INTERFACE_LINK_LIBRARIES=全部 absl .a 路径)。

**已提交的两个改动**(均在 fork `fix/ndk-r28b-preset`,已 push 到 PR #2):

1. `cmake/packages/protobuf/protobuf.cmake`: 加 `-Dprotobuf_BUILD_PROTOC_BINARIES=OFF`
   (保持 `PROTOBUF_BINARIES=ON`、`LIBPROTOC=ON`、`LIBUPB=ON` 默认)。
   - 效果: 只挡 protoc 可执行(及其 install.cmake 的 install/export 规则,58 行起);
     库与 install 目标不受影响;与 tflite_vars_shim.cmake:24 既有用法一致。
2. `cmake/packages/protobuf/protobuf_patcher.cmake`: 追加 patch 注释掉上游根
   CMakeLists.txt:297 的 `include(upb_generators.cmake)` → `#include(/cmake/...)`。
   - 效果: protoc-gen-upb* 目标不再生成(P8 链接失败消失);libupb 保留(libprotoc 依赖);
     install.cmake **完全不动**(libupb 在 libs 列表 → 安装+export;生成器 install 块已被
     PROTOC_BINARIES=OFF 门控,不产生悬空 install(TARGETS))。

**验证方式**: 对 pristine v35.1 clone 单独运行 patcher(`cmake -P`,传
`-DLITERTLM_PROTOBUF_SRC_DIR=...` 等)后 grep 确认: 根 CMakeLists 出现
`#include(/cmake/upb_generators.cmake)`,install.cmake 零改动。

**P8 对话记录修正**: P8 日志错误其实是**广泛 absl 符号缺失**(StrCat/Cord/Status/
log_internal/numbers_internal...),即生成器链接行根本没进任何 absl 库——不是"只有 hash"。
因为上游 `upb_generators.cmake` 链接行只写 `libprotobuf utf8_validity ${protobuf_LIB_UPB}`。
方案是让这些 host-only 生成器**根本不编**,而非补链接。

P5/P6 已被本修复连带解决: arm protoc 不再产生(P6),install 目录无 protoc 可供
find_package 覆盖(sentencepiece 用 `-DProtobuf_PROTOC_EXECUTABLE=${LITERTLM_HOST_PROTOC}`,
cf424b41 保留生效)。P1(miniaudio 重配)与 P4(`-D__ANDROID_API__=28` env)仍在,fresh 重建
按 §5/§7 执行。

**本修复后 fresh 重建的预期路径**(§7 + §5): configure(preset, r28b, 代理) →
miniaudio 内层重配 → build。protobuf 段应: 库编译+安装完成(ProtobufConfig.cmake
出现)、sentencepiece 用 host protoc 生成、无 protoc-gen-* 目标。

## 11. 后续修复(2026-09-01,构建中反馈迭代,PR #2)

调试过程中又修了三个问题(都已在 fork `fix/ndk-r28b-preset`):

| commit | 问题 | 修法 |
|---|---|---|
| `ac98a436` | **P5 真根因**: `protobuf_config.cmake` 的 phase override(host protoc)是 **no-FORCE CACHE set**——前面同一次 configure 里已用 no-FORCE 建了 ARM 路径条目,override 静默失效 → sentencepiece/tflite 的生成规则依赖不存在的 arm protoc(报 *no rule to make .../install/bin/protoc*) | override 三行加 `FORCE` |
| `210e7a14` | PROTOC_BINARIES=OFF **漏进了 prebuild(host)阶段**(protobuf.cmake 两阶段共用)→ host protoc 从未构建 → `LITERTLM_PREBUILD_PROTOC` 指向不存在的文件 | knob 按 phase 派生: prebuild=ON(供 host protoc), litert_lm=OFF |
| `4b9e3d10` | upb 生成器注释(prebuild 不需要?不——prebuild 也跑同一 patcher)→ prebuild 的 install.cmake 生成器安装块引用不存在的 protoc-gen-upb* → configure 报 *install TARGETS ... target not found* | upb comment-out 也按 phase 门控: 仅 litert_lm(cross);prebuild 保留生成器(host 可链接,host 90f 已证明) |

**经验教训(补 P1/P7/P9)**: 本 fork 的 protobuf/sentencepiece 包由 **prebuild 与
litert_lm 两阶段共用同一批 `.cmake` 文件**,任何 knob/patcher 改动都要检查是
否需要按 `LITERTLM_ORCHESTRATION_PHASE` 门控。工具链变量链(`LITERTLM_TOOLCHAIN_ARGS`
的 CACHE INTERNAL + no-FORCE)有两处静默失效,见 §10 的 pre-seed 绕行——
重新 configure 前把 `-DLITERTLM_TOOLCHAIN_ARGS="..."` 完整塞进(litert_lm 内层),
否则 EPs 拿到残缺的 ABI 参数。

**本地绕行(仅本机构建,非 fork 提交)**: litert_lm 内层 reinject 时必须
`-DLITERTLM_TOOLCHAIN_ARGS="$TA"`(完整 11 项,见 §10),否则 arm 侧
absl/gtest/protobuf/tflite/litert EP 全拿到只有 ABI 的残缺参数 → NDK 找不到
(报 `/build/cmake/android.toolchain.cmake`)。

## 12. 构建进展(2026-09-01 深夜会话,截至 Oracle 咨询前)

**已验证通过的段(arm64 fresh 树)**: 预构建 host 工具(absl/gtest/protobuf[含 protoc+生成器]/flatbuffers)→ arm absl → arm protobuf(libprotobuf/libprotoc/libupb,install OK,无 protoc-gen-* 目标,@P7/P8 修复生效)→ arm sentencepiece(host protoc 生成)→ tokenizers-cpp(含 Rust tokenizers crate,onig_sys 通过)→ tflite(ruy/fft2d/cpuinfo/XNNPACK/kleidiai 编出)→ litert **configure 通过**(host flatc 绕行)→ litert 构建至 93% 链到 link。

**当前阻塞(在调,Oracle 咨询中)**: litert 的 link 需要 tflite 库路径
`tflite_external/install/lib/libruy_*.a + libfft2d_{alloc,shrtdct,fft4f2d,fftsg3d}.a +
libcpuinfo_internals.a + libeight_bit_int_gemm.a`——实际产物位置不一致:
- ruy: `_deps/ruy-build/ruy/libruy_*.a`(33 个全在,但不在 install/lib)
- fft2d: `_deps/fft2d-build/` 只有 fftsg+fftsg2d(**其余 4 个库不存在**)
- install/lib 只有 5-6 个库(XNNPACK/kleidiai/pthreadpool/profiling...)
- fork 的 `tflite_target_map.cmake` 把 ruy/fft2d/cpuinfo 指向 install/lib
- `TFLITE_ENABLE_INSTALL=OFF`(fork 的 tflite.cmake),lite 自身 install 被跳过
- 旧 host 树的 install/lib 是满的(33+6),但其 _deps/fft2d-build 有全部 6 个 — 不能确定是
  当前 flow 产生(host 树是旧 flow 残留混合态)
- 两处 fft2d 源(host/arm)都是无根 CMakeLists 的 google/fft2d checkout

**⚠️ 本地 reinject 的完整要素(每改 fork 的 *_cmake 后必须重跑)**:
```
cmake -S cmake/packages/litert_lm -B cmake/build/android-arm64/litert_lm/build \
  -DCMAKE_TOOLCHAIN_FILE=<wrapper> -DCMAKE_SYSTEM_VERSION=28 -DCMAKE_ANDROID_NDK=$NDK \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_shared \
  -DANDROID_NDK_ROOT=$NDK \
  "-DLITERTLM_TOOLCHAIN_FILE=-DCMAKE_TOOLCHAIN_FILE=<wrapper>" \   # ← 必须是完整 -D 串!
  -DLITERTLM_TOOLCHAIN_ARGS="$TA" \                                # ← 完整 11 项(见 §11)
  ...(其余 LITERTLM_* 同 §5 列表)...
```
不传全 `LITERTLM_TOOLCHAIN_FILE`(-D 串)或 TA → 各 EP 拿残缺参数(NDK 找不到/C++ 标准
丢),表现为各种 /build 路径或 link 符号错。

## 13. 本次会话新增 fork 提交(均在 PR #2,fix/ndk-r28b-preset)

| commit | 内容 |
|---|---|
| `a6e69711` | protobuf: PROTOC_BINARIES=OFF(仅 cross)+ upb 生成器 include 注释(仅 cross,4b9e3d10 门控前的版本) |
| `ac98a436` | protobuf_config: phase override 加 FORCE(host protoc 真的生效,P5 根因) |
| `210e7a14` | protobuf: PROTOC_BINARIES 按 phase 派生(prebuild=ON 供 host protoc;litert_lm=OFF) |
| `4b9e3d10` | protobuf_patcher: upb 生成器注释按 phase 门控(cross only;prebuild 保留并安装生成器) |
| `a05bdab1``96aef1d5``f7f4e29a` | tokenizers: 显式传 ANDROID_TOOLCHAIN_ROOT + ANDROID_NATIVE_API_LEVEL(NDK r28b 无 API-less clang;API 级从 ANDROID_PLATFORM 派生;string() 不能放在 CMAKE_ARGS 列表内——会被当字面参数) |
| `20274a98` | tflite: 传 TENSORFLOW_SOURCE_DIR=预克隆树(跳过 ~GB 级 github FetchContent) |
| `54a164e4``2206e4cb` | litert: 传 TFLITE_HOST_TOOLS_DIR=host flatc 目录(跳过 fork 桩化的 host_flatc_build) |

**构建期重要经验**: tflite/litert 的 configure 若曾在"残缺参数"时期跑过,其 CMakeCache
会残留 `/build`/Android-1 等脏值 → 必须 `rm -rf` 对应 `*-build` + `*-stamp`(保留 src)后
重跑;litert EP 不依赖 tflite EP(并行 race),链接失败重跑即可收敛。

## 14. ✅ 完成(2026-09-02)

**arm64 归档重建成功并发布**。

- 构建完成: `Built target litert_lm`(arm64, NDK r28b, API 28)
- split: `/tmp/opencode/arm64-dist/` → `libce_staging.a`(340M)+ `libce_external.a`(1.17G)
- 校验(全过):
  - 成员 = ELF ARM aarch64(debug_info, arm64-v8a)
  - `kLiteRtRuntimeBuiltin`: **单一 `D` 定义**(external 归档,无 R/B;仅 1 处 U 引用)
  - `litert_lm_engine_tokenize` + 全套 tokenize/detokenize/result 导出 ✓
  - `litert_lm_*` 导出数 = **190**(与 host 归档一致)
- 发布: release **`litert-lm-prebuilt-arm64-20260902`**(ZenWayne/ZenAgent),两个 .a 已上传:
  https://github.com/ZenWayne/ZenAgent/releases/tag/litert-lm-prebuilt-arm64-20260902
  - staging sha256: `52ed0746398bdcff6855fdc81d481bbd6ca1e1f3a3845534dd3457f40a2b4d70`
  - external sha256: `6d0ed95bf5c16d05d30fa15133faa1f2b81e24439325b0b50e9b17c33817fb26`
- MODULE.bazel arm64 条目已重指(工作区改动,未提交——归 ZenAgent PR #40):
  URL → `litert-lm-prebuilt-arm64-20260902`,sha256 同上。

**最终 fork 提交清单(PR #2,fix/ndk-r28b-preset)**: a6e69711, ac98a436, 210e7a14,
4b9e3d10, a05bdab1, 96aef1d5, f7f4e29a, 20274a98, 54a164e4, 2206e4cb, 32dbece9,
a4a831eb, 40e0d33b(13 个)。

**三个"最后一公里"修复的核心**(构建期发现的 fork 缺陷):
| commit | 问题/修法 |
|---|---|
| `32dbece9` | tflite_target_map: ruy/fft2d/cpuinfo/gemmlowp 是 EXCLUDE_FROM_ALL 子构建,产物只在 _deps/*-build/,install/lib 永远没有 → 链路路径全部指向 _deps 实际位置;删 6 个从不构建也从不被链接的条目(fft2d_shrtdct/fft4f2d/fftsg3d/alloc、cpuinfo_internals、ruy::profiler_profiler) |
| `a4a831eb` | kleidiai 追加写错变量(`TFLITE_TARGET_MAP` 而非 `LITERTLM_TFLITE_TARGET_MAP`)→ libkleidiai.a 从未进入链接;XNNPACK 的 KleidiAI(SME)符号在 arm64 上被引用(x86 被编译裁剪,故 host 不炸) |
| `40e0d33b` | `-DTFLITE_ENABLE_GPU=OFF` 与 litert 的 Android-only `litert_tflite_gpu_gl_core`(无条件构建)冲突 → GPU 符号(SizeOf/IsPowerVR)未定义 → 改 ON |

**遗留(下一步,链第 4 项)**: `AGENTFLOW_ENABLE_LITERTLM_TOKENIZER_API=1` → Bazel
repro 套件 → 本地 deep-search 4 会话 teardown 原始 bug 验证。MODULE.bazel 已指向新
归档(未提交)。

## 15. App 侧验证修复(2026-09-02,zen_mobile 编译中发现)

arm64 JNI app 链接暴露了归档的**两个 final 缺陷**:

1. `166c769b`(fork): **re2 未 PIC**——re2 EP 是唯一没设 `-DCMAKE_POSITION_INDEPENDENT_CODE=ON`
   的包;非 PIC 对象使 `libagentflow_jni.so` 链接失败
   (`R_AARCH64_ADR_PREL_PG_HI21 cannot be used against symbol 'vtable for re2::...'`)。
   → 重新 split 后 `libce_external.a` sha256 变为
   `b3a3891d7185e802c915ecd9204b7bca44c885e089e1b88aff19785933933726`(staging 不变),
   已重新发布(clobber)+ 重指 MODULE.bazel。
2. **NDK 版本必须 r28b**: 归档的 antlr4 对象使用 libc++ **19 新异常 ABI**
   (`make_exception_ptr[abi:ne190000]`、`__cxa_init_primary_exception`),r26d 的
   libc++(17)不提供 → 链接报 undefined symbol。App 侧 Bazel android_arm64 构建
   的 NDK 从 r26d 切到 **r28b**(`.bazelrc`/AGENTS.md/build_jni_arm64.sh/MODULE.bazel
   注释同步更新);`@androidndk` repo(env 非追踪)需删除缓存重取。JNI .so 用
   **静态 libc++**(NEEDED 仅系统库,`__cxa_init_primary_exception` T 定义在 so 内)
   → APK 无需 libc++_shared,运行时 ABI 一致。

**最终验证链**(app 侧): JNI arm64 .so(NDK r28b,69.7MB)→ android-inference AAR →
zen_mobile APK(53MB,内部 so md5 与 bazel 产物一致,成员 arm64-v8a)。

## 16. 真机闪退:protobuf Map ABI 不匹配(2026-09-02,框架侧修复)

**症状**: 设备运行 JSON-workflow 推理(`native-token-st` 线程)SIGABRT:
`map.h:837] Check failed: num_elements_ <= CalculateHiCutoff(num_buckets_) (1 vs. 0)`。
**栈**: `WorkflowLoader::Load` → `WorkflowSpec::~WorkflowSpec` →
`google::protobuf::Map<string,WorkflowSpec_AgentDef>::~Map()` →
`KeyMapBase::AssertLoadFactor`(absl)。

**根因**: JNI .so 混入两种 protobuf——框架 Bazel 侧 pin **v31.1**,归档(90f42140)是
**7.35.1**(fork `v35.1`)。两者 `google::protobuf::Map` 内部布局(absl 表格)不同 →
Map 字段析构活在对方版本的解释下 → 检查崩。**与归档/引擎无关**(引擎运行时此前已通过)。

**修复**(commit `c9d1450` + `f049d69`,PR #40):
- MODULE.bazel: protobuf git_override v31.1 → **v35.1**(35cd01f9);Bazel 7.4.1 与
  protobuf v35.1 不兼容(`>= 8.0.0`)→ 用 **bazelisk 8.4.2** 构建;加 rules_java 8.6.1
  + 显式注册 `@local_jdk`(Bazel 8 移除隐式 repo;`local_jdk_extension` 在 8.6.1 中
  名为 `toolchains`)。
- proto/BUILD.bazel: `cc_proto_library` 改从 `@com_google_protobuf//bazel` 加载
  (Bazel 8 从 rules_cc 移除)。
- build_jni_arm64.sh: `bazel` → `bazelisk`(USE_BAZEL_VERSION=8.4.2)。

JNI 已重编(1581 actions,protobuf 35.1)并通过 arm64 链接;AAR/APK 已重建。
**遗留**: 设备在重装前断开(QV7808CA8G 无 USB)——接回后:
`adb install -r app-debug.apk && make start-inference-bc72` 复测。

### 复测结果(设备 QV7808CA8G,2026-09-02 15:1x)

**✅ Appium 推理套件通过**: `02_inference_streaming.test.js → TC-INF-001 1 passing (23.7s)`。
真机手动驱动也确认: 消息发送 → agent 完整回复(真实模型输出,无崩溃)。
期间踩到一个坑: 首次重装后 APK 内的 .so 仍是旧的(BuildID `477eb1ca` = protobuf 修复前)
——build_jni_arm64.sh 只在 12:53 跑过(修复前),protobuf 重编后只拷进了
android-inference/jniLibs,**app 的 jniLibs 副本漏了**(路径曾是 zen 内相对路径 bug)。
用 APK 内 .so 的 BuildID 与 bazel 产物对比定位;补拷 → 重建 APK → 复测通过。
**经验: 换 .so 后必须同时更新两处 jniLibs(android-inference 的 AAR 源 + zen_mobile/app),
并用 BuildID/md5 校验 APK 内实际装载的库。**
