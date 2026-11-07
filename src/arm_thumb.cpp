/* Thumb step (IT blocks included) and the 16-bit Thumb (T16) decoder for
 * ArmCpu, following QEMU 5.0's t16.decode as built into Unicorn 2.1.4; the
 * 32-bit encodings are in arm_thumb2.cpp. Differentially fuzzed against
 * Unicorn (scratchpad cpufuzz_gen.py + tests/cpufuzz_main.cpp).
 */
#include "arm_cpu.h"

#include <string.h>

/* 32-bit Thumb-2 first halfwords: 0b111 01/10/11 in bits[15:11]. */
static bool is_thumb32(u16 hw) { return (hw & 0xF800) >= 0xE800; }

bool ArmCpu::step_thumb()
{
    u32 pc = r_[15];
    u32 insn;
    bool wide;
    u16 hw;
    if (__builtin_expect(mem.fetch_thumb_fast(pc, insn, wide), 1)) {
        hw = wide ? (u16)(insn >> 16) : (u16)insn;
    } else {
        bool ok;
        hw = mem.fetch16(pc, ok);
        if (!ok) {
            /* see step_arm: the unmapped hook declines a fetch */
            if (mem.on_unmapped && mem.on_unmapped(mem.owner, &mem, pc, 2, Memory::UNMAPPED_FETCH))
                hw = mem.fetch16(pc, ok);
            if (!ok) {
                error_ = strf("Invalid memory fetch (UC_ERR_FETCH_UNMAPPED)");
                fault_addr_ = pc;
                return false;
            }
        }
        wide = is_thumb32(hw);
        insn = hw;
        if (wide) {
            u16 hw2 = mem.fetch16(pc + 2, ok);
            if (!ok) { /* second halfword on an unmapped page */
                error_ = strf("Invalid memory fetch (UC_ERR_FETCH_UNMAPPED)");
                fault_addr_ = pc + 2;
                return false;
            }
            insn = ((u32)hw << 16) | hw2;
        }
    }
    u32 width = wide ? 4 : 2;
    branched_ = false;
    r_[15] = pc + 4; /* Thumb pipeline: reads of r15 see pc+4 (word aligned via &~3 in use) */

    /* IT block (rare): ITSTATE lives in CPSR[26:25,15:10]. A state with
     * IT[3:0] = 0 is dropped (QEMU clears it at the next TB start); an active
     * one makes every instruction but BKPT conditional on IT[7:4]. */
    bool in_it = false;
    if (cpsr_ & 0x0600FC00u) {
        u32 its = ((cpsr_ >> 8) & 0xFC) | ((cpsr_ >> 25) & 3);
        if ((its & 0xF) == 0) {
            cpsr_ &= ~0x0600FC00u;
        } else {
            in_it = true;
            it_set_ = false;
            u32 cond = its >> 4;
            if (cond < 0xE && (wide || (hw & 0xFF00) != 0xBE00) && !cond_passed(cond)) {
                if (!wide && (hw & 0xFF00) == 0xBF00 && (hw & 0xF)) /* IT itself: state is static */
                    cpsr_ = (cpsr_ & ~0x0600FC00u) | ((hw & 0xFC) << 8) | ((hw & 3) << 25);
                else
                    it_advance();
                r_[15] = pc + width;
                /* A deferred store fault (see mem_store) is raised at the next
                 * TB start: after the block, or after a skipped instruction
                 * that ends the TB - an UNDEF one (found by a dry run). */
                if (!pending_err_.empty()
                    && ((cpsr_ & 0x06000C00u) == 0 || (pend_req_ && dry_run_undef(insn, wide, pc)))) {
                    error_ = pending_err_;
                    pending_err_.clear();
                    return false;
                }
                return true;
            }
            defer_store_ = true;
            /* this insn's code hook runs: Unicorn reverts a stop request a
             * faulted store of the block made (revert_uc_emu_stop) */
            pend_req_ = false;
        }
    }

    bool valid = wide ? exec_thumb32(insn, pc, in_it) : exec_thumb16(hw, pc, in_it);
    if (!in_it && valid && error_.empty()) { /* hot path */
        if (!branched_)
            r_[15] = pc + width;
        return true;
    }
    if (in_it)
        defer_store_ = false;

    if (!valid) {
        /* YIELD/WFE: PC (and the IT state) already past the instruction */
        if (undef_next_ && in_it)
            it_advance();
        r_[15] = undef_next_ ? undef_next_pc_ : pc;
        undef_next_ = false;
        pending_err_.clear();
        return invalid(pc, insn);
    }
    if (!error_.empty()) {
        if (!pending_err_.empty()) { /* the first (deferred store) error code wins */
            error_ = pending_err_;
            pending_err_.clear();
        }
        r_[15] = pc; /* see step_arm: Unicorn reports the faulting insn */
        return false;
    }
    /* IT, SVC and a swallowed exception (stall) set it_set_: no advance */
    if (in_it && !it_set_)
        it_advance();
    if (!branched_)
        r_[15] = pc + width;
    if (in_it && !pending_err_.empty() && !(cpsr_ & 0x06000C00u)) {
        /* the IT block is over: Unicorn now stops on the deferred store
         * fault, with PC at the next instruction */
        error_ = pending_err_;
        pending_err_.clear();
        return false;
    }
    return true;
}

