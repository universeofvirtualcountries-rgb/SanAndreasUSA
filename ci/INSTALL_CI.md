# Updating the Android CI workflow

The active workflow is `.github/workflows/build_android.yml`. The updated
[`build_android.yml`](build_android.yml) template also invokes the standalone
ARM source check. The GitHub App cannot update workflow files without
`workflows` permission. A maintainer can sync the active copy using either
option below. Until then, the active workflow runs the new host cases, the
CMake source-list guard and both NDK builds.

**Option A — GitHub web UI**

1. Open <https://github.com/universeofvirtualcountries-rgb/SanAndreasUSA>.
2. Open the active workflow and select *Edit this file* (create it if missing).
3. Path: `.github/workflows/build_android.yml`.
4. Paste the contents of `ci/build_android.yml` (skip the three `###` header lines),
   commit to `main`.

**Option B — git from a machine with a personal token**

```bash
mkdir -p .github/workflows
cp ci/build_android.yml .github/workflows/build_android.yml
sed -i '1,3d' .github/workflows/build_android.yml
git add .github/workflows/build_android.yml
git commit -m "ci: enable Android build"
git push
```

After that, every push and pull request runs:

1. `host-tests` — CMake/CTest, `parse_ide.py --self-test`, source-list checks,
   JSON validation and Zig compile checks for both ARM ABIs;
2. `android` (matrix) — NDK r27d, `arm64-v8a` and `armeabi-v7a`, `android-24`,
   `c++_static`, exports `libUSA1to1.so` and checks the symbol table;
3. `package` — merges both ABIs into **`USA1to1-full` → `USA1to1.zip`** containing
   `lib/<abi>/libUSA1to1.so`, `assets/` and `README_INSTALL.md`.

Artifacts: *Actions → Build libUSA1to1.so → latest run → Artifacts*.

Until the workflow is enabled, the repository is still verifiable locally:

```bash
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release && cmake --build build-host -j
./build-host/usa1to1_tests
python3 tools/parse_ide.py --self-test
ZIG=/path/to/zig tools/arm_compile_check.sh   # both ABIs, code-level
```
