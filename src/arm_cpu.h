/* A from-scratch ARM CPU interpreter: ARM (A32) and Thumb/Thumb-2 (T16/T32),
 * banked registers, CPSR/SPSR, and manual exception entry - everything the
 * Python version got from Unicorn 2.1.4 (whose default core is a Cortex-A15).
 *
 * The memory bus routes accesses: a hooked MMIO range calls back into the
 * emulator (which may stage a value in backing store), everything else hits a
 * flat page table directly. emu_start(pc, count) runs `count` instructions and
 * returns, like Unicorn's counted run. Unknown encodings stop with
 * ERR_INVALID; an unmapped access invokes the hook, which can map a page and
 * resume. All register and flag semantics were verified against Unicorn
 * (see PORTING_NOTES.md).
 */
#ifndef BK_ARM_CPU_H
#define BK_ARM_CPU_H

#include "common.h"

/* ARM register indices, matching Unicorn's UC_ARM_REG_* ordering we care about.
 * (REG_R1..REG_R12 are omitted because glibc <sys/ucontext.h> on x86_64 Linux
 * defines REG_R8..REG_R15 in the global namespace via <signal.h>.) */
enum {
    REG_R0 = 0,
    REG_SP = 13, REG_LR = 14, REG_PC = 15,
    REG_CPSR = 16, REG_SPSR = 17, REG_COUNT = 18
};

/* CPU processor modes (CPSR[4:0]). */
enum {
    MODE_USR = 0x10, MODE_FIQ = 0x11, MODE_IRQ = 0x12, MODE_SVC = 0x13,
    MODE_ABT = 0x17, MODE_UND = 0x1B, MODE_SYS = 0x1F
};

/* CPSR bit masks (u32 constants so they don't warn in ?: with plain 0). */
static const u32 PSR_N = 0x80000000u, PSR_Z = 0x40000000u, PSR_C = 0x20000000u, PSR_V = 0x10000000u;
static const u32 PSR_Q = 0x08000000u, PSR_T = 0x00000020u, PSR_F = 0x00000040u, PSR_I = 0x00000080u;
static const u32 PSR_A = 0x00000100u, PSR_E = 0x00000200u, PSR_MODE = 0x0000001Fu;

enum RunResult {
    RUN_COUNT_DONE = 0, /* ran the requested instruction count */
    RUN_STOPPED,        /* stop() was requested (emu_stop) */
    RUN_HIT_UNTIL,      /* reached the `until` address */
    RUN_ERR_INVALID,    /* undefined/unsupported instruction */
    RUN_ERR_MEM         /* unrecoverable memory fault */
};

class ArmCpu;

/* Backing memory plus MMIO dispatch. The emulator supplies the MMIO callbacks. */
class Memory {
public:
    Memory();
    ~Memory();

    /* Map `size` bytes of zero-initialised backing at `base` (page aligned). */
    bool map(u32 base, u32 size);
    bool is_mapped(u32 addr) const;

    /* Register a hooked range [begin, end); reads/writes there call the emulator. */
    void add_read_hook(u32 begin, u32 end);
    void add_write_hook(u32 begin, u32 end);

    /* Direct backing access, i.e. Unicorn's mem_read / mem_write (no hooks). */
    u32 phys_read(u32 addr, int size) const
    {
        u8 *h = host_[addr >> 12];
        if (!h)
            return 0;
        u32 off = addr & (PAGE - 1);
        if (__builtin_expect(off + (u32)size <= PAGE, 1)) {
            const u8 *p = h + off;
            if (size == 4)
                return rd32le(p);
            if (size == 1)
                return p[0];
            return rd16le(p);
        }
        return phys_read_slow(addr, size);
    }
    void phys_write(u32 addr, int size, u32 value)
    {
        u8 *h = host_[addr >> 12];
        if (!h)
            return;
        u32 off = addr & (PAGE - 1);
        if (__builtin_expect(off + (u32)size <= PAGE, 1)) {
            u8 *p = h + off;
            if (size == 4)
                wr32le(p, value);
            else if (size == 1)
                p[0] = (u8)value;
            else
                wr16le(p, (u16)value);
            return;
        }
        phys_write_slow(addr, size, value);
    }
    u32 phys_read_slow(u32 addr, int size) const;
    void phys_write_slow(u32 addr, int size, u32 value);
    void phys_read_block(u32 addr, u8 *dst, u32 len) const;
    void phys_write_block(u32 addr, const u8 *src, u32 len);
    Bytes read_bytes(u32 addr, u32 len) const;
    void write_bytes(u32 addr, const Bytes &data);
    void fill(u32 addr, u8 value, u32 len);

