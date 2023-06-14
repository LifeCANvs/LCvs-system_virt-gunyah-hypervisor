// © 2021 Qualcomm Innovation Center, Inc. All rights reserved.
//
// SPDX-License-Identifier: BSD-3-Clause

// AArch64 System Register Encoding
//
// This list is not exhaustive, it contains mostly registers likely to be
// trapped and accessed indirectly.

#define ISS_OP0_OP1_CRN_CRM_OP2(op0, op1, crn, crm, op2)                       \
	(uint32_t)(((3U & (uint32_t)(op0)) << 20) |                            \
		   ((7U & (uint32_t)(op2)) << 17) |                            \
		   ((7U & (uint32_t)(op1)) << 14) |                            \
		   ((15U & (uint32_t)(crn)) << 10) |                           \
		   ((15U & (uint32_t)(crm)) << 1))

// - op0 = 3 : Moves to and from non-debug System registers, Special-purpose
//             registers
#define ISS_MRS_MSR_REVIDR_EL1	     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 0, 6)
#define ISS_MRS_MSR_ID_PFR0_EL1	     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 1, 0)
#define ISS_MRS_MSR_ID_PFR1_EL1	     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 1, 1)
#define ISS_MRS_MSR_ID_PFR2_EL1	     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 3, 4)
#define ISS_MRS_MSR_ID_DFR0_EL1	     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 1, 2)
#define ISS_MRS_MSR_ID_AFR0_EL1	     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 1, 3)
#define ISS_MRS_MSR_ID_MMFR0_EL1     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 1, 4)
#define ISS_MRS_MSR_ID_MMFR1_EL1     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 1, 5)
#define ISS_MRS_MSR_ID_MMFR2_EL1     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 1, 6)
#define ISS_MRS_MSR_ID_MMFR3_EL1     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 1, 7)
#define ISS_MRS_MSR_ID_MMFR4_EL1     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 2, 6)
#define ISS_MRS_MSR_ID_ISAR0_EL1     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 2, 0)
#define ISS_MRS_MSR_ID_ISAR1_EL1     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 2, 1)
#define ISS_MRS_MSR_ID_ISAR2_EL1     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 2, 2)
#define ISS_MRS_MSR_ID_ISAR3_EL1     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 2, 3)
#define ISS_MRS_MSR_ID_ISAR4_EL1     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 2, 4)
#define ISS_MRS_MSR_ID_ISAR5_EL1     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 2, 5)
#define ISS_MRS_MSR_ID_ISAR6_EL1     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 2, 7)
#define ISS_MRS_MSR_MVFR0_EL1	     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 3, 0)
#define ISS_MRS_MSR_MVFR1_EL1	     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 3, 1)
#define ISS_MRS_MSR_MVFR2_EL1	     ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 3, 2)
#define ISS_MRS_MSR_ID_AA64PFR0_EL1  ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 4, 0)
#define ISS_MRS_MSR_ID_AA64PFR1_EL1  ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 4, 1)
#define ISS_MRS_MSR_ID_AA64ZFR0_EL1  ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 4, 4)
#define ISS_MRS_MSR_ID_AA64SMFR0_EL1 ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 4, 5)
#define ISS_MRS_MSR_ID_AA64DFR0_EL1  ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 5, 0)
#define ISS_MRS_MSR_ID_AA64DFR1_EL1  ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 5, 1)
#define ISS_MRS_MSR_ID_AA64AFR0_EL1  ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 5, 4)
#define ISS_MRS_MSR_ID_AA64AFR1_EL1  ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 5, 5)
#define ISS_MRS_MSR_ID_AA64ISAR0_EL1 ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 6, 0)
#define ISS_MRS_MSR_ID_AA64ISAR1_EL1 ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 6, 1)
#define ISS_MRS_MSR_ID_AA64ISAR2_EL1 ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 6, 2)
#define ISS_MRS_MSR_ID_AA64MMFR0_EL1 ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 7, 0)
#define ISS_MRS_MSR_ID_AA64MMFR1_EL1 ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 7, 1)
#define ISS_MRS_MSR_ID_AA64MMFR2_EL1 ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 0, 7, 2)
#define ISS_MRS_MSR_AIDR_EL1	     ISS_OP0_OP1_CRN_CRM_OP2(3, 1, 0, 0, 7)
#define ISS_MRS_MSR_PMCR_EL0	     ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 9, 12, 0)
#define ISS_MRS_MSR_PMCNTENSET_EL0   ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 9, 12, 1)
#define ISS_MRS_MSR_PMCNTENCLR_EL0   ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 9, 12, 2)
#define ISS_MRS_MSR_PMOVSCLR_EL0     ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 9, 12, 3)
#define ISS_MRS_MSR_PMSWINC_EL0	     ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 9, 12, 4)
#define ISS_MRS_MSR_PMSELR_EL0	     ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 9, 12, 5)
#define ISS_MRS_MSR_PMCEID0_EL0	     ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 9, 12, 6)
#define ISS_MRS_MSR_PMCEID1_EL0	     ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 9, 12, 7)
#define ISS_MRS_MSR_PMCCNTR_EL0	     ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 9, 13, 0)
#define ISS_MRS_MSR_PMXEVTYPER_EL0   ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 9, 13, 1)
#define ISS_MRS_MSR_PMXEVCNTR_EL0    ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 9, 13, 2)
#define ISS_MRS_MSR_PMUSERENR_EL0    ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 9, 14, 0)
#define ISS_MRS_MSR_PMINTENSET_EL1   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 9, 14, 1)
#define ISS_MRS_MSR_PMINTENCLR_EL1   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 9, 14, 2)
#define ISS_MRS_MSR_PMOVSSET_EL0     ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 9, 14, 3)
#define ISS_MRS_MSR_PMCCFILTR_EL0    ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 14, 15, 7)

