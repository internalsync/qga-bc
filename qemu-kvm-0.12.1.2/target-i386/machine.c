#include "hw/hw.h"
#include "hw/boards.h"
#include "hw/pc.h"
#include "hw/isa.h"

#include "exec-all.h"
#include "kvm.h"
#include "qemu-kvm.h"

static const VMStateDescription vmstate_segment = {
    .name = "segment",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT32(selector, SegmentCache),
        VMSTATE_UINTTL(base, SegmentCache),
        VMSTATE_UINT32(limit, SegmentCache),
        VMSTATE_UINT32(flags, SegmentCache),
        VMSTATE_END_OF_LIST()
    }
};

#define VMSTATE_SEGMENT(_field, _state) {                            \
    .name       = (stringify(_field)),                               \
    .size       = sizeof(SegmentCache),                              \
    .vmsd       = &vmstate_segment,                                  \
    .flags      = VMS_STRUCT,                                        \
    .offset     = offsetof(_state, _field)                           \
            + type_check(SegmentCache,typeof_field(_state, _field))  \
}

#define VMSTATE_SEGMENT_ARRAY(_field, _state, _n)                    \
    VMSTATE_STRUCT_ARRAY(_field, _state, _n, 0, vmstate_segment, SegmentCache)

static const VMStateDescription vmstate_xmm_reg = {
    .name = "xmm_reg",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT64(XMM_Q(0), XMMReg),
        VMSTATE_UINT64(XMM_Q(1), XMMReg),
        VMSTATE_END_OF_LIST()
    }
};

#define VMSTATE_XMM_REGS(_field, _state, _n)                         \
    VMSTATE_STRUCT_ARRAY(_field, _state, _n, 0, vmstate_xmm_reg, XMMReg)

/* YMMH format is the same as XMM */
static const VMStateDescription vmstate_ymmh_reg = {
    .name = "ymmh_reg",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT64(XMM_Q(0), XMMReg),
        VMSTATE_UINT64(XMM_Q(1), XMMReg),
        VMSTATE_END_OF_LIST()
    }
};

#define VMSTATE_YMMH_REGS_VARS(_field, _state, _n, _v)                         \
    VMSTATE_STRUCT_ARRAY(_field, _state, _n, _v, vmstate_ymmh_reg, XMMReg)

static const VMStateDescription vmstate_mtrr_var = {
    .name = "mtrr_var",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT64(base, MTRRVar),
        VMSTATE_UINT64(mask, MTRRVar),
        VMSTATE_END_OF_LIST()
    }
};

#define VMSTATE_MTRR_VARS(_field, _state, _n, _v)                    \
    VMSTATE_STRUCT_ARRAY(_field, _state, _n, _v, vmstate_mtrr_var, MTRRVar)

static void put_fpreg_error(QEMUFile *f, void *opaque, size_t size)
{
    fprintf(stderr, "call put_fpreg() with invalid arguments\n");
    exit(0);
}

#ifdef USE_X86LDOUBLE
/* XXX: add that in a FPU generic layer */
union x86_longdouble {
    uint64_t mant;
    uint16_t exp;
};

#define MANTD1(fp)	(fp & ((1LL << 52) - 1))
#define EXPBIAS1 1023
#define EXPD1(fp)	((fp >> 52) & 0x7FF)
#define SIGND1(fp)	((fp >> 32) & 0x80000000)

static void fp64_to_fp80(union x86_longdouble *p, uint64_t temp)
{
    int e;
    /* mantissa */
    p->mant = (MANTD1(temp) << 11) | (1LL << 63);
    /* exponent + sign */
    e = EXPD1(temp) - EXPBIAS1 + 16383;
    e |= SIGND1(temp) >> 16;
    p->exp = e;
}

static int get_fpreg(QEMUFile *f, void *opaque, size_t size)
{
    FPReg *fp_reg = opaque;
    uint64_t mant;
    uint16_t exp;

    qemu_get_be64s(f, &mant);
    qemu_get_be16s(f, &exp);
    fp_reg->d = cpu_set_fp80(mant, exp);
    return 0;
}

static void put_fpreg(QEMUFile *f, void *opaque, size_t size)
{
    FPReg *fp_reg = opaque;
    uint64_t mant;
    uint16_t exp;
    /* we save the real CPU data (in case of MMX usage only 'mant'
       contains the MMX register */
    cpu_get_fp80(&mant, &exp, fp_reg->d);
    qemu_put_be64s(f, &mant);
    qemu_put_be16s(f, &exp);
}

