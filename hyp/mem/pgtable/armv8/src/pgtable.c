// © 2021 Qualcomm Innovation Center, Inc. All rights reserved.
//
// SPDX-License-Identifier: BSD-3-Clause

// TODO:
//
// * Make this code thread-safe. Currently the calling code is required to take
// a lock before using these functions. The level reference counting needs to be
// implemented for modification operations, and RCU used for level deletions.
//
// * Fix misra for pointer type cast.
//
// * add more checks in API level. Like, the size should be multiple of page
// size.
//
// * use the only one category of level instead of two.
//
// * change internal function to use return type (value + error code)
//
// * add test case for contiguous bit.

#include <assert.h>
#include <hyptypes.h>
#include <string.h>
#if defined(HOST_TEST)
#include <stdio.h>
#endif

#include <hypconstants.h>
#include <hypcontainers.h>

#if !defined(HOST_TEST)
#include <hypregisters.h>

#include <log.h>
#include <preempt.h>
#include <thread.h>
#include <trace.h>
#else
#include <string_util.h>
#endif

#include <compiler.h>
#include <hyp_aspace.h>
#include <panic.h>
#include <partition.h>
#include <pgtable.h>
#include <spinlock.h>
#include <util.h>

#if !defined(HOST_TEST)
#include <asm/barrier.h>
#endif

#ifdef HOST_TEST
#define TEST_EXPORTED
#else
#define TEST_EXPORTED static
#endif

#include <platform_cpu.h>

#include "event_handlers.h"
#include "events/pgtable.h"

#define SHIFT_4K  (12U)
#define SHIFT_16K (14U)
#define SHIFT_64K (16U)

// mask for [e, s]
#define segment_mask(e, s) (util_mask((e) + 1U) & (~util_mask(s)))

// Every legal entry type except next level tables
static const pgtable_entry_types_t VMSA_ENTRY_TYPE_LEAF =
	pgtable_entry_types_cast(PGTABLE_ENTRY_TYPES_BLOCK_MASK |
				 PGTABLE_ENTRY_TYPES_PAGE_MASK |
				 PGTABLE_ENTRY_TYPES_INVALID_MASK);

#if defined(HOST_TEST)
// Definitions for host test

bool pgtable_op = true;

#define LOG(...) LOG_I(__VA_ARGS__, 5, 4, 3, 2, 1, 0, _unspecified_id)
#define LOG_I(tclass, log_level, fmt, a0, a1, a2, a3, a4, n, ...)              \
	LOG_##n((fmt), (a0), (a1), (a2), (a3), (a4), __VA_ARGS__)
#define LOG_0(fmt, ...)			LOG_V((fmt), 0, 0, 0, 0, 0)
#define LOG_1(fmt, a0, ...)		LOG_V((fmt), (a0), 0, 0, 0, 0)
#define LOG_2(fmt, a0, a1, ...)		LOG_V((fmt), (a0), (a1), 0, 0, 0)
#define LOG_3(fmt, a0, a1, a2, ...)	LOG_V((fmt), (a0), (a1), (a2), 0, 0)
#define LOG_4(fmt, a0, a1, a2, a3, ...) LOG_V((fmt), (a0), (a1), (a2), (a3), 0)
#define LOG_5(fmt, a0, a1, a2, a3, a4, ...)                                    \
	LOG_V((fmt), (a0), (a1), (a2), (a3), (a4))
#define LOG_V(fmt, a0, a1, a2, a3, a4)                                         \
	do {                                                                   \
		char log[1024];                                                \
		snprint(log, 1024, (fmt), (a0), (a1), (a2), (a3), (a4));       \
		puts(log);                                                     \
	} while (0)

#define PGTABLE_VM_PAGE_SIZE 4096

#define PGTABLE_TRANSLATION_TABLE_WALK_EVENT_EXTERNAL                          \
	(PGTABLE_TRANSLATION_TABLE_WALK_EVENT__MAX + 1)
#else
// For target HW

#if defined(NDEBUG)
// pgtable_op is not actually defined for NDEBUG
extern bool pgtable_op;
#else
static _Thread_local bool pgtable_op;
#endif

extern vmsa_general_entry_t aarch64_pt_ttbr_level1;

#endif

#if (CPU_PGTABLE_BBM_LEVEL > 0) && !defined(ARCH_ARM_FEAT_BBM)
#error CPU_PGTABLE_BBM_LEVEL > 0 but ARCH_ARM_FEAT_BBM not defined
#endif

#define PGTABLE_LEVEL_NUM ((index_t)PGTABLE_LEVEL__MAX + 1U)

typedef struct stack_elem {
	paddr_t		    paddr;
	vmsa_level_table_t *table;
	count_t		    entry_cnt;
	bool		    mapped;
	bool		    need_unmap;
	char		    padding[2];
} stack_elem_t;

typedef struct {
	uint8_t level;
	uint8_t padding[7];
	size_t	size;
} get_start_level_info_ret_t;

#if defined(PLATFORM_PGTABLE_4K_GRANULE)
// Statically support only 4k granule size for now
#define level_conf info_4k_granules

static const pgtable_level_info_t info_4k_granules[PGTABLE_LEVEL_NUM] = {
	// level 0
	{ .msb				      = 47,
	  .lsb				      = 39,
	  .table_mask			      = segment_mask(47U, 12),
	  .block_and_page_output_address_mask = 0U,
	  .is_offset			      = false,
	  .allowed_types		      = pgtable_entry_types_cast(
		       PGTABLE_ENTRY_TYPES_NEXT_LEVEL_TABLE_MASK),
	  .addr_size		= util_bit(39),
	  .entry_cnt		= (count_t)util_bit(9),
	  .level		= PGTABLE_LEVEL_0,
	  .contiguous_entry_cnt = 0U },
	// level 1
	{ .msb				      = 38,
	  .lsb				      = 30,
	  .table_mask			      = segment_mask(47U, 12),
	  .block_and_page_output_address_mask = segment_mask(47U, 30),
	  .is_offset			      = false,
	  .allowed_types		      = pgtable_entry_types_cast(
		       PGTABLE_ENTRY_TYPES_NEXT_LEVEL_TABLE_MASK |
		       PGTABLE_ENTRY_TYPES_BLOCK_MASK),
	  .addr_size		= util_bit(30),
	  .entry_cnt		= (count_t)util_bit(9),
	  .level		= PGTABLE_LEVEL_1,
	  .contiguous_entry_cnt = 16U },
	// level 2
	{ .msb				      = 29,
	  .lsb				      = 21,
	  .table_mask			      = segment_mask(47U, 12),
	  .block_and_page_output_address_mask = segment_mask(47U, 21),
	  .is_offset			      = false,
	  .allowed_types		      = pgtable_entry_types_cast(
		       PGTABLE_ENTRY_TYPES_NEXT_LEVEL_TABLE_MASK |
		       PGTABLE_ENTRY_TYPES_BLOCK_MASK),
	  .addr_size		= util_bit(21),
	  .entry_cnt		= (count_t)util_bit(9),
	  .level		= PGTABLE_LEVEL_2,
	  .contiguous_entry_cnt = 16U },
	// level 3
	{ .msb				      = 20,
	  .lsb				      = 12,
	  .table_mask			      = 0,
	  .block_and_page_output_address_mask = segment_mask(47U, 12),
	  .is_offset			      = false,
	  .allowed_types =
		  pgtable_entry_types_cast(PGTABLE_ENTRY_TYPES_PAGE_MASK),
	  .addr_size		= util_bit(12),
	  .entry_cnt		= (count_t)util_bit(9),
	  .level		= PGTABLE_LEVEL_3,
	  .contiguous_entry_cnt = 16U },
	// offset
	{ .msb				      = 11,
	  .lsb				      = 0,
	  .table_mask			      = 0U,
	  .block_and_page_output_address_mask = 0U,
	  .is_offset			      = true,
	  .allowed_types		      = pgtable_entry_types_cast(0U),
	  .addr_size			      = 0U,
	  .entry_cnt			      = 0U,
	  .level			      = PGTABLE_LEVEL_OFFSET,
	  .contiguous_entry_cnt		      = 0U }
};

#elif defined(PLATFORM_PGTABLE_16K_GRANULE)
#define level_conf info_16k_granules

// FIXME: temporarily disable it, enable it for run time configuration
static const pgtable_level_info_t info_16k_granules[PGTABLE_LEVEL_NUM] = {
	// FIXME: level 0 is not permitted for stage-2 (in VTCR_EL2), must use
	// two concatenated level-1 entries.
	// level 0
	{ .msb				      = 47,
	  .lsb				      = 47,
	  .table_mask			      = segment_mask(47U, 14),
	  .block_and_page_output_address_mask = 0U,
	  .is_offset			      = false,
	  .allowed_types		      = pgtable_entry_types_cast(
		       PGTABLE_ENTRY_TYPES_NEXT_LEVEL_TABLE_MASK),
	  .addr_size		= util_bit(47),
	  .entry_cnt		= (count_t)util_bit(1),
	  .level		= PGTABLE_LEVEL_0,
	  .contiguous_entry_cnt = 0U },
	// level 1
	{ .msb				      = 46,
	  .lsb				      = 36,
	  .table_mask			      = segment_mask(47U, 14),
	  .block_and_page_output_address_mask = segment_mask(47U, 36),
	  .is_offset			      = false,
	  .allowed_types		      = pgtable_entry_types_cast(
		       PGTABLE_ENTRY_TYPES_NEXT_LEVEL_TABLE_MASK),
	  .addr_size		= util_bit(36),
	  .entry_cnt		= (count_t)util_bit(11),
	  .level		= PGTABLE_LEVEL_1,
	  .contiguous_entry_cnt = 0U },
	// level 2
	{ .msb				      = 35,
	  .lsb				      = 25,
	  .table_mask			      = segment_mask(47U, 14),
	  .block_and_page_output_address_mask = segment_mask(47U, 25),
	  .is_offset			      = false,
	  .allowed_types		      = pgtable_entry_types_cast(
		       PGTABLE_ENTRY_TYPES_NEXT_LEVEL_TABLE_MASK |
		       PGTABLE_ENTRY_TYPES_BLOCK_MASK),
	  .addr_size		= util_bit(25),
	  .entry_cnt		= (count_t)util_bit(11),
	  .level		= PGTABLE_LEVEL_2,
	  .contiguous_entry_cnt = 32U },
	// level 3
	{ .msb				      = 24,
	  .lsb				      = 14,
	  .table_mask			      = 0U,
	  .block_and_page_output_address_mask = segment_mask(47U, 14),
	  .is_offset			      = false,
	  .allowed_types =
		  pgtable_entry_types_cast(PGTABLE_ENTRY_TYPES_PAGE_MASK),
	  .addr_size		= util_bit(14),
	  .entry_cnt		= (count_t)util_bit(11),
	  .level		= PGTABLE_LEVEL_3,
	  .contiguous_entry_cnt = 128U },
	// offset
	{ .msb				      = 13,
	  .lsb				      = 0,
	  .table_mask			      = 0U,
	  .block_and_page_output_address_mask = 0U,
	  .is_offset			      = true,
	  .allowed_types		      = pgtable_entry_types_cast(0U),
	  .addr_size			      = 0U,
	  .entry_cnt			      = 0U,
	  .level			      = PGTABLE_LEVEL_OFFSET,
	  .contiguous_entry_cnt		      = 0U }
};

#elif defined(PLATFORM_PGTABLE_64K_GRANULE)
#define level_conf info_64k_granules

// NOTE: check page 2416, table D5-20 properties of the address lookup levels
// 64kb granule size
static const pgtable_level_info_t info_64k_granules[PGTABLE_LEVEL_NUM] = {
	// level 0
	{ .msb				      = 47,
	  .lsb				      = 42,
	  .table_mask			      = segment_mask(47U, 16),
	  .block_and_page_output_address_mask = 0U,
	  .is_offset			      = false,
	  // No LPA support, so no block type
	  .allowed_types = pgtable_entry_types_cast(
		  PGTABLE_ENTRY_TYPES_NEXT_LEVEL_TABLE_MASK),
	  .addr_size		= util_bit(42),
	  .entry_cnt		= (count_t)util_bit(6),
	  .level		= PGTABLE_LEVEL_1,
	  .contiguous_entry_cnt = 0U },
	// level 1
	{ .msb				      = 41,
	  .lsb				      = 29,
	  .table_mask			      = segment_mask(47U, 16),
	  .block_and_page_output_address_mask = segment_mask(47U, 29),
	  .is_offset			      = false,
	  .allowed_types		      = pgtable_entry_types_cast(
		       PGTABLE_ENTRY_TYPES_NEXT_LEVEL_TABLE_MASK |
		       PGTABLE_ENTRY_TYPES_BLOCK_MASK),
	  .addr_size		= util_bit(29),
	  .entry_cnt		= (count_t)util_bit(13),
	  .level		= PGTABLE_LEVEL_2,
	  .contiguous_entry_cnt = 32U },
	// level 2
	{ .msb				      = 28,
	  .lsb				      = 16,
	  .table_mask			      = 0U,
	  .block_and_page_output_address_mask = segment_mask(47U, 16),
	  .is_offset			      = false,
	  .allowed_types =
		  pgtable_entry_types_cast(PGTABLE_ENTRY_TYPES_PAGE_MASK),
	  .addr_size		= util_bit(16),
	  .entry_cnt		= (count_t)util_bit(13),
	  .level		= PGTABLE_LEVEL_3,
	  .contiguous_entry_cnt = 32U },
	// offset
	{ .msb				      = 15,
	  .lsb				      = 0,
	  .table_mask			      = 0U,
	  .block_and_page_output_address_mask = 0U,
	  .is_offset			      = true,
	  .allowed_types		      = pgtable_entry_types_cast(0U),
	  .addr_size			      = 0U,
	  .entry_cnt			      = 0U,
	  .level			      = PGTABLE_LEVEL_OFFSET,
	  .contiguous_entry_cnt		      = 0U }
};
#else
#error Need to specify page table granule for pgtable module
#endif

static pgtable_hyp_t hyp_pgtable;
static paddr_t	     ttbr0_phys;

#if !defined(NDEBUG)
// just for debug
void
pgtable_hyp_dump(void);

void
pgtable_vm_dump(pgtable_vm_t *pgt);
#endif // !defined(NDEBUG)

#if defined(HOST_TEST)
// Private type for external modifier, only used by test cases
typedef pgtable_modifier_ret_t (*ext_func_t)(
	pgtable_t *pgt, vmaddr_t virtual_address, size_t size, index_t idx,
	index_t level, pgtable_entry_types_t type,
	stack_elem_t stack[PGTABLE_LEVEL_NUM], void *data, index_t *next_level,
	vmaddr_t *next_virtual_address, size_t *next_size, paddr_t next_table);

typedef struct ext_modifier_args {
	ext_func_t func;
	void	  *data;
} ext_modifier_args_t;

void
pgtable_hyp_ext(vmaddr_t virtual_address, size_t size,
		pgtable_entry_types_t entry_types, ext_func_t func, void *data);

void
pgtable_vm_ext(pgtable_vm_t *pgtable, vmaddr_t virtual_address, size_t size,
	       pgtable_entry_types_t entry_types, ext_func_t func, void *data);
#endif // defined(HOST_TEST)

static void
hyp_tlbi_va(vmaddr_t virtual_address)
{
	// FIXME: before invalidate tlb, should we wait for all device/normal
	// memory write operations done.
	vmsa_tlbi_va_input_t input;

	vmsa_tlbi_va_input_init(&input);
	vmsa_tlbi_va_input_set_VA(&input, virtual_address);

#ifndef HOST_TEST
	__asm__ volatile("tlbi VAE2IS, %[VA]	;"
			 : "+m"(asm_ordering)
			 : [VA] "r"(vmsa_tlbi_va_input_raw(input)));
#endif
}

static void
vm_tlbi_ipa(vmaddr_t virtual_address, bool outer_shareable)
{
	vmsa_tlbi_ipa_input_t input;

	vmsa_tlbi_ipa_input_init(&input);
	vmsa_tlbi_ipa_input_set_IPA(&input, virtual_address);

#ifndef HOST_TEST
#ifdef ARCH_ARM_FEAT_TLBIOS
	if (outer_shareable) {
		__asm__ volatile("tlbi IPAS2E1OS, %[VA]	;"
				 : "+m"(asm_ordering)
				 : [VA] "r"(vmsa_tlbi_ipa_input_raw(input)));
	} else {
		__asm__ volatile("tlbi IPAS2E1IS, %[VA]	;"
				 : "+m"(asm_ordering)
				 : [VA] "r"(vmsa_tlbi_ipa_input_raw(input)));
	}

#else
	(void)outer_shareable;
	__asm__ volatile("tlbi IPAS2E1IS, %[VA]	;"
			 : "+m"(asm_ordering)
			 : [VA] "r"(vmsa_tlbi_ipa_input_raw(input)));
#endif
#endif
}

#ifdef ARCH_ARM_FEAT_TLBIRANGE
static tlbi_range_tg_t
hyp_tlbi_range_get_tg(count_t granule_shift)
{
	tlbi_range_tg_t tg;

	switch (granule_shift) {
	case SHIFT_4K:
		tg = TLBI_RANGE_TG_GRANULE_SIZE_4KB;
		break;
	case SHIFT_16K:
		tg = TLBI_RANGE_TG_GRANULE_SIZE_16KB;
		break;
	case SHIFT_64K:
		tg = TLBI_RANGE_TG_GRANULE_SIZE_64KB;
		break;
	default:
		panic("Invalid granule size");
	}

	return tg;
}

// Find a (scale, num) pair for the requested range.
//
// The range covered by the TLBI range instructions is:
// ((NUM + 1) * (2 ^ (5 * SCALE + 1)) * Translation_Granule_Size
//
// Returns false if the requested size is bigger than the maximum possible range
// size (8GB for 4K granules) after alignment.
static bool
hyp_tlbi_range_find_scale_num(uint64_t size, count_t granule_shift,
			      uint8_t *scale, uint8_t *num)
{
	uint8_t	 calc_scale;
	uint64_t granules = size >> granule_shift;

	for (calc_scale = 0U; calc_scale <= TLBI_RANGE_SCALE_MAX;
	     calc_scale++) {
		count_t	 scale_shift = (5U * (count_t)calc_scale) + 1U;
		uint64_t aligned_granules =
			util_p2align_up(granules, scale_shift);
		uint64_t calc_num = (aligned_granules >> scale_shift) - 1U;
		if (calc_num <= TLBI_RANGE_NUM_MAX) {
			// Found a pair of scale, num
			*scale = calc_scale;
			*num   = (uint8_t)calc_num;
			break;
		}
	}

	return calc_scale <= TLBI_RANGE_SCALE_MAX;
}

static void
hyp_tlbi_va_range(vmaddr_t va_start, size_t size, count_t granule_shift)
{
	uint8_t num, scale;

	bool ret = hyp_tlbi_range_find_scale_num(size, granule_shift, &scale,
						 &num);

	if (ret) {
		vmsa_tlbi_va_range_input_t input;
		vmsa_tlbi_va_range_input_init(&input);
		vmsa_tlbi_va_range_input_set_BaseADDR(
			&input, va_start >> granule_shift);
		vmsa_tlbi_va_range_input_set_NUM(&input, num);
		vmsa_tlbi_va_range_input_set_SCALE(&input, scale);
		vmsa_tlbi_va_range_input_set_TG(
			&input, hyp_tlbi_range_get_tg(granule_shift));

#ifndef HOST_TEST
		__asm__ volatile(
			"tlbi RVAE2IS, %[VA]	;"
			: "+m"(asm_ordering)
			: [VA] "r"(vmsa_tlbi_va_range_input_raw(input)));
#endif
	} else {
		// Range is >8GB; flush the whole address space.
		__asm__ volatile("tlbi ALLE2IS" : "+m"(asm_ordering));
	}
}

static void
hyp_tlbi_ipa_range(vmaddr_t ipa_start, size_t size, count_t granule_shift,
		   bool outer_shareable)
{
	uint8_t num, scale;

	bool ret = hyp_tlbi_range_find_scale_num(size, granule_shift, &scale,
						 &num);
	if (ret) {
		vmsa_tlbi_ipa_range_input_t input;
		vmsa_tlbi_ipa_range_input_init(&input);
		vmsa_tlbi_ipa_range_input_set_BaseADDR(
			&input, ipa_start >> granule_shift);
		vmsa_tlbi_ipa_range_input_set_NUM(&input, num);
		vmsa_tlbi_ipa_range_input_set_SCALE(&input, scale);
		vmsa_tlbi_ipa_range_input_set_TG(
			&input, hyp_tlbi_range_get_tg(granule_shift));

#ifndef HOST_TEST
#if defined(ARCH_ARM_FEAT_TLBIOS)
		if (outer_shareable) {
			__asm__ volatile(
				"tlbi RIPAS2E1OS, %[VA]	;"
				: "+m"(asm_ordering)
				: [VA] "r"(
					vmsa_tlbi_ipa_range_input_raw(input)));
		} else {
			__asm__ volatile(
				"tlbi RIPAS2E1IS, %[VA]	;"
				: "+m"(asm_ordering)
				: [VA] "r"(
					vmsa_tlbi_ipa_range_input_raw(input)));
		}
#else
		(void)outer_shareable;
		__asm__ volatile(
			"tlbi RIPAS2E1IS, %[VA]	;"
			: "+m"(asm_ordering)
			: [VA] "r"(vmsa_tlbi_ipa_range_input_raw(input)));
#endif
#endif
	} else {
#ifndef HOST_TEST
		// Range is >8GB; flush the whole address space.
#if defined(ARCH_ARM_FEAT_TLBIOS)
		if (outer_shareable) {
			__asm__ volatile("tlbi VMALLS12E1OS"
					 : "+m"(asm_ordering));
		} else {
			__asm__ volatile("tlbi VMALLS12E1IS"
					 : "+m"(asm_ordering));
		}
#else
		__asm__ volatile("tlbi VMALLS12E1IS" : "+m"(asm_ordering));
#endif
#endif
	}
}
#endif

