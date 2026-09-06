# LiteRT-LM Android arm64-v8a prebuilt archives

The arm64 C-engine archives imported by `BUILD.bazel`
(`libce_staging.a`, `libce_external.a`, `libkissfft-float.so.131`) are
machine-built and **gitignored** (`/third_party/litert_lm/lib/`). They are
staged under `lib/arm64-v8a/`. This note documents how to reproduce them.

## Build

NDK **r26d** is required (the `android-arm64` preset hard-codes
`$HOME/android-ndk/android-ndk-r26d`).

```bash
export ANDROID_NDK_HOME="$HOME/android-ndk/android-ndk-r26d"
export ANDROID_NDK_ROOT="$ANDROID_NDK_HOME"
export https_proxy=http://127.0.0.1:10809 http_proxy=http://127.0.0.1:10809

cd LiteRT-LM
cmake --preset android-arm64                                   # configure
cmake --build cmake/build/android-arm64 -j"$(nproc)"           # build
# NOTE: CMakePresets.json has no buildPresets; `cmake --build --preset` fails.
```

### Out-of-band prerequisites (the build cannot fetch these itself)

The `tflite_external` and `litert_external` ExternalProjects use
`DOWNLOAD_COMMAND ""` — their sources must be pre-populated as depth-1 shallow
clones at the pinned SHAs *before* building, in **both** the host-prebuild and
the arm64 main build trees:

- tensorflow @ `862baf45439c742ac3a9d43e88088943bd3a582d` →
  `<build>/external/tensorflow/src/tflite_external`
- litert     @ `fb16353a648922cb6c67a8e9a7a9ebc946360ad2` →
  `<build>/external/litert/src/litert_external`

(`<build>` = `cmake/build/android-arm64/prebuild/build` for the prebuild phase
and `cmake/build/android-arm64/litert_lm/build` for the main phase.) The patch
step runs `git checkout -- . && git clean -df`, so each source dir must be a
git checkout at the pinned SHA.

### Toolchain fixes needed for the arm64 cross-compile

1. **Rust target**: corrosion needs the `aarch64-linux-android` rustup target.
   The configured TUNA mirror 404s on `rust-std`; install from the official
   server:
   `RUSTUP_DIST_SERVER=https://static.rust-lang.org rustup target add aarch64-linux-android`.
2. **miniaudio**: the optional `miniaudio_libvorbis`/`miniaudio_libopus`
   decoders need vorbis/opus headers absent from the NDK sysroot, and are NOT
   in the link payload. Configure the main build with
   `-DMINIAUDIO_NO_LIBVORBIS=ON -DMINIAUDIO_NO_LIBOPUS=ON`.
3. **re2 / absl_DIR**: `absl_aggregate.cmake` FORCE-clobbers the `absl_DIR`
   cache var to the string `"Abseil merged archive"`, which re2's
   `find_package(absl)` then fails on. re2 must be configured with
   `-Dabsl_DIR=<build>/external/abseil-cpp/install/lib/cmake/absl`.
4. **cxx / libc++**: cxx 1.0.149's generated `rust::Slice<T>::iterator` does not
   satisfy NDK r26d libc++'s `std::contiguous_iterator` (missing
   `pointer_traits` element type), failing the `contiguous_range` static_assert
   in the generated cxxbridge headers. Add `using element_type = T;` to the
   `Slice<T>::iterator` in the generated `corrosion_generated/.../*.h`
   (cxx.h, parsers.h, parsers.cpp).

## Stage + split

The two `.a`s are not emitted directly by the build; they are derived from the
`litert_lm_main` link line. Use the reproducible recipe:

```bash
AR=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar \
  third_party/litert_lm/scripts/split_archives.sh \
  $PWD/LiteRT-LM/cmake/build/android-arm64/litert_lm/build \
  third_party/litert_lm/lib/arm64-v8a
# then strip debug info to keep them small:
$ANDROID_NDK_HOME/.../llvm-strip --strip-debug lib/arm64-v8a/libce_*.a lib/arm64-v8a/*.so.131
```

`split_archives.sh` parses `CMakeFiles/litert_lm_main.dir/link.txt`:
- `libce_staging.a`  = union of all `staging/lib/*.a` component archives.
- `libce_external.a` = all other linked `.a` EXCEPT abseil (Bazel supplies it).
- `libkissfft-float.so.131` = the arm64 kissfft `.so` (Android SONAME is
  unversioned `libkissfft-float.so`; staged under the `.131` import name).
  NOTE: kissfft is not always emitted into the main link tree — if the split
  reports it cannot find `libkissfft-float.so.131`, keep the existing staged
  copy (it is unchanged across builds).

### Duplicate-member hazard (do not regress)

