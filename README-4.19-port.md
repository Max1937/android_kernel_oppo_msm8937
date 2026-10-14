# 4.19-test — OPPO MSM8937/MSM8953 内核 4.9 → 4.19 移植

本分支把 `bakasu-susfs-bpf`（4.9.337）上的 OPPO 机型支持，移植到
**Linux 4.19.325-cip136** 基线上。目标机型是 **OPPO R9s（16017 / 16027，
MSM8953 / 骁龙 625）**。

> ⚠️ **状态：内核可以编出来，但尚未在真机上验证开机。** 详见下方「已验证 / 未验证」。

## 基线来源

```
4.19 基线：https://github.com/imren0x/msm-4.19 @ mi8953/latest (b05d368e0e48)
          = LineageOS/android_kernel_qcom_sdm660 lineage-20
            + CIP 4.19.325-cip136 合并后的统一 MSM8953/8937/450 内核
```

以**全新根提交**导入，与 4.9 各分支历史相互独立（4.9 分支不受影响）。

基线本身已自带：

- 完整 MSM8953 SoC 支持（`ARCH_MSM8953` / `ARCH_SDM450` / `ARCH_MSM8937`）
- `drivers/kernelsu`（KSU_VERSION **32567**，非 4.9 分支的 BakaSU 35212）
- `techpack/xiaomi-msm8953`（小米机型支持）

所以本分支的工作重点是**把 OPPO 机型支持重新移植上来**，而不是从零引入 root 方案。

## 分支结构

| 提交 | 内容 |
|---|---|
| `import:` | 4.19.325-cip136 msm8953 基线（原样） |
| `ci:` | 构建工作流 `.github/workflows/build-4.19.yml` |
| `port(oppo):` | OPPO R9s 设备树 + 驱动移植 |
| `fix(oppo):` | 4.9 → 4.19 的 API 断点修正 |

## 构建

用 GitHub Actions（`.github/workflows/build-4.19.yml`）。

本分支不是默认分支，`workflow_dispatch` 需要工作流文件位于默认分支才会被
GitHub 索引，所以工作流另外提供了一条 **push 触发通道**：

```sh
# 改 ci/targets 决定要构建哪些目标，push 即触发
printf 'base\nR9s\n' > ci/targets
git add -f ci/targets && git commit -m "ci: build" && git push origin 4.19-test
```

`ci/targets` 里可选的目标：

| target | 说明 |
|---|---|
| `base` | 4.19 基线冒烟（`vendor/msm8953-perf_defconfig` + `vendor/mi8953.config`） |
| `R9s` | OPPO R9s（基线 + `vendor/oppo-16017.config`），产 AnyKernel3 zip |

> Release 默认 `draft: true`，不会自动对外发布。

### 本地构建

```sh
make O=out ARCH=arm64 \
     vendor/msm8953-perf_defconfig \
     vendor/mi8953.config \
     vendor/oppo-16017.config
make O=out ARCH=arm64 -j"$(nproc)" CC=clang \
     CLANG_TRIPLE=aarch64-linux-gnu- CROSS_COMPILE=aarch64-linux-gnu- \
     Image.gz-dtb
```

## ⚠️ `vendor/mi8953.config` 是必需项，不是可选项

`drivers/power/supply/qcom/qpnp-fg.c` 在**没有** `IS_ENABLED(CONFIG_MACH_XIAOMI_MSM8953)`
保护的情况下直接调用 `xiaomi_msm8953_mach_get()` / `XIAOMI_MSM8953_MACH_*`。
因此不合并 `vendor/mi8953.config`（即 `CONFIG_MACH_XIAOMI_MSM8953=y`）时，
**基线自身就编不过**：

```
error: implicit declaration of function 'xiaomi_msm8953_mach_get'
error: use of undeclared identifier 'XIAOMI_MSM8953_MACH_MIDO'
```

本仓库面向 OPPO、不启用任何小米机型，但该符号仍必须为 `y`，
好让 `qpnp-fg` 走 `mach_get() == UNKNOWN` 的分支。

## 移植过程中修掉的 4.9 → 4.19 断点

这些都用**真实内核构建系统**逐个目标编译验证过（不是推测）：