static void
dsb(bool outer_shareable)
{
#ifndef HOST_TEST
#if defined(ARCH_ARM_FEAT_TLBIOS)
	if (outer_shareable) {
		__asm__ volatile("dsb osh" ::: "memory");
	} else {
		__asm__ volatile("dsb ish" ::: "memory");
	}
#else
	(void)outer_shareable;
	__asm__ volatile("dsb ish" ::: "memory");
#endif
#endif
}

static void
vm_tlbi_vmalle1(bool outer_shareable)
{
#ifndef HOST_TEST
#ifdef ARCH_ARM_FEAT_TLBIOS
	if (outer_shareable) {
		__asm__ volatile("tlbi VMALLE1OS" ::: "memory");
	} else {
		__asm__ volatile("tlbi VMALLE1IS" ::: "memory");
	}
#else
	(void)outer_shareable;
	__asm__ volatile("tlbi VMALLE1IS" ::: "memory");
#endif
#endif
}

// return true if it's top virt address
static bool
is_high_virtual_address(vmaddr_t virtual_address);

// check if the virtual address (VA/IPA) bit count is under restriction.
// true if it's right
static bool
addr_check(vmaddr_t virtual_address, size_t bit_count, bool is_high);

#if defined(HOST_TEST)
// Unit test need these helper functions
vmsa_entry_t
get_entry(vmsa_level_table_t *table, index_t idx);

pgtable_entry_types_t
get_entry_type(vmsa_entry_t *entry, const pgtable_level_info_t *level_info);

void
get_entry_paddr(const pgtable_level_info_t *level_info, vmsa_entry_t *entry,
		pgtable_entry_types_t type, paddr_t *paddr);

count_t
get_table_refcount(vmsa_level_table_t *table, index_t idx);
#else
static vmsa_entry_t
get_entry(vmsa_level_table_t *table, index_t idx);

static pgtable_entry_types_t
get_entry_type(vmsa_entry_t *entry, const pgtable_level_info_t *level_info);

static void
get_entry_paddr(const pgtable_level_info_t *level_info, vmsa_entry_t *entry,
		pgtable_entry_types_t type, paddr_t *paddr);

static count_t
get_table_refcount(vmsa_level_table_t *table, index_t idx);
#endif

static void
set_table_refcount(vmsa_level_table_t *table, index_t idx, count_t refcount);

static pgtable_vm_memtype_t
map_stg2_attr_to_memtype(vmsa_lower_attrs_t attrs);

static pgtable_hyp_memtype_t
map_stg1_attr_to_memtype(vmsa_lower_attrs_t attrs);

static vmsa_lower_attrs_t
get_lower_attr(vmsa_entry_t entry);

static vmsa_upper_attrs_t
get_upper_attr(vmsa_entry_t entry);

static pgtable_access_t
map_stg1_attr_to_access(vmsa_upper_attrs_t upper_attrs,
			vmsa_lower_attrs_t lower_attrs);

static void
map_stg2_attr_to_access(vmsa_upper_attrs_t upper_attrs,
			vmsa_lower_attrs_t lower_attrs,
			pgtable_access_t  *kernel_access,
			pgtable_access_t  *user_access);

static void
map_stg2_memtype_to_attrs(pgtable_vm_memtype_t	   memtype,
			  vmsa_stg2_lower_attrs_t *lower_attrs);

static void
map_stg1_memtype_to_attrs(pgtable_hyp_memtype_t	   memtype,
			  vmsa_stg1_lower_attrs_t *lower_attrs);

static void
map_stg1_access_to_attrs(pgtable_access_t	  access,
			 vmsa_stg1_upper_attrs_t *upper_attrs,
			 vmsa_stg1_lower_attrs_t *lower_attrs);

static void
map_stg2_access_to_attrs(pgtable_access_t	  kernel_access,
			 pgtable_access_t	  user_access,
			 vmsa_stg2_upper_attrs_t *upper_attrs,
			 vmsa_stg2_lower_attrs_t *lower_attrs);

static void
set_invalid_entry(vmsa_level_table_t *table, index_t idx);

static void
set_table_entry(vmsa_level_table_t *table, index_t idx, paddr_t addr,
		count_t count);

static void
set_page_entry(vmsa_level_table_t *table, index_t idx, paddr_t addr,
	       vmsa_upper_attrs_t upper_attrs, vmsa_lower_attrs_t lower_attrs,
	       bool contiguous, bool fence);

static void
set_block_entry(vmsa_level_table_t *table, index_t idx, paddr_t addr,
		vmsa_upper_attrs_t upper_attrs, vmsa_lower_attrs_t lower_attrs,
		bool contiguous, bool fence, bool notlb);

#if CPU_PGTABLE_BBM_LEVEL == 1U
static void
set_notlb_flag(vmsa_label_table_t *table, index_t idx, bool nt);
#endif

// Helper function for translation table walking. Stop walking if modifier
// returns false
static bool
translation_table_walk(pgtable_t *pgt, vmaddr_t virtual_address,
		       size_t virtual_address_size,
		       pgtable_translation_table_walk_event_t event,
		       pgtable_entry_types_t expected, void *data);

static error_t
alloc_level_table(partition_t *partition, size_t size, size_t alignment,
		  paddr_t *paddr, vmsa_level_table_t **table);

static void
set_pgtables(vmaddr_t virtual_address, stack_elem_t stack[PGTABLE_LEVEL_NUM],
	     index_t first_new_table_level, index_t cur_level,
	     count_t initial_refcount, index_t start_level);

static pgtable_modifier_ret_t
map_modifier(pgtable_t *pgt, vmaddr_t virtual_address, size_t size,
	     vmsa_entry_t cur_entry, index_t idx, index_t cur_level,
	     pgtable_entry_types_t type, stack_elem_t stack[PGTABLE_LEVEL_NUM],
	     void *data, index_t *next_level, vmaddr_t *next_virtual_address,
	     size_t *next_size, paddr_t next_table);

static pgtable_modifier_ret_t
lookup_modifier(pgtable_t *pgt, vmsa_entry_t cur_entry, index_t level,
		pgtable_entry_types_t type, void *data);

static void
check_refcount(pgtable_t *pgt, partition_t *partition, vmaddr_t virtual_address,
	       size_t size, index_t upper_level,
	       stack_elem_t stack[PGTABLE_LEVEL_NUM], bool need_dec,
	       size_t preserved_size, index_t *next_level,
	       vmaddr_t *next_virtual_address, size_t *next_size);

#if 0
static bool
map_should_set_cont(vmaddr_t virtual_address, size_t size,
		    vmaddr_t entry_address, index_t level);
#endif

static bool
unmap_should_clear_cont(vmaddr_t virtual_address, size_t size, index_t level);

static void
unmap_clear_cont_bit(vmsa_level_table_t *table, vmaddr_t virtual_address,
		     index_t			       level,
		     vmsa_page_and_block_attrs_entry_t attr_entry,
		     pgtable_unmap_modifier_args_t    *margs,
		     count_t granule_shift, index_t start_level);

static pgtable_modifier_ret_t
unmap_modifier(pgtable_t *pgt, vmaddr_t virtual_address, size_t size,
	       index_t idx, index_t level, pgtable_entry_types_t type,
	       stack_elem_t stack[PGTABLE_LEVEL_NUM], void *data,
	       index_t *next_level, vmaddr_t *next_virtual_address,
	       size_t *next_size, bool only_matching);

static pgtable_modifier_ret_t
prealloc_modifier(pgtable_t *pgt, vmaddr_t virtual_address, size_t size,
		  index_t level, pgtable_entry_types_t type,
		  stack_elem_t stack[PGTABLE_LEVEL_NUM], void *data,
		  index_t *next_level, vmaddr_t *next_virtual_address,
		  size_t *next_size);

// Return entry idx, it can make sure the returned index is always in the
// range
static inline index_t
get_index(vmaddr_t addr, const pgtable_level_info_t *info, bool is_first_level)
{
	index_t index;
	if (is_first_level) {
		index = (index_t)(addr >> info->lsb);
	} else {
		index = (index_t)((addr & segment_mask(info->msb, info->lsb)) >>
				  info->lsb);
	}
	return index;
}

#ifndef NDEBUG
static inline vmaddr_t
set_index(vmaddr_t addr, const pgtable_level_info_t *info, index_t idx)
{
	// FIXME: double check if it might cause issue to clear [63, 48] bits
	return (addr & (~segment_mask(info->msb, info->lsb))) |
	       (((vmaddr_t)idx << info->lsb) &
		segment_mask(info->msb, info->lsb));
}
#endif

static inline vmaddr_t
step_virtual_address(vmaddr_t virtual_address, const pgtable_level_info_t *info)
{
	// should be fine if it overflows, might need to report error
	return (virtual_address + info->addr_size) & (~util_mask(info->lsb));
}

// Return the actual size of current entry within the specified virtual address
// range.
static inline size_t
size_on_level(vmaddr_t virtual_address, size_t size,
	      const pgtable_level_info_t *level_info)
{
	vmaddr_t v_s = virtual_address, v_e = virtual_address + size - 1U;
	vmaddr_t l_s = 0U, l_e = 0U;

	assert(!util_add_overflows(virtual_address, size - 1U));

	l_s = (virtual_address >> level_info->lsb) << level_info->lsb;
	// even for the level 0, it will set the bit 49, will not overflow for
	// 64 bit
	l_e = l_s + level_info->addr_size - 1U;

	assert(!util_add_overflows(l_s, level_info->addr_size - 1U));

	l_s = util_max(l_s, v_s);
	l_e = util_min(v_e, l_e);

	return l_e - l_s + 1U;
}

static inline vmaddr_t
entry_start_address(vmaddr_t			virtual_address,
		    const pgtable_level_info_t *level_info)
{
	return (virtual_address >> level_info->lsb) << level_info->lsb;
}

static inline bool
is_preserved_table_entry(size_t			     preserved_size,
			 const pgtable_level_info_t *level_info)
{
	assert(util_is_p2_or_zero(preserved_size));
	return preserved_size < level_info->addr_size;
}

bool
pgtable_access_check(pgtable_access_t access, pgtable_access_t access_check)
{
	return (((register_t)access & (register_t)access_check) ==
		(register_t)access_check);
}

bool
pgtable_access_is_equal(pgtable_access_t access, pgtable_access_t access_check)
{
	return ((register_t)access == (register_t)access_check);
}

pgtable_access_t
pgtable_access_mask(pgtable_access_t access, pgtable_access_t access_mask)
{
	register_t combined_access =
		((register_t)access & (register_t)access_mask);
	return (pgtable_access_t)combined_access;
}

pgtable_access_t
pgtable_access_combine(pgtable_access_t access1, pgtable_access_t access2)
{
	register_t combined_access =
		((register_t)access1 | (register_t)access2);
	return (pgtable_access_t)combined_access;
}

// Helper function to manipulate page table entry
TEST_EXPORTED vmsa_entry_t
get_entry(vmsa_level_table_t *table, index_t idx)
{
	partition_phys_access_enable(&table[idx]);
	vmsa_entry_t entry = {
		.base = atomic_load_explicit(&table[idx], memory_order_relaxed),
	};

	partition_phys_access_disable(&table[idx]);
	return entry;
}

static bool
is_high_virtual_address(vmaddr_t virtual_address)
{
#if ARCH_IS_64BIT
	// When PAC, MTE or TBI are enabled, the high bits other than bit 55
	// may be used for other purposes. Bit 55 is always used to select the
	// TTBR, regardless of these features or the address space sizes.
	return (virtual_address & util_bit(55U)) != 0U;
#else
#error unimplemented
#endif
}

static bool
addr_check(vmaddr_t virtual_address, size_t bit_count, bool is_high)
{
#if ARCH_IS_64BIT
	static_assert(sizeof(vmaddr_t) == 8, "vmaddr_t expected to be 64bits");

	uint64_t v     = is_high ? ~virtual_address : virtual_address;
	size_t	 count = (v == 0U) ? 0U : 64U - (compiler_clz(v) + 1);
#else
#error unimplemented
#endif

	return count <= bit_count;
}

TEST_EXPORTED pgtable_entry_types_t
get_entry_type(vmsa_entry_t *entry, const pgtable_level_info_t *level_info)
{
	pgtable_entry_types_t ret = pgtable_entry_types_default();
	vmsa_general_entry_t  g	  = entry->base;

	if (vmsa_general_entry_get_is_valid(&g)) {
		if (vmsa_general_entry_get_is_table(&g)) {
			if (pgtable_entry_types_get_next_level_table(
				    &level_info->allowed_types)) {
				pgtable_entry_types_set_next_level_table(&ret,
									 true);
			} else {
				pgtable_entry_types_set_page(&ret, true);
			}
		} else {
			if (pgtable_entry_types_get_block(
				    &level_info->allowed_types)) {
				pgtable_entry_types_set_block(&ret, true);
			} else {
				pgtable_entry_types_set_reserved(&ret, true);
			}
		}
	} else {
		pgtable_entry_types_set_invalid(&ret, true);
	}

	return ret;
}

TEST_EXPORTED void
get_entry_paddr(const pgtable_level_info_t *level_info, vmsa_entry_t *entry,
		pgtable_entry_types_t type, paddr_t *paddr)
{
	vmsa_block_entry_t blk;
	vmsa_page_entry_t  pg;
	vmsa_table_entry_t tb;

	*paddr = 0U;
	if (pgtable_entry_types_get_block(&type)) {
		blk    = entry->block;
		*paddr = vmsa_block_entry_get_OutputAddress(&blk) &
			 level_info->block_and_page_output_address_mask;

	} else if (pgtable_entry_types_get_page(&type)) {
		pg     = entry->page;
		*paddr = vmsa_page_entry_get_OutputAddress(&pg) &
			 level_info->block_and_page_output_address_mask;
	} else if (pgtable_entry_types_get_next_level_table(&type)) {
		tb     = entry->table;
		*paddr = vmsa_table_entry_get_NextLevelTableAddress(&tb) &
			 level_info->table_mask;
	} else {
		panic("Invalid entry get paddr");
	}
}

TEST_EXPORTED count_t
get_table_refcount(vmsa_level_table_t *table, index_t idx)
{
	vmsa_entry_t	   g	 = get_entry(table, idx);
	vmsa_table_entry_t entry = g.table;

	return vmsa_table_entry_get_refcount(&entry);
}

static inline void
set_table_refcount(vmsa_level_table_t *table, index_t idx, count_t refcount)
{
	vmsa_entry_t	   g   = get_entry(table, idx);
	vmsa_table_entry_t val = g.table;

	vmsa_table_entry_set_refcount(&val, refcount);
	partition_phys_access_enable(&table[idx]);
	g.table = val;
	atomic_store_explicit(&table[idx], g.base, memory_order_relaxed);
	partition_phys_access_disable(&table[idx]);
}

static pgtable_vm_memtype_t
map_stg2_attr_to_memtype(vmsa_lower_attrs_t attrs)
{
	vmsa_stg2_lower_attrs_t val = vmsa_stg2_lower_attrs_cast(attrs);
	return vmsa_stg2_lower_attrs_get_mem_attr(&val);
}

static pgtable_hyp_memtype_t
map_stg1_attr_to_memtype(vmsa_lower_attrs_t attrs)
{
	vmsa_stg1_lower_attrs_t val = vmsa_stg1_lower_attrs_cast(attrs);
	// only the MAIR index decides the memory type, it's directly map
	return vmsa_stg1_lower_attrs_get_attr_idx(&val);
}

static vmsa_lower_attrs_t
get_lower_attr(vmsa_entry_t entry)
{
	vmsa_page_and_block_attrs_entry_t val = entry.attrs;
	return vmsa_page_and_block_attrs_entry_get_lower_attrs(&val);
}

static vmsa_upper_attrs_t
get_upper_attr(vmsa_entry_t entry)
{
	vmsa_page_and_block_attrs_entry_t val = entry.attrs;
	return vmsa_page_and_block_attrs_entry_get_upper_attrs(&val);
}

static pgtable_access_t
map_stg1_attr_to_access(vmsa_upper_attrs_t upper_attrs,
			vmsa_lower_attrs_t lower_attrs)
{
	vmsa_stg1_lower_attrs_t l   = vmsa_stg1_lower_attrs_cast(lower_attrs);
	vmsa_stg1_upper_attrs_t u   = vmsa_stg1_upper_attrs_cast(upper_attrs);
	bool			xn  = false;
	vmsa_stg1_ap_t		ap  = VMSA_STG1_AP_EL0_NONE_UPPER_READ_ONLY;
	pgtable_access_t	ret = PGTABLE_ACCESS_NONE;

#if defined(ARCH_ARM_FEAT_VHE)
	xn = vmsa_stg1_upper_attrs_get_PXN(&u);
#else
	xn = vmsa_stg1_upper_attrs_get_XN(&u);
#endif
	ap = vmsa_stg1_lower_attrs_get_AP(&l);

	switch (ap) {
#if ARCH_AARCH64_USE_PAN
	case VMSA_STG1_AP_ALL_READ_WRITE:
	case VMSA_STG1_AP_ALL_READ_ONLY:
		// EL0 has access, so no access in EL2 (unless PAN is disabled)
		ret = PGTABLE_ACCESS_NONE;
		break;
#else // !ARCH_AARCH64_USE_PAN
	case VMSA_STG1_AP_ALL_READ_WRITE:
#endif
	case VMSA_STG1_AP_EL0_NONE_UPPER_READ_WRITE:
		// XN is ignored due to SCTLR_EL2.WXN=1; it should be true
		assert(xn);
		ret = PGTABLE_ACCESS_RW;
		break;
#if !ARCH_AARCH64_USE_PAN
	case VMSA_STG1_AP_ALL_READ_ONLY:
#endif
	case VMSA_STG1_AP_EL0_NONE_UPPER_READ_ONLY:
		ret = xn ? PGTABLE_ACCESS_R : PGTABLE_ACCESS_RX;
		break;
	default:
		// Access None
		break;
	}

	return ret;
}

static void
map_stg2_attr_to_access(vmsa_upper_attrs_t upper_attrs,
			vmsa_lower_attrs_t lower_attrs,
			pgtable_access_t  *kernel_access,
			pgtable_access_t  *user_access)
{
	// Map from S2AP to R and W access bits
	static const pgtable_access_t stg2_ap_map[] = {
		// AP 0x0
		PGTABLE_ACCESS_NONE,
		// AP 0x1
		PGTABLE_ACCESS_R,
		// AP 0x2
		PGTABLE_ACCESS_W,
		// AP 0x3
		PGTABLE_ACCESS_RW,
	};

	vmsa_stg2_lower_attrs_t l = vmsa_stg2_lower_attrs_cast(lower_attrs);
	vmsa_stg2_upper_attrs_t u = vmsa_stg2_upper_attrs_cast(upper_attrs);

	vmsa_s2ap_t	 ap = vmsa_stg2_lower_attrs_get_S2AP(&l);
	pgtable_access_t rw = stg2_ap_map[ap];

#if defined(ARCH_ARM_FEAT_XNX)
	bool uxn	 = vmsa_stg2_upper_attrs_get_UXN(&u);
	bool pxn_xor_uxn = vmsa_stg2_upper_attrs_get_PXNxorUXN(&u);
	bool pxn	 = (bool)(pxn_xor_uxn ^ uxn);
	*user_access = uxn ? rw : pgtable_access_combine(rw, PGTABLE_ACCESS_X);
	*kernel_access = pxn ? rw
			     : pgtable_access_combine(rw, PGTABLE_ACCESS_X);
#else
	bool xn	       = vmsa_stg2_upper_attrs_get_XN(&u);
	*user_access   = xn ? rw : pgtable_access_combine(rw, PGTABLE_ACCESS_X);
	*kernel_access = *user_access;
#endif
}

