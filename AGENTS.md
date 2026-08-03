# AGENTS.md

## Cursor Cloud specific instructions

This repository is a **Linux 6.12 kernel fork for the Alif Ensemble SoC**
(`mach-ensemble`, Cortex-A32 / 32-bit ARM, Thumb2 **XIP** kernel).

- `main` is an intentionally empty placeholder branch (see `README.md`). The
  actual kernel source lives on `v6.12-dev` (the primary development branch) and
  its feature branches. To build or work on real code, check out `v6.12-dev`
  (or the relevant feature branch) first.
- Cross-build environment variables: `ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-`.
  The startup update script installs the `gcc-arm-linux-gnueabihf` toolchain and
  the kernel build deps (`bc flex bison libssl-dev libelf-dev build-essential
  libncurses-dev codespell`), so no manual dependency install is needed.
- Official CI (`automation/Jenkinsfile`) sources the proprietary Alif Yocto musl
  SDK (`/opt/alif/apss-tiny/.../environment-setup-cortexa32hf-neon-poky-linux-musleabi`),
  which is **not** present in this environment. The Ubuntu `arm-linux-gnueabihf`
  (glibc) toolchain compiles the tree cleanly and is sufficient to verify builds;
  only use the Yocto musl SDK when producing final board-flashable artifacts.

### Build (matches `automation/Jenkinsfile`)

Run from a checkout of `v6.12-dev` (or a feature branch):

```
export ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
make -j"$(nproc)" devkit_e8_defconfig           # also: appkit_e8_defconfig, devkit_e6_defconfig, *_unicore_defconfig
make -j"$(nproc)" xipImage                       # -> arch/arm/boot/xipImage (XIP kernel)
make alif/ensemble/devkit/devkit-e8.dtb          # -> arch/arm/boot/dts/alif/ensemble/devkit/devkit-e8.dtb
```

`make distclean` between config switches (as CI does). There is no QEMU model for
this XIP `mach-ensemble` target, so end-to-end verification is a clean cross-compile
producing `xipImage` + the board DTB (plus lint below), not booting.

### Lint (CI gates, run per commit as in `automation/Jenkinsfile`)

```
# checkpatch (strict, with CI's ignore list)
git show --stat HEAD > /tmp/cp.txt; git show HEAD >> /tmp/cp.txt
./scripts/checkpatch.pl --strict \
  --ignore=COMMIT_LOG_LONG_LINE,NO_AUTHOR_SIGN_OFF \
  --ignore=NEW_TYPEDEFS,BAD_SIGN_OFF \
  --ignore=FILE_PATH_CHANGES,LONG_LINE_STRING \
  --ignore=LONG_LINE_COMMENT - < /tmp/cp.txt

# codespell on added lines
git show HEAD | grep "^+" | codespell -
```