/* Would this (condition-failed) T32/T16 instruction be UNDEF? Executes it
 * with stores suppressed and puts the whole CPU state back. Rare path. */
bool ArmCpu::dry_run_undef(u32 insn, bool wide, u32 pc)
{
    CpuContext ctx;
    context_save(ctx);
    u32 s_usr = spsr_usr_, s_fpexc = fpexc_, s_ea = excl_addr_, s_fa = fault_addr_;
    u64 s_ev = excl_val_;
    bool s_ex = excl_valid_, s_br = branched_, s_un = undef_next_, s_its = it_set_, s_def = defer_store_;
    u32 s_unpc = undef_next_pc_;
    std::string s_err = error_, s_pend = pending_err_;
    dry_run_ = true;
    cpsr_ |= PSR_E; /* routes stores to the dry-run check in mem_store */
    r_[15] = pc + 4;
    bool valid = wide ? exec_thumb32(insn, pc, true) : exec_thumb16(insn & 0xFFFF, pc, true);
    dry_run_ = false;
    context_restore(ctx);
    spsr_usr_ = s_usr;
    fpexc_ = s_fpexc;
    excl_addr_ = s_ea;
    excl_val_ = s_ev;
    excl_valid_ = s_ex;
    fault_addr_ = s_fa;
    branched_ = s_br;
    undef_next_ = s_un;
    undef_next_pc_ = s_unpc;
    it_set_ = s_its;
    defer_store_ = s_def;
    error_ = s_err;
    pending_err_ = s_pend;
    return !valid;
}

void ArmCpu::it_advance()
{
    u32 its = ((cpsr_ >> 8) & 0xFC) | ((cpsr_ >> 25) & 3);
    its = (its & 7) ? ((its & 0xE0) | ((its << 1) & 0x1F)) : 0;
    cpsr_ = (cpsr_ & ~0x0600FC00u) | ((its & 0xFC) << 8) | ((its & 3) << 25);
}