static void
map_stg2_memtype_to_attrs(pgtable_vm_memtype_t	   memtype,
			  vmsa_stg2_lower_attrs_t *lower_attrs)
{
	vmsa_stg2_lower_attrs_set_mem_attr(lower_attrs, memtype);
	switch (memtype) {
	case PGTABLE_VM_MEMTYPE_NORMAL_NC:
	case PGTABLE_VM_MEMTYPE_NORMAL_ONC_IWT:
	case PGTABLE_VM_MEMTYPE_NORMAL_ONC_IWB:
	case PGTABLE_VM_MEMTYPE_NORMAL_OWT_INC:
	case PGTABLE_VM_MEMTYPE_NORMAL_WT:
	case PGTABLE_VM_MEMTYPE_NORMAL_OWT_IWB:
	case PGTABLE_VM_MEMTYPE_NORMAL_OWB_INC:
	case PGTABLE_VM_MEMTYPE_NORMAL_OWB_IWT:
	case PGTABLE_VM_MEMTYPE_NORMAL_WB:
#if SCHEDULER_CAN_MIGRATE
		vmsa_stg2_lower_attrs_set_SH(lower_attrs,
					     VMSA_SHAREABILITY_INNER_SHAREABLE);
#else
		vmsa_stg2_lower_attrs_set_SH(lower_attrs,
					     VMSA_SHAREABILITY_NON_SHAREABLE);
#endif
		break;
	case PGTABLE_VM_MEMTYPE_DEVICE_NGNRNE:
	case PGTABLE_VM_MEMTYPE_DEVICE_NGNRE:
	case PGTABLE_VM_MEMTYPE_DEVICE_NGRE:
	case PGTABLE_VM_MEMTYPE_DEVICE_GRE:
	default:
		vmsa_stg2_lower_attrs_set_SH(lower_attrs,
					     VMSA_SHAREABILITY_NON_SHAREABLE);
		break;
	}
}

static void
map_stg1_memtype_to_attrs(pgtable_hyp_memtype_t	   memtype,
			  vmsa_stg1_lower_attrs_t *lower_attrs)
{
	vmsa_stg1_lower_attrs_set_attr_idx(lower_attrs, memtype);
}

static void
map_stg1_access_to_attrs(pgtable_access_t	  access,
			 vmsa_stg1_upper_attrs_t *upper_attrs,
			 vmsa_stg1_lower_attrs_t *lower_attrs)
{
	bool	       xn;
	vmsa_stg1_ap_t ap;

	switch (access) {
	case PGTABLE_ACCESS_RX:
	case PGTABLE_ACCESS_X:
		xn = false;
		break;
	case PGTABLE_ACCESS_NONE:
	case PGTABLE_ACCESS_W:
	case PGTABLE_ACCESS_R:
	case PGTABLE_ACCESS_RW:
		xn = true;
		break;
	case PGTABLE_ACCESS_RWX:
	default:
		panic("Invalid stg1 access type");
	}

	/* set AP */
	switch (access) {
	case PGTABLE_ACCESS_W:
	case PGTABLE_ACCESS_RW:
		ap = VMSA_STG1_AP_EL0_NONE_UPPER_READ_WRITE;
		break;
	case PGTABLE_ACCESS_NONE:
#if ARCH_AARCH64_USE_PAN
		ap = VMSA_STG1_AP_ALL_READ_WRITE;
		break;
#endif
	case PGTABLE_ACCESS_R:
	case PGTABLE_ACCESS_RX:
	case PGTABLE_ACCESS_X:
		ap = VMSA_STG1_AP_EL0_NONE_UPPER_READ_ONLY;
		break;
	case PGTABLE_ACCESS_RWX:
	default:
		panic("Invalid stg1 access type");
	}

	vmsa_stg1_lower_attrs_set_AP(lower_attrs, ap);
#if defined(ARCH_ARM_FEAT_VHE)
	vmsa_stg1_upper_attrs_set_PXN(upper_attrs, xn);
	// For compatibility with EL2 single stage MMU keep these equal.
	vmsa_stg1_upper_attrs_set_UXN(upper_attrs, xn);
#else
	vmsa_stg1_upper_attrs_set_XN(upper_attrs, xn);
#endif

#if defined(ARCH_ARM_FEAT_BTI)
	// Guard the executable pages only if requested from platform
	vmsa_stg1_upper_attrs_set_GP(upper_attrs,
				     (!xn && platform_cpu_bti_enabled()));
#endif
}

static void
map_stg2_access_to_attrs(pgtable_access_t	  kernel_access,
			 pgtable_access_t	  user_access,
			 vmsa_stg2_upper_attrs_t *upper_attrs,
			 vmsa_stg2_lower_attrs_t *lower_attrs)
{
	bool kernel_exec =
		pgtable_access_check(kernel_access, PGTABLE_ACCESS_X);
	bool user_exec = pgtable_access_check(user_access, PGTABLE_ACCESS_X);

#if defined(ARCH_ARM_FEAT_XNX)
	vmsa_stg2_upper_attrs_set_UXN(upper_attrs, !user_exec);
	vmsa_stg2_upper_attrs_set_PXNxorUXN(upper_attrs,
					    (bool)(!kernel_exec ^ !user_exec));
#else
	vmsa_stg2_upper_attrs_set_XN(upper_attrs, !kernel_exec || !user_exec);
#endif

	// set AP
	// kernel access and user access (RW) should be the same
	vmsa_s2ap_t ap;
	static_assert(PGTABLE_ACCESS_X == 1,
		      "expect PGTABLE_ACCESS_X is bit 0");
	assert(((kernel_access ^ user_access) >> 1) == 0);
	assert(!pgtable_access_is_equal(kernel_access, PGTABLE_ACCESS_X));

	switch (kernel_access) {
	case PGTABLE_ACCESS_R:
	case PGTABLE_ACCESS_RX:
		ap = VMSA_S2AP_READ_ONLY;
		break;
	case PGTABLE_ACCESS_W:
		ap = VMSA_S2AP_WRITE_ONLY;
		break;
	case PGTABLE_ACCESS_RW:
	case PGTABLE_ACCESS_RWX:
		ap = VMSA_S2AP_READ_WRITE;
		break;
	case PGTABLE_ACCESS_NONE:
	case PGTABLE_ACCESS_X:
	default:
		ap = VMSA_S2AP_NONE;
		break;
	}

	vmsa_stg2_lower_attrs_set_S2AP(lower_attrs, ap);
}

static void
set_invalid_entry(vmsa_level_table_t *table, index_t idx)
{
	vmsa_general_entry_t entry = { 0 };

	partition_phys_access_enable(&table[idx]);
	atomic_store_explicit(&table[idx], entry, memory_order_relaxed);
	partition_phys_access_disable(&table[idx]);
}

static void
set_table_entry(vmsa_level_table_t *table, index_t idx, paddr_t addr,
		count_t count)
{
	vmsa_table_entry_t entry = vmsa_table_entry_default();

	vmsa_table_entry_set_NextLevelTableAddress(&entry, addr);
	vmsa_table_entry_set_refcount(&entry, count);

	partition_phys_access_enable(&table[idx]);
	vmsa_entry_t g = { .table = entry };
	atomic_store_explicit(&table[idx], g.base, memory_order_release);
	partition_phys_access_disable(&table[idx]);
}

static void
set_page_entry(vmsa_level_table_t *table, index_t idx, paddr_t addr,
	       vmsa_upper_attrs_t upper_attrs, vmsa_lower_attrs_t lower_attrs,
	       bool contiguous, bool fence)
{
	vmsa_page_entry_t	  entry = vmsa_page_entry_default();
	vmsa_common_upper_attrs_t u;

	u = vmsa_common_upper_attrs_cast(upper_attrs);
	vmsa_common_upper_attrs_set_cont(&u, contiguous);

	vmsa_page_entry_set_lower_attrs(&entry, lower_attrs);
	vmsa_page_entry_set_upper_attrs(
		&entry, (vmsa_upper_attrs_t)vmsa_common_upper_attrs_raw(u));
	vmsa_page_entry_set_OutputAddress(&entry, addr);

	partition_phys_access_enable(&table[idx]);
	vmsa_entry_t g = { .page = entry };
	if (fence) {
		atomic_store_explicit(&table[idx], g.base,
				      memory_order_release);
	} else {
		atomic_store_explicit(&table[idx], g.base,
				      memory_order_relaxed);
	}
	partition_phys_access_disable(&table[idx]);
}

static void
set_block_entry(vmsa_level_table_t *table, index_t idx, paddr_t addr,
		vmsa_upper_attrs_t upper_attrs, vmsa_lower_attrs_t lower_attrs,
		bool contiguous, bool fence, bool notlb)
{
	vmsa_block_entry_t	  entry = vmsa_block_entry_default();
	vmsa_common_upper_attrs_t u;

	u = vmsa_common_upper_attrs_cast(upper_attrs);
	vmsa_common_upper_attrs_set_cont(&u, contiguous);

	vmsa_block_entry_set_lower_attrs(&entry, lower_attrs);
	vmsa_block_entry_set_upper_attrs(
		&entry, (vmsa_upper_attrs_t)vmsa_common_upper_attrs_raw(u));
	vmsa_block_entry_set_OutputAddress(&entry, addr);
#if CPU_PGTABLE_BBM_LEVEL > 0U
	vmsa_block_entry_set_nT(&entry, notlb);
#else
	(void)notlb;
#endif

	partition_phys_access_enable(&table[idx]);
	vmsa_entry_t g = { .block = entry };
	if (fence) {
		atomic_store_explicit(&table[idx], g.base,
				      memory_order_release);
	} else {
		atomic_store_explicit(&table[idx], g.base,
				      memory_order_relaxed);
	}
	partition_phys_access_disable(&table[idx]);
}

#if CPU_PGTABLE_BBM_LEVEL == 1U
static void
set_notlb_flag(vmsa_label_table_t *table, index_t idx, bool nt)
{
	vmsa_block_entry_t entry;

	partition_phys_access_enable(&table[idx]);
	atomic_load_explicit(&table[idx], entry, memory_order_relaxed);
	vmsa_general_entry_t  g	   = { .block = entry_cnt };
	pgtable_entry_types_t type = get_entry_type(&g);
	assert(pgtable_entry_types_get_block(&type));
	vmsa_block_entry_set_nT(g.block, nt);
	atomic_store_explicit(&table[idx], g.base, memory_order_relaxed);
	partition_phys_access_disable(&table[idx]);
}
#endif // CPU_PGTABLE_BBM_LEVEL == 1U

static error_t
alloc_level_table(partition_t *partition, size_t size, size_t alignment,
		  paddr_t *paddr, vmsa_level_table_t **table)
{
	void_ptr_result_t alloc_ret;

	// actually only used to allocate a page
	alloc_ret = partition_alloc(partition, size, alignment);
	if (compiler_expected(alloc_ret.e == OK)) {
		(void)memset_s(alloc_ret.r, size, 0, size);

		*table = (vmsa_level_table_t *)alloc_ret.r;
		*paddr = partition_virt_to_phys(partition,
						(uintptr_t)alloc_ret.r);
		assert(*paddr != PADDR_INVALID);
	}
	return alloc_ret.e;
}

// Helper function to map all sub page table/set entry count, following a FIFO
// order, so the last entry to write is the one which actually hook the whole
// new page table levels on the existing page table.
static void
set_pgtables(vmaddr_t virtual_address, stack_elem_t stack[PGTABLE_LEVEL_NUM],
	     index_t first_new_table_level, index_t cur_level,
	     count_t initial_refcount, index_t start_level)
{
	paddr_t			    lower;
	vmsa_level_table_t	   *table;
	const pgtable_level_info_t *level_info = NULL;
	index_t			    idx;
	vmsa_entry_t		    g;
	pgtable_entry_types_t	    type;
	count_t			    refcount = initial_refcount;
	index_t			    level    = cur_level;

	while (first_new_table_level < level) {
		lower = stack[level].paddr;
		table = stack[level - 1U].table;

		assert(stack[level - 1U].mapped);

		level_info = &level_conf[level - 1U];

		idx  = get_index(virtual_address, level_info,
				 ((level - 1U) == start_level));
		g    = get_entry(table, idx);
		type = get_entry_type(&g, level_info);

		if (pgtable_entry_types_get_next_level_table(&type)) {
			// This should be the last level we are updating
			assert(first_new_table_level == (level - 1U));

			// Update the table's entry count
			refcount = get_table_refcount(table, idx) + 1U;
			set_table_refcount(table, idx, refcount);
		} else {
			// Write the table entry.
			set_table_entry(table, idx, lower, refcount);

			// The refcount for the remaining levels should be 1.
			refcount = 1;
		}

		level--;
	}
}

// Check if the existing mapping can remain unchanged.
static bool
pgtable_maybe_keep_mapping(vmsa_entry_t cur_entry, pgtable_entry_types_t type,
			   pgtable_map_modifier_args_t *margs, index_t level)
{
	assert(pgtable_entry_types_get_block(&type) ||
	       pgtable_entry_types_get_page(&type));
	const pgtable_level_info_t *cur_level_info = &level_conf[level];

	paddr_t expected_phys = margs->phys & ~util_mask(cur_level_info->lsb);

	paddr_t phys_addr;
	get_entry_paddr(cur_level_info, &cur_entry, type, &phys_addr);
	vmsa_upper_attrs_t upper_attrs = get_upper_attr(cur_entry);
	vmsa_lower_attrs_t lower_attrs = get_lower_attr(cur_entry);

	bool keep_mapping = (phys_addr == expected_phys) &&
			    (upper_attrs == margs->upper_attrs) &&
			    (lower_attrs == margs->lower_attrs);
	if (keep_mapping) {
		margs->phys = expected_phys + cur_level_info->addr_size;
	}
	return keep_mapping;
}

// Check if only the page access needs to be changed and update it.
static bool
pgtable_maybe_update_access(pgtable_t	*pgt,
			    stack_elem_t stack[PGTABLE_LEVEL_NUM], index_t idx,
			    pgtable_entry_types_t	 type,
			    pgtable_map_modifier_args_t *margs, index_t level,
			    vmaddr_t virtual_address, size_t size,
			    vmaddr_t *next_virtual_address, size_t *next_size,
			    index_t *next_level)
{
	bool ret = false;

	// If only the entry's access permissions are changing, this can be
	// done without a break before make.

	const pgtable_level_info_t *cur_level_info = &level_conf[level];

	size_t	 addr_size = cur_level_info->addr_size;
	vmaddr_t entry_virtual_address =
		entry_start_address(virtual_address, cur_level_info);
	vmaddr_t start_virtual_address = virtual_address;

	if (pgtable_entry_types_get_block(&type) &&
	    ((virtual_address != entry_virtual_address) ||
	     (size < addr_size))) {
		goto out;
	}
	assert(virtual_address == entry_virtual_address);

	size_t idx_stop = util_min(idx + (size >> cur_level_info->lsb),
				   stack[level].entry_cnt);

	paddr_t cur_phys = margs->phys;

	vmsa_level_table_t *table = stack[level].table;
	partition_phys_access_enable(&table[0]);
	while (idx != idx_stop) {
		vmsa_entry_t cur_entry = {
			.base = atomic_load_explicit(&table[idx],
						     memory_order_relaxed),
		};
		vmsa_upper_attrs_t upper_attrs = get_upper_attr(cur_entry);
		vmsa_lower_attrs_t lower_attrs = get_lower_attr(cur_entry);
#if defined(ARCH_ARM_FEAT_XNX)
		uint64_t xn_mask = VMSA_STG2_UPPER_ATTRS_UXN_MASK |
				   VMSA_STG2_UPPER_ATTRS_PXNXORUXN_MASK;
#else
		uint64_t xn_mask = VMSA_STG2_UPPER_ATTRS_XN_MASK;
#endif
		uint64_t s2ap_mask = VMSA_STG2_LOWER_ATTRS_S2AP_MASK;

		pgtable_entry_types_t cur_type =
			get_entry_type(&cur_entry, cur_level_info);
		if (!pgtable_entry_types_is_equal(cur_type, type)) {
			goto out_access;
		}

		paddr_t phys_addr;
		get_entry_paddr(&level_conf[level], &cur_entry, type,
				&phys_addr);

		if (phys_addr != cur_phys) {
			goto out_access;
		}
		vmsa_common_upper_attrs_t upper_attrs_bitfield =
			vmsa_common_upper_attrs_cast(upper_attrs);
		if (vmsa_common_upper_attrs_get_cont(&upper_attrs_bitfield)) {
			goto out_access;
		}
		if ((upper_attrs & ~xn_mask) !=
		    (margs->upper_attrs & ~xn_mask)) {
			goto out_access;
		}
		if ((lower_attrs & ~s2ap_mask) !=
		    (margs->lower_attrs & ~s2ap_mask)) {
			goto out_access;
		}

		vmsa_page_entry_t entry = cur_entry.page;

		if ((upper_attrs & xn_mask) != (margs->upper_attrs & xn_mask)) {
			vmsa_page_entry_set_upper_attrs(&entry,
							margs->upper_attrs);
		}
		if ((lower_attrs & s2ap_mask) !=
		    (margs->lower_attrs & s2ap_mask)) {
			vmsa_page_entry_set_lower_attrs(&entry,
							margs->lower_attrs);
		}

		cur_entry.page = entry;
		atomic_store_explicit(&table[idx], cur_entry.base,
				      memory_order_release);

		idx++;
		cur_phys += cur_level_info->addr_size;
		virtual_address += cur_level_info->addr_size;
	}
	partition_phys_access_disable(&table[0]);

	ret = true;

	size_t updated_size = cur_phys - margs->phys;

#if defined(ARCH_ARM_FEAT_TLBIRANGE)
	if (margs->stage == PGTABLE_HYP_STAGE_1) {
		dsb(false);
		hyp_tlbi_va_range(start_virtual_address, updated_size,
				  pgt->granule_shift);
	} else {
		dsb(margs->outer_shareable);
		hyp_tlbi_ipa_range(start_virtual_address, updated_size,
				   pgt->granule_shift, margs->outer_shareable);
	}
#else
	dsb(margs->outer_shareable);

	for (size_t offset = 0U; offset < updated_size; offset += addr_size) {
		if (margs->stage == PGTABLE_HYP_STAGE_1) {
			hyp_tlbi_va(start_virtual_address + offset);
		} else {
			vm_tlbi_ipa(start_virtual_address + offset,
				    margs->outer_shareable);
		}
	}
#endif

	*next_size	      = size - updated_size;
	margs->phys	      = cur_phys;
	*next_virtual_address = virtual_address;

	// Walk back up the tree if needed
	if (idx == stack[level].entry_cnt) {
		idx -= 1U; // Last updated index
		while (idx == stack[level].entry_cnt - 1U) {
			if (level == pgt->start_level) {
				break;
			} else {
				level--;
			}

			cur_level_info = &level_conf[level];
			// virtual_address is already stepped, use previous one
			// to check
			idx = get_index(virtual_address, cur_level_info,
					(level == pgt->start_level));
		}
		*next_level = level;
	}

out:
	return ret;
out_access:
	partition_phys_access_disable(&table[0]);
	goto out;
}

static error_t
pgtable_add_table_entry(pgtable_t *pgt, pgtable_map_modifier_args_t *margs,
			index_t cur_level, stack_elem_t *stack,
			vmaddr_t virtual_address, size_t size,
			index_t *next_level, vmaddr_t *next_virtual_address,
			size_t *next_size, bool set_start_level)
{
	error_t		    ret;
	paddr_t		    new_pgtable_paddr;
	vmsa_level_table_t *new_pgt	 = NULL;
	index_t		    level	 = cur_level;
	size_t		    pgtable_size = util_bit(pgt->granule_shift);

	// allocate page and fill right value first, then update entry
	// to existing table
	ret = alloc_level_table(margs->partition, pgtable_size, pgtable_size,
				&new_pgtable_paddr, &new_pgt);
	if (ret != OK) {
		LOG(ERROR, WARN, "Failed to alloc page table level.\n");
		margs->error = ret;
		goto out;
	}

	if ((margs->new_page_start_level == PGTABLE_INVALID_LEVEL) &&
	    set_start_level) {
		margs->new_page_start_level =
			level > pgt->start_level ? level - 1U : level;
	}

	if (level >= (PGTABLE_LEVEL_NUM - 1U)) {
		LOG(ERROR, WARN, "invalid level ({:d}).\n", level);
		ret = ERROR_ARGUMENT_INVALID;
		goto out;
	}

	// just record the new level in the stack
	stack[level + 1U] = (stack_elem_t){
		.paddr	    = new_pgtable_paddr,
		.table	    = new_pgt,
		.mapped	    = true,
		.need_unmap = false,
		.entry_cnt  = level_conf[level + 1U].entry_cnt,
	};

	// guide translation_table_walk step into the new sub page table
	// level
	*next_level	      = level + 1U;
	*next_virtual_address = virtual_address;
	*next_size	      = size;

out:
	return ret;
}