    /* Hooked accesses used by the CPU. Return false on an unrecoverable fault. */
    bool bus_read(u32 addr, int size, u32 &out);
    bool bus_write(u32 addr, int size, u32 value);

    /* Fast instruction fetch (never hooked). Inlined on the hot path. */
    u32 fetch32(u32 addr, bool &ok) const
    {
        u8 *h = host_[addr >> 12];
        u32 off = addr & (PAGE - 1);
        if (__builtin_expect(h != 0 && off <= PAGE - 4, 1)) {
            ok = true;
            return rd32le(h + off);
        }
        return fetch32_slow(addr, ok);
    }
    u16 fetch16(u32 addr, bool &ok) const
    {
        u8 *h = host_[addr >> 12];
        u32 off = addr & (PAGE - 1);
        if (__builtin_expect(h != 0 && off <= PAGE - 2, 1)) {
            ok = true;
            return rd16le(h + off);
        }
        return fetch16_slow(addr, ok);
    }
    /* Combined Thumb fetch: when pc is at least 4 bytes from page end, loads
     * both halfwords in one 32-bit read if the first halfword is Thumb-2. */
    bool fetch_thumb_fast(u32 pc, u32 &insn, bool &wide) const
    {
        u8 *h = host_[pc >> 12];
        u32 off = pc & (PAGE - 1);
        if (__builtin_expect(h != 0 && off <= PAGE - 4, 1)) {
            u32 raw = rd32le(h + off);
            u16 hw = (u16)raw;
            if ((hw & 0xF800) >= 0xE800) {
                wide = true;
                insn = ((u32)hw << 16) | (raw >> 16);
            } else {
                wide = false;
                insn = hw;
            }
            return true;
        }
        return false;
    }
    u32 fetch32_slow(u32 addr, bool &ok) const;
    u16 fetch16_slow(u32 addr, bool &ok) const;

    /* Fast path for the CPU data accesses: a host pointer when the whole
     * `size`-byte access lands in one directly-addressable (mapped, unhooked)
     * page, else 0 (caller uses bus_read/bus_write). */
    u8 *fast_ptr(u32 addr, int size, bool for_write)
    {
        u8 *h = (for_write ? fast_wr_ : fast_rd_)[addr >> 12];
        u32 off = addr & 0xFFFu;
        if (__builtin_expect(h != 0 && off + (u32)size <= 0x1000u, 1))
            return h + off;
        return 0;
    }

    /* list of mapped regions, for snapshots: (begin, size). */
    void regions(std::vector<std::pair<u32, u32> > &out) const;

    /* The owning emulator, for MMIO callbacks (set by the emulator). */
    void *owner;
    /* callbacks: read hook stages a value in backing (may be a no-op); write
     * hook performs the register logic and any backing write. */
    void (*on_read)(void *owner, Memory *mem, u32 addr, int size);
    void (*on_write)(void *owner, Memory *mem, u32 addr, int size, u32 value);
    /* unmapped access handler: returns true if it mapped a page (then retry). */
    /* type is Unicorn's code: UNMAPPED_READ / UNMAPPED_WRITE / UNMAPPED_FETCH */
    bool (*on_unmapped)(void *owner, Memory *mem, u32 addr, int size, int type);
    enum { UNMAPPED_READ = 19, UNMAPPED_WRITE = 20, UNMAPPED_FETCH = 21 };
    /* Set when an access was mapped on demand but Unicorn would still fail it
     * with UC_ERR_MAP (see bus_read); the CPU reports that error instead. */
    bool map_error;

    /* A CPU context (banked registers) for save/restore, like Unicorn context. */
private:
    static const u32 PAGE = 0x1000;
    static const u32 NPAGES = 1u << 20; /* 4 GB / 4 KB */
    u8 **host_;                         /* per-page backing pointer or 0 */
    u8 **fast_rd_;                      /* per-page unhooked read pointer or 0 */
    u8 **fast_wr_;                      /* per-page unhooked write pointer or 0 */
    u8 *rd_hook_;                       /* per-page read-hook flag */
    u8 *wr_hook_;                       /* per-page write-hook flag */
    std::vector<u8 *> blocks_;          /* owned backing allocations */
    std::vector<std::pair<u32, u32> > regions_;
    Memory(const Memory &);
    Memory &operator=(const Memory &);
};