static const VMStateInfo vmstate_fpreg = {
    .name = "fpreg",
    .get  = get_fpreg,
    .put  = put_fpreg,
};

static int get_fpreg_1_mmx(QEMUFile *f, void *opaque, size_t size)
{
    union x86_longdouble *p = opaque;
    uint64_t mant;

    qemu_get_be64s(f, &mant);
    p->mant = mant;
    p->exp = 0xffff;
    return 0;
}

static const VMStateInfo vmstate_fpreg_1_mmx = {
    .name = "fpreg_1_mmx",
    .get  = get_fpreg_1_mmx,
    .put  = put_fpreg_error,
};

static int get_fpreg_1_no_mmx(QEMUFile *f, void *opaque, size_t size)
{
    union x86_longdouble *p = opaque;
    uint64_t mant;

    qemu_get_be64s(f, &mant);
    fp64_to_fp80(p, mant);
    return 0;
}

static const VMStateInfo vmstate_fpreg_1_no_mmx = {
    .name = "fpreg_1_no_mmx",
    .get  = get_fpreg_1_no_mmx,
    .put  = put_fpreg_error,
};

static bool fpregs_is_0(void *opaque, int version_id)
{
    CPUState *env = opaque;

    return (env->fpregs_format_vmstate == 0);
}

static bool fpregs_is_1_mmx(void *opaque, int version_id)
{
    CPUState *env = opaque;
    int guess_mmx;

    guess_mmx = ((env->fptag_vmstate == 0xff) &&
                 (env->fpus_vmstate & 0x3800) == 0);
    return (guess_mmx && (env->fpregs_format_vmstate == 1));
}

static bool fpregs_is_1_no_mmx(void *opaque, int version_id)
{
    CPUState *env = opaque;
    int guess_mmx;

    guess_mmx = ((env->fptag_vmstate == 0xff) &&
                 (env->fpus_vmstate & 0x3800) == 0);
    return (!guess_mmx && (env->fpregs_format_vmstate == 1));
}

#define VMSTATE_FP_REGS(_field, _state, _n)                               \
    VMSTATE_ARRAY_TEST(_field, _state, _n, fpregs_is_0, vmstate_fpreg, FPReg), \
    VMSTATE_ARRAY_TEST(_field, _state, _n, fpregs_is_1_mmx, vmstate_fpreg_1_mmx, FPReg), \
    VMSTATE_ARRAY_TEST(_field, _state, _n, fpregs_is_1_no_mmx, vmstate_fpreg_1_no_mmx, FPReg)

#else
static int get_fpreg(QEMUFile *f, void *opaque, size_t size)
{
    FPReg *fp_reg = opaque;

    qemu_get_be64s(f, &fp_reg->mmx.MMX_Q(0));
    return 0;
}

static void put_fpreg(QEMUFile *f, void *opaque, size_t size)
{
    FPReg *fp_reg = opaque;
    /* if we use doubles for float emulation, we save the doubles to
       avoid losing information in case of MMX usage. It can give
       problems if the image is restored on a CPU where long
       doubles are used instead. */
    qemu_put_be64s(f, &fp_reg->mmx.MMX_Q(0));
}

const VMStateInfo vmstate_fpreg = {
    .name = "fpreg",
    .get  = get_fpreg,
    .put  = put_fpreg,
};

static int get_fpreg_0_mmx(QEMUFile *f, void *opaque, size_t size)
{
    FPReg *fp_reg = opaque;
    uint64_t mant;
    uint16_t exp;

    qemu_get_be64s(f, &mant);
    qemu_get_be16s(f, &exp);
    fp_reg->mmx.MMX_Q(0) = mant;
    return 0;
}

const VMStateInfo vmstate_fpreg_0_mmx = {
    .name = "fpreg_0_mmx",
    .get  = get_fpreg_0_mmx,
    .put  = put_fpreg_error,
};

static int get_fpreg_0_no_mmx(QEMUFile *f, void *opaque, size_t size)
{
    FPReg *fp_reg = opaque;
    uint64_t mant;
    uint16_t exp;

    qemu_get_be64s(f, &mant);
    qemu_get_be16s(f, &exp);

    fp_reg->d = cpu_set_fp80(mant, exp);
    return 0;
}