// Split a block into the next smaller block size.
//
// This is called when a map or unmap operation encounters a block entry that
// overlaps but is not completely covered by the range of the operation.
static pgtable_modifier_ret_t
pgtable_split_block(pgtable_t *pgt, vmaddr_t virtual_address, size_t size,
		    vmsa_entry_t cur_entry, index_t idx, index_t level,
		    pgtable_entry_types_t	 type,
		    stack_elem_t		 stack[PGTABLE_LEVEL_NUM],
		    pgtable_map_modifier_args_t *margs, index_t *next_level,
		    vmaddr_t *next_virtual_address, size_t *next_size)
{
	pgtable_modifier_ret_t vret = PGTABLE_MODIFIER_RET_CONTINUE;
	error_t		       ret;

	assert(pgtable_entry_types_get_block(&level_conf[level].allowed_types));

	// Get the values of the block before it is invalidated
	const pgtable_level_info_t *cur_level_info = &level_conf[level];
	size_t			    addr_size	   = cur_level_info->addr_size;
	vmaddr_t		    entry_virtual_address =
		entry_start_address(virtual_address, cur_level_info);
	vmsa_upper_attrs_t cur_upper_attrs = get_upper_attr(cur_entry);
	vmsa_lower_attrs_t cur_lower_attrs = get_lower_attr(cur_entry);
	paddr_t		   phys_addr;
	get_entry_paddr(cur_level_info, &cur_entry, type, &phys_addr);

#if (CPU_PGTABLE_BBM_LEVEL < 2U) && !defined(PLATFORM_PGTABLE_AVOID_BBM)
	// We can't just replace the large entry; coherency might be broken. We
	// need a TLB flush.
#if CPU_PGTABLE_BBM_LEVEL == 0U
	// The nT bit is not supported; we need a full break-before-make
	// sequence, with an invalid entry in the page table. This might trigger
	// spurious stage 2 faults on other cores or SMMUs.
	set_invalid_entry(stack[level].table, idx);
#else
	// The nT bit is supported; we can set it before flushing the TLB to
	// ensure that the large mapping isn't cached again before we replace it
	// with the split mappings, but the entry itself can remain valid so
	// SMMUs and other cores may not fault.
	set_notlb_flag(stack[level].table, idx, true);
#endif

	// Flush the TLB entry
	if (margs->stage == PGTABLE_HYP_STAGE_1) {
		dsb(false);
		hyp_tlbi_va(entry_virtual_address);
	} else {
		dsb(margs->outer_shareable);
		vm_tlbi_ipa(entry_virtual_address, margs->outer_shareable);
		// The full stage-1 flushing below is really sub-optimal.
		// FIXME:
		dsb(margs->outer_shareable);
		vm_tlbi_vmalle1(margs->outer_shareable);
	}
#else // (CPU_PGTABLE_BBM_LEVEL >= 2U) || PLATFORM_PGTABLE_AVOID_BBM
      // Either the CPU supports page size changes without BBM, or we must
      // avoid BBM to prevent unrecoverable SMMU faults. We will just replace
      // the block entry with the split table entry.
#endif

	ret = pgtable_add_table_entry(pgt, margs, level, stack, virtual_address,
				      size, next_level, next_virtual_address,
				      next_size, false);
	if (ret != OK) {
		vret = PGTABLE_MODIFIER_RET_ERROR;
		goto out;
	}

	// Update current level and values as now we want to add all the
	// pages to the table just created
	level		= *next_level;
	virtual_address = *next_virtual_address;

	cur_level_info = &level_conf[level];

	size_t	page_size = cur_level_info->addr_size;
	count_t new_pages = (count_t)(addr_size / page_size);
	assert(new_pages == cur_level_info->entry_cnt);

#if 0
	// FIXME: also need to search forwards for occupied entries
	bool contiguous = map_should_set_cont(
		margs->orig_virtual_address, margs->orig_size,
		virtual_address, level);
#else
	bool contiguous = false;
#endif
	bool	page_block_fence;
	index_t new_page_start_level;

	if (margs->new_page_start_level != PGTABLE_INVALID_LEVEL) {
		new_page_start_level	    = margs->new_page_start_level;
		page_block_fence	    = false;
		margs->new_page_start_level = PGTABLE_INVALID_LEVEL;
	} else {
		new_page_start_level = level > pgt->start_level ? level - 1U
								: level;
		page_block_fence     = true;
	}

	// Create all pages that cover the old block and hook them to
	// the new table entry
	assert(virtual_address >= entry_virtual_address);

	assert(pgtable_entry_types_get_block(&type));
	const bool use_block =
		pgtable_entry_types_get_block(&cur_level_info->allowed_types);

	paddr_t phys;
	idx = 0;
	for (count_t i = 0; i < new_pages; i++) {
		vmsa_upper_attrs_t upper_attrs;
		vmsa_lower_attrs_t lower_attrs;

		phys	    = phys_addr;
		upper_attrs = cur_upper_attrs;
		lower_attrs = cur_lower_attrs;

		phys_addr += page_size;

		if (use_block) {
			set_block_entry(stack[level].table, idx, phys,
					upper_attrs, lower_attrs, contiguous,
					page_block_fence, false);
		} else {
			set_page_entry(stack[level].table, idx, phys,
				       upper_attrs, lower_attrs, contiguous,
				       page_block_fence);
		}
		assert(!util_add_overflows(margs->phys, (paddr_t)page_size));
		assert(!util_add_overflows(phys_addr, (paddr_t)page_size));
		idx++;
	}

#if (CPU_PGTABLE_BBM_LEVEL < 2U) && !defined(PLATFORM_PGTABLE_AVOID_BBM)
	// Wait for the TLB flush before inserting the new table entry
	dsb(margs->outer_shareable);
#endif
	set_pgtables(entry_virtual_address, stack, new_page_start_level, level,
		     new_pages, pgt->start_level);

#if (CPU_PGTABLE_BBM_LEVEL >= 2U) || defined(PLATFORM_PGTABLE_AVOID_BBM)
	// Flush the old entry from the TLB now, to avoid TLB conflicts later.
	if (margs->stage == PGTABLE_HYP_STAGE_1) {
		dsb(false);
		hyp_tlbi_va(entry_virtual_address);
	} else {
		dsb(margs->outer_shareable);
		vm_tlbi_ipa(entry_virtual_address, margs->outer_shareable);
	}
#endif

out:
	return vret;
}

// Attempt to merge a subtree into a single block.
//
// This is called when a remap operation encounters a next-level table entry. It
// checks whether the map operation is able to merge the next-level table into a
// single large block, and replaces it with that block if possible.
//
// The criteria are as follows:
//
// - This level's entry size is less than the operation's merge limit
// - Block mappings are allowed at this level
// - The input and output addresses are equal modulo this level's entry size
// - The input address range is aligned to the next level's entry size
// - If the next-level table is allowed to contain next-level table entries, it
//   does not contain any such entries
// - If try_map is true, any entries in the next-level table that are inside the
//   mapped region are invalid
// - If the next-level table is not completely within the mapped region, any
//   entries that are outside the mapped region are congruent, i.e. have the
//   same attributes and physical addresses that they would have if the mapped
//   region was extended to cover them
static pgtable_modifier_ret_t
pgtable_maybe_merge_block(pgtable_t *pgt, vmaddr_t virtual_address, size_t size,
			  vmsa_entry_t cur_entry, index_t idx, index_t level,
			  pgtable_entry_types_t	       type,
			  stack_elem_t		       stack[PGTABLE_LEVEL_NUM],
			  pgtable_map_modifier_args_t *margs,
			  index_t *next_level, paddr_t next_table_paddr)
{
	assert(pgtable_entry_types_get_next_level_table(&type));
	assert(*next_level < PGTABLE_LEVEL_NUM);

	pgtable_modifier_ret_t	    vret = PGTABLE_MODIFIER_RET_CONTINUE;
	const pgtable_level_info_t *cur_level_info = &level_conf[level];

	if (cur_level_info->addr_size >= margs->merge_limit) {
		// Block size exceeds the merge limit. There are three reasons
		// we enforce this:
		//
		// - If a VM address space with an SMMU or other IOMMU attached
		//   that can't handle break-before-make or TLB conflicts
		//   safely, we need to disable merging entirely.
		//
		// - In the hypervisor address space, we need to avoid merging
		//   pages in a way that might cause a fault on an address that
		//   is touched during the hypervisor's handling of that fault,
		//   since that would fault recursively.
		//
		// - In the hypervisor address space, we need to avoid merging
		//   across partition ownership boundaries, because that might
		//   free the next-level table into the wrong partition.
		goto out;
	}

	if (!pgtable_entry_types_get_block(&cur_level_info->allowed_types)) {
		// Block entries aren't possible at this level. This might
		// happen if merge limit is set to ~0U for a VM address space.
		goto out;
	}

	if ((virtual_address & util_mask(cur_level_info->lsb)) !=
	    (margs->phys & util_mask(cur_level_info->lsb))) {
		// Input and output addresses misaligned for block size.
		goto out;
	}

	const pgtable_level_info_t *next_level_info = &level_conf[*next_level];
	size_t			    level_size =
		size_on_level(virtual_address, size, cur_level_info);
	if (((virtual_address & util_mask(next_level_info->lsb)) != 0U) ||
	    ((level_size & util_mask(next_level_info->lsb)) != 0U)) {
		// Mapping will be partial at the next level. A merge
		// might be possible at the next level, but we currently
		// can't extend it to this level as that requires a
		// multi-level search for non-congruent entries.
		goto out;
	}

	count_t covered_entries = (count_t)(level_size >> next_level_info->lsb);
	count_t other_entries	= next_level_info->entry_cnt - covered_entries;

	count_t table_refcount =
		vmsa_table_entry_get_refcount(&cur_entry.table);

	if (table_refcount < other_entries) {
		// Some of the entries not covered by the map operation must be
		// invalid, so a merge won't be possible.
		goto out;
	}

	vmaddr_t entry_virtual_address =
		entry_start_address(virtual_address, cur_level_info);
	paddr_t entry_phys = margs->phys & ~util_mask(cur_level_info->lsb);

	bool need_search = ((pgtable_entry_types_get_next_level_table(
				    &next_level_info->allowed_types)) ||
			    (other_entries > 0U) ||
			    (margs->try_map && (table_refcount > 0U)));

	if (need_search) {
		// It's possible that the level to be merged contains entries
		// that will prevent the merge. Map the level to be merged.
		vmsa_level_table_t *next_table =
			(vmsa_level_table_t *)partition_phys_map(
				next_table_paddr, util_bit(pgt->granule_shift));
		if (next_table == NULL) {
			LOG(ERROR, WARN,
			    "Failed to map table (pa {:#x}, level {:d}) for merge\n",
			    next_table_paddr, *next_level);
			vret = PGTABLE_MODIFIER_RET_ERROR;
			goto out;
		}

		// Check for any next-level entries that will prevent merge
		vmaddr_t next_level_addr = entry_virtual_address;
		paddr_t	 expected_phys	 = entry_phys;
		index_t	 next_level_idx;
		for (next_level_idx = 0U;
		     next_level_idx < next_level_info->entry_cnt;
		     next_level_idx++) {
			vmsa_entry_t next_level_entry =
				get_entry(next_table, next_level_idx);
			pgtable_entry_types_t next_level_type = get_entry_type(
				&next_level_entry, next_level_info);
			if (pgtable_entry_types_get_next_level_table(
				    &next_level_type)) {
				// We don't try to handle multi-level merges.
				// It's complex and typically not worth the
				// effort.
				break;
			}
			if ((margs->try_map &&
			     (!pgtable_entry_types_get_invalid(
				     &next_level_type))) ||
			    ((next_level_addr < virtual_address) ||
			     ((next_level_addr + next_level_info->addr_size -
			       1U) > (virtual_address + size - 1U)))) {
				// This entry is not completely covered by the
				// current map operation. It must be valid, and
				// must map the same physical address as the
				// current map operation would, with the same
				// attributes.
				if ((!pgtable_entry_types_get_block(
					    &next_level_type)) &&
				    (!pgtable_entry_types_get_page(
					    &next_level_type))) {
					// Not valid. Merging would incorrectly
					// map it.
					break;
				}

				paddr_t phys_addr;
				get_entry_paddr(next_level_info,
						&next_level_entry,
						next_level_type, &phys_addr);
				vmsa_upper_attrs_t upper_attrs =
					get_upper_attr(next_level_entry);
				vmsa_lower_attrs_t lower_attrs =
					get_lower_attr(next_level_entry);
				if ((phys_addr != expected_phys) ||
				    (upper_attrs != margs->upper_attrs) ||
				    (lower_attrs != margs->lower_attrs)) {
					// Inconsistent mapping; can't merge.
					break;
				}
			}
			expected_phys += next_level_info->addr_size;
			next_level_addr += next_level_info->addr_size;
		}

		partition_phys_unmap(next_table, next_table_paddr,
				     util_bit(pgt->granule_shift));

		if (next_level_idx < next_level_info->entry_cnt) {
			// We exited the next-level table check early, which
			// means we found an entry that prevented merge. Nothing
			// more to do.
			goto out;
		}
	}

	assert(stack[level].mapped);
	vmsa_level_table_t *cur_table = stack[level].table;

#if (CPU_PGTABLE_BBM_LEVEL < 1U) && !defined(PLATFORM_PGTABLE_AVOID_BBM)
	// The nT bit is not supported; we need a full break-before-make
	// sequence, with an invalid entry in the page table. This might
	// trigger spurious stage 2 faults on other cores or SMMUs.
	set_invalid_entry(stack[level].table, idx);
#elif (CPU_PGTABLE_BBM_LEVEL < 2U) && !defined(PLATFORM_PGTABLE_AVOID_BBM)
	// We can write the new block entry with the nT bit set, and then flush
	// the old page entries. The new entry will stay out of the TLBs until
	// we clear the nT bit below, so there will be no TLB conflicts, but
	// there may be translation faults depending on the CPU implementation.
	set_block_entry(cur_table, idx, entry_phys, margs->upper_attrs,
			margs->lower_attrs, false, false, true);

#else // (CPU_PGTABLE_BBM_LEVEL >= 2U) || PLATFORM_PGTABLE_AVOID_BBM

	// Either the CPU supports block size changes without BBM, or we must
	// avoid BBM operations to prevent unrecoverable SMMU faults.
	//
	// We can just go ahead and write the new block entry. We still flush
	// the TLB afterwards to avoid TLB conflicts. It is probably cheaper to
	// do that now than to take up to 512 TLB conflict faults later,
	// especially if the CPU supports range flushes.
	set_block_entry(cur_table, idx, entry_phys, margs->upper_attrs,
			margs->lower_attrs, false, false, false);

#endif

	// Flush the TLB entries for the merged pages.
	vmaddr_t next_level_addr = entry_virtual_address;
#ifdef ARCH_ARM_FEAT_TLBIRANGE
	if (margs->stage == PGTABLE_HYP_STAGE_1) {
		dsb(false);
		hyp_tlbi_va_range(entry_virtual_address,
				  cur_level_info->addr_size,
				  pgt->granule_shift);
	} else {
		dsb(margs->outer_shareable);
		hyp_tlbi_ipa_range(next_level_addr, cur_level_info->addr_size,
				   pgt->granule_shift, margs->outer_shareable);
	}
#else
	dsb((margs->stage != PGTABLE_HYP_STAGE_1) && margs->outer_shareable);
	for (index_t i = 0; i < next_level_info->entry_cnt; i++) {
		if (margs->stage == PGTABLE_HYP_STAGE_1) {
			hyp_tlbi_va(next_level_addr);
		} else {
			vm_tlbi_ipa(next_level_addr, margs->outer_shareable);
		}
		next_level_addr += next_level_info->addr_size;
	}
#endif

#if (CPU_PGTABLE_BBM_LEVEL < 2U) && !defined(PLATFORM_PGTABLE_AVOID_BBM)
	if (margs->stage != PGTABLE_HYP_STAGE_1) {
		// The full stage-1 flushing below is really sub-optimal.
		// FIXME:
		dsb(margs->outer_shareable);
		vm_tlbi_vmalle1(margs->outer_shareable);
	}

	// Wait for the TLB flush before inserting the new table entry
	dsb((margs->stage != PGTABLE_HYP_STAGE_1) && margs->outer_shareable);

	set_block_entry(cur_table, idx, entry_phys, margs->upper_attrs,
			margs->lower_attrs, false, false, false);
#endif

	// Release the page table memory
	(void)partition_free_phys(margs->partition, next_table_paddr,
				  util_bit(pgt->granule_shift));

	// Ensure that translation_table_walk revisits the entry we just
	// replaced, instead of traversing into the now-freed table. We don't
	// update the virt or phys addresses or size, because the next call to
	// map_modifier() will call pgtable_maybe_keep_mapping() to do it.
	*next_level = level;

out:
	return vret;
}

static pgtable_modifier_ret_t
pgtable_modify_mapping(pgtable_t *pgt, vmaddr_t virtual_address, size_t size,
		       vmsa_entry_t cur_entry, index_t idx, index_t cur_level,
		       pgtable_entry_types_t	    type,
		       stack_elem_t		    stack[PGTABLE_LEVEL_NUM],
		       pgtable_map_modifier_args_t *margs, index_t *next_level,
		       vmaddr_t *next_virtual_address, size_t *next_size)
{
	pgtable_modifier_ret_t vret  = PGTABLE_MODIFIER_RET_CONTINUE;
	index_t		       level = cur_level;

	const pgtable_level_info_t *cur_level_info = &level_conf[level];
	size_t			    addr_size	   = cur_level_info->addr_size;
	vmaddr_t		    entry_virtual_address =
		entry_start_address(virtual_address, cur_level_info);

	if (pgtable_entry_types_get_block(&type) &&
	    ((virtual_address != entry_virtual_address) ||
	     (size < addr_size))) {
		// Split the block into pages
		vret = pgtable_split_block(pgt, virtual_address, size,
					   cur_entry, idx, level, type, stack,
					   margs, next_level,
					   next_virtual_address, next_size);
	} else {
		// The new mapping will cover this entire range, either because
		// it's a single page, or because it's a block that didn't need
		// to be split. We need to unmap the existing page or block.
		pgtable_unmap_modifier_args_t margs2 = { 0 };

		margs2.partition      = margs->partition;
		margs2.preserved_size = PGTABLE_HYP_UNMAP_PRESERVE_NONE;
		margs2.stage	      = margs->stage;

		vret = unmap_modifier(pgt, virtual_address, addr_size, idx,
				      cur_level, type, stack, &margs2,
				      next_level, next_virtual_address,
				      next_size, false);

		if (margs->stage == PGTABLE_VM_STAGE_2) {
			// flush entire stage 1 tlb
			dsb(margs->outer_shareable);
			vm_tlbi_vmalle1(margs->outer_shareable);
			dsb(margs->outer_shareable);
		} else {
			dsb(false);
		}

		// Retry at the same address, so we can do the make part of the
		// break-before-make sequence.
		*next_virtual_address = virtual_address;
		*next_size	      = size;
	}

	return vret;
}