/* Full CPU register state, including every banked register - this is what a
 * snapshot saves and a reset restores (Unicorn's context_save/restore). */
struct CpuContext {
    u32 r[16];      /* current-mode view r0..r15 (r15 = PC) */
    u32 cpsr;
    /* banked registers */
    u32 r8_usr, r9_usr, r10_usr, r11_usr, r12_usr, sp_usr, lr_usr;
    u32 r8_fiq, r9_fiq, r10_fiq, r11_fiq, r12_fiq, sp_fiq, lr_fiq;
    u32 sp_irq, lr_irq, sp_svc, lr_svc, sp_abt, lr_abt, sp_und, lr_und;
    u32 spsr_fiq, spsr_irq, spsr_svc, spsr_abt, spsr_und;
};

class ArmCpu {
public:
    explicit ArmCpu(Memory &mem);

    void reset();
    u32 reg_read(int reg) const;
    void reg_write(int reg, u32 value);

    /* emu_start: begin at `start` (bit 0 selects Thumb), stop after `count`
     * instructions or on reaching `until`, stop(), or a fault. */
    RunResult run(u32 start, u32 until, u64 count);
    void request_stop() { stop_ = true; }
    void clear_stop() { stop_ = false; }

    /* context save/restore for snapshots and resets */
    void context_save(CpuContext &ctx) const;
    void context_restore(const CpuContext &ctx);

    /* the error text for the last RUN_ERR_* result (like UcError's message) */
    const std::string &error() const { return error_; }
    u32 last_fault_addr() const { return fault_addr_; }

    /* exception vector base the emulator applies to its manual IRQ/FIQ/SWI
     * entry (ArmCpu does the SWI entry itself; IRQ/FIQ the emulator triggers). */
    u32 vector_base;

    /* Called by the emulator to deliver IRQ/FIQ between runs, matching the
     * Python trigger_irq/trigger_fiq exactly. Returns true if taken. */
    bool take_irq();
    bool take_fiq();

    /* PC watchpoints for --trace-pc: when PC first reaches a listed address the
     * callback fires. Checked per instruction only while the list is non-empty. */
    std::vector<u32> watch_pcs;
    void (*watch_cb)(void *ctx, u32 addr);
    void (*on_swi)(void *ctx);
    void *watch_ctx;

    Memory &mem;

private:
    u32 r_[16];
    u32 cpsr_;
    u32 spsr_;              /* current mode's SPSR view */
    /* banked storage (USR/SYS share) */
    u32 bank_usr_[7];       /* r8..r14 user/system */
    u32 bank_fiq_[7];       /* r8..r14 fiq */
    u32 bank_irq_[2];       /* sp, lr */
    u32 bank_svc_[2];
    u32 bank_abt_[2];
    u32 bank_und_[2];
    u32 spsr_fiq_, spsr_irq_, spsr_svc_, spsr_abt_, spsr_und_;
    /* QEMU keeps a real SPSR slot for USR/SYS (banked_spsr[0]): MRS/MSR SPSR
     * in SYS mode and Unicorn's reg read/write in USR/SYS reach it. Not part
     * of CpuContext (that struct is the raw snapshot format). */
    u32 spsr_usr_;

    bool stop_;
    std::string error_;
    u32 fault_addr_;

    /* --- helpers --- */
    bool thumb() const { return (cpsr_ & PSR_T) != 0; }
    u32 cur_mode() const { return cpsr_ & PSR_MODE; }
    void switch_mode(u32 new_mode);
    void set_cpsr(u32 value, bool privileged);
    u32 get_spsr() const;
    void set_spsr(u32 value);

    bool cond_passed(u32 cond) const
    {
        if (__builtin_expect(cond == 0xE, 1))
            return true;
        static const u16 lut[16] = {
            0xF0F0u, 0x0F0Fu, 0xCCCCu, 0x3333u,
            0xFF00u, 0x00FFu, 0xAAAAu, 0x5555u,
            0x0C0Cu, 0xF3F3u, 0xAA55u, 0x55AAu,
            0x0A05u, 0xF5FAu, 0xFFFFu, 0xFFFFu
        };
        return ((lut[cond] >> (cpsr_ >> 28)) & 1u) != 0;
    }
    void set_nz(u32 result)
    {
        cpsr_ = (cpsr_ & ~(PSR_N | PSR_Z)) | (result & 0x80000000u) | (result == 0 ? PSR_Z : 0u);
    }
    u32 shift_c(u32 type, u32 value, u32 amount, bool &carry, bool imm_form) const
    {
        if (__builtin_expect(amount == 0 && (type == 0 || !imm_form), 1))
            return value;
        return shift_c_slow(type, value, amount, carry, imm_form);
    }
    u32 shift_c_slow(u32 type, u32 value, u32 amount, bool &carry, bool imm_form) const;