| # | 问题 | 修法 |
|---|---|---|
| 1 | 4.9 的 `<asm/uaccess.h>` 在 arm64 4.19 上不再传递 `<linux/uaccess.h>`，`copy_to_user` 等变成隐式声明 | 按 4.19 树惯例（1120 处）统一改 `<linux/uaccess.h>` |
| 2 | arm64 4.19 置了 `CONFIG_ARCH_HAS_SYSCALL_WRAPPER=y`，`include/linux/syscalls.h` 把 `sys_open/sys_write/sys_close` 放在 `#ifndef CONFIG_ARCH_HAS_SYSCALL_WRAPPER` 里 | 改用 `ksys_open()/ksys_write()/ksys_close()`（21 处 / 3 文件） |
| 3 | `smem_alloc()` 在 4.19 已移除，`<soc/qcom/smem.h>` 也已移走，`SMEM_ID_VENDOR1/2` 枚举被裁掉 | 改用 `qcom_smem_get(QCOM_SMEM_HOST_ANY, ...)`；item 号补进 `include/soc/oppo/oppo_smem_ids.h`（用编译器求值确认 VENDOR1=135、VENDOR2=136） |
| 4 | 4.19 的 `qpnp-fg.c` 缺 OPPO gauge 后端接口，`oppo_bq27541` 会 undefined symbol | 补回 `fg_oppo_register()`/`fg_oppo_unregister()` 及 4 处取值钩子 |
| 5 | `merge_config.sh` 的 `OUTPUT` 默认是 `.`，在 `O=out` 之外手工调用会把 `.config` 写进**源码树**，触发 `prepare3` 的 `.. is not clean` 检查 | 把 defconfig 与所有片段交给**一次 make 调用**，用内核自带的 `%.config` 规则合并 |

## 已验证 / 未验证

### 已验证（有构建产物为证）

- 4.19 基线在 CI 上编译通过
- OPPO 驱动逐个目标编译通过（`soc/oppo` ×3、`power/supply/oppo` ×2、
  `synaptics_oppo` ×5、`oppo_touchscreen` ×3、含 OPPO 钩子的 `qpnp-fg.o`）
- 完整 `Image.gz-dtb` 本地构建成功（20 MiB），OPPO 符号已链入 `vmlinux`
  （`bq27541`、`vooc`、`oppo_project`、`get_boot_mode`）
- 两个 OPPO DTB（`msm8953-mtp_16017.dtb`、`msm8953-mtp_16027.dtb`）编译成功，
  且其内容（`oppo16027jdi_r63452`、`oppo16017samsung_ea8064`、
  `oppo,bq27541-battery`、`oppo,stm8s-vooc`、`synaptics,s1302`）
  已确认嵌进 `Image.gz-dtb`

### 未验证（**没有真机证据，别当事实用**）

- **能否开机**：完全没有真机测试。4.19 与 4.9 的 SoC 级 `msm8953.dtsi`
  差异很大（968 行），下游驱动（显示、充电、WLAN）行为需实机确认。
- **面板**：据 `XuexGao/oppo-r9s-mainline` 的实机记录，真机是 **16027 / JDI
  R63452**（不是 16017 的 Samsung EA8064 AMOLED）。两个 DTB 都已编出，
  但没有实测点亮。
- **未移植的 4.9 驱动**（4.19 基线里没有，暂缓）：
  `BACKLIGHT_LM3697`（16027 背光）、`KTD2026_SLED`、`INPUT_FPC1020`（指纹）、
  `SND_SOC_AK4376`、`SND_SOC_TFA98XX`。这些在 `oppo-16017.config` 里以注释记录。
- **root 方案版本不一致**：基线自带 KernelSU `KSU_VERSION=32567`，
  而 4.9 分支用的是 BakaSU 35212 + SUSFS。管理器版本码需与内核对齐。
- **`vooc_charger.c` 的既有缺陷**（4.9 分支就有，非本次引入）：
  `vooc_poll_func()` 里 `data |= ...` 前未初始化；若干 `dev_info` 格式串
  与参数不匹配（`-Wformat-extra-args`）。只报警告，不阻断编译。

## 相关仓库

- 4.9 原树：`XuexGao/android_kernel_oppo_msm8937` @ `bakasu-susfs-bpf`
- 主线 bring-up（含权威硬件记录）：`XuexGao/oppo-r9s-mainline`
- 4.19 基线：`imren0x/msm-4.19` @ `mi8953/latest`