// @brief Modify current entry for mapping the specified virt address to the
// physical address.
//
// This modifier simply focuses on just one entry during the mapping procedure.
// Depends on the type of the entry, it may:
// * directly map the physical address as block/page
// * allocate/setup the page table level, and recursively (up to MAX_LEVEL) to
// handle the mapping. After the current page table level entry setup, it will
// drive @see translation_table_walk to the next entry at the same level.
static pgtable_modifier_ret_t
map_modifier(pgtable_t *pgt, vmaddr_t virtual_address, size_t size,
	     vmsa_entry_t cur_entry, index_t idx, index_t cur_level,
	     pgtable_entry_types_t type, stack_elem_t stack[PGTABLE_LEVEL_NUM],
	     void *data, index_t *next_level, vmaddr_t *next_virtual_address,
	     size_t *next_size, paddr_t next_table)
{
	pgtable_map_modifier_args_t *margs =
		(pgtable_map_modifier_args_t *)data;
	pgtable_modifier_ret_t vret = PGTABLE_MODIFIER_RET_CONTINUE;

	// Attempt merging or replacement of small mappings.
	if (pgtable_entry_types_get_next_level_table(&type)) {
		vret = pgtable_maybe_merge_block(pgt, virtual_address, size,
						 cur_entry, idx, cur_level,
						 type, stack, margs, next_level,
						 next_table);
		goto out;
	}

	// Handle existing block or page mappings.
	if (!pgtable_entry_types_get_invalid(&type)) {
		// If the existing mapping is consistent with the required
		// mapping, we don't need to do anything, even if try_map is
		// true.
		if (pgtable_maybe_keep_mapping(cur_entry, type, margs,
					       cur_level)) {
			goto out;
		}

		// If try_map is set, we will abort the mapping operation.
		if (margs->try_map) {
			margs->error		     = ERROR_EXISTING_MAPPING;
			margs->partially_mapped_size = margs->orig_size - size;
			vret = PGTABLE_MODIFIER_RET_ERROR;
			goto out;
		}

		// If this is only an access update, we can do it in place.
		if (pgtable_maybe_update_access(pgt, stack, idx, type, margs,
						cur_level, virtual_address,
						size, next_virtual_address,
						next_size, next_level)) {
			goto out;
		}

		// We need a non-trivial update using a BBM sequence and/or a
		// block split.
		vret = pgtable_modify_mapping(pgt, virtual_address, size,
					      cur_entry, idx, cur_level, type,
					      stack, margs, next_level,
					      next_virtual_address, next_size);
		goto out;
	}

	assert(data != NULL);
	assert(pgt != NULL);

	// current level should be mapped
	index_t level = cur_level;
	assert(stack[level].mapped);
	vmsa_level_table_t *cur_table = stack[level].table;

	const pgtable_level_info_t *cur_level_info = &level_conf[cur_level];
	size_t			    addr_size	   = cur_level_info->addr_size;
	pgtable_entry_types_t	    allowed = cur_level_info->allowed_types;

	const bool use_block = pgtable_entry_types_get_block(&allowed);
	size_t	   level_size =
		size_on_level(virtual_address, size, cur_level_info);

	if ((addr_size <= level_size) &&
	    (use_block || pgtable_entry_types_get_page(&allowed)) &&
	    (util_is_baligned(margs->phys, addr_size))) {
		index_t new_page_start_level;
		bool	page_block_fence;

		if (margs->new_page_start_level != PGTABLE_INVALID_LEVEL) {
			new_page_start_level = margs->new_page_start_level;
			page_block_fence     = false;
			margs->new_page_start_level = PGTABLE_INVALID_LEVEL;
		} else {
			// if current level is start level, no need to update
			// entry count
			new_page_start_level =
				level > pgt->start_level ? level - 1U : level;
			page_block_fence = true;
		}

#if 0
		// FIXME: also need to search forwards for occupied entries
		bool contiguous = map_should_set_cont(
			margs->orig_virtual_address, margs->orig_size,
			virtual_address, level);
#else
		bool contiguous = false;
#endif

		// allowed to map a block
		if (use_block) {
			set_block_entry(cur_table, idx, margs->phys,
					margs->upper_attrs, margs->lower_attrs,
					contiguous, page_block_fence, false);
		} else {
			set_page_entry(cur_table, idx, margs->phys,
				       margs->upper_attrs, margs->lower_attrs,
				       contiguous, page_block_fence);
		}

		// check if need to set all page table levels
		set_pgtables(virtual_address, stack, new_page_start_level,
			     level, 1U, pgt->start_level);

		// update the physical address for next mapping
		margs->phys += addr_size;
		assert(!util_add_overflows(margs->phys, addr_size));
	} else if (pgtable_entry_types_get_next_level_table(&allowed)) {
		error_t ret = pgtable_add_table_entry(
			pgt, margs, level, stack, virtual_address, size,
			next_level, next_virtual_address, next_size, true);
		if (ret != OK) {
			vret = PGTABLE_MODIFIER_RET_ERROR;
			goto failed_map;
		}
	} else {
		LOG(ERROR, WARN, "Unexpected condition during mapping:\n");
		LOG(ERROR, WARN,
		    "Mapping pa({:x}) to va({:x}), size({:d}), level({:d})",
		    margs->phys, virtual_address, size, (register_t)level);
		// should not be here
		panic("map_modifier bad type");
	}

failed_map:
	cur_table = NULL;

	// free all pages if something wrong
	if ((vret == PGTABLE_MODIFIER_RET_ERROR) &&
	    (margs->new_page_start_level != PGTABLE_INVALID_LEVEL)) {
		size_t pgtable_size = util_bit(pgt->granule_shift);
		while (margs->new_page_start_level < level) {
			// all new table level, no need to unmap
			assert(!stack[level].need_unmap);
			(void)partition_free(margs->partition,
					     stack[level].table, pgtable_size);
			stack[level].paddr  = 0U;
			stack[level].table  = NULL;
			stack[level].mapped = false;
			level--;
		}
	}

out:
	return vret;
}

// @brief Collect information while walking along the virtual address.
//
// This modifier is actually just a visitor, it accumulates the size of the
// entry/entries, and return to the caller. NOTE that the memory type and
// memory attribute should be the same, or else, it only return the attributes
// of the last entry.
static pgtable_modifier_ret_t
lookup_modifier(pgtable_t *pgt, vmsa_entry_t cur_entry, index_t level,
		pgtable_entry_types_t type, void *data)
{
	pgtable_lookup_modifier_args_t *margs =
		(pgtable_lookup_modifier_args_t *)data;
	const pgtable_level_info_t *cur_level_info = &level_conf[level];

	// expected types in the walk should be page|block
	assert(pgtable_entry_types_get_page(&type) ||
	       pgtable_entry_types_get_block(&type));

	assert(pgt != NULL);

	get_entry_paddr(cur_level_info, &cur_entry, type, &margs->phys);

	margs->entry = cur_entry;

	// set size & return for check
	margs->size = cur_level_info->addr_size;

	return PGTABLE_MODIFIER_RET_STOP;
}

// helper to check entry count from the parent page table level,
// free empty upper levels if needed
static void
check_refcount(pgtable_t *pgt, partition_t *partition, vmaddr_t virtual_address,
	       size_t size, index_t upper_level,
	       stack_elem_t stack[PGTABLE_LEVEL_NUM], bool need_dec,
	       size_t preserved_size, index_t *next_level,
	       vmaddr_t *next_virtual_address, size_t *next_size)
{
	const pgtable_level_info_t *cur_level_info = NULL;
	vmsa_level_table_t	   *cur_table	   = NULL;
	index_t			    level	   = upper_level;
	index_t			    cur_idx;
	count_t			    refcount;
	bool			    is_preserved = false;
	stack_elem_t		   *free_list[PGTABLE_LEVEL_NUM];
	index_t			    free_idx = 0;
	bool			    dec	     = need_dec;
	size_t			    walked_size;

	while (level >= pgt->start_level) {
		assert(stack[level].mapped);
		cur_table = stack[level].table;

		cur_level_info = &level_conf[level];
		cur_idx	       = get_index(virtual_address, cur_level_info,
					   (level == pgt->start_level));
		refcount       = get_table_refcount(cur_table, cur_idx);

		if (dec) {
			// decrease entry count
			refcount--;
			set_table_refcount(cur_table, cur_idx, refcount);
			dec = false;
		}

		if (refcount == 0U) {
			is_preserved = is_preserved_table_entry(preserved_size,
								cur_level_info);

			if (is_preserved) {
				break;
			}

			// Make sure the general page table walk does not step
			// into the level that is being freed. The correct step
			// might either be forward one entry (if there are more
			// entries in the current level) or up one level (if
			// this is the last entry in the current level).
			// The following diagram shows the edge case:
			//
			//         Next Entry
			//            +
			//            |
			//            |
			//+-----------v-------------+
			//|           T T           |    *next_level
			//+-----------+-+-----------+
			//            | |
			//      +-----+ +--+
			//      |          |
			//  +---v----+  +--v-+--------+
			//  |      |B|  |  T |      |B|  level
			//  +--------+  +-+--+--------+
			//                |
			//                v
			//              +-+-+------+
			//              | T |      |   freed
			//              +-+-+------+
			//                |
			//                v
			//              +----------+
			//              |P|        |   freed
			//              +----------+
			// Here two levels are freed, the *next entry* is the
			// entry for next iteration.
			*next_level = util_min(*next_level, level);
			// bigger virtual address, further
			*next_virtual_address =
				util_max(*next_virtual_address,
					 step_virtual_address(virtual_address,
							      cur_level_info));
			walked_size = (*next_virtual_address - virtual_address);
			*next_size  = util_max(size, walked_size) - walked_size;

			free_list[free_idx] = &stack[level + 1U];
			free_idx++;
			// invalidate current entry
			set_invalid_entry(cur_table, cur_idx);

			dec = true;
		}

		if ((refcount == 0U) && (level > 0U)) {
			// will decrease it's parent level entry count
			level--;
		} else {
			// break if current level entry count is non-zero
			break;
		}

		cur_table = NULL;
	}

	// free the page table levels at one time, the free will do the fence
	while (free_idx > 0U) {
		free_idx--;

		if (free_list[free_idx]->need_unmap) {
			// Only used by unmap, should always need unamp
			partition_phys_unmap(free_list[free_idx]->table,
					     free_list[free_idx]->paddr,
					     util_bit(pgt->granule_shift));
			free_list[free_idx]->need_unmap = false;
		}

		(void)partition_free_phys(partition, free_list[free_idx]->paddr,
					  util_bit(pgt->granule_shift));
		free_list[free_idx]->table  = NULL;
		free_list[free_idx]->paddr  = 0U;
		free_list[free_idx]->mapped = false;
	}
}

#if 0
static bool
map_should_set_cont(vmaddr_t virtual_address, size_t size,
		    vmaddr_t entry_address, index_t level)
{
	const pgtable_level_info_t *info = &level_conf[level];

	assert(info->contiguous_entry_cnt != 0U);

	size_t	 cont_size  = info->addr_size * info->contiguous_entry_cnt;
	vmaddr_t cont_start = util_balign_down(entry_address, cont_size);

	assert(!util_add_overflows(cont_start, cont_size - 1U));
	vmaddr_t cont_end = cont_start + cont_size - 1U;

	assert(!util_add_overflows(virtual_address, size - 1U));
	vmaddr_t virtual_end = virtual_address + size - 1U;

	return (cont_start >= virtual_address) && (cont_end &= virtual_end);
}
#endif

static bool
unmap_should_clear_cont(vmaddr_t virtual_address, size_t size, index_t level)
{
	const pgtable_level_info_t *info = &level_conf[level];

	assert(info->contiguous_entry_cnt != 0U);

	size_t	 cont_size  = info->addr_size * info->contiguous_entry_cnt;
	vmaddr_t cont_start = util_balign_down(virtual_address, cont_size);

	assert(!util_add_overflows(cont_start, cont_size - 1U));
	vmaddr_t cont_end = cont_start + cont_size - 1U;

	assert(!util_add_overflows(virtual_address, size - 1U));
	vmaddr_t virtual_end = virtual_address + size - 1U;

	return (cont_start < virtual_address) || (cont_end > virtual_end);
}

static void
unmap_clear_cont_bit(vmsa_level_table_t *table, vmaddr_t virtual_address,
		     index_t			       level,
		     vmsa_page_and_block_attrs_entry_t attr_entry,
		     pgtable_unmap_modifier_args_t    *margs,
		     count_t granule_shift, index_t start_level)
{
	const pgtable_level_info_t *info = &level_conf[level];

	assert(info->contiguous_entry_cnt != 0U);

	// get index range in current table to clear cont bit
	index_t cur_idx =
		get_index(virtual_address, info, (level == start_level));
	index_t idx_start =
		util_balign_down(cur_idx, info->contiguous_entry_cnt);
	index_t idx_end =
		(index_t)(idx_start + info->contiguous_entry_cnt - 1U);

	// start break-before-make sequence: clear all contiguous entries
	for (index_t idx = idx_start; idx <= idx_end; idx++) {
		set_invalid_entry(table, idx);
	}

	// flush all contiguous entries from TLB (note that the CPU may not
	// implement the contiguous bit at this level, so we are required to
	// flush addresses in all entries)
	vmaddr_t vaddr =
		virtual_address &
		~((util_bit(info->lsb) * info->contiguous_entry_cnt) - 1U);
#ifdef ARCH_ARM_FEAT_TLBIRANGE
	if (margs->stage == PGTABLE_HYP_STAGE_1) {
		dsb(false);
		hyp_tlbi_va_range(vaddr,
				  info->contiguous_entry_cnt * info->addr_size,
				  granule_shift);
	} else {
		dsb(margs->outer_shareable);
		hyp_tlbi_ipa_range(vaddr,
				   info->contiguous_entry_cnt * info->addr_size,
				   granule_shift, margs->outer_shareable);
	}
#else
	dsb(margs->outer_shareable);
	for (index_t i = 0; i < info->contiguous_entry_cnt; i++) {
		if (margs->stage == PGTABLE_HYP_STAGE_1) {
			hyp_tlbi_va(vaddr);
		} else {
			vm_tlbi_ipa(vaddr, margs->outer_shareable);
		}
		vaddr += info->addr_size;
	}
	(void)granule_shift;
#endif

	// Restore the entries other than cur_idx, with the cont bit cleared
	vmsa_upper_attrs_t upper_attrs =
		vmsa_page_and_block_attrs_entry_get_upper_attrs(&attr_entry);
	vmsa_lower_attrs_t lower_attrs =
		vmsa_page_and_block_attrs_entry_get_lower_attrs(&attr_entry);
	vmsa_common_upper_attrs_t upper_attrs_bitfield =
		vmsa_common_upper_attrs_cast(upper_attrs);
	assert(vmsa_common_upper_attrs_get_cont(&upper_attrs_bitfield));
	vmsa_common_upper_attrs_set_cont(&upper_attrs_bitfield, false);
	upper_attrs = vmsa_common_upper_attrs_raw(upper_attrs_bitfield);
	vmsa_page_and_block_attrs_entry_set_upper_attrs(&attr_entry,
							upper_attrs);

	vmsa_entry_t entry = {
		.attrs = attr_entry,
	};

	const bool use_block =
		pgtable_entry_types_get_block(&info->allowed_types);
	paddr_t		      entry_phys = 0U;
	pgtable_entry_types_t type	 = pgtable_entry_types_default();
	for (index_t idx = idx_start; idx <= idx_end; idx++) {
		if (idx == cur_idx) {
			// This should be left invalid
		} else if (use_block) {
			pgtable_entry_types_set_block(&type, true);
			get_entry_paddr(info, &entry, type, &entry_phys);
			entry_phys &= ~((util_bit(info->lsb) *
					 info->contiguous_entry_cnt) -
					1U);

			set_block_entry(table, idx, entry_phys, upper_attrs,
					lower_attrs, false, false, false);
		} else {
			pgtable_entry_types_set_page(&type, true);
			get_entry_paddr(info, &entry, type, &entry_phys);
			entry_phys &= ~((util_bit(info->lsb) *
					 info->contiguous_entry_cnt) -
					1U);
			set_page_entry(table, idx, entry_phys, upper_attrs,
				       lower_attrs, false, false);
		}
		entry_phys += info->addr_size;
	}
}

// @brief Unmap the current entry if possible.
//
// This modifier will try to:
// * Decrease the reference count (entry count) of current entry. If it's
// allowed (not preserved) and possible (ref count == 0), it will free the
// next page table level. In this case, It will guide @see
// translation_table_walk to step onto the next entry at the same level, and
// update the size as well.
// * Invalidate current entry.
static pgtable_modifier_ret_t
unmap_modifier(pgtable_t *pgt, vmaddr_t virtual_address, size_t size,
	       index_t idx, index_t level, pgtable_entry_types_t type,
	       stack_elem_t stack[PGTABLE_LEVEL_NUM], void *data,
	       index_t *next_level, vmaddr_t *next_virtual_address,
	       size_t *next_size, bool only_matching)
{
	const pgtable_level_info_t    *cur_level_info = NULL;
	pgtable_unmap_modifier_args_t *margs =
		(pgtable_unmap_modifier_args_t *)data;
	pgtable_modifier_ret_t vret	 = PGTABLE_MODIFIER_RET_CONTINUE;
	vmsa_level_table_t    *cur_table = NULL;
	vmsa_entry_t	       cur_entry;
	bool		       need_dec = false;

	assert(pgt != NULL);

	// current level should be mapped
	assert(stack[level].mapped);
	cur_table = stack[level].table;

	cur_level_info = &level_conf[level];
	// FIXME: if cur_entry is not used, remove it
	cur_entry = get_entry(cur_table, idx);

	// Set invalid entry and unmap/free the page level
	// NOTE: it's possible to forecast if we can free the whole sub page
	// table levels when we got a page table level entry, but it's just for
	// certain cases (especially the last second page)

	// No need to decrease entry count in upper page table level by default,
	// for INVALID entry.
	need_dec = false;

	if (only_matching && (pgtable_entry_types_get_block(&type) ||
			      pgtable_entry_types_get_page(&type))) {
		// Check if it is mapped to different phys_addr than expected
		// If so, do not unmap this address
		paddr_t phys_addr;

		get_entry_paddr(cur_level_info, &cur_entry, type, &phys_addr);
		if ((phys_addr < margs->phys) ||
		    (phys_addr > (margs->phys + margs->size - 1U))) {
			goto out;
		}
	}

	// Split the block if necessary
	if (pgtable_entry_types_get_block(&type)) {
		size_t	 addr_size = cur_level_info->addr_size;
		vmaddr_t entry_virtual_address =
			entry_start_address(virtual_address, cur_level_info);
		if ((virtual_address != entry_virtual_address) ||
		    (size < addr_size)) {
			// Partial unmap; split the block into 4K pages.
			vmsa_page_and_block_attrs_entry_t attr_entry =
				vmsa_page_and_block_attrs_entry_cast(
					vmsa_general_entry_raw(cur_entry.base));
			vmsa_lower_attrs_t lower_attrs;
			lower_attrs =
				vmsa_page_and_block_attrs_entry_get_lower_attrs(
					&attr_entry);
			vmsa_upper_attrs_t upper_attrs =
				vmsa_page_and_block_attrs_entry_get_upper_attrs(
					&attr_entry);
			paddr_t entry_phys;
			get_entry_paddr(cur_level_info, &cur_entry, type,
					&entry_phys);

			pgtable_map_modifier_args_t mremap_args = { 0 };
			mremap_args.phys			= entry_phys;
			mremap_args.partition	= margs->partition;
			mremap_args.lower_attrs = lower_attrs;
			mremap_args.upper_attrs = upper_attrs;
			mremap_args.new_page_start_level =
				PGTABLE_INVALID_LEVEL;
			mremap_args.try_map = true;
			mremap_args.stage   = margs->stage;

			vret = pgtable_split_block(
				pgt, virtual_address, size, cur_entry, idx,
				level, type, stack, &mremap_args, next_level,
				next_virtual_address, next_size);

			goto out;
		}
	}

	if (pgtable_entry_types_get_block(&type) ||
	    pgtable_entry_types_get_page(&type)) {
		vmsa_upper_attrs_t upper_attrs = get_upper_attr(cur_entry);
		vmsa_common_upper_attrs_t upper_attrs_bitfield =
			vmsa_common_upper_attrs_cast(upper_attrs);

		// clear contiguous bit if needed
		if (vmsa_common_upper_attrs_get_cont(&upper_attrs_bitfield) &&
		    unmap_should_clear_cont(virtual_address, size, level)) {
			vmsa_page_and_block_attrs_entry_t attr_entry =
				vmsa_page_and_block_attrs_entry_cast(
					vmsa_general_entry_raw(cur_entry.base));
			unmap_clear_cont_bit(cur_table, virtual_address, level,
					     attr_entry, margs,
					     pgt->granule_shift,
					     pgt->start_level);
		} else {
			set_invalid_entry(cur_table, idx);

			// need to decrease entry count for this table level
			need_dec = true;

			if (margs->stage == PGTABLE_HYP_STAGE_1) {
				dsb(false);
				hyp_tlbi_va(virtual_address);
			} else {
				dsb(margs->outer_shareable);
				vm_tlbi_ipa(virtual_address,
					    margs->outer_shareable);
			}
		}
	} else {
		assert(pgtable_entry_types_get_invalid(&type));
	}

	if (level != pgt->start_level) {
		check_refcount(pgt, margs->partition, virtual_address, size,
			       level - 1U, stack, need_dec,
			       margs->preserved_size, next_level,
			       next_virtual_address, next_size);
	}

out:
	cur_table = NULL;

	return vret;
}