    /* instruction steps; return false to stop the run (fault/invalid/until). */
    bool step_arm();
    bool step_thumb();
    bool invalid(u32 pc, u32 insn);

    /* Thumb execute groups; return false when the encoding is undefined. */
    bool exec_thumb16(u32 hw, u32 pc, bool in_it);
    bool exec_thumb32(u32 insn, u32 pc, bool in_it);
    bool thumb_dp_reg(u32 hw, bool s);
    bool thumb_misc(u32 hw, u32 pc, bool in_it);
    u32 thumb_expand_imm(u32 imm12, bool &carry) const;
    void it_advance(); /* ITAdvance() on the CPSR IT bits */
    bool t32_alu(u32 opc, bool s, u32 rd, u32 a, u32 b, bool shc);
    bool t32_ldst(bool load, int size, bool sign, u32 rt, u32 rn, u32 off, bool rr, bool p, bool u, bool w);
    bool t32_ldrd(bool load, u32 rt, u32 rt2, u32 rn, u32 imm, bool p, bool u, bool w);
    bool t32_group_e8(u32 insn, u32 pc);
    bool t32_group_f0(u32 insn, u32 pc, bool in_it);
    bool t32_group_f8(u32 insn, u32 pc);
    u32 fpexc_; /* VFP FPEXC (only the EN bit is kept, as in QEMU) */
    /* Inside an IT block Unicorn cannot stop: a store to unmapped memory is
     * skipped, the block runs on, and the error is raised after it. */
    bool defer_store_;
    std::string pending_err_;
    bool dry_run_; /* stores are suppressed (dry_run_undef) */
    bool pend_req_; /* the deferred fault's stop request is still pending */
    bool dry_run_undef(u32 insn, bool wide, u32 pc);
    bool it_set_;      /* this insn wrote ITSTATE itself (IT, SVC): skip the advance */

    /* ARM execute groups; the bool ones return false for an UNDEF encoding
     * (QEMU 5.0 / Unicorn 2.1.4 a32.decode accept set). */
    bool exec_arm_dp(u32 insn);
    bool exec_arm_ls(u32 insn);        /* single data transfer */
    bool exec_arm_ls_extra(u32 insn);  /* LDRH/STRH/LDRD/LDRSB etc */
    bool exec_arm_block(u32 insn);     /* LDM/STM */
    bool exec_arm_mul(u32 insn, u32 pc); /* multiplies, SWP, LDREX/STREX */
    void exec_arm_branch(u32 insn);
    bool exec_arm_misc(u32 insn, u32 pc); /* MRS/MSR/CLZ/BX/QADD/SMLAxy/BKPT/...; false if invalid */
    bool exec_arm_misc_imm(u32 insn, u32 pc); /* MOVW/MOVT/MSR imm/hints */
    bool exec_arm_media(u32 insn);
    bool exec_coproc(u32 insn);        /* MRC/MCR/MRRC/MCRR (+ LDC/STC aliases) */

