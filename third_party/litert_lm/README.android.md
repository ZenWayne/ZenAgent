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

## Verification

Each archive member / the `.so` must report `ELF 64-bit ... ARM aarch64`:

```bash
for f in lib/arm64-v8a/libce_*.a; do
  m=$(llvm-ar t "$f" | head -1); llvm-ar p "$f" "$m" | file -
done
file lib/arm64-v8a/libkissfft-float.so.131
```