// @brief Pre-allocate specified page table level for certain virtual address
// range.
//
// This modifier just allocate/adds some page table level using the specified
// partition. The usage of this call is to guarantee the currently used page
// table level is still valid after release certain partition.
static pgtable_modifier_ret_t
prealloc_modifier(pgtable_t *pgt, vmaddr_t virtual_address, size_t size,
		  index_t level, pgtable_entry_types_t type,
		  stack_elem_t stack[PGTABLE_LEVEL_NUM], void *data,
		  index_t *next_level, vmaddr_t *next_virtual_address,
		  size_t *next_size)
{
	pgtable_prealloc_modifier_args_t *margs =
		(pgtable_prealloc_modifier_args_t *)data;
	pgtable_modifier_ret_t	    vret = PGTABLE_MODIFIER_RET_CONTINUE;
	error_t			    ret	 = OK;
	const pgtable_level_info_t *cur_level_info = NULL;
	paddr_t			    new_pgt_paddr;
	size_t			    addr_size = 0U, level_size = 0U;
	vmsa_level_table_t	   *new_pgt = NULL;

	assert(pgtable_entry_types_get_invalid(&type));
	assert(data != NULL);
	assert(pgt != NULL);

	assert(stack[level].mapped);

	cur_level_info = &level_conf[level];
	addr_size      = cur_level_info->addr_size;
	level_size     = size_on_level(virtual_address, size, cur_level_info);

	// FIXME: since all size are at least page aligned, level_size should also
	// be page aligned, add assert here

	if (addr_size <= level_size) {
		// hook pages to the existing page table levels
		// for the case that the root level is the level need to
		// preserve, it just return since new_page_start_level is not
		// set
		if (margs->new_page_start_level != PGTABLE_INVALID_LEVEL) {
			set_pgtables(virtual_address, stack,
				     margs->new_page_start_level, level, 0U,
				     pgt->start_level);

			margs->new_page_start_level = PGTABLE_INVALID_LEVEL;
		}

		// go to next entry at the same level
		goto out;
	} else {
		// if (addr_size > level_size)
		ret = alloc_level_table(margs->partition,
					util_bit(pgt->granule_shift),
					util_bit(pgt->granule_shift),
					&new_pgt_paddr, &new_pgt);
		if (ret != OK) {
			LOG(ERROR, WARN, "Failed to allocate page.\n");
			vret	     = PGTABLE_MODIFIER_RET_ERROR;
			margs->error = ret;
			goto out;
		}

		if (margs->new_page_start_level == PGTABLE_INVALID_LEVEL) {
			margs->new_page_start_level =
				level > pgt->start_level ? level - 1U : level;
		}

		stack[level + 1U] = (stack_elem_t){
			.paddr	    = new_pgt_paddr,
			.table	    = new_pgt,
			.mapped	    = true,
			.need_unmap = false,
			.entry_cnt  = level_conf[level + 1U].entry_cnt,
		};

		// step into the next sub level, with nothing stepped
		*next_virtual_address = virtual_address;
		*next_size	      = size;
		*next_level	      = level + 1U;
	}

out:
	return vret;
}

#if !defined(NDEBUG)
static pgtable_modifier_ret_t
dump_modifier(vmaddr_t virtual_address, size_t size,
	      stack_elem_t stack[PGTABLE_LEVEL_NUM], index_t idx, index_t level,
	      pgtable_entry_types_t type)
{
	const pgtable_level_info_t *cur_level_info = NULL;
	vmsa_level_table_t	   *cur_table	   = NULL;
	vmsa_entry_t		    cur_entry;
	uint64_t		   *entry_val = &cur_entry.base.bf[0];
	paddr_t			    p;
	count_t			    refcount;
	vmaddr_t		    cur_virtual_address;
	const char		   *msg_type = "[X]";
	char			    indent[16];
	index_t			    i;
	pgtable_modifier_ret_t	    vret      = PGTABLE_MODIFIER_RET_CONTINUE;
	size_t			    addr_size = 0U;

	if (size == 0U) {
		vret = PGTABLE_MODIFIER_RET_STOP;
		goto out;
	}

	assert(stack[level].mapped);
	cur_table = stack[level].table;

	cur_level_info = &level_conf[level];
	addr_size      = cur_level_info->addr_size;
	cur_entry      = get_entry(cur_table, idx);
	refcount       = get_table_refcount(cur_table, idx);

	if (!pgtable_entry_types_get_invalid(&type)) {
		get_entry_paddr(cur_level_info, &cur_entry, type, &p);
	} else {
		p = 0U;
	}

	// FIXME: check if cur_virtual_address is right
	cur_virtual_address = set_index(virtual_address, cur_level_info, idx) &
			      (~util_mask(cur_level_info->lsb));

	assert((size_t)level < (sizeof(indent) - 1U));
	indent[0] = '|';
	for (i = 1; i <= level; i++) {
		indent[i] = '\t';
	}
	indent[i] = '\0';

	if (pgtable_entry_types_get_next_level_table(&type)) {
		msg_type = "[Table]";
		LOG(DEBUG, INFO,
		    "{:s}->{:s} entry[{:#x}] virtual_address({:#x})",
		    (register_t)indent, (register_t)msg_type, *entry_val,
		    cur_virtual_address);
		LOG(DEBUG, INFO,
		    "{:s}phys({:#x}) idx({:d}) cnt({:d}) level({:d})",
		    (register_t)indent, (register_t)p, (register_t)idx,
		    (register_t)refcount, (register_t)cur_level_info->level);
		LOG(DEBUG, INFO, "{:s}addr_size({:#x})", (register_t)indent,
		    (register_t)addr_size);
	} else if (pgtable_entry_types_get_block(&type) ||
		   pgtable_entry_types_get_page(&type)) {
		if (pgtable_entry_types_get_block(&type)) {
			msg_type = "[Block]";
		} else {
			msg_type = "[Page]";
		}
		LOG(DEBUG, INFO,
		    "{:s}->{:s} entry[{:#x}] virtual_address({:#x})",
		    (register_t)indent, (register_t)msg_type,
		    (register_t)*entry_val, (register_t)cur_virtual_address);
		LOG(DEBUG, INFO, "{:s}phys({:#x}) idx({:d}) level({:d})",
		    (register_t)indent, (register_t)p, (register_t)idx,
		    (register_t)cur_level_info->level);
		LOG(DEBUG, INFO, "{:s}addr_size({:#x})", (register_t)indent,
		    (register_t)addr_size);
	} else {
		if (!pgtable_entry_types_get_invalid(&type)) {
			if (pgtable_entry_types_get_reserved(&type)) {
				msg_type = "[Reserved]";
			} else if (pgtable_entry_types_get_error(&type)) {
				msg_type = "[Error]";
			} else {
				// Nothing to do
			}
			LOG(DEBUG, INFO,
			    "{:s}->{:s} virtual_address({:#x}) idx({:d})",
			    (register_t)indent, (register_t)msg_type,
			    (register_t)cur_virtual_address, (register_t)idx);
		}
	}

out:
	cur_table = NULL;

	return vret;
}
#endif // !defined(NDEBUG)

#if defined(HOST_TEST)
static pgtable_modifier_ret_t
external_modifier(pgtable_t *pgt, vmaddr_t virtual_address, size_t size,
		  index_t idx, index_t level, pgtable_entry_types_t type,
		  stack_elem_t stack[PGTABLE_LEVEL_NUM], void *data,
		  index_t *next_level, vmaddr_t *next_virtual_address,
		  size_t *next_size, paddr_t next_table)
{
	ext_modifier_args_t   *margs	 = (ext_modifier_args_t *)data;
	void		      *func_data = margs->data;
	pgtable_modifier_ret_t ret	 = PGTABLE_MODIFIER_RET_STOP;

	if (margs->func != NULL) {
		ret = margs->func(pgt, virtual_address, size, idx, level, type,
				  stack, func_data, next_level,
				  next_virtual_address, next_size, next_table);
	}

	return ret;
}
#endif // !defined(HOST_TEST)

// @brief Generic code to walk through translation table.
//
// This function is generic for stage 1 and stage 2 translation table walking.
// Depends on the specified event, it triggers proper function (modifier) to
// handle current table entry.
// When the modifier function is called, it can get the following information:
// * Current start virtual address
// * Current entry type
// * Current page table level physical address
// * Current page table level
//   There are two kinds of level. One level is 0 based. The other concept of
//   level is based on ARM reference manual. It's defined @see level_type.
//   For example, the 64K granule page table with LPA feature does not have
//   the first level.
//   The previous level is used to control the loop, and the second level is
//   used to manipulate page table level stack.
// * Remaining size for the walking
//   If the address range specified by current start virtual address and
//   remaining size is fully visited, this function will return.
// * The current page table control structure
// * The page table level physical address stack
//   This stack can be used to get back to upper level.
// * A private pointer which modifier can interpret by itself, it remains the
//   same during the walking procedure.
//
// The modifier function can also control the walking by set:
// * Next page table physical address
// * Next level
// * Next start virtual address
// * Next remaining size
// And these four variables can fully control the flow of walking. Also, the
// return value of the modifier function provides a simple way to
// stop/continue, or report errors.
//
// The walking process is surely ended:
// * Finished visiting the specified address range.
// * Reach the maximum virtual address.
// But when the modifier changed internal variables, it's modifier's
// responsibility to make sure it can ended.
//
// @param pgt page table control structure.
// @param root_pa the physical address of the page table we started to walk.
// It's allowed to use the mid level page tables to do the walking.
// @param root the virtual address of page table.
// @param virtual_address the start virtual address.
// @param size the size of virtual address it needs to visit.
// @param event specifies the modifier to call.
// @param expected specifies the table entry type the modifier needs.
// @param data specifier an opaque data structure specific for modifier.
// @return true if the finished the walking without error. Or false indicates
// the failure.
static bool
translation_table_walk(pgtable_t *pgt, vmaddr_t virtual_address,
		       size_t virtual_address_size,
		       pgtable_translation_table_walk_event_t event,
		       pgtable_entry_types_t expected, void *data)
{
	paddr_t		      root_pa	  = pgt->root_pgtable;
	vmsa_level_table_t   *root	  = pgt->root;
	index_t		      start_level = pgt->start_level;
	index_t		      prev_level;
	index_t		      prev_idx;
	vmaddr_t	      prev_virtual_address;
	size_t		      prev_size;
	vmsa_entry_t	      prev_entry;
	pgtable_entry_types_t prev_type;

	// loop control variable
	index_t	 cur_level	     = start_level;
	paddr_t	 cur_table_paddr     = 0U;
	vmaddr_t cur_virtual_address = virtual_address;

	const pgtable_level_info_t *cur_level_info = NULL;
	index_t			    cur_idx;
	stack_elem_t		    stack[PGTABLE_LEVEL_NUM];
	vmsa_level_table_t	   *cur_table = NULL;
	vmsa_entry_t		    cur_entry;
	pgtable_entry_types_t	    cur_type;
	size_t			    cur_size = virtual_address_size;
	// ret: indicates whether walking is successful or not.
	// done: indicates the walking got a stop sign and need to return. It
	// can be changed by modifier.
	// ignores the modifier.
	bool ret = false, done = false;

	stack[start_level]	  = (stack_elem_t){ 0U };
	stack[start_level].paddr  = root_pa;
	stack[start_level].table  = root;
	stack[start_level].mapped = true;
	stack[start_level].entry_cnt =
		(count_t)(pgt->start_level_size / sizeof(cur_entry));

	while (cur_level < (index_t)util_array_size(level_conf)) {
		cur_level_info = &level_conf[cur_level];
		cur_idx	       = get_index(cur_virtual_address, cur_level_info,
					   (cur_level == start_level));

		if (compiler_unexpected(cur_level_info->is_offset)) {
			// Arrived offset segment, mapping is supposed to
			// be finished
			panic("pgtable walk depth error");
		}

		if (compiler_unexpected(cur_idx >=
					stack[cur_level].entry_cnt)) {
			// Index is outside the bounds of the table; address
			// range was not properly range-checked
			LOG(ERROR, WARN,
			    "Stepped out of the table (va {:#x}, level {:d}, idx {:d})",
			    cur_virtual_address, cur_level, cur_idx);
			panic("pgtable walk");
		}

		cur_table_paddr = stack[cur_level].paddr;
		if (stack[cur_level].mapped) {
			cur_table = stack[cur_level].table;
		} else {
			cur_table = (vmsa_level_table_t *)partition_phys_map(
				cur_table_paddr,
				stack[cur_level].entry_cnt * sizeof(cur_entry));
			if (compiler_unexpected(cur_table == NULL)) {
				LOG(ERROR, WARN,
				    "Failed to map table (pa {:#x}, level {:d}, idx {:d})\n",
				    cur_table_paddr, cur_level, cur_idx);
				panic("pgtable fault");
			}

			stack[cur_level].table	    = cur_table;
			stack[cur_level].mapped	    = true;
			stack[cur_level].need_unmap = true;
		}

		cur_entry = get_entry(cur_table, cur_idx);
		cur_type  = get_entry_type(&cur_entry, cur_level_info);

		// record the argument for modifier
		prev_virtual_address = cur_virtual_address;
		prev_level	     = cur_level;
		prev_idx	     = cur_idx;
		prev_entry	     = cur_entry;
		prev_type	     = cur_type;
		prev_size	     = cur_size;

		if (pgtable_entry_types_get_next_level_table(&cur_type)) {
			cur_level++;
			assert(cur_level < PGTABLE_LEVEL_NUM);

			get_entry_paddr(
				cur_level_info, &cur_entry,
				pgtable_entry_types_cast(
					PGTABLE_ENTRY_TYPES_NEXT_LEVEL_TABLE_MASK),
				&cur_table_paddr);

			cur_level_info		   = &level_conf[cur_level];
			stack[cur_level]	   = (stack_elem_t){ 0U };
			stack[cur_level].paddr	   = cur_table_paddr;
			stack[cur_level].mapped	   = false;
			stack[cur_level].table	   = NULL;
			stack[cur_level].entry_cnt = cur_level_info->entry_cnt;
		} else if (pgtable_entry_types_get_invalid(&cur_type) ||
			   pgtable_entry_types_get_page(&cur_type) ||
			   pgtable_entry_types_get_block(&cur_type)) {
			// for invalid entry, it must be handled by modifier.
			// Also by default, the next entry for the walking
			// is simply set to the next entry. The modifier should
			// guide the walking to go to the proper next entry.
			// Unless the modifier asks for continue the walking,
			// the walking process must stopped by default.

			// update virt address to visit next entry
			cur_virtual_address = step_virtual_address(
				cur_virtual_address, cur_level_info);
			size_t step_size =
				(cur_virtual_address - prev_virtual_address);

			if (cur_size >= step_size) {
				cur_size -= step_size;
			} else {
				cur_size = 0U;
			}

			// If we're at the lowest level
			if (pgtable_entry_types_get_page(
				    &cur_level_info->allowed_types)) {
				if (compiler_unexpected(
					    prev_size <
					    cur_level_info->addr_size)) {
					// wrong, size must be at least multiple
					// of page
					panic("pgtable bad size");
				}
			}

			if (cur_size == 0U) {
				// the whole walk is done, but modifier can
				// still ask for loop by changing the size
				done = true;
				ret  = true;
			} else {
				done = false;
				ret  = true;
			}

			// Iterate up on the last entry in the level(s)
			while (cur_idx == (stack[cur_level].entry_cnt - 1U)) {
				if (cur_level == start_level) {
					done = true;
					break;
				} else {
					cur_level--;
				}

				cur_level_info = &level_conf[cur_level];
				// cur_virtual_address is already stepped, use
				// previous one to check
				cur_idx = get_index(prev_virtual_address,
						    cur_level_info,
						    (cur_level == start_level));
			}
		} else {
			// shouldn't be here
			panic("pgtable corrupt entry");
		}

		cur_table = NULL;

		if (!pgtable_entry_types_is_empty(
			    pgtable_entry_types_intersection(prev_type,
							     expected))) {
			pgtable_modifier_ret_t vret;

			switch (event) {
			case PGTABLE_TRANSLATION_TABLE_WALK_EVENT_MMAP:
				vret = map_modifier(pgt, prev_virtual_address,
						    prev_size, prev_entry,
						    prev_idx, prev_level,
						    prev_type, stack, data,
						    &cur_level,
						    &cur_virtual_address,
						    &cur_size, cur_table_paddr);
				break;
			case PGTABLE_TRANSLATION_TABLE_WALK_EVENT_UNMAP:
				vret = unmap_modifier(pgt, prev_virtual_address,
						      prev_size, prev_idx,
						      prev_level, prev_type,
						      stack, data, &cur_level,
						      &cur_virtual_address,
						      &cur_size, false);
				break;
			case PGTABLE_TRANSLATION_TABLE_WALK_EVENT_UNMAP_MATCH:
				vret = unmap_modifier(pgt, prev_virtual_address,
						      prev_size, prev_idx,
						      prev_level, prev_type,
						      stack, data, &cur_level,
						      &cur_virtual_address,
						      &cur_size, true);
				break;
			case PGTABLE_TRANSLATION_TABLE_WALK_EVENT_LOOKUP:
				vret = lookup_modifier(pgt, prev_entry,
						       prev_level, prev_type,
						       data);
				break;
			case PGTABLE_TRANSLATION_TABLE_WALK_EVENT_PREALLOC:
				vret = prealloc_modifier(
					pgt, prev_virtual_address, prev_size,
					prev_level, prev_type, stack, data,
					&cur_level, &cur_virtual_address,
					&cur_size);
				break;
#ifndef NDEBUG
			case PGTABLE_TRANSLATION_TABLE_WALK_EVENT_DUMP:
				vret = dump_modifier(prev_virtual_address,
						     prev_size, stack, prev_idx,
						     prev_level, prev_type);
				break;
#endif
#if defined(HOST_TEST)
			case PGTABLE_TRANSLATION_TABLE_WALK_EVENT_EXTERNAL:
				vret = external_modifier(
					pgt, prev_virtual_address, prev_size,
					prev_idx, prev_level, prev_type, stack,
					data, &cur_level, &cur_virtual_address,
					&cur_size, cur_table_paddr);
				break;
#endif
			default:
				panic("pgtable bad event");
			}

			if (vret == PGTABLE_MODIFIER_RET_STOP) {
				ret  = true;
				done = true;
			} else if (vret == PGTABLE_MODIFIER_RET_ERROR) {
				ret  = false;
				done = true;
			} else if (vret == PGTABLE_MODIFIER_RET_CONTINUE) {
				// It's modifier's responsibility to work around
				// the walk error if it wishes to continue
				ret  = true;
				done = false;
			} else {
				// unknown return, just stop
				panic("pgtable bad vret");
			}
		}

		while (prev_level > cur_level) {
			// Discard page table level which is not used. If
			// modifier changes stack, it's modifier's
			// responsibility to unmap & maintain correct stack
			// status.
			// Since it's possible for modifier to unmap the table
			// level, need to double check if need to unmap the
			// levels here.
			if (!stack[prev_level].mapped) {
				prev_level--;
				continue;
			}

			if (stack[prev_level].need_unmap) {
				partition_phys_unmap(
					stack[prev_level].table,
					stack[prev_level].paddr,
					util_bit(pgt->granule_shift));
				stack[prev_level].need_unmap = false;
			}
			stack[prev_level].table	 = NULL;
			stack[prev_level].paddr	 = 0U;
			stack[prev_level].mapped = false;
			prev_level--;
		}

		// only next table should continue the loop
		if (done || (cur_size == 0U)) {
			break;
		}
	}
	while (cur_level > start_level) {
		if (stack[cur_level].mapped && stack[cur_level].need_unmap) {
			partition_phys_unmap(stack[cur_level].table,
					     stack[cur_level].paddr,
					     util_bit(pgt->granule_shift));
			stack[cur_level].need_unmap = false;
		}
		stack[cur_level].mapped = false;
		stack[cur_level].table	= NULL;
		cur_level--;
	}

	return ret;
}

static get_start_level_info_ret_t
get_start_level_info(const pgtable_level_info_t *infos, index_t msb,
		     bool is_stage2)
{
	get_start_level_info_ret_t ret = { .level = 0U, .size = 0UL };

	uint8_t level;
	count_t msb_offset = is_stage2 ? 4U : 0U;

	for (level = PGTABLE_LEVEL_NUM - 1U; level < PGTABLE_LEVEL_NUM;
	     level--) {
		const pgtable_level_info_t *level_info = &infos[level];
		if ((msb <= (level_info->msb + msb_offset)) &&
		    (msb >= level_info->lsb)) {
			size_t entry_cnt = util_bit(msb - level_info->lsb + 1U);
			ret.level	 = level;
			ret.size = sizeof(vmsa_general_entry_t) * entry_cnt;
			break;
		}
	}

	return ret;
}