    /* Operations shared by A32 and T32, with QEMU's trans_* semantics. */
    void wreg(u32 rd, u32 v) /* QEMU store_reg: r15 is a same-ISA branch */
    {
        if (rd == 15)
            branch_same(v);
        else
            r_[rd] = v;
    }
    void write_cpsr_insn(u32 val, u32 mask);
    void exception_return(u32 pc, u32 spsr);
    void stall(u32 pc); /* an exception Unicorn's hook swallows: PC stays */
    bool hint(u32 op8, u32 next_pc);
    u32 mrs_value(bool spsr) const;
    bool op_msr(u32 val, u32 fieldmask, bool spsr);
    bool banked_decode(bool r, u32 sysm, u32 &tgtmode, u32 &regno) const;
    bool op_mrs_banked(bool r, u32 sysm, u32 rd);
    bool op_msr_banked(bool r, u32 sysm, u32 rn);
    u32 user_reg(u32 i) const;
    void set_user_reg(u32 i, u32 v);
    void op_mla(u32 rd, u32 rn, u32 rm, u32 ra, bool add, bool s);
    void op_mlal(u32 ra, u32 rd, u32 rn, u32 rm, bool uns, bool add, bool s);
    void op_umaal(u32 ra, u32 rd, u32 rn, u32 rm);
    void op_qaddsub(u32 rd, u32 rn, u32 rm, bool add, bool doub);
    void op_smlaxy(u32 rd, u32 ra, u32 rn, u32 rm, int add_long, bool nt, bool mt);
    void op_smlawy(u32 rd, u32 ra, u32 rn, u32 rm, bool add, bool mt);
    void op_par_addsub(u32 kind, u32 op, u32 rd, u32 rn, u32 rm);
    void op_pkh(u32 rd, u32 rn, u32 rm, u32 shift, bool tb);
    void op_sat(u32 rd, u32 rn, u32 shift, bool sh, u32 satimm, int kind);
    void op_xta(u32 rd, u32 rn, u32 rm, u32 rot, int kind);
    void op_sel(u32 rd, u32 rn, u32 rm);
    void op_rev(u32 rd, u32 rm, int kind);
    void op_smlad(u32 rd, u32 ra, u32 rn, u32 rm, bool swap, bool sub);
    void op_smlald(u32 ra, u32 rd, u32 rn, u32 rm, bool swap, bool sub);
    void op_smmla(u32 rd, u32 ra, u32 rn, u32 rm, bool round, bool sub);
    void op_div(u32 rd, u32 rn, u32 rm, bool u);
    void op_usada8(u32 rd, u32 ra, u32 rn, u32 rm);
    bool op_bfx(u32 rd, u32 rn, u32 lsb, u32 widthm1, bool u);
    bool op_bfci(u32 rd, u32 rn, u32 lsb, u32 msb);
    bool op_ldrex(u32 rt, u32 rt2, u32 rn, u32 imm, int size, u32 pc);
    bool op_strex(u32 rd, u32 rt, u32 rt2, u32 rn, u32 imm, int size, u32 pc);
    bool op_swp(u32 rt, u32 rt2, u32 rn, bool byte, u32 pc);
    bool op_ldm(u32 rn, u32 list, bool w, bool b, bool inc, bool user, int min_n);
    bool op_stm(u32 rn, u32 list, bool w, bool b, bool inc, bool user, int min_n);
    bool op_srs(u32 mode, u32 pu, bool w);
    bool op_rfe(u32 rn, u32 pu, bool w);
    void op_cps(u32 imod, bool m, u32 aif, u32 mode);

    /* data accesses: the common case (little-endian, one directly addressable
     * page) inline here, everything else in the out-of-line _slow versions */
    bool mem_load(u32 addr, int size, u32 &out, bool sign)
    {
        u8 *p;
        if (!(cpsr_ & PSR_E) && (p = mem.fast_ptr(addr, size, false)) != 0) {
            if (size == 4)
                out = rd32le(p);
            else if (size == 2)
                out = sign ? (u32)(s32)(s16)rd16le(p) : rd16le(p);
            else
                out = sign ? (u32)(s32)(s8)p[0] : p[0];
            return true;
        }
        return mem_load_slow(addr, size, out, sign);
    }
    bool mem_store(u32 addr, int size, u32 value)
    {
        u8 *p;
        if (!(cpsr_ & PSR_E) && (p = mem.fast_ptr(addr, size, true)) != 0) {
            if (size == 4)
                wr32le(p, value);
            else if (size == 2)
                wr16le(p, (u16)value);
            else
                p[0] = (u8)value;
            return true;
        }
        return mem_store_slow(addr, size, value);
    }
    bool mem_load_slow(u32 addr, int size, u32 &out, bool sign);
    bool mem_store_slow(u32 addr, int size, u32 value);

    bool untilset_;
    u32 until_;
    bool hit_until_;
    bool branched_; /* set by any PC-writing path during the current instruction */
    /* exclusive monitor (QEMU exclusive_addr / exclusive_val) */
    bool excl_valid_;
    u32 excl_addr_;
    u64 excl_val_;
    /* YIELD/WFE stop with UC_ERR_INSN_INVALID but PC already advanced */
    bool undef_next_;
    u32 undef_next_pc_;

    void branch_interwork(u32 value); /* BXWritePC: bit0 selects Thumb */
    void branch_same(u32 value);      /* BranchWritePC: stay in current ISA */
};

#endif