/* ------------------------------------------------------------- 16-bit Thumb */
bool ArmCpu::exec_thumb16(u32 hw, u32 pc, bool in_it)
{
    u32 op = hw >> 12;
    bool s = !in_it; /* T16 data processing sets flags only outside an IT block */

    switch (op) {
    case 0x0:
    case 0x1: {
        u32 op2 = (hw >> 11) & 3;
        if (op2 != 3) {
            /* LSL/LSR/ASR immediate (move shifted register) */
            u32 imm5 = (hw >> 6) & 0x1F;
            u32 rm = (hw >> 3) & 7;
            u32 rd = hw & 7;
            bool c = (cpsr_ & PSR_C) != 0;
            u32 res = shift_c(op2, r_[rm], imm5, c, true);
            r_[rd] = res;
            if (s) {
                set_nz(res);
                cpsr_ = (cpsr_ & ~PSR_C) | (c ? PSR_C : 0);
            }
        } else {
            /* ADD/SUB register or immediate(3) */
            bool sub = (hw >> 9) & 1;
            bool imm = (hw >> 10) & 1;
            u32 rn = (hw >> 3) & 7;
            u32 rd = hw & 7;
            u32 b = imm ? ((hw >> 6) & 7) : r_[(hw >> 6) & 7];
            u32 a = r_[rn], res;
            bool cf, vf;
            if (sub) {
                res = a - b;
                cf = a >= b;
                vf = (((a ^ b) & (a ^ res)) & 0x80000000u) != 0;
            } else {
                u64 sum = (u64)a + b;
                res = (u32)sum;
                cf = (sum >> 32) != 0;
                vf = ((~(a ^ b) & (a ^ res)) & 0x80000000u) != 0;
            }
            r_[rd] = res;
            if (s) {
                set_nz(res);
                cpsr_ = (cpsr_ & ~(PSR_C | PSR_V)) | (cf ? PSR_C : 0) | (vf ? PSR_V : 0);
            }
        }
        return true;
    }
    case 0x2:
    case 0x3: {
        /* MOV/CMP/ADD/SUB immediate(8) */
        u32 sub_op = (hw >> 11) & 3;
        u32 rd = (hw >> 8) & 7;
        u32 imm = hw & 0xFF;
        u32 a = r_[rd], res;
        bool cf = (cpsr_ & PSR_C) != 0, vf = (cpsr_ & PSR_V) != 0;
        switch (sub_op) {
        case 0: /* MOV */
            r_[rd] = imm;
            if (s)
                set_nz(imm);
            return true;
        case 1: /* CMP */
            res = a - imm;
            cf = a >= imm;
            vf = (((a ^ imm) & (a ^ res)) & 0x80000000u) != 0;
            set_nz(res);
            cpsr_ = (cpsr_ & ~(PSR_C | PSR_V)) | (cf ? PSR_C : 0) | (vf ? PSR_V : 0);
            return true;
        case 2: { /* ADD */
            u64 sum = (u64)a + imm;
            res = (u32)sum;
            cf = (sum >> 32) != 0;
            vf = ((~(a ^ imm) & (a ^ res)) & 0x80000000u) != 0;
            r_[rd] = res;
            if (s) {
                set_nz(res);
                cpsr_ = (cpsr_ & ~(PSR_C | PSR_V)) | (cf ? PSR_C : 0) | (vf ? PSR_V : 0);
            }
            return true;
        }
        default: /* SUB */
            res = a - imm;
            cf = a >= imm;
            vf = (((a ^ imm) & (a ^ res)) & 0x80000000u) != 0;
            r_[rd] = res;
            if (s) {
                set_nz(res);
                cpsr_ = (cpsr_ & ~(PSR_C | PSR_V)) | (cf ? PSR_C : 0) | (vf ? PSR_V : 0);
            }
            return true;
        }
    }
    case 0x4: {
        if ((hw & 0x0400) == 0 && (hw & 0x0800) == 0) {
            /* data processing register (0100 00) */
            return thumb_dp_reg(hw, s);
        }
        if ((hw & 0x0C00) == 0x0400) {
            /* special data / BX/BLX (0100 01) */
            u32 opc = (hw >> 8) & 3;
            u32 rm = (hw >> 3) & 0xF;
            u32 rd = ((hw & 0x80) >> 4) | (hw & 7);
            if (opc == 3) {
                /* BX / BLX register: bits 2:0 SBZ (xx100 = BXNS/BLXNS, UNDEF) */
                if (hw & 7)
                    return false;
                bool link = (hw >> 7) & 1;
                u32 target = r_[rm];
                if (link)
                    r_[14] = (pc + 2) | 1;
                branch_interwork(target);
                return true;
            }
            u32 a = r_[rd], b = r_[rm];
            if (opc == 0) { /* ADD (no flags) */
                u32 res = a + b;
                if (rd == 15)
                    branch_same(res);
                else
                    r_[rd] = res;
            } else if (opc == 1) { /* CMP (flags) */
                u32 res = a - b;
                bool cf = a >= b;
                bool vf = (((a ^ b) & (a ^ res)) & 0x80000000u) != 0;
                set_nz(res);
                cpsr_ = (cpsr_ & ~(PSR_C | PSR_V)) | (cf ? PSR_C : 0) | (vf ? PSR_V : 0);
            } else { /* MOV (no flags) */
                if (rd == 15)
                    branch_same(b);
                else
                    r_[rd] = b;
            }
            return true;
        }
        /* LDR literal (0100 1) */
        u32 rd = (hw >> 8) & 7;
        u32 imm = (hw & 0xFF) << 2;
        u32 addr = ((pc + 4) & ~3u) + imm;
        u32 val;
        if (!mem_load(addr, 4, val, false))
            return true;
        r_[rd] = val;
        return true;
    }
    case 0x5: {
        /* load/store register offset */
        u32 opc = (hw >> 9) & 7;
        u32 rm = (hw >> 6) & 7;
        u32 rn = (hw >> 3) & 7;
        u32 rd = hw & 7;
        u32 addr = r_[rn] + r_[rm];
        u32 val;
        switch (opc) {
        case 0: /* STR */
            mem_store(addr, 4, r_[rd]);
            break;
        case 1: /* STRH */
            mem_store(addr, 2, r_[rd]);
            break;
        case 2: /* STRB */
            mem_store(addr, 1, r_[rd]);
            break;
        case 3: /* LDRSB */
            if (mem_load(addr, 1, val, true))
                r_[rd] = val;
            break;
        case 4: /* LDR */
            if (mem_load(addr, 4, val, false))
                r_[rd] = val;
            break;
        case 5: /* LDRH */
            if (mem_load(addr, 2, val, false))
                r_[rd] = val;
            break;
        case 6: /* LDRB */
            if (mem_load(addr, 1, val, false))
                r_[rd] = val;
            break;
        case 7: /* LDRSH */
            if (mem_load(addr, 2, val, true))
                r_[rd] = val;
            break;
        }
        return true;
    }
    case 0x6:
    case 0x7: {
        /* LDR/STR immediate (word / byte) */
        bool load = (hw >> 11) & 1;
        bool byte = (op == 0x7);
        u32 imm5 = (hw >> 6) & 0x1F;
        u32 rn = (hw >> 3) & 7;
        u32 rd = hw & 7;
        u32 addr = r_[rn] + (byte ? imm5 : (imm5 << 2));
        u32 val;
        if (load) {
            if (mem_load(addr, byte ? 1 : 4, val, false))
                r_[rd] = val;
        } else {
            mem_store(addr, byte ? 1 : 4, r_[rd]);
        }
        return true;
    }
    case 0x8: {
        /* LDRH/STRH immediate */
        bool load = (hw >> 11) & 1;
        u32 imm5 = (hw >> 6) & 0x1F;
        u32 rn = (hw >> 3) & 7;
        u32 rd = hw & 7;
        u32 addr = r_[rn] + (imm5 << 1);
        u32 val;
        if (load) {
            if (mem_load(addr, 2, val, false))
                r_[rd] = val;
        } else {
            mem_store(addr, 2, r_[rd]);
        }
        return true;
    }
    case 0x9: {
        /* LDR/STR SP-relative */
        bool load = (hw >> 11) & 1;
        u32 rd = (hw >> 8) & 7;
        u32 imm = (hw & 0xFF) << 2;
        u32 addr = r_[13] + imm;
        u32 val;
        if (load) {
            if (mem_load(addr, 4, val, false))
                r_[rd] = val;
        } else {
            mem_store(addr, 4, r_[rd]);
        }
        return true;
    }
    case 0xA: {
        /* ADR / ADD SP */
        u32 rd = (hw >> 8) & 7;
        u32 imm = (hw & 0xFF) << 2;
        if ((hw >> 11) & 1)
            r_[rd] = r_[13] + imm;
        else
            r_[rd] = ((pc + 4) & ~3u) + imm;
        return true;
    }
    case 0xB:
        return thumb_misc(hw, pc, in_it);
    case 0xC: {
        /* LDMIA / STMIA: writeback unless an LDM loads its base; the empty
         * list is UNDEF */
        u32 rn = (hw >> 8) & 7;
        u32 list = hw & 0xFF;
        if ((hw >> 11) & 1)
            return op_ldm(rn, list, !(list & (1u << rn)), false, true, false, 1);
        return op_stm(rn, list, true, false, true, false, 1);
    }
    case 0xD: {
        u32 cond = (hw >> 8) & 0xF;
        if (cond == 0xF) {
            /* SVC: the hook sees the IT state already advanced */
            if (in_it) {
                it_advance();
                it_set_ = true;
            }
            if (on_swi)
                on_swi(watch_ctx);
            u32 c = cpsr_;
            u32 npc = pc + 2;
            switch_mode(MODE_SVC);
            cpsr_ = (c & ~0x3Fu) | 0x13 | 0x80;
            cpsr_ &= ~PSR_T; /* SVC entry is ARM state */
            set_spsr(c);
            r_[14] = npc;
            r_[15] = vector_base + 0x08;
            branched_ = true;
            return true;
        }
        if (cond == 0xE || in_it) {
            return false; /* UDF; a conditional branch inside an IT block */
        }
        /* conditional branch */
        if (cond_passed(cond)) {
            s32 off = (s32)(s8)(hw & 0xFF);
            branch_same((pc + 4) + (u32)(off << 1));
        }
        return true;
    }
    case 0xE: {
        /* unconditional branch (16-bit) */
        u32 imm11 = hw & 0x7FF;
        s32 off = (imm11 & 0x400) ? (s32)(imm11 | 0xFFFFF800u) : (s32)imm11;
        branch_same((pc + 4) + (u32)(off << 1));
        if (in_it) /* QEMU loses the IT state over a direct branch (gen_goto_tb) */
            cpsr_ &= ~0x0600FC00u;
        return true;
    }
    default:
        return false;
    }
}