void
pgtable_handle_boot_cold_init(void)
{
	index_t	      bottom_msb;
	const count_t page_shift = SHIFT_4K;
	partition_t  *partition	 = partition_get_private();

#if !defined(HOST_TEST)
	ID_AA64MMFR2_EL1_t mmfr2 = register_ID_AA64MMFR2_EL1_read();
	assert(ID_AA64MMFR2_EL1_get_BBM(&mmfr2) >= CPU_PGTABLE_BBM_LEVEL);
#endif
	spinlock_init(&hyp_pgtable.lock);

	hyp_pgtable.bottom_control.granule_shift = page_shift;
	hyp_pgtable.bottom_control.address_bits	 = HYP_ASPACE_LOW_BITS;
	bottom_msb				 = HYP_ASPACE_LOW_BITS - 1U;

	assert((HYP_ASPACE_LOW_BITS != level_conf[0].msb + 1) ||
	       (HYP_ASPACE_LOW_BITS != level_conf[1].msb + 1) ||
	       (HYP_ASPACE_LOW_BITS != level_conf[2].msb + 1) ||
	       (HYP_ASPACE_LOW_BITS != level_conf[3].msb + 1));

	get_start_level_info_ret_t bottom_info =
		get_start_level_info(level_conf, bottom_msb, false);
	hyp_pgtable.bottom_control.start_level	    = bottom_info.level;
	hyp_pgtable.bottom_control.start_level_size = bottom_info.size;

#if defined(ARCH_ARM_FEAT_VHE)
	index_t top_msb;
	error_t ret = OK;

	// FIXME: refine with more configurable code
	hyp_pgtable.top_control.granule_shift = page_shift;
	hyp_pgtable.top_control.address_bits  = HYP_ASPACE_HIGH_BITS;
	top_msb				      = HYP_ASPACE_HIGH_BITS - 1U;
	// FIXME: change to static check (with constant?)??
	// Might be better to use hyp_pgtable.top_control.address_bits
	assert((HYP_ASPACE_HIGH_BITS != level_conf[0].msb + 1) ||
	       (HYP_ASPACE_HIGH_BITS != level_conf[1].msb + 1) ||
	       (HYP_ASPACE_HIGH_BITS != level_conf[2].msb + 1) ||
	       (HYP_ASPACE_HIGH_BITS != level_conf[3].msb + 1));

	// update level info based on virtual_address bits
	get_start_level_info_ret_t top_info =
		get_start_level_info(level_conf, top_msb, false);
	hyp_pgtable.top_control.start_level	 = top_info.level;
	hyp_pgtable.top_control.start_level_size = top_info.size;

#if defined(HOST_TEST)
	// allocate the top page table
	ret = alloc_level_table(partition, top_info.size,
				util_max(top_info.size, VMSA_TABLE_MIN_ALIGN),
				&hyp_pgtable.top_control.root_pgtable,
				&hyp_pgtable.top_control.root);
	if (ret != OK) {
		LOG(ERROR, WARN, "Failed to allocate high page table level.\n");
		goto out;
	}
#else
	hyp_pgtable.top_control.root =
		(vmsa_level_table_t *)&aarch64_pt_ttbr_level1;
	hyp_pgtable.top_control.root_pgtable = partition_virt_to_phys(
		partition, (uintptr_t)hyp_pgtable.top_control.root);
#endif

	// allocate the root page table
	ret = alloc_level_table(partition, bottom_info.size,
				util_max(bottom_info.size,
					 VMSA_TABLE_MIN_ALIGN),
				&hyp_pgtable.bottom_control.root_pgtable,
				&hyp_pgtable.bottom_control.root);
	if (ret != OK) {
		LOG(ERROR, WARN,
		    "Failed to allocate bottom page table level.\n");
		goto out;
	}
#else
	hyp_pgtable.bottom_control.root =
		(vmsa_level_table_t *)&aarch64_pt_ttbr_level1;
	hyp_pgtable.bottom_control.root_pgtable = partition_virt_to_phys(
		partition, (uintptr_t)hyp_pgtable.bottom_control.root);
#endif

	ttbr0_phys = hyp_pgtable.bottom_control.root_pgtable;

	// activate the lower address space now for cold-boot
	pgtable_handle_boot_runtime_warm_init();

#if defined(ARCH_ARM_FEAT_VHE)
out:
	if (ret != OK) {
		panic("Failed to initialize hypervisor root page-table");
	}
#endif
}

#if !defined(HOST_TEST)
void
pgtable_handle_boot_runtime_warm_init(void)
{
#if defined(ARCH_ARM_FEAT_VHE)
	TTBR0_EL2_t ttbr0_val = TTBR0_EL2_default();
	TTBR0_EL2_set_BADDR(&ttbr0_val, ttbr0_phys);
	TTBR0_EL2_set_CnP(&ttbr0_val, true);

	TCR_EL2_E2H1_t tcr_val = register_TCR_EL2_E2H1_read();
	TCR_EL2_E2H1_set_T0SZ(&tcr_val, (uint8_t)(64U - HYP_ASPACE_LOW_BITS));
	TCR_EL2_E2H1_set_EPD0(&tcr_val, false);
	TCR_EL2_E2H1_set_ORGN0(&tcr_val, TCR_RGN_NORMAL_WRITEBACK_RA_WA);
	TCR_EL2_E2H1_set_IRGN0(&tcr_val, TCR_RGN_NORMAL_WRITEBACK_RA_WA);
	TCR_EL2_E2H1_set_SH0(&tcr_val, TCR_SH_INNER_SHAREABLE);
	TCR_EL2_E2H1_set_TG0(&tcr_val, TCR_TG0_GRANULE_SIZE_4KB);

	register_TTBR0_EL2_write_barrier(ttbr0_val);
	register_TCR_EL2_E2H1_write_barrier(tcr_val);

	asm_context_sync_fence();
#endif
}
#endif

#if defined(HOST_TEST)
void
pgtable_hyp_destroy(partition_t *partition)
{
	vmaddr_t virtual_address = 0x0U;
	size_t	 size		 = 0x0U;

	assert(partition != NULL);

	// we should unmap everything
	virtual_address = 0x0U;
	size		= util_bit(hyp_pgtable.bottom_control.address_bits);
	pgtable_hyp_unmap(partition, virtual_address, size,
			  PGTABLE_HYP_UNMAP_PRESERVE_NONE);

	virtual_address = ~util_mask(hyp_pgtable.top_control.address_bits);
	size		= util_bit(hyp_pgtable.top_control.address_bits);
	pgtable_hyp_unmap(partition, virtual_address, size,
			  PGTABLE_HYP_UNMAP_PRESERVE_NONE);

	// free top level page table
	partition_free(partition, hyp_pgtable.top_control.root,
		       util_bit(hyp_pgtable.top_control.granule_shift));
	hyp_pgtable.top_control.root = NULL;
	partition_free(partition, hyp_pgtable.bottom_control.root,
		       util_bit(hyp_pgtable.bottom_control.granule_shift));
	hyp_pgtable.bottom_control.root = NULL;

	memset(&hyp_pgtable, 0, sizeof(hyp_pgtable));
}
#endif

bool
pgtable_hyp_lookup(uintptr_t virtual_address, paddr_t *mapped_base,
		   size_t *mapped_size, pgtable_hyp_memtype_t *mapped_memtype,
		   pgtable_access_t *mapped_access)
{
	bool			       walk_ret = false;
	pgtable_lookup_modifier_args_t margs	= { 0 };
	pgtable_entry_types_t entry_types	= pgtable_entry_types_default();
	vmsa_upper_attrs_t    upper_attrs;
	vmsa_lower_attrs_t    lower_attrs;
	pgtable_t	     *pgt = NULL;

	assert(mapped_base != NULL);
	assert(mapped_size != NULL);
	assert(mapped_memtype != NULL);
	assert(mapped_access != NULL);

	bool is_high = is_high_virtual_address(virtual_address);
	if (is_high) {
#if defined(ARCH_ARM_FEAT_VHE)
		pgt = &hyp_pgtable.top_control;
#else
		walk_ret = false;
		goto out;
#endif
	} else {
		pgt = &hyp_pgtable.bottom_control;
	}

	if (!addr_check(virtual_address, pgt->address_bits, is_high)) {
		walk_ret = false;
		goto out;
	}

	if (is_high) {
		virtual_address &= util_mask(pgt->address_bits);
	}

	pgtable_entry_types_set_block(&entry_types, true);
	pgtable_entry_types_set_page(&entry_types, true);
	// just try to lookup a page, but if it's a block, the modifier will
	// stop the walk and return success
	walk_ret = translation_table_walk(
		pgt, virtual_address, util_bit(pgt->granule_shift),
		PGTABLE_TRANSLATION_TABLE_WALK_EVENT_LOOKUP, entry_types,
		&margs);

	if (margs.size == 0U) {
		// Return error (not-mapped) if lookup found no pages.
		walk_ret = false;
	}

	if (walk_ret) {
		*mapped_base = margs.phys;
		*mapped_size = margs.size;

		// FIXME: we can simplify below 4 line
		lower_attrs	= get_lower_attr(margs.entry);
		upper_attrs	= get_upper_attr(margs.entry);
		*mapped_memtype = map_stg1_attr_to_memtype(lower_attrs);
		*mapped_access =
			map_stg1_attr_to_access(upper_attrs, lower_attrs);
	} else {
		*mapped_base	= 0U;
		*mapped_size	= 0U;
		*mapped_memtype = PGTABLE_HYP_MEMTYPE_WRITEBACK;
		*mapped_access	= PGTABLE_ACCESS_NONE;
	}

out:
	return walk_ret;
}

error_t
pgtable_hyp_preallocate(partition_t *partition, uintptr_t virtual_address,
			size_t size)
{
	pgtable_prealloc_modifier_args_t margs = { 0 };
	pgtable_t			*pgt   = NULL;
	pgtable_entry_types_t entry_types      = pgtable_entry_types_default();

	assert(partition != NULL);
	assert((size & (size - 1)) == 0U);
	assert((virtual_address & (size - 1)) == 0);

	bool is_high = is_high_virtual_address(virtual_address);
	if (is_high) {
#if defined(ARCH_ARM_FEAT_VHE)
		pgt = &hyp_pgtable.top_control;
#else
		margs.error = ERROR_ADDR_INVALID;
		goto out;
#endif
	} else {
		pgt = &hyp_pgtable.bottom_control;
	}

	assert(!util_add_overflows(virtual_address, size - 1));

	assert(addr_check(virtual_address, pgt->address_bits, is_high) &&
	       addr_check(virtual_address + size - 1, pgt->address_bits,
			  is_high));

	if (is_high) {
		virtual_address &= util_mask(pgt->address_bits);
	}

	margs.partition		   = partition;
	margs.new_page_start_level = PGTABLE_INVALID_LEVEL;
	margs.error		   = OK;

	pgtable_entry_types_set_invalid(&entry_types, true);
	bool walk_ret = translation_table_walk(
		pgt, virtual_address, size,
		PGTABLE_TRANSLATION_TABLE_WALK_EVENT_PREALLOC, entry_types,
		&margs);

	if (!walk_ret && (margs.error == OK)) {
		margs.error = ERROR_FAILURE;
		goto out;
	}

out:
	return margs.error;
}

// FIXME: right now assume the virt address with size is free,
// no need to retry
// FIXME: assume the size must be single page size or available block
// size, or else, just map it as one single page.
static error_t
pgtable_do_hyp_map(partition_t *partition, uintptr_t virtual_address,
		   size_t size, paddr_t phys, pgtable_hyp_memtype_t memtype,
		   pgtable_access_t access, vmsa_shareability_t shareability,
		   bool try_map, size_t merge_limit)
	REQUIRE_LOCK(pgtable_hyp_map_lock)
{
	pgtable_map_modifier_args_t margs = { 0 };
	vmsa_stg1_lower_attrs_t	    l;
	vmsa_stg1_upper_attrs_t	    u;
	pgtable_t		   *pgt = NULL;

	assert(pgtable_op);

	assert(partition != NULL);

	bool is_high = is_high_virtual_address(virtual_address);
	if (is_high) {
#if defined(ARCH_ARM_FEAT_VHE)
		pgt = &hyp_pgtable.top_control;
#else
		margs.error = ERROR_ADDR_INVALID;
		goto out;
#endif
	} else {
		pgt = &hyp_pgtable.bottom_control;
	}

	if (util_add_overflows(virtual_address, size - 1U)) {
		margs.error = ERROR_ADDR_OVERFLOW;
		goto out;
	}

	if (!util_is_p2aligned(virtual_address, pgt->granule_shift)) {
		margs.error = ERROR_ARGUMENT_ALIGNMENT;
		goto out;
	}

	if (!util_is_p2aligned(phys, pgt->granule_shift)) {
		margs.error = ERROR_ARGUMENT_ALIGNMENT;
		goto out;
	}

	if (!util_is_p2aligned(size, pgt->granule_shift)) {
		margs.error = ERROR_ARGUMENT_ALIGNMENT;
		goto out;
	}

	if (!addr_check(virtual_address, pgt->address_bits, is_high) ||
	    !addr_check(virtual_address + size - 1U, pgt->address_bits,
			is_high)) {
		margs.error = ERROR_ADDR_INVALID;
		goto out;
	}

	if (is_high) {
		virtual_address &= util_mask(pgt->address_bits);
	}

	margs.orig_virtual_address = virtual_address;
	margs.orig_size		   = size;
	margs.phys		   = phys;
	margs.partition		   = partition;
	vmsa_stg1_lower_attrs_init(&l);
	vmsa_stg1_upper_attrs_init(&u);

	map_stg1_memtype_to_attrs(memtype, &l);
	map_stg1_access_to_attrs(access, &u, &l);
	vmsa_stg1_lower_attrs_set_SH(&l, shareability);
	margs.lower_attrs	   = vmsa_stg1_lower_attrs_raw(l);
	margs.upper_attrs	   = vmsa_stg1_upper_attrs_raw(u);
	margs.new_page_start_level = PGTABLE_INVALID_LEVEL;
	margs.error		   = OK;
	margs.try_map		   = try_map;
	margs.stage		   = PGTABLE_HYP_STAGE_1;
	margs.merge_limit	   = merge_limit;

	// FIXME: try to unify the level number, just use one kind of level
	pgtable_entry_types_t entry_types = VMSA_ENTRY_TYPE_LEAF;
	pgtable_entry_types_set_next_level_table(&entry_types, true);
	bool walk_ret = translation_table_walk(
		pgt, virtual_address, size,
		PGTABLE_TRANSLATION_TABLE_WALK_EVENT_MMAP, entry_types, &margs);

	if (!walk_ret && (margs.error == OK)) {
		margs.error = ERROR_FAILURE;
	}
	if ((margs.error != OK) && (margs.partially_mapped_size != 0U)) {
		pgtable_hyp_unmap(partition, virtual_address,
				  margs.partially_mapped_size,
				  PGTABLE_HYP_UNMAP_PRESERVE_ALL);
	}
out:
	return margs.error;
}

error_t
pgtable_hyp_map_merge(partition_t *partition, uintptr_t virtual_address,
		      size_t size, paddr_t phys, pgtable_hyp_memtype_t memtype,
		      pgtable_access_t access, vmsa_shareability_t shareability,
		      size_t merge_limit)
{
	return pgtable_do_hyp_map(partition, virtual_address, size, phys,
				  memtype, access, shareability, true,
				  merge_limit);
}

error_t
pgtable_hyp_remap_merge(partition_t *partition, uintptr_t virtual_address,
			size_t size, paddr_t phys,
			pgtable_hyp_memtype_t memtype, pgtable_access_t access,
			vmsa_shareability_t shareability, size_t merge_limit)
{
	return pgtable_do_hyp_map(partition, virtual_address, size, phys,
				  memtype, access, shareability, false,
				  merge_limit);
}

// FIXME: assume the size must be multiple of single page size or available
// block size, or else, just unmap it with page size aligned range.
// May be something like some blocks + several pages.
//
// Also, will unmap the vaddress without considering the memtype and access
// permission.
//
// It's caller's responsibility to make sure the virt address is already fully
// mapped. There's no roll back, so any failure will cause partially unmap
// operation.
void
pgtable_hyp_unmap(partition_t *partition, uintptr_t virtual_address,
		  size_t size, size_t preserved_prealloc)
{
	pgtable_unmap_modifier_args_t margs = { 0 };
	pgtable_t		     *pgt   = NULL;

	assert(pgtable_op);

	assert(partition != NULL);
	assert(util_is_p2_or_zero(preserved_prealloc));

	bool is_high = is_high_virtual_address(virtual_address);
	if (is_high) {
#if defined(ARCH_ARM_FEAT_VHE)
		pgt = &hyp_pgtable.top_control;
#else
		goto out;
#endif
	} else {
		pgt = &hyp_pgtable.bottom_control;
	}

	assert(!util_add_overflows(virtual_address, size - 1));

	assert(addr_check(virtual_address, pgt->address_bits, is_high));
	assert(addr_check(virtual_address + size - 1, pgt->address_bits,
			  is_high));

	assert(util_is_p2aligned(virtual_address, pgt->granule_shift));
	assert(util_is_p2aligned(size, pgt->granule_shift));

	if (is_high) {
		virtual_address &= util_mask(pgt->address_bits);
	}

	margs.partition	     = partition;
	margs.preserved_size = preserved_prealloc;
	margs.stage	     = PGTABLE_HYP_STAGE_1;

	bool walk_ret = translation_table_walk(
		pgt, virtual_address, size,
		PGTABLE_TRANSLATION_TABLE_WALK_EVENT_UNMAP,
		VMSA_ENTRY_TYPE_LEAF, &margs);
	if (!walk_ret) {
		panic("Error in pgtable_hyp_unmap");
	}

#if !defined(ARCH_ARM_FEAT_VHE)
out:
#endif
	return;
}

void
pgtable_hyp_start(void) LOCK_IMPL
{
	// Nothing to do here.

	// The pgtable_hyp code has to run with a lock and preempt disabled to
	// ensure forward progress and because the code is not thread safe.
	spinlock_acquire(&hyp_pgtable.lock);
#if !defined(NDEBUG)
	assert(!pgtable_op);
	pgtable_op = true;
#endif
}

void
pgtable_hyp_commit(void) LOCK_IMPL
{
	dsb(false);
#if !defined(NDEBUG)
	assert(pgtable_op);
	pgtable_op = false;
#endif
	spinlock_release(&hyp_pgtable.lock);
}

#ifndef NDEBUG
void
pgtable_hyp_dump(void)
{
	pgtable_entry_types_t entry_types =
		pgtable_entry_types_inverse(pgtable_entry_types_default());
	vmaddr_t virtual_address = 0U;
	size_t	 size		 = 0U;

	LOG(DEBUG, INFO, "+---------------- page table ----------------\n");
#if defined(ARCH_ARM_FEAT_VHE)
	LOG(DEBUG, INFO, "| TTBR1[{:#x}]:\n",
	    hyp_pgtable.top_control.root_pgtable);
	size = util_bit(hyp_pgtable.top_control.address_bits);
	(void)translation_table_walk(&hyp_pgtable.top_control, virtual_address,
				     size,
				     PGTABLE_TRANSLATION_TABLE_WALK_EVENT_DUMP,
				     entry_types, NULL);
#endif
	LOG(DEBUG, INFO, "\n");
	LOG(DEBUG, INFO, "| TTBR0[{:#x}]:\n",
	    hyp_pgtable.bottom_control.root_pgtable);
	size = util_bit(hyp_pgtable.bottom_control.address_bits);
	(void)translation_table_walk(&hyp_pgtable.bottom_control,
				     virtual_address, size,
				     PGTABLE_TRANSLATION_TABLE_WALK_EVENT_DUMP,
				     entry_types, NULL);
	LOG(DEBUG, INFO, "+--------------------------------------------\n\n");
}
#endif

#ifdef HOST_TEST
void
pgtable_hyp_ext(vmaddr_t virtual_address, size_t size,
		pgtable_entry_types_t entry_types, ext_func_t func, void *data)
{
	ext_modifier_args_t margs = { 0 };
	pgtable_t	   *pgt	  = NULL;

	bool is_high = is_high_virtual_address(virtual_address);
	if (is_high) {
		pgt = &hyp_pgtable.top_control;
	} else {
		pgt = &hyp_pgtable.bottom_control;
	}

	assert(addr_check(virtual_address, pgt->address_bits, is_high));
	assert(addr_check(virtual_address + size - 1, pgt->address_bits,
			  is_high));

	margs.func = func;
	margs.data = data;

	if (!util_is_p2aligned(size, pgt->granule_shift) ||
	    !util_is_p2aligned(size, pgt->granule_shift)) {
		LOG(DEBUG, INFO, "size not aligned\n");
		goto out;
	}
	if (!addr_check(virtual_address, pgt->address_bits, is_high)) {
		LOG(DEBUG, INFO, "address out of range\n");
		goto out;
	}
	if (!util_is_p2aligned(virtual_address, pgt->granule_shift)) {
		LOG(DEBUG, INFO, "address not aligned\n");
		goto out;
	}

	if (is_high) {
		virtual_address &= util_mask(pgt->address_bits);
	}

	(void)translation_table_walk(
		pgt, virtual_address, size,
		PGTABLE_TRANSLATION_TABLE_WALK_EVENT_EXTERNAL, entry_types,
		&margs);
out:
	return;
}
#endif

#ifndef NDEBUG
void
pgtable_vm_dump(pgtable_vm_t *pgt)
{
	assert(pgt != NULL);

	pgtable_entry_types_t entry_types =
		pgtable_entry_types_inverse(pgtable_entry_types_default());

	size_t size = util_bit(pgt->control.address_bits);

	LOG(DEBUG, INFO, "+---------------- page table ----------------\n");
	LOG(DEBUG, INFO, "| TTBR({:#x}):\n", pgt->control.root_pgtable);
	(void)translation_table_walk(&pgt->control, 0L, size,
				     PGTABLE_TRANSLATION_TABLE_WALK_EVENT_DUMP,
				     entry_types, NULL);
	LOG(DEBUG, INFO, "+--------------------------------------------\n\n");
}
#endif

#ifdef HOST_TEST
void
pgtable_vm_ext(pgtable_vm_t *pgt, vmaddr_t virtual_address, size_t size,
	       pgtable_entry_types_t entry_types, ext_func_t func, void *data)
{
	ext_modifier_args_t margs = { 0 };

	assert(pgt != NULL);
	assert(addr_check(virtual_address, pgt->control.address_bits, false));
	assert(addr_check(virtual_address + size - 1, pgt->control.address_bits,
			  false));

	margs.func = func;
	margs.data = data;

	(void)translation_table_walk(
		&pgt->control, virtual_address, size,
		PGTABLE_TRANSLATION_TABLE_WALK_EVENT_EXTERNAL, entry_types,
		&margs);
}
#endif

