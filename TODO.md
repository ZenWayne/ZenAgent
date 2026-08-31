# TODO

## 重建 arm64 归档以对齐上游快照

**状态**：待办，非阻塞。x86_64 路径（实际发布的容器镜像）已完全可复现。

### 问题

`third_party/litert_lm` 的 host 与 arm64 预置归档**来自不同的上游快照**：

| | 来源快照 | 构建日期 |
|---|---|---|
| host x86_64 | `LiteRT-LM@26df558b` | 2026-08-25 |
| arm64-v8a | 更早的状态（构建树已清除，无法追溯） | 2026-06-18 |

因此 release `litert-lm-prebuilt-20260825` **只发布了 host 那一对**。
`BUILD.bazel` 里 `--config=android` 分支仍指向 `lib/arm64-v8a/`，而该目录在干净
检出里不存在——**这条路径在引入 http_file 之前就是断的**，不是那次改动造成的。

六月那批 arm64 归档本身是**好的**（已验证：`litert_runtime_no_builtin` 已排除、
`cpuinfo_` 204 个定义、`xnn_` 5266 个、`TfLiteInterpreter` 50 个、539 种重复
basename 保留、确为 aarch64），只是与 host 不同源，故未发布。

### 要做什么

用 `LiteRT-LM@26df558b` 重跑 android-arm64 交叉编译，再用
`third_party/litert_lm/scripts/split_archives.sh` 聚合，上传到 release 并在
`MODULE.bazel` 加对应的 `http_file` 条目（照 host 那两条的写法）。

### 前置条件（截至 2026-08-31 的实测）

- ✅ NDK r26d：`$HOME/android-ndk/android-ndk-r26d`
- ✅ rustup `aarch64-linux-android` target 已装
- ❌ tensorflow / litert 预置源码不在。两者是 `DOWNLOAD_COMMAND ""`，必须**手工**
  克隆到 README.android.md 指定的 SHA，且要放进 prebuild 与 main **两棵**构建树
- ❌ 六月那次的 arm64 构建树已清除

### 已知难点

1. **磁盘**：host 构建树 9.6G，arm64 规模相当，加上两个大仓库克隆，需预留
   ~25G 以上。上次尝试时可用空间只有 9.7G。
2. **中途需要手工介入**：README.android.md 记载，corrosion 生成的 cxxbridge 头
   （`cxx.h` / `parsers.h` / `parsers.cpp`）里 `Slice<T>::iterator` 缺
   `using element_type = T;`，会在 NDK r26d libc++ 的 `contiguous_range`
   静态断言处失败。改的是**生成物**，无法脚本化，所以这不是一条能无人值守跑完
   的路。
3. README 还记了 re2 的 `absl_DIR`、miniaudio 的 `-DMINIAUDIO_NO_LIBVORBIS=ON`
   `-DMINIAUDIO_NO_LIBOPUS=ON`、以及 rustup 要换官方源（TUNA 镜像对
   `rust-std` 404）等若干绕行。

### 完成后

把 `BUILD.bazel` 里 `:android_build` 分支从 `lib/arm64-v8a/...` 改成
`@litert_ce_staging_arm64//file:...` 等，并删除本节。

---

## 补 asio 的 sha256

`MODULE.bazel` 里 asio 的 `http_archive` 没有 `sha256`，同样不可复现。
下载一次算出来填上即可，成本很低。