const VMStateInfo vmstate_fpreg_0_no_mmx = {
    .name = "fpreg_0_no_mmx",
    .get  = get_fpreg_0_no_mmx,
    .put  = put_fpreg_error,
};

static bool fpregs_is_1(void *opaque, int version_id)
{
    CPUState *env = opaque;

    return env->fpregs_format_vmstate == 1;
}

static bool fpregs_is_0_mmx(void *opaque, int version_id)
{
    CPUState *env = opaque;
    int guess_mmx;

    guess_mmx = ((env->fptag_vmstate == 0xff) &&
                 (env->fpus_vmstate & 0x3800) == 0);
    return guess_mmx && env->fpregs_format_vmstate == 0;
}

static bool fpregs_is_0_no_mmx(void *opaque, int version_id)
{
    CPUState *env = opaque;
    int guess_mmx;

    guess_mmx = ((env->fptag_vmstate == 0xff) &&
                 (env->fpus_vmstate & 0x3800) == 0);
    return !guess_mmx && env->fpregs_format_vmstate == 0;
}

#define VMSTATE_FP_REGS(_field, _state, _n)                               \
    VMSTATE_ARRAY_TEST(_field, _state, _n, fpregs_is_1, vmstate_fpreg, FPReg), \
    VMSTATE_ARRAY_TEST(_field, _state, _n, fpregs_is_0_mmx, vmstate_fpreg_0_mmx, FPReg), \
    VMSTATE_ARRAY_TEST(_field, _state, _n, fpregs_is_0_no_mmx, vmstate_fpreg_0_no_mmx, FPReg)

#endif /* USE_X86LDOUBLE */

static bool version_is_5(void *opaque, int version_id)
{
    return version_id == 5;
}

#ifdef TARGET_X86_64
static bool less_than_7(void *opaque, int version_id)
{
    return version_id < 7;
}

static int get_uint64_as_uint32(QEMUFile *f, void *pv, size_t size)
{
    uint64_t *v = pv;
    *v = qemu_get_be32(f);
    return 0;
}

static void put_uint64_as_uint32(QEMUFile *f, void *pv, size_t size)
{
    uint64_t *v = pv;
    qemu_put_be32(f, *v);
}

static const VMStateInfo vmstate_hack_uint64_as_uint32 = {
    .name = "uint64_as_uint32",
    .get  = get_uint64_as_uint32,
    .put  = put_uint64_as_uint32,
};

#define VMSTATE_HACK_UINT32(_f, _s, _t)                                  \
    VMSTATE_SINGLE_TEST(_f, _s, _t, 0, vmstate_hack_uint64_as_uint32, uint64_t)
#endif

static void cpu_pre_save(void *opaque)
{
    CPUState *env = opaque;
    int i;

    cpu_synchronize_state(env);
    if (kvm_enabled()) {
        kvm_save_mpstate(env);
        kvm_get_vcpu_events(env);
    }

    /* FPU */
    env->fpus_vmstate = (env->fpus & ~0x3800) | (env->fpstt & 0x7) << 11;
    env->fptag_vmstate = 0;
    for(i = 0; i < 8; i++) {
        env->fptag_vmstate |= ((!env->fptags[i]) << i);
    }

#ifdef USE_X86LDOUBLE
    env->fpregs_format_vmstate = 0;
#else
    env->fpregs_format_vmstate = 1;
#endif

    /*
     * Real mode guest segments register DPL should be zero.
     * Older KVM version were setting it wrongly.
     * Fixing it will allow live migration to host with unrestricted guest
     * support (otherwise the migration will fail with invalid guest state
     * error).
     */
    if (!(env->cr[0] & CR0_PE_MASK) &&
        (env->segs[R_CS].flags >> DESC_DPL_SHIFT & 3) != 0) {
        env->segs[R_CS].flags &= ~(env->segs[R_CS].flags & DESC_DPL_MASK);
        env->segs[R_DS].flags &= ~(env->segs[R_DS].flags & DESC_DPL_MASK);
        env->segs[R_ES].flags &= ~(env->segs[R_ES].flags & DESC_DPL_MASK);
        env->segs[R_FS].flags &= ~(env->segs[R_FS].flags & DESC_DPL_MASK);
        env->segs[R_GS].flags &= ~(env->segs[R_GS].flags & DESC_DPL_MASK);
        env->segs[R_SS].flags &= ~(env->segs[R_SS].flags & DESC_DPL_MASK);
    }
}