Several component archives contain MULTIPLE members with the same basename:
`libXNNPACK.a` has two `pack-lh.c.o` (only one defines `xnn_*_pack_lh_*`),
`libcpuinfo.a` has duplicate `cache.c.o` / `init.c.o`, and
`libtensorflow-lite.a` has 8 duplicated basenames (incl. the TFLite C API
core that defines `TfLiteIntArray*`, `TfLiteTypeGetName`,
`tflite::ErrorReporter::Report`). A naive `ar x <archive>` extracts all members
of an archive into one directory, so a same-named member silently OVERWRITES the
earlier one and its symbols vanish from the aggregate. This is what produced the
earlier too-small (130 MB) arm64 `libce_external.a` that failed the JNI link
with undefined `cpuinfo_*` / `xnn_*_pack_lh_*` / `TfLite*` symbols.

`split_archives.sh` now extracts each member INDIVIDUALLY by ordinal
(`ar xN <occurrence> <archive> <member>`) into a globally-unique path, so all
duplicate-named members survive. After the split, strip debug info; the arm64
`libce_external.a` should be ~144 MB (vs the host x86-64 reference's 155 MB).

### Bogus `kLiteRtRuntimeBuiltin` null def (do not regress)

`litert_runtime_no_builtin.cc.o` defines `kLiteRtRuntimeBuiltin` as a read-only
symbol with value 0 (i.e. `nullptr`); `llvm-nm` shows it as
`0000000000000000 R kLiteRtRuntimeBuiltin`. The REAL definition lives in
`litert_runtime_builtin.cc.o` as `D kLiteRtRuntimeBuiltin` (points at
`kBuiltinStruct`). Under the `-Wl,--allow-multiple-definition` linkopt the C
engine needs (see `BUILD.bazel`), the null `R` def can win over the real `D`
def, after which `litert::Environment::Create` aborts with:

```
Check failed: 'runtime_c_api == nullptr ? kLiteRtRuntimeBuiltin : runtime_c_api' Must be non-null
```

The host `litert_lm_main` `link.txt` never references the `no_builtin` object,
so the host `libce_external.a` only ever contained the real `D` def. The arm64
aggregation accidentally pulled in BOTH objects, reintroducing the null def.
`split_archives.sh` now excludes `litert_runtime_no_builtin.cc.o` (see
`EXCLUDE_OBJECT_BASENAMES`); that object carries only this one symbol, so
dropping it loses nothing. Verify after staging:

```bash
# Must show exactly one DEFINED def, of type D (non-null), and no R/B null def:
llvm-nm -A lib/arm64-v8a/libce_external.a | grep kLiteRtRuntimeBuiltin
# In the linked .so it must be DEFINED at a non-zero address (D), not undefined:
llvm-nm bazel-bin/jni/libagentflow_jni.so | grep kLiteRtRuntimeBuiltin
```

If a re-staged archive ever reintroduces the `R`/null def, either the exclude
list was bypassed or upstream renamed the object; re-add the new basename.

## Verification

Each archive member / the `.so` must report `ELF 64-bit ... ARM aarch64`:

```bash
for f in lib/arm64-v8a/libce_*.a; do
  m=$(llvm-ar t "$f" | head -1); llvm-ar p "$f" "$m" | file -
done
file lib/arm64-v8a/libkissfft-float.so.131
```

## Reproducing from a clean checkout
The arm64 LiteRT-LM build requires submodule-local CMake fixes captured in
`android-arm64-litertlm.patch`. Apply before building:
```
cd LiteRT-LM && git apply ../third_party/litert_lm/android-arm64-litertlm.patch
```
Then run `../build_jni_arm64.sh` (builds + verifies aarch64 + stages the .so).

## Host archives and -fPIC

The host `libce_external.a` (the `litert_ce_external` http_file in
MODULE.bazel) was built without `-fPIC`. Its re2 objects therefore carry
`R_X86_64_PC32` relocations against global symbols, and ld.gold refuses to
link them into a shared library:

```
requires dynamic R_X86_64_PC32 reloc against '...' which may overflow at
runtime; recompile with -fPIC
```

Nothing on-device is affected — the arm64 archive was already rebuilt this
way in 5bc421a — but it takes out the whole Kotlin JVM test layer
(`//kotlin`: HostToolBridgeTest, SmokeTest, WorkflowJsonTest), which loads
`libagentflow_jni.so`.

re2 is the only offender, so `scripts/rebuild_re2_pic_host.sh` rebuilds re2
alone with `CMAKE_POSITION_INDEPENDENT_CODE=ON` and swaps its objects into the
archive, instead of rebuilding LiteRT-LM (hours, and a 13 GB build tree).

The rebuilt archive is published as
`litert-lm-prebuilt-host-pic-20260904` and `litert_ce_external` already points
at it, so a fresh checkout links out of the box.

Verified 2026-09-04 against that release URL (not a local file): the host
`libagentflow_jni.so` links (x86-64) and `gradle test` in `kotlin/` is 6/6 green
with `MODEL_PATH` set.

Note when re-verifying: the `:test` task does not track the native `.so` as an
input, so Gradle reports UP-TO-DATE after a rebuild. Use `gradle cleanTest test`
or the run proves nothing.

If you rebuild the archive again, upload it to a **new** tag and update both
`urls` and `sha256`. Pointing the http_file at a local `file://` path is fine
for verification but must never be committed — it only resolves on one machine.