#define ISS_MRS_MSR_SCTLR_EL1	   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 1, 0, 0)
#define ISS_MRS_MSR_ACTLR_EL1	   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 1, 0, 1)
#define ISS_MRS_MSR_TTBR0_EL1	   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 2, 0, 0)
#define ISS_MRS_MSR_TTBR1_EL1	   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 2, 0, 1)
#define ISS_MRS_MSR_TCR_EL1	   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 2, 0, 2)
#define ISS_MRS_MSR_ESR_EL1	   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 5, 2, 0)
#define ISS_MRS_MSR_FAR_EL1	   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 6, 0, 0)
#define ISS_MRS_MSR_AFSR0_EL1	   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 5, 1, 0)
#define ISS_MRS_MSR_AFSR1_EL1	   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 5, 1, 1)
#define ISS_MRS_MSR_MAIR_EL1	   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 10, 2, 0)
#define ISS_MRS_MSR_AMAIR_EL1	   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 10, 3, 0)
#define ISS_MRS_MSR_CONTEXTIDR_EL1 ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 13, 0, 1)

#define ISS_MRS_MSR_ICC_IAR0_EL1    ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 8, 0)
#define ISS_MRS_MSR_ICC_EOIR0_EL1   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 8, 1)
#define ISS_MRS_MSR_ICC_HPPIR0_EL1  ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 8, 2)
#define ISS_MRS_MSR_ICC_BPR0_EL1    ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 8, 3)
#define ISS_MRS_MSR_ICC_AP0R0_EL1   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 8, 4)
#define ISS_MRS_MSR_ICC_AP0R1_EL1   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 8, 5)
#define ISS_MRS_MSR_ICC_AP0R2_EL1   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 8, 6)
#define ISS_MRS_MSR_ICC_AP0R3_EL1   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 8, 7)
#define ISS_MRS_MSR_ICC_AP1R0_EL1   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 9, 0)
#define ISS_MRS_MSR_ICC_AP1R1_EL1   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 9, 1)
#define ISS_MRS_MSR_ICC_AP1R2_EL1   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 9, 2)
#define ISS_MRS_MSR_ICC_AP1R3_EL1   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 9, 3)
#define ISS_MRS_MSR_ICC_DIR_EL1	    ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 11, 1)
#define ISS_MRS_MSR_ICC_SGI1R_EL1   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 11, 5)
#define ISS_MRS_MSR_ICC_ASGI1R_EL1  ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 11, 6)
#define ISS_MRS_MSR_ICC_SGI0R_EL1   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 11, 7)
#define ISS_MRS_MSR_ICC_IAR1_EL1    ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 12, 0)
#define ISS_MRS_MSR_ICC_EOIR1_EL1   ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 12, 1)
#define ISS_MRS_MSR_ICC_HPPIR1_EL1  ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 12, 2)
#define ISS_MRS_MSR_ICC_BPR1_EL1    ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 12, 3)
#define ISS_MRS_MSR_ICC_SRE_EL1	    ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 12, 5)
#define ISS_MRS_MSR_ICC_IGRPEN0_EL1 ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 12, 6)
#define ISS_MRS_MSR_ICC_IGRPEN1_EL1 ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 12, 12, 7)

#define ISS_MRS_MSR_IMP_CLUSTERIDR_EL1 ISS_OP0_OP1_CRN_CRM_OP2(3, 0, 15, 3, 1)

#define ISS_MRS_MSR_DC_CSW  ISS_OP0_OP1_CRN_CRM_OP2(1, 0, 7, 10, 2)
#define ISS_MRS_MSR_DC_CISW ISS_OP0_OP1_CRN_CRM_OP2(1, 0, 7, 14, 2)
#define ISS_MRS_MSR_DC_ISW  ISS_OP0_OP1_CRN_CRM_OP2(1, 0, 7, 6, 2)

#if defined(ARCH_ARM_FEAT_AMUv1) || defined(ARCH_ARM_FEAT_AMUv1p1)
#define ISS_MRS_MSR_AMCR_EL0	    ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 13, 2, 0)
#define ISS_MRS_MSR_AMCFGR_EL0	    ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 13, 2, 1)
#define ISS_MRS_MSR_AMCGCR_EL0	    ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 13, 2, 2)
#define ISS_MRS_MSR_AMUSERENR_EL0   ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 13, 2, 3)
#define ISS_MRS_MSR_AMCNTENCLR0_EL0 ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 13, 2, 4)
#define ISS_MRS_MSR_AMCNTENSET0_EL0 ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 13, 2, 5)
#define ISS_MRS_MSR_AMCNTENCLR1_EL0 ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 13, 3, 0)
#define ISS_MRS_MSR_AMCNTENSET1_EL0 ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 13, 3, 1)
#if defined(ARCH_ARM_FEAT_AMUv1p1)
#define ISS_MRS_MSR_AMCG1IDR_EL0 ISS_OP0_OP1_CRN_CRM_OP2(3, 3, 13, 2, 6)
#endif
#endif