static int cpu_pre_load(void *opaque)
{
    CPUState *env = opaque;

    cpu_synchronize_state(env);
    return 0;
}

static int cpu_post_load(void *opaque, int version_id)
{
    CPUState *env = opaque;
    int i;

    /*
     * Real mode guest segments register DPL should be zero.
     * Older KVM version were setting it wrongly.
     * Fixing it will allow live migration from such host that don't have
     * restricted guest support to a host with unrestricted guest support
     * (otherwise the migration will fail with invalid guest state
     * error).
     */
    if (!(env->cr[0] & CR0_PE_MASK) &&
        (env->segs[R_CS].flags >> DESC_DPL_SHIFT & 3) != 0) {
        env->segs[R_CS].flags &= ~(env->segs[R_CS].flags & DESC_DPL_MASK);
        env->segs[R_DS].flags &= ~(env->segs[R_DS].flags & DESC_DPL_MASK);
        env->segs[R_ES].flags &= ~(env->segs[R_ES].flags & DESC_DPL_MASK);
        env->segs[R_FS].flags &= ~(env->segs[R_FS].flags & DESC_DPL_MASK);
        env->segs[R_GS].flags &= ~(env->segs[R_GS].flags & DESC_DPL_MASK);
        env->segs[R_SS].flags &= ~(env->segs[R_SS].flags & DESC_DPL_MASK);
    }

    /* XXX: restore FPU round state */
    env->fpstt = (env->fpus_vmstate >> 11) & 7;
    env->fpus = env->fpus_vmstate & ~0x3800;
    env->fptag_vmstate ^= 0xff;
    for(i = 0; i < 8; i++) {
        env->fptags[i] = (env->fptag_vmstate >> i) & 1;
    }

    cpu_breakpoint_remove_all(env, BP_CPU);
    cpu_watchpoint_remove_all(env, BP_CPU);
    for (i = 0; i < 4; i++)
        hw_breakpoint_insert(env, i);

    tlb_flush(env, 1);

    if (kvm_enabled()) {
        /* when in-kernel irqchip is used, env->halted causes deadlock
           because no userspace IRQs will ever clear this flag */
        env->halted = 0;

        kvm_load_tsc(env);
        kvm_load_mpstate(env);
        kvm_put_vcpu_events(env);
    }

    return 0;
}

static bool vmstate_xsave_needed(void *opaque)
{
    CPUState *cs = opaque;

    return (cs->cpuid_ext_features & CPUID_EXT_XSAVE);
}

static const VMStateDescription vmstate_xsave ={
    .name = "cpu/xsave",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
	VMSTATE_UINT64_V(xcr0, CPUState, 1),
	VMSTATE_UINT64_V(xstate_bv, CPUState, 1),
	VMSTATE_YMMH_REGS_VARS(ymmh_regs, CPUState, CPU_NB_REGS, 1),
	VMSTATE_END_OF_LIST()
    }
};

static bool pv_eoi_msr_needed(void *opaque)
{
    CPUState *cpu = opaque;

    return cpu->pv_eoi_en_msr != 0;
}

static const VMStateDescription vmstate_pv_eoi_msr = {
    .name = "cpu/async_pv_eoi_msr",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT64(pv_eoi_en_msr, CPUX86State),
        VMSTATE_END_OF_LIST()
    }
};

static bool tscdeadline_needed(void *opaque)
{
    CPUState *env = opaque;

    return env->tsc_deadline != 0;
}

static const VMStateDescription vmstate_msr_tscdeadline = {
    .name = "cpu/msr_tscdeadline",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT64(tsc_deadline, CPUState),
        VMSTATE_END_OF_LIST()
    }
};

static bool hyperv_hypercall_needed(void *opaque)
{
    CPUState *env = opaque;

    return env->hyperv_guest_os_id != 0;
}

static const VMStateDescription vmstate_msr_hyperv_hypercall = {
    .name = "cpu/msr_hyperv_hypercall",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT64(hyperv_guest_os_id, CPUState),
        VMSTATE_UINT64(hyperv_hypercall, CPUState),
        VMSTATE_END_OF_LIST()
    }
};

static bool steal_time_msr_needed(void *opaque)
{
    CPUState *env = opaque;

    return migrate_steal_time_msr && (env->steal_time_msr != 0);
}

