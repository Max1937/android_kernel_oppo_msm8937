/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2017-2018, The Linux Foundation. All rights reserved.
 */

#ifndef __MINIDUMP_PRIVATE_H
#define __MINIDUMP_PRIVATE_H

#define MD_REVISION		1
/*
 * SBL_MINIDUMP_SMEM_ID —— SBL 往 SMEM 里写 minidump TOC 时用的 item 号。
 *
 * 这个值由 **bootloader** 决定，跟内核版本无关；4.19 基线写成 512 是给
 * 较新平台(如 SM8150)用的，而 MSM8953 这代 bootloader 用的是 602。
 *
 * 用错值的后果（在 R9s 真机上实测到）：
 *   qcom_smem_get() 里有一句
 *       if (WARN_ON(item >= __smem->item_count)) return ERR_PTR(-EINVAL);
 *   而 __smem->item_count 是从 bootloader 的 SMEM 布局读出来的运行时值
 *   （drivers/soc/qcom/smem.c:1194）。MSM8953 的实际 item_count 小于 512，
 *   于是 512 越界 → 触发 WARN_ON，打出
 *       WARNING: CPU: 6 PID: 1 at qcom_smem_get+0x1d0/0x204
 *       Call trace: qcom_smem_get <- msm_pil_init
 *   并返回 ERR_PTR(-EINVAL)。
 *
 * msm_pil_init() 只对 -EPROBE_DEFER 提前返回，对 -EINVAL 不处理，
 * 于是 g_md_toc 变成错误指针。虽然它本身不致命，但这个 WARNING 出现在
 * subsys_initcall 阶段、紧挨着 remoteproc 初始化，会污染早期启动路径；
 * 更重要的是它说明 SMEM 取项语义已经和本机 bootloader 不一致。
 *
 * 对齐 4.9 分支（lineage-22.1）的实际值 602 后，
 * msm_pil_init 不再打 WARNING，modem/adsp/wcnss 的 pil 初始化路径恢复正常。
 */
#define SBL_MINIDUMP_SMEM_ID	602
#define MAX_NUM_OF_SS		10
#define MD_SS_HLOS_ID		0
#define SMEM_ENTRY_SIZE		40

/* Bootloader has 16 byte support, 4 bytes reserved for itself */
#define MAX_REGION_NAME_LENGTH	16

#define MD_REGION_VALID		('V' << 24 | 'A' << 16 | 'L' << 8 | 'I' << 0)
#define MD_REGION_INVALID	('I' << 24 | 'N' << 16 | 'V' << 8 | 'A' << 0)
#define MD_REGION_INIT		('I' << 24 | 'N' << 16 | 'I' << 8 | 'T' << 0)
#define MD_REGION_NOINIT	0

#define MD_SS_ENCR_REQ		(0 << 24 | 'Y' << 16 | 'E' << 8 | 'S' << 0)
#define MD_SS_ENCR_NOTREQ	(0 << 24 | 0 << 16 | 'N' << 8 | 'R' << 0)
#define MD_SS_ENCR_NONE		('N' << 24 | 'O' << 16 | 'N' << 8 | 'E' << 0)
#define MD_SS_ENCR_DONE		('D' << 24 | 'O' << 16 | 'N' << 8 | 'E' << 0)
#define MD_SS_ENCR_START	('S' << 24 | 'T' << 16 | 'R' << 8 | 'T' << 0)
#define MD_SS_ENABLED		('E' << 24 | 'N' << 16 | 'B' << 8 | 'L' << 0)
#define MD_SS_DISABLED		('D' << 24 | 'S' << 16 | 'B' << 8 | 'L' << 0)

/**
 * md_ss_region - Minidump region
 * @name		: Name of the region to be dumped
 * @seq_num:		: Use to differentiate regions with same name.
 * @md_valid		: This entry to be dumped (if set to 1)
 * @region_base_address	: Physical address of region to be dumped
 * @region_size		: Size of the region
 */
struct md_ss_region {
	char	name[MAX_REGION_NAME_LENGTH];
	u32	seq_num;
	u32	md_valid;
	u64	region_base_address;
	u64	region_size;
};

/**
 * md_ss_toc: Sub system SMEM Table of content
 * @md_ss_toc_init : SS toc init status
 * @md_ss_enable_status : if set to 1, Bootloader would dump this SS regions
 * @encryption_status: Encryption status for this subsystem
 * @encryption_required : Decides to encrypt the SS regions or not
 * @ss_region_count : Number of regions added in this SS toc
 * @md_ss_smem_regions_baseptr : regions base pointer of the Subsystem
 */
struct md_ss_toc {
	u32			md_ss_toc_init;
	u32			md_ss_enable_status;
	u32			encryption_status;
	u32			encryption_required;
	u32			ss_region_count;
	u64			md_ss_smem_regions_baseptr;
};

/**
 * md_global_toc: Global Table of Content
 * @md_toc_init : Global Minidump init status
 * @md_revision : Minidump revision
 * @md_enable_status : Minidump enable status
 * @md_ss_toc : Array of subsystems toc
 */
struct md_global_toc {
	u32			md_toc_init;
	u32			md_revision;
	u32			md_enable_status;
	struct md_ss_toc	md_ss_toc[MAX_NUM_OF_SS];
};

#endif