bool ArmCpu::thumb_dp_reg(u32 hw, bool s)
{
    u32 opc = (hw >> 6) & 0xF;
    u32 rm = (hw >> 3) & 7;
    u32 rd = hw & 7;
    u32 a = r_[rd], b = r_[rm], res;
    bool c = (cpsr_ & PSR_C) != 0, v = (cpsr_ & PSR_V) != 0;
    bool wrote = true, logical = false, usec = false;
    /* inside an IT block only TST/CMP/CMN (always) touch the flags */
    if (!s && opc != 0x8 && opc != 0xA && opc != 0xB) {
        switch (opc) {
        case 0x0:
            res = a & b;
            break;
        case 0x1:
            res = a ^ b;
            break;
        case 0x2:
        case 0x3:
        case 0x4:
        case 0x7:
            res = shift_c(opc == 0x7 ? 3 : opc - 2, a, b & 0xFF, c, false);
            break;
        case 0x5:
            res = a + b + (c ? 1 : 0);
            break;
        case 0x6:
            res = a - b - (c ? 0 : 1);
            break;
        case 0x9:
            res = 0 - b;
            break;
        case 0xC:
            res = a | b;
            break;
        case 0xD:
            res = a * b;
            break;
        case 0xE:
            res = a & ~b;
            break;
        default:
            res = ~b;
            break;
        }
        r_[rd] = res;
        return true;
    }
    switch (opc) {
    case 0x0:
        res = a & b;
        logical = true;
        break;
    case 0x1:
        res = a ^ b;
        logical = true;
        break;
    case 0x2:
        res = shift_c(0, a, b & 0xFF, c, false);
        logical = true;
        usec = true;
        break;
    case 0x3:
        res = shift_c(1, a, b & 0xFF, c, false);
        logical = true;
        usec = true;
        break;
    case 0x4:
        res = shift_c(2, a, b & 0xFF, c, false);
        logical = true;
        usec = true;
        break;
    case 0x5: {
        u64 s = (u64)a + b + (c ? 1 : 0);
        res = (u32)s;
        c = (s >> 32) != 0;
        v = ((~(a ^ b) & (a ^ res)) & 0x80000000u) != 0;
        set_nz(res);
        cpsr_ = (cpsr_ & ~(PSR_C | PSR_V)) | (c ? PSR_C : 0) | (v ? PSR_V : 0);
        r_[rd] = res;
        return true;
    }
    case 0x6: {
        u64 d = (u64)a - b - (c ? 0 : 1);
        res = (u32)d;
        c = (d >> 32) == 0;
        v = (((a ^ b) & (a ^ res)) & 0x80000000u) != 0;
        set_nz(res);
        cpsr_ = (cpsr_ & ~(PSR_C | PSR_V)) | (c ? PSR_C : 0) | (v ? PSR_V : 0);
        r_[rd] = res;
        return true;
    }
    case 0x7:
        res = shift_c(3, a, b & 0xFF, c, false);
        logical = true;
        usec = true;
        break;
    case 0x8:
        res = a & b;
        logical = true;
        wrote = false;
        break;
    case 0x9: /* RSB #0 (NEG) */
        res = 0 - b;
        c = (0u >= b);
        v = ((b & res) & 0x80000000u) != 0;
        set_nz(res);
        cpsr_ = (cpsr_ & ~(PSR_C | PSR_V)) | (c ? PSR_C : 0) | (v ? PSR_V : 0);
        r_[rd] = res;
        return true;
    case 0xA: /* CMP */
        res = a - b;
        c = a >= b;
        v = (((a ^ b) & (a ^ res)) & 0x80000000u) != 0;
        set_nz(res);
        cpsr_ = (cpsr_ & ~(PSR_C | PSR_V)) | (c ? PSR_C : 0) | (v ? PSR_V : 0);
        return true;
    case 0xB: { /* CMN */
        u64 s = (u64)a + b;
        res = (u32)s;
        c = (s >> 32) != 0;
        v = ((~(a ^ b) & (a ^ res)) & 0x80000000u) != 0;
        set_nz(res);
        cpsr_ = (cpsr_ & ~(PSR_C | PSR_V)) | (c ? PSR_C : 0) | (v ? PSR_V : 0);
        return true;
    }
    case 0xC:
        res = a | b;
        logical = true;
        break;
    case 0xD: { /* MUL */
        res = a * b;
        set_nz(res);
        r_[rd] = res;
        return true;
    }
    case 0xE:
        res = a & ~b;
        logical = true;
        break;
    default: /* 0xF MVN */
        res = ~b;
        logical = true;
        break;
    }
    if (wrote)
        r_[rd] = res;
    set_nz(res);
    if (logical) {
        if (usec)
            cpsr_ = (cpsr_ & ~PSR_C) | (c ? PSR_C : 0);
    }
    return true;
}