static const VMStateDescription vmstate_steal_time_msr = {
    .name = "cpu/steal_time_msr",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT64(steal_time_msr, CPUState),
        VMSTATE_END_OF_LIST()
    }
};

static bool pmu_enable_needed(void *opaque)
{
    CPUState *env = opaque;
    int i;

    if (!migrate_pmu) {
        return false;
    }
    if (env->msr_fixed_ctr_ctrl || env->msr_global_ctrl ||
        env->msr_global_status || env->msr_global_ovf_ctrl) {
        return true;
    }
    for (i = 0; i < MAX_FIXED_COUNTERS; i++) {
        if (env->msr_fixed_counters[i]) {
            return true;
        }
    }
    for (i = 0; i < MAX_GP_COUNTERS; i++) {
        if (env->msr_gp_counters[i] || env->msr_gp_evtsel[i]) {
            return true;
        }
    }

    return false;
}

static const VMStateDescription vmstate_msr_architectural_pmu = {
    .name = "cpu/msr_architectural_pmu",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT64(msr_fixed_ctr_ctrl, CPUState),
        VMSTATE_UINT64(msr_global_ctrl, CPUState),
        VMSTATE_UINT64(msr_global_status, CPUState),
        VMSTATE_UINT64(msr_global_ovf_ctrl, CPUState),
        VMSTATE_UINT64_ARRAY(msr_fixed_counters, CPUState, MAX_FIXED_COUNTERS),
        VMSTATE_UINT64_ARRAY(msr_gp_counters, CPUState, MAX_GP_COUNTERS),
        VMSTATE_UINT64_ARRAY(msr_gp_evtsel, CPUState, MAX_GP_COUNTERS),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_cpu = {
    .name = "cpu",
    .version_id = CPU_SAVE_VERSION,
    .max_version_id = CPU_SAVE_MAX_VERSION,
    .minimum_version_id = 3,
    .minimum_version_id_old = 3,
    .pre_save = cpu_pre_save,
    .pre_load = cpu_pre_load,
    .post_load = cpu_post_load,
    .fields      = (VMStateField []) {
        VMSTATE_UINTTL_ARRAY(regs, CPUState, CPU_NB_REGS),
        VMSTATE_UINTTL(eip, CPUState),
        VMSTATE_UINTTL(eflags, CPUState),
        VMSTATE_UINT32(hflags, CPUState),
        /* FPU */
        VMSTATE_UINT16(fpuc, CPUState),
        VMSTATE_UINT16(fpus_vmstate, CPUState),
        VMSTATE_UINT16(fptag_vmstate, CPUState),
        VMSTATE_UINT16(fpregs_format_vmstate, CPUState),
        VMSTATE_FP_REGS(fpregs, CPUState, 8),

        VMSTATE_SEGMENT_ARRAY(segs, CPUState, 6),
        VMSTATE_SEGMENT(ldt, CPUState),
        VMSTATE_SEGMENT(tr, CPUState),
        VMSTATE_SEGMENT(gdt, CPUState),
        VMSTATE_SEGMENT(idt, CPUState),

        VMSTATE_UINT32(sysenter_cs, CPUState),
#ifdef TARGET_X86_64
        /* Hack: In v7 size changed from 32 to 64 bits on x86_64 */
        VMSTATE_HACK_UINT32(sysenter_esp, CPUState, less_than_7),
        VMSTATE_HACK_UINT32(sysenter_eip, CPUState, less_than_7),
        VMSTATE_UINTTL_V(sysenter_esp, CPUState, 7),
        VMSTATE_UINTTL_V(sysenter_eip, CPUState, 7),
#else
        VMSTATE_UINTTL(sysenter_esp, CPUState),
        VMSTATE_UINTTL(sysenter_eip, CPUState),
#endif

        VMSTATE_UINTTL(cr[0], CPUState),
        VMSTATE_UINTTL(cr[2], CPUState),
        VMSTATE_UINTTL(cr[3], CPUState),
        VMSTATE_UINTTL(cr[4], CPUState),
        VMSTATE_UINTTL_ARRAY(dr, CPUState, 8),
        /* MMU */
        VMSTATE_INT32(a20_mask, CPUState),
        /* XMM */
        VMSTATE_UINT32(mxcsr, CPUState),
        VMSTATE_XMM_REGS(xmm_regs, CPUState, CPU_NB_REGS),

#ifdef TARGET_X86_64
        VMSTATE_UINT64(efer, CPUState),
        VMSTATE_UINT64(star, CPUState),
        VMSTATE_UINT64(lstar, CPUState),
        VMSTATE_UINT64(cstar, CPUState),
        VMSTATE_UINT64(fmask, CPUState),
        VMSTATE_UINT64(kernelgsbase, CPUState),
#endif
        VMSTATE_UINT32_V(smbase, CPUState, 4),

        VMSTATE_UINT64_V(pat, CPUState, 5),
        VMSTATE_UINT32_V(hflags2, CPUState, 5),

        VMSTATE_UINT32_TEST(halted, CPUState, version_is_5),
        VMSTATE_UINT64_V(vm_hsave, CPUState, 5),
        VMSTATE_UINT64_V(vm_vmcb, CPUState, 5),
        VMSTATE_UINT64_V(tsc_offset, CPUState, 5),
        VMSTATE_UINT64_V(intercept, CPUState, 5),
        VMSTATE_UINT16_V(intercept_cr_read, CPUState, 5),
        VMSTATE_UINT16_V(intercept_cr_write, CPUState, 5),
        VMSTATE_UINT16_V(intercept_dr_read, CPUState, 5),
        VMSTATE_UINT16_V(intercept_dr_write, CPUState, 5),
        VMSTATE_UINT32_V(intercept_exceptions, CPUState, 5),
        VMSTATE_UINT8_V(v_tpr, CPUState, 5),
        /* MTRRs */
        VMSTATE_UINT64_ARRAY_V(mtrr_fixed, CPUState, 11, 8),
        VMSTATE_UINT64_V(mtrr_deftype, CPUState, 8),
        VMSTATE_MTRR_VARS(mtrr_var, CPUState, 8, 8),
        /* KVM-related states */
        VMSTATE_INT32_V(interrupt_injected, CPUState, 9),
        VMSTATE_UINT32_V(mp_state, CPUState, 9),
        VMSTATE_UINT64_V(tsc, CPUState, 9),
        VMSTATE_INT32_V(exception_injected, CPUState, 11),
        VMSTATE_UINT8_V(soft_interrupt, CPUState, 11),
        VMSTATE_UINT8_V(nmi_injected, CPUState, 11),
        VMSTATE_UINT8_V(nmi_pending, CPUState, 11),
        VMSTATE_UINT8_V(has_error_code, CPUState, 11),
        VMSTATE_UINT32_V(sipi_vector, CPUState, 11),
        /* MCE */
        VMSTATE_UINT64_V(mcg_cap, CPUState, 10),
        VMSTATE_UINT64_V(mcg_status, CPUState, 10),
        VMSTATE_UINT64_V(mcg_ctl, CPUState, 10),
        VMSTATE_UINT64_ARRAY_V(mce_banks, CPUState, MCE_BANKS_DEF *4, 10),
        /* rdtscp */
        VMSTATE_UINT64_V(tsc_aux, CPUState, 11),
        /* KVM pvclock msr */
        VMSTATE_UINT64_V(system_time_msr, CPUState, 11),
        VMSTATE_UINT64_V(wall_clock_msr, CPUState, 11),
        VMSTATE_END_OF_LIST()
        /* The above list is not sorted /wrt version numbers, watch out! */
    },
    /*
       Put the XSAVE/PV_EOI state in sub-sections to allow compatibility with
	older save files.
     */
    .subsections = (VMStateSubsection []) {
	{
	    .vmsd = &vmstate_xsave,
	    .needed = vmstate_xsave_needed,
	}, {
            .vmsd = &vmstate_pv_eoi_msr,
            .needed = pv_eoi_msr_needed,
        }, {
            .vmsd = &vmstate_msr_tscdeadline,
            .needed = tscdeadline_needed,
        }, {
            .vmsd = &vmstate_msr_hyperv_hypercall,
            .needed = hyperv_hypercall_needed,
        }, {
            .vmsd = &vmstate_steal_time_msr,
            .needed = steal_time_msr_needed, 
        }, {
            .vmsd = &vmstate_msr_architectural_pmu,
            .needed = pmu_enable_needed,
        } , {
	    /* empty */
	}
    }
};

void cpu_save(QEMUFile *f, void *opaque)
{
    vmstate_save_state(f, &vmstate_cpu, opaque);
}

int cpu_load(QEMUFile *f, void *opaque, int version_id)
{
    return vmstate_load_state(f, &vmstate_cpu, opaque, version_id);
}