static tcr_tg0_t
vtcr_get_tg0_code(size_t granule_shift)
{
	tcr_tg0_t tg0;

	switch (granule_shift) {
	case SHIFT_4K:
		tg0 = TCR_TG0_GRANULE_SIZE_4KB;
		break;
	case SHIFT_16K:
		tg0 = TCR_TG0_GRANULE_SIZE_16KB;
		break;
	case SHIFT_64K:
		tg0 = TCR_TG0_GRANULE_SIZE_64KB;
		break;
	default:
		panic("Invalid granule size");
	}

	return tg0;
}

#if !defined(HOST_TEST)
// Note: the nested macros are are needed to expand the config to a number
// before it is pasted into the enum name.
#define PLATFORM_TCR_PS	     TCR_PS_FOR_CONFIG(PLATFORM_PHYS_ADDRESS_BITS)
#define TCR_PS_FOR_CONFIG(x) TCR_PS_FOR_SIZE(x)
#define TCR_PS_FOR_SIZE(x)   TCR_PS_SIZE_##x##BITS

static void
pgtable_vm_init_regs(pgtable_vm_t *vm_pgtable)
{
	assert(vm_pgtable != NULL);

	// Init Virtualization Translation Control Register

	VTCR_EL2_init(&vm_pgtable->vtcr_el2);

	uint8_t t0sz = (uint8_t)(64U - vm_pgtable->control.address_bits);

	VTCR_EL2_set_T0SZ(&vm_pgtable->vtcr_el2, t0sz);

	if (vm_pgtable->control.granule_shift == SHIFT_4K) {
		switch (vm_pgtable->control.start_level) {
		case 0:
			VTCR_EL2_set_SL0(&vm_pgtable->vtcr_el2, 0x2);
			break;
		case 1:
			VTCR_EL2_set_SL0(&vm_pgtable->vtcr_el2, 0x1);
			break;
		case 2:
			VTCR_EL2_set_SL0(&vm_pgtable->vtcr_el2, 0x0);
			break;
		default:
			panic("Invalid SL0");
		}
	} else {
		switch (vm_pgtable->control.start_level) {
		case 1:
			VTCR_EL2_set_SL0(&vm_pgtable->vtcr_el2, 0x2);
			break;
		case 2:
			VTCR_EL2_set_SL0(&vm_pgtable->vtcr_el2, 0x1);
			break;
		case 3:
			VTCR_EL2_set_SL0(&vm_pgtable->vtcr_el2, 0x0);
			break;
		case 0:
		default:
			panic("Invalid SL0");
		}
	}
	VTCR_EL2_set_IRGN0(&vm_pgtable->vtcr_el2,
			   TCR_RGN_NORMAL_WRITEBACK_RA_WA);
	VTCR_EL2_set_ORGN0(&vm_pgtable->vtcr_el2,
			   TCR_RGN_NORMAL_WRITEBACK_RA_WA);
	VTCR_EL2_set_SH0(&vm_pgtable->vtcr_el2, TCR_SH_INNER_SHAREABLE);

	tcr_tg0_t tg0 = vtcr_get_tg0_code(vm_pgtable->control.granule_shift);
	VTCR_EL2_set_TG0(&vm_pgtable->vtcr_el2, tg0);

	// The output size is defined by the platform.
	VTCR_EL2_set_PS(&vm_pgtable->vtcr_el2, PLATFORM_TCR_PS);

	// The platform's implemented physical address space must be no
	// larger than the CPU's implemented physical address size.
	ID_AA64MMFR0_EL1_t id_aa64mmfr0 = register_ID_AA64MMFR0_EL1_read();
	assert(ID_AA64MMFR0_EL1_get_PARange(&id_aa64mmfr0) >=
	       VTCR_EL2_get_PS(&vm_pgtable->vtcr_el2));

	// The stage-2 input address size must be no larger than the CPU's
	// implemented physical address size (though it may be larger than the
	// platform's implemented physical address space, if that is smaller
	// than the CPU's).
	switch (ID_AA64MMFR0_EL1_get_PARange(&id_aa64mmfr0)) {
	case TCR_PS_SIZE_32BITS:
		assert(vm_pgtable->control.address_bits <= 32);
		break;
	case TCR_PS_SIZE_36BITS:
		assert(vm_pgtable->control.address_bits <= 36);
		break;
	case TCR_PS_SIZE_40BITS:
		assert(vm_pgtable->control.address_bits <= 40);
		break;
	case TCR_PS_SIZE_42BITS:
		assert(vm_pgtable->control.address_bits <= 42);
		break;
	case TCR_PS_SIZE_44BITS:
		assert(vm_pgtable->control.address_bits <= 44);
		break;
	case TCR_PS_SIZE_48BITS:
		assert(vm_pgtable->control.address_bits <= 48);
		break;
	case TCR_PS_SIZE_52BITS:
		assert(vm_pgtable->control.address_bits <= 52);
		break;
	default:
		panic("bad PARange");
	}

#if defined(ARCH_ARM_FEAT_VMID16)
	VTCR_EL2_set_VS(&vm_pgtable->vtcr_el2, true);
#endif

#if defined(ARCH_ARM_FEAT_HAFDBS)
	VTCR_EL2_set_HA(&vm_pgtable->vtcr_el2, true);
	ID_AA64MMFR1_EL1_t hw_mmfr1 = register_ID_AA64MMFR1_EL1_read();
	if (ID_AA64MMFR1_EL1_get_HAFDBS(&hw_mmfr1) == 2U) {
		VTCR_EL2_set_HD(&vm_pgtable->vtcr_el2, true);
	}
#endif

#if defined(ARCH_ARM_FEAT_HPDS2)
	VTCR_EL2_set_HWU059(&vm_pgtable->vtcr_el2, false);
	VTCR_EL2_set_HWU060(&vm_pgtable->vtcr_el2, false);
	VTCR_EL2_set_HWU061(&vm_pgtable->vtcr_el2, false);
	VTCR_EL2_set_HWU062(&vm_pgtable->vtcr_el2, false);
#endif

#if defined(ARCH_ARM_FEAT_SEC_EL2)
	VTCR_EL2_set_NSW(&vm_pgtable->vtcr_el2, true);
	VTCR_EL2_set_NSA(&vm_pgtable->vtcr_el2, true);
#endif

	// Init Virtualization Translation Table Base Register

	VTTBR_EL2_init(&vm_pgtable->vttbr_el2);
	VTTBR_EL2_set_CnP(&vm_pgtable->vttbr_el2, true);
	VTTBR_EL2_set_BADDR(&vm_pgtable->vttbr_el2,
			    vm_pgtable->control.root_pgtable);
#if defined(ARCH_ARM_FEAT_VMID16)
	VTTBR_EL2_set_VMID(&vm_pgtable->vttbr_el2, vm_pgtable->control.vmid);
#else
	VTTBR_EL2_set_VMID(&vm_pgtable->vttbr_el2,
			   (uint8_t)vm_pgtable->control.vmid);
#endif
}

void
pgtable_vm_load_regs(pgtable_vm_t *vm_pgtable)
{
	register_VTCR_EL2_write(vm_pgtable->vtcr_el2);
	register_VTTBR_EL2_write(vm_pgtable->vttbr_el2);
}
#endif

error_t
pgtable_vm_init(partition_t *partition, pgtable_vm_t *pgtable, vmid_t vmid)
{
	error_t ret = OK;
	index_t msb;

	if (pgtable->control.root != NULL) {
		// Address already setup by another module
		assert(pgtable->control.vmid == vmid);
		goto out;
	}

	// FIXME:
	// FIXME: refine with more configurable code
#if PGTABLE_VM_PAGE_SIZE == 4096
	pgtable->control.granule_shift = SHIFT_4K;
#else
#error untested granule size
#endif
	pgtable->control.address_bits = PLATFORM_VM_ADDRESS_SPACE_BITS;
	msb			      = PLATFORM_VM_ADDRESS_SPACE_BITS - 1;
	pgtable->control.vmid	      = vmid;

	get_start_level_info_ret_t info =
		get_start_level_info(level_conf, msb, true);
	pgtable->control.start_level	  = info.level;
	pgtable->control.start_level_size = info.size;
	pgtable->issue_dvm_cmd		  = false;

	// allocate the level 0 page table
	ret = alloc_level_table(partition, info.size,
				util_max(info.size, VMSA_TABLE_MIN_ALIGN),
				&pgtable->control.root_pgtable,
				&pgtable->control.root);
	if (ret != OK) {
		goto out;
	}

#if !defined(HOST_TEST)
	pgtable_vm_init_regs(pgtable);
#endif

out:
	return ret;
}

void
pgtable_vm_destroy(partition_t *partition, pgtable_vm_t *pgtable)
{
	vmaddr_t virtual_address = 0x0U;
	size_t	 size		 = 0x0U;

	assert(partition != NULL);
	assert(pgtable != NULL);
	assert(pgtable->control.root != NULL);

	virtual_address = 0x0U;
	size		= util_bit(pgtable->control.address_bits);
	// we should unmap everything
	pgtable_vm_start(pgtable);
	pgtable_vm_unmap(partition, pgtable, virtual_address, size);
	pgtable_vm_commit(pgtable);

	// free top level page table
	(void)partition_free(partition, pgtable->control.root,
			     pgtable->control.start_level_size);
	pgtable->control.root = NULL;
}

bool
pgtable_vm_lookup(pgtable_vm_t *pgtable, vmaddr_t virtual_address,
		  paddr_t *mapped_base, size_t *mapped_size,
		  pgtable_vm_memtype_t *mapped_memtype,
		  pgtable_access_t     *mapped_vm_kernel_access,
		  pgtable_access_t     *mapped_vm_user_access)
{
	bool			       walk_ret;
	pgtable_lookup_modifier_args_t margs = { 0 };
	pgtable_entry_types_t entry_types    = pgtable_entry_types_default();
	vmsa_upper_attrs_t    upper_attrs;
	vmsa_lower_attrs_t    lower_attrs;

	assert(pgtable != NULL);
	assert(mapped_base != NULL);
	assert(mapped_size != NULL);
	assert(mapped_memtype != NULL);
	assert(mapped_vm_kernel_access != NULL);
	assert(mapped_vm_user_access != NULL);

	if (!addr_check(virtual_address, pgtable->control.address_bits,
			false)) {
		// Address is out of range
		walk_ret = false;
		goto out;
	}

	pgtable_entry_types_set_block(&entry_types, true);
	pgtable_entry_types_set_page(&entry_types, true);

	// just try to lookup a page, but if it's a block, the modifier will
	// stop the walk and return success
	walk_ret = translation_table_walk(
		&pgtable->control, virtual_address,
		util_bit(pgtable->control.granule_shift),
		PGTABLE_TRANSLATION_TABLE_WALK_EVENT_LOOKUP, entry_types,
		&margs);

	if (margs.size == 0U) {
		// Return error (not-mapped) if lookup found no pages.
		walk_ret = false;
	}

	if (walk_ret) {
		*mapped_base = margs.phys;
		*mapped_size = margs.size;

		lower_attrs	= get_lower_attr(margs.entry);
		upper_attrs	= get_upper_attr(margs.entry);
		*mapped_memtype = map_stg2_attr_to_memtype(lower_attrs);
		map_stg2_attr_to_access(upper_attrs, lower_attrs,
					mapped_vm_kernel_access,
					mapped_vm_user_access);

	} else {
		*mapped_base		 = 0U;
		*mapped_size		 = 0U;
		*mapped_memtype		 = PGTABLE_VM_MEMTYPE_DEVICE_NGNRNE;
		*mapped_vm_kernel_access = PGTABLE_ACCESS_NONE;
		*mapped_vm_user_access	 = PGTABLE_ACCESS_NONE;
	}

out:
	return walk_ret;
}

// FIXME: right now assume the virt address with size is free,
// no need to retry
// FIXME: assume the size must be single page size or available block
// size, or else, just map it as one single page.
error_t
pgtable_vm_map(partition_t *partition, pgtable_vm_t *pgtable,
	       vmaddr_t virtual_address, size_t size, paddr_t phys,
	       pgtable_vm_memtype_t memtype, pgtable_access_t vm_kernel_access,
	       pgtable_access_t vm_user_access, bool try_map, bool allow_merge)
{
	pgtable_map_modifier_args_t margs = { 0 };
	vmsa_stg2_lower_attrs_t	    l;
	vmsa_stg2_upper_attrs_t	    u;

	assert(pgtable_op);

	assert(pgtable != NULL);
	assert(partition != NULL);

	if (!addr_check(virtual_address, pgtable->control.address_bits,
			false)) {
		margs.error = ERROR_ADDR_INVALID;
		goto fail;
	}

	if (util_add_overflows(virtual_address, size - 1U) ||
	    !addr_check(virtual_address + size - 1U,
			pgtable->control.address_bits, false)) {
		margs.error = ERROR_ADDR_OVERFLOW;
		goto fail;
	}

	// FIXME:
	// Supporting different granule sizes will need support and additional
	// checking to be added to memextent code.
	if (!util_is_p2aligned(virtual_address,
			       pgtable->control.granule_shift) ||
	    !util_is_p2aligned(phys, pgtable->control.granule_shift) ||
	    !util_is_p2aligned(size, pgtable->control.granule_shift)) {
		margs.error = ERROR_ARGUMENT_ALIGNMENT;
		goto fail;
	}

	// FIXME: how to check phys, read tcr in init?
	// FIXME: no need to to check vm memtype, right?

	margs.orig_virtual_address = virtual_address;
	margs.orig_size		   = size;
	margs.phys		   = phys;
	margs.partition		   = partition;
	vmsa_stg2_lower_attrs_init(&l);
	vmsa_stg2_upper_attrs_init(&u);
	map_stg2_memtype_to_attrs(memtype, &l);
	map_stg2_access_to_attrs(vm_kernel_access, vm_user_access, &u, &l);
	margs.lower_attrs	   = vmsa_stg2_lower_attrs_raw(l);
	margs.upper_attrs	   = vmsa_stg2_upper_attrs_raw(u);
	margs.new_page_start_level = PGTABLE_INVALID_LEVEL;
	margs.error		   = OK;
	margs.try_map		   = try_map;
	margs.stage		   = PGTABLE_VM_STAGE_2;
	margs.outer_shareable	   = pgtable->issue_dvm_cmd;
#if (CPU_PGTABLE_BBM_LEVEL > 0) || !defined(PLATFORM_PGTABLE_AVOID_BBM)
	// We can either trigger TLB conflicts safely because they will be
	// delivered to EL2, or else can use BBM.
	margs.merge_limit = allow_merge ? ~(size_t)0U : 0U;
#else
	// We can't use BBM, and merging without it might cause TLB conflict
	// aborts in EL1. This is unsafe because:
	// - the EL1 abort handler might trigger the same abort again, and
	// - Linux VMs treat TLB conflict aborts as fatal errors.
	(void)allow_merge;
	margs.merge_limit = false;
#endif

	// FIXME: try to unify the level number, just use one kind of level
	pgtable_entry_types_t entry_types = VMSA_ENTRY_TYPE_LEAF;
	pgtable_entry_types_set_next_level_table(&entry_types, true);
	bool walk_ret = translation_table_walk(
		&pgtable->control, virtual_address, size,
		PGTABLE_TRANSLATION_TABLE_WALK_EVENT_MMAP, entry_types, &margs);

	if (!walk_ret && (margs.error == OK)) {
		margs.error = ERROR_FAILURE;
	}
	if ((margs.error != OK) && (margs.partially_mapped_size != 0U)) {
		pgtable_vm_unmap(partition, pgtable, virtual_address,
				 margs.partially_mapped_size);
	}

fail:
	return margs.error;
}

void
pgtable_vm_unmap(partition_t *partition, pgtable_vm_t *pgtable,
		 vmaddr_t virtual_address, size_t size)
{
	pgtable_unmap_modifier_args_t margs = { 0 };

	assert(pgtable_op);

	assert(pgtable != NULL);
	assert(partition != NULL);

	if (!addr_check(virtual_address, pgtable->control.address_bits,
			false)) {
		panic("Bad arguments in pgtable_vm_unmap");
	}

	if (util_add_overflows(virtual_address, size - 1U) ||
	    !addr_check(virtual_address + size - 1U,
			pgtable->control.address_bits, false)) {
		panic("Bad arguments in pgtable_vm_unmap");
	}

	if (!util_is_p2aligned(virtual_address,
			       pgtable->control.granule_shift) ||
	    !util_is_p2aligned(size, pgtable->control.granule_shift)) {
		panic("Bad arguments in pgtable_vm_unmap");
	}

	margs.partition = partition;
	// no need to preserve table levels here
	margs.preserved_size  = PGTABLE_HYP_UNMAP_PRESERVE_NONE;
	margs.stage	      = PGTABLE_VM_STAGE_2;
	margs.outer_shareable = pgtable->issue_dvm_cmd;

	bool walk_ret = translation_table_walk(
		&pgtable->control, virtual_address, size,
		PGTABLE_TRANSLATION_TABLE_WALK_EVENT_UNMAP,
		VMSA_ENTRY_TYPE_LEAF, &margs);
	if (!walk_ret) {
		panic("Error in pgtable_vm_unmap");
	}
}

void
pgtable_vm_unmap_matching(partition_t *partition, pgtable_vm_t *pgtable,
			  vmaddr_t virtual_address, paddr_t phys, size_t size)
{
	pgtable_unmap_modifier_args_t margs = { 0 };

	assert(pgtable_op);

	assert(pgtable != NULL);
	assert(partition != NULL);

	if (!addr_check(virtual_address, pgtable->control.address_bits,
			false)) {
		panic("Bad arguments in pgtable_vm_unmap_matching");
	}

	if (util_add_overflows(virtual_address, size - 1U) ||
	    !addr_check(virtual_address + size - 1U,
			pgtable->control.address_bits, false)) {
		panic("Bad arguments in pgtable_vm_unmap_matching");
	}

	margs.partition = partition;
	// no need to preserve table levels here
	margs.preserved_size  = PGTABLE_HYP_UNMAP_PRESERVE_NONE;
	margs.stage	      = PGTABLE_VM_STAGE_2;
	margs.phys	      = phys;
	margs.size	      = size;
	margs.outer_shareable = pgtable->issue_dvm_cmd;

	bool walk_ret = translation_table_walk(
		&pgtable->control, virtual_address, size,
		PGTABLE_TRANSLATION_TABLE_WALK_EVENT_UNMAP_MATCH,
		VMSA_ENTRY_TYPE_LEAF, &margs);
	if (!walk_ret) {
		panic("Error in pgtable_vm_unmap_matching");
	}
}

void
pgtable_vm_start(pgtable_vm_t *pgtable) LOCK_IMPL
{
	assert(pgtable != NULL);
#ifndef HOST_TEST
	// FIXME:
	// We need to to run VM pagetable code with preempt disable due to
	// TLB flushes.
	preempt_disable();
#if !defined(NDEBUG)
	assert(!pgtable_op);
	pgtable_op = true;
#endif

	thread_t *thread = thread_get_self();

	// Since the pagetable code may need to flush the target VMID, we need
	// to ensure that it is current for the pagetable operations.
	// We set the VMID which is in the VTTBR register. Note, no need to set
	// VTCR - so ensure no TLB walks take place!.  This also assumes that
	// preempt is disabled otherwise a context-switch would restore the
	// original registers.
	if ((thread->addrspace == NULL) ||
	    (&thread->addrspace->vm_pgtable != pgtable)) {
		register_VTTBR_EL2_write_ordered(pgtable->vttbr_el2,
						 &asm_ordering);
		asm_context_sync_ordered(&asm_ordering);
	}
#endif
}

void
pgtable_vm_commit(pgtable_vm_t *pgtable) LOCK_IMPL
{
#ifndef HOST_TEST
#if !defined(NDEBUG)
	assert(pgtable_op);
	pgtable_op = false;
#endif

	dsb(pgtable->issue_dvm_cmd);
	// This is only needed when unmapping. Consider some flags to
	// track to flush requirements.
	vm_tlbi_vmalle1(pgtable->issue_dvm_cmd);
	dsb(pgtable->issue_dvm_cmd);

	thread_t *thread = thread_get_self();

	// Since the pagetable code flushes the target VMID, we set it as the
	// current VMID for the pagetable operations. We need to restore the
	// original VMID (in VTTBR_EL2) here.
	if ((thread->addrspace != NULL) &&
	    (&thread->addrspace->vm_pgtable != pgtable)) {
		register_VTTBR_EL2_write_ordered(
			thread->addrspace->vm_pgtable.vttbr_el2, &asm_ordering);
	}

	preempt_enable();
	trigger_pgtable_vm_commit_event(pgtable);
#endif // !HOST_TEST
}