bool ArmCpu::thumb_misc(u32 hw, u32 pc, bool in_it)
{
    u32 top = (hw >> 8) & 0xF;
    if (top == 0x0) {
        /* ADD/SUB SP immediate */
        u32 imm = (hw & 0x7F) << 2;
        if (hw & 0x80)
            r_[13] -= imm;
        else
            r_[13] += imm;
        return true;
    }
    if ((hw & 0xF600) == 0xB400) {
        /* PUSH = STMDB sp!, POP = LDMIA sp! (bit 8 adds LR / PC); an empty
         * list is UNDEF */
        if ((hw >> 11) & 1)
            return op_ldm(13, (hw & 0xFF) | ((hw & 0x100) << 7), true, false, true, false, 1);
        return op_stm(13, (hw & 0xFF) | ((hw & 0x100) << 6), true, true, false, false, 1);
    }
    switch (top) {
    case 0x1:
    case 0x3:
    case 0x9:
    case 0xB: {
        /* CBZ / CBNZ */
        u32 rn = hw & 7;
        bool nonzero = (hw >> 11) & 1;
        u32 imm = (((hw >> 9) & 1) << 6) | (((hw >> 3) & 0x1F) << 1);
        bool taken = nonzero ? (r_[rn] != 0) : (r_[rn] == 0);
        if (taken)
        {
            branch_same((pc + 4) + imm);
            if (in_it) /* see the 16-bit B */
                cpsr_ &= ~0x0600FC00u;
        }
        return true;
    }
    case 0x2: {
        /* SXTH/SXTB/UXTH/UXTB */
        u32 kind = (hw >> 6) & 3;
        u32 rm = (hw >> 3) & 7;
        u32 rd = hw & 7;
        u32 v = r_[rm];
        switch (kind) {
        case 0:
            r_[rd] = (u32)(s32)(s16)(u16)v;
            break;
        case 1:
            r_[rd] = (u32)(s32)(s8)(u8)v;
            break;
        case 2:
            r_[rd] = v & 0xFFFF;
            break;
        default:
            r_[rd] = v & 0xFF;
            break;
        }
        return true;
    }
    case 0xA: {
        /* REV / REV16 / REVSH */
        u32 kind = (hw >> 6) & 3;
        u32 rm = (hw >> 3) & 7;
        u32 rd = hw & 7;
        u32 v = r_[rm];
        if (kind == 0)
            r_[rd] = ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
        else if (kind == 1)
            r_[rd] = ((v >> 8) & 0x00FF00FF) | ((v << 8) & 0xFF00FF00);
        else if (kind == 3) {
            u32 t = ((v << 8) & 0xFF00) | ((v >> 8) & 0xFF);
            r_[rd] = (u32)(s32)(s16)(u16)t;
        } else {
            return false; /* 1011 1010 10: HLT (UNDEF without semihosting) */
        }
        return true;
    }
    case 0x6:
        if ((hw & 0xF7) == 0x50) { /* SETEND: only CPSR.E */
            cpsr_ = (cpsr_ & ~PSR_E) | ((hw & 8) ? PSR_E : 0);
            return true;
        }
        if ((hw & 0xE8) == 0x60) { /* CPSIE/CPSID {a,i,f} (no mode change) */
            op_cps((hw & 0x10) ? 3 : 2, false, (hw & 7) << 6, 0);
            return true;
        }
        return false;
    case 0xE:
        /* BKPT: EXCP_BKPT goes to the hook, which ignores it: PC stays */
        stall(pc);
        return true;
    case 0xF:
        if (hw & 0xF) { /* IT: ITSTATE = firstcond:mask */
            cpsr_ = (cpsr_ & ~0x0600FC00u) | ((hw & 0xFC) << 8) | ((hw & 3) << 25);
            it_set_ = true;
            return true;
        }
        return hint((hw >> 4) & 0xF, pc + 2); /* NOP/YIELD/WFE/WFI/SEV */
    default:
        return false; /* 1011 0111, 1011 1000 */
    }
}
