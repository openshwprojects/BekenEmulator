/* 32-bit Thumb-2 (T32) decode/execute for ArmCpu. The Beken firmware is mostly
 * ARMv5 Thumb (whose only 32-bit encoding is BL/BLX), but Unicorn's A15 decodes
 * the whole T32 space: this is a port of QEMU 5.0's t32.decode tree (accept
 * set and semantics), sharing the operation helpers of arm_exec.cpp. */
#include "arm_cpu.h"

#include <string.h>

/* ThumbExpandImm with carry (ARM ARM A6.3.2). */
u32 ArmCpu::thumb_expand_imm(u32 imm12, bool &carry) const
{
    if ((imm12 & 0xC00) == 0) {
        u32 b = (imm12 >> 8) & 3;
        u32 v = imm12 & 0xFF;
        switch (b) {
        case 0:
            return v;
        case 1:
            return (v << 16) | v;
        case 2:
            return (v << 24) | (v << 8);
        default:
            return (v << 24) | (v << 16) | (v << 8) | v;
        }
    }
    u32 unrot = 0x80 | (imm12 & 0x7F);
    u32 rot = (imm12 >> 7) & 0x1F;
    u32 res = (unrot >> rot) | (unrot << (32 - rot));
    carry = (res & 0x80000000u) != 0;
    return res;
}

/* T32 decode, a port of QEMU 5.0's t32.decode tree (Unicorn 2.1.4). */
bool ArmCpu::exec_thumb32(u32 insn, u32 pc, bool in_it)
{
    if (__builtin_expect((insn & 0xF800C000u) == 0xF000C000u, 1)) {
        u32 s = (insn >> 26) & 1;
        u32 j1 = (insn >> 13) & 1, j2 = (insn >> 11) & 1;
        u32 i1 = (~(j1 ^ s)) & 1, i2 = (~(j2 ^ s)) & 1;
        s32 off = (s32)((s << 24) | (i1 << 23) | (i2 << 22) | (((insn >> 16) & 0x3FF) << 12) | ((insn & 0x7FF) << 1));
        off = (off << 7) >> 7;
        if (!(insn & 0x1000)) { /* BLX (to ARM): H bit (imm bit 1) must be 0 */
            if (insn & 1)
                return false;
            r_[14] = (pc + 4) | 1;
            cpsr_ &= ~PSR_T;
            r_[15] = ((pc + 4) & ~3u) + (u32)off;
        } else { /* BL */
            r_[14] = (pc + 4) | 1;
            r_[15] = pc + 4 + (u32)off;
        }
        branched_ = true;
        if (in_it)
            cpsr_ &= ~0x0600FC00u;
        return true;
    }
    switch ((insn >> 27) & 0x1F) {
    case 0x1D:
        return t32_group_e8(insn, pc);
    case 0x1E:
        return t32_group_f0(insn, pc, in_it);
    default:
        return t32_group_f8(insn, pc);
    }
}

/* Data processing core for the T32 shifted-register / immediate forms:
 * QEMU op_s_rrr_shi / op_s_rri_rot + store_reg_kind in Thumb (no
 * interworking; SUBS/MOVS PC are exception returns). opc: A32 numbering
 * (8-11 = TST/TEQ/CMP/CMN), 16 = ORN. */
bool ArmCpu::t32_alu(u32 opc, bool s, u32 rd, u32 a, u32 b, bool shc)
{
    u32 res;
    bool c = (cpsr_ & PSR_C) != 0, v = (cpsr_ & PSR_V) != 0, logical = false, store = true;
    switch (opc) {
    case 0:
    case 8:
        res = a & b;
        logical = true;
        store = opc == 0;
        break;
    case 1:
    case 9:
        res = a ^ b;
        logical = true;
        store = opc == 1;
        break;
    case 2:
    case 10:
        res = a - b;
        c = a >= b;
        v = (((a ^ b) & (a ^ res)) >> 31) != 0;
        store = opc == 2;
        break;
    case 3:
        res = b - a;
        c = b >= a;
        v = (((b ^ a) & (b ^ res)) >> 31) != 0;
        break;
    case 4:
    case 11: {
        u64 sum = (u64)a + b;
        res = (u32)sum;
        c = (sum >> 32) != 0;
        v = ((~(a ^ b) & (a ^ res)) >> 31) != 0;
        store = opc == 4;
        break;
    }
    case 5: {
        u64 sum = (u64)a + b + (c ? 1 : 0);
        res = (u32)sum;
        c = (sum >> 32) != 0;
        v = ((~(a ^ b) & (a ^ res)) >> 31) != 0;
        break;
    }
    case 6: {
        u64 d = (u64)a - b - (c ? 0 : 1);
        res = (u32)d;
        c = (d >> 32) == 0;
        v = (((a ^ b) & (a ^ res)) >> 31) != 0;
        break;
    }
    case 12:
        res = a | b;
        logical = true;
        break;
    case 13:
        res = b;
        logical = true;
        break;
    case 14:
        res = a & ~b;
        logical = true;
        break;
    case 15:
        res = ~b;
        logical = true;
        break;
    default:
        res = a | ~b;
        logical = true;
        break;
    }
    if (store && rd == 15 && s && (opc == 2 || opc == 13)) {
        if (cur_mode() == MODE_USR)
            return false;
        exception_return(res, get_spsr());
        return true;
    }
    if (s) {
        set_nz(res);
        if (logical)
            cpsr_ = (cpsr_ & ~PSR_C) | (shc ? PSR_C : 0);
        else
            cpsr_ = (cpsr_ & ~(PSR_C | PSR_V)) | (c ? PSR_C : 0) | (v ? PSR_V : 0);
    }
    if (store)
        wreg(rd, res);
    return true;
}

/* QEMU op_load_ri / op_store_ri (and the p=1 w=0 _rr form) in Thumb: an
 * immediate offset from PC uses Align(PC, 4); writeback before the loaded
 * value; a load to PC interworks. */
bool ArmCpu::t32_ldst(bool load, int size, bool sign, u32 rt, u32 rn, u32 off, bool rr, bool p, bool u, bool w)
{
    u32 addr;
    if (rr)
        addr = r_[rn] + off;
    else {
        u32 base = rn == 15 ? (r_[15] & ~3u) : r_[rn];
        addr = p ? (u ? base + off : base - off) : base;
    }
    bool wb = !rr && (!p || w);
    u32 wbv = p ? addr : (u ? addr + off : addr - off);
    if (load) {
        u32 v;
        if (!mem_load(addr, size, v, sign))
            return true;
        if (wb)
            wreg(rn, wbv);
        if (rt == 15)
            branch_interwork(v);
        else
            r_[rt] = v;
    } else {
        if (!mem_store(addr, size, r_[rt]))
            return true;
        if (wb)
            wreg(rn, wbv);
    }
    return true;
}

/* QEMU op_ldrd_ri / op_strd_ri (T32: any registers, no UNDEF cases) */
bool ArmCpu::t32_ldrd(bool load, u32 rt, u32 rt2, u32 rn, u32 imm, bool p, bool u, bool w)
{
    u32 base = rn == 15 ? (r_[15] & ~3u) : r_[rn];
    u32 addr = p ? (u ? base + imm : base - imm) : base;
    if (load) {
        u32 lo, hi;
        if (!mem_load(addr, 4, lo, false))
            return true;
        wreg(rt, lo);
        if (!mem_load(addr + 4, 4, hi, false))
            return true;
        wreg(rt2, hi);
    } else {
        if (!mem_store(addr, 4, r_[rt]))
            return true;
        if (!mem_store(addr + 4, 4, r_[rt2]))
            return true;
    }
    if (!p || w)
        wreg(rn, p ? addr : (u ? addr + imm : addr - imm));
    return true;
}

/* 11101: load/store multiple, dual, exclusive, table branch, data
 * processing (shifted register), coprocessor */
bool ArmCpu::t32_group_e8(u32 insn, u32 pc)
{
    u32 rn = (insn >> 16) & 0xF;
    u32 rt = (insn >> 12) & 0xF;
    u32 r8 = (insn >> 8) & 0xF;
    u32 r0 = insn & 0xF;
    bool w = (insn >> 21) & 1;
    switch (insn & 0x07400000u) {
    case 0x00000000u:   /* 11101000 .0: SRS DB, RFE DB, STM IA, LDM IA */
    case 0x01000000u: { /* 11101001 .0: STMDB, LDMDB, SRS IA, RFE IA */
        bool e9 = (insn & 0x01000000u) != 0;
        bool rfe_srs = ((insn & 0x00800000u) != 0) == e9;
        bool l = (insn >> 20) & 1;
        if (rfe_srs) {
            u32 pu = e9 ? 1 : 2;
            if (l)
                return (insn & 0xFFFF) == 0xC000 ? op_rfe(rn, pu, w) : false;
            return ((insn >> 5) & 0x7FFF) == 0x6E00 ? op_srs(insn & 0x1F, pu, w) : false;
        }
        /* T32: writeback with the base in the list is UNDEF, <2 registers too */
        if (w && (insn & (1u << rn)))
            return false;
        if (l)
            return op_ldm(rn, insn & 0xFFFF, w, e9, !e9, false, 2);
        return op_stm(rn, insn & 0xFFFF, w, e9, !e9, false, 2);
    }
    case 0x00400000u: /* 11101000 .1: exclusives, TBB/TBH, LDRD/STRD post-index */
        switch ((insn >> 20) & 3) {
        case 0:
            if (!(insn & (1u << 23))) /* STREX Rd, Rt, [Rn, #imm8*4] */
                return op_strex(r8, rt, 15, rn, (insn & 0xFF) * 4, 4, pc);
            switch ((insn >> 4) & 0xF) {
            case 4:
            case 5:
                if (r8 != 0xF)
                    return false;
                return op_strex(r0, rt, 15, rn, 0, ((insn >> 4) & 0xF) == 4 ? 1 : 2, pc);
            case 7:
                return op_strex(r0, rt, r8, rn, 0, 8, pc);
            default:
                return false; /* STL/STLEX (v8) */
            }
        case 1:
            if (!(insn & (1u << 23))) { /* LDREX Rt, [Rn, #imm8*4] */
                if (r8 != 0xF)
                    return false;
                return op_ldrex(rt, 15, rn, (insn & 0xFF) * 4, 4, pc);
            }
            switch ((insn >> 4) & 0xF) {
            case 0:
            case 1: { /* TBB / TBH */
                if (((insn >> 8) & 0xFF) != 0xF0)
                    return false;
                bool half = (insn >> 4) & 1;
                u32 idx = r_[r0];
                if (half)
                    idx += idx;
                u32 v;
                if (!mem_load(r_[rn] + idx, half ? 2 : 1, v, false))
                    return true;
                wreg(15, r_[15] + v + v);
                return true;
            }
            case 4:
            case 5:
                if ((insn & 0xF0F) != 0xF0F)
                    return false;
                return op_ldrex(rt, 15, rn, 0, ((insn >> 4) & 0xF) == 4 ? 1 : 2, pc);
            case 7:
                if (r0 != 0xF)
                    return false;
                return op_ldrex(rt, r8, rn, 0, 8, pc);
            default:
                return false; /* LDA/LDAEX (v8) */
            }
        default: /* STRD/LDRD post-indexed */
            return t32_ldrd((insn >> 20) & 1, rt, r8, rn, (insn & 0xFF) * 4, false, (insn >> 23) & 1, true);
        }
    case 0x01400000u: /* 11101001 .1: LDRD/STRD offset / pre-indexed */
        return t32_ldrd((insn >> 20) & 1, rt, r8, rn, (insn & 0xFF) * 4, true, (insn >> 23) & 1, (insn >> 21) & 1);
    case 0x02000000u:
    case 0x02400000u:
    case 0x03000000u:
    case 0x03400000u: { /* data processing (shifted register) */
        if (insn & 0x8000)
            return false;
        u32 op = (insn >> 21) & 0xF;
        bool s = (insn >> 20) & 1;
        u32 shim = ((insn >> 10) & 0x1C) | ((insn >> 6) & 3);
        if (op == 6) { /* PKH */
            if (insn & 0x00100010u)
                return false;
            op_pkh(r8, rn, r0, shim, (insn >> 5) & 1);
            return true;
        }
        u32 opc;
        switch (op) {
        case 0:
            opc = (s && r8 == 15) ? 8 : 0;
            break;
        case 1:
            opc = 14;
            break;
        case 2:
            opc = rn == 15 ? 13 : 12;
            break;
        case 3:
            opc = rn == 15 ? 15 : 16;
            break;
        case 4:
            opc = (s && r8 == 15) ? 9 : 1;
            break;
        case 8:
            opc = (s && r8 == 15) ? 11 : 4;
            break;
        case 10:
            opc = 5;
            break;
        case 11:
            opc = 6;
            break;
        case 13:
            opc = (s && r8 == 15) ? 10 : 2;
            break;
        case 14:
            opc = 3;
            break;
        default:
            return false;
        }
        bool c = (cpsr_ & PSR_C) != 0;
        u32 b = shift_c((insn >> 4) & 3, r_[r0], shim, c, true);
        return t32_alu(opc, s, r8, r_[rn], b, c);
    }
    default: /* 1110 11xx: coprocessor (0xEF = Advanced SIMD: UNDEF) */
        if ((insn & 0x0F000000u) == 0x0F000000u)
            return false;
        return exec_coproc(insn);
    }
}

/* 11110: data processing (modified / plain immediate), branches, misc control */
bool ArmCpu::t32_group_f0(u32 insn, u32 pc, bool in_it)
{
    u32 rn = (insn >> 16) & 0xF;
    u32 rd = (insn >> 8) & 0xF;
    if (!(insn & 0x8000)) {
        u32 i12 = ((insn >> 15) & 0x800) | ((insn >> 4) & 0x700) | (insn & 0xFF);
        u32 sel = (insn >> 22) & 0xF;
        if (sel < 8) { /* modified immediate */
            u32 op = (insn >> 21) & 0xF;
            bool s = (insn >> 20) & 1;
            u32 opc;
            switch (op) {
            case 0:
                opc = (s && rd == 15) ? 8 : 0;
                break;
            case 1:
                opc = 14;
                break;
            case 2:
                opc = rn == 15 ? 13 : 12;
                break;
            case 3:
                opc = rn == 15 ? 15 : 16;
                break;
            case 4:
                opc = (s && rd == 15) ? 9 : 1;
                break;
            case 8:
                opc = (s && rd == 15) ? 11 : 4;
                break;
            case 10:
                opc = 5;
                break;
            case 11:
                opc = 6;
                break;
            case 13:
                opc = (s && rd == 15) ? 10 : 2;
                break;
            case 14:
                opc = 3;
                break;
            default:
                return false;
            }
            bool c = (cpsr_ & PSR_C) != 0;
            u32 imm = thumb_expand_imm(i12, c);
            return t32_alu(opc, s, rd, r_[rn], imm, c);
        }
        u32 sub = (insn >> 20) & 3;
        switch (sel) {
        case 0x8: /* ADDW / ADR (add) */
        case 0xA: { /* SUBW / ADR (sub) */
            if (sub != (sel == 0x8 ? 0u : 2u))
                return false;
            if (rn == 15) { /* ADR: store_reg_bx (v7: BX semantics for PC) */
                u32 v = sel == 0x8 ? (r_[15] & ~3u) + i12 : (r_[15] & ~3u) - i12;
                if (rd == 15)
                    branch_interwork(v);
                else
                    r_[rd] = v;
                return true;
            }
            wreg(rd, sel == 0x8 ? r_[rn] + i12 : r_[rn] - i12);
            return true;
        }
        case 0x9: /* MOVW */
        case 0xB: { /* MOVT */
            if (sub)
                return false;
            u32 imm16 = (rn << 12) | i12;
            wreg(rd, sel == 0x9 ? imm16 : ((r_[rd] & 0xFFFF) | (imm16 << 16)));
            return true;
        }
        case 0xC: /* SSAT / SSAT16 */
        case 0xE: { /* USAT / USAT16 */
            if (insn & 0x04100020u)
                return false;
            if ((insn & 0x002070C0u) == 0x00200000u)
                op_sat(rd, rn, 0, false, insn & 0x1F, sel == 0xC ? 2 : 3);
            else
                op_sat(rd, rn, ((insn >> 10) & 0x1C) | ((insn >> 6) & 3), (insn >> 21) & 1, insn & 0x1F,
                       sel == 0xC ? 0 : 1);
            return true;
        }
        case 0xD:
        case 0xF: { /* SBFX / BFI,BFC / UBFX */
            u32 lsb = ((insn >> 10) & 0x1C) | ((insn >> 6) & 3);
            u32 k = insn & 0x04300020u;
            if (sel == 0xD && k == 0)
                return op_bfx(rd, rn, lsb, insn & 0x1F, false);
            if (sel == 0xD && k == 0x00200000u)
                return op_bfci(rd, rn, lsb, insn & 0x1F);
            if (sel == 0xF && k == 0)
                return op_bfx(rd, rn, lsb, insn & 0x1F, true);
            return false;
        }
        default:
            return false;
        }
    }

    /* branches and miscellaneous control */
    u32 s = (insn >> 26) & 1;
    u32 j1 = (insn >> 13) & 1, j2 = (insn >> 11) & 1;
    switch (insn & 0x5000) {
    case 0x0000: {
        if ((insn & 0x03800000u) == 0x03800000u) {
            if ((insn & 0x047F2F00u) == 0x002F0000u) /* hints */
                return hint(insn & 0xFF, pc + 4);
            if ((insn & 0x047F2800u) == 0x002F0000u) { /* CPS */
                op_cps((insn >> 9) & 3, (insn >> 8) & 1, (insn & 0xE0) << 1, insn & 0x1F);
                return true;
            }
            if ((insn & 0x047F2F80u) == 0x003F0F00u) { /* CLREX / DSB / DMB / ISB */
                u32 o = insn & 0x7F;
                if (o == 0x2F) {
                    excl_valid_ = false;
                    return true;
                }
                if ((o & 0x70) == 0x40 || (o & 0x70) == 0x50 || (o & 0x70) == 0x60)
                    return true;
            }
            if ((insn & 0x04602000u) == 0x00600000u) { /* MRS */
                if ((insn & 0xEF) == 0x20)
                    return op_mrs_banked((insn >> 20) & 1, ((insn & 0x10) ? 0x10 : 0) | rn, rd);
                if ((insn & 0x000F00FFu) == 0x000F0000u) {
                    bool r = (insn >> 20) & 1;
                    if (r && cur_mode() == MODE_USR)
                        return false;
                    wreg(rd, mrs_value(r));
                    return true;
                }
            }
            if ((insn & 0x04602000u) == 0x00000000u) { /* MSR */
                if ((insn & 0xEF) == 0x20)
                    return op_msr_banked((insn >> 20) & 1, ((insn & 0x10) ? 0x10 : 0) | rd, rn);
                if ((insn & 0xFF) == 0)
                    return op_msr(r_[rn], rd, (insn >> 20) & 1);
            }
            if ((insn & 0x04702FFFu) == 0x00400F00u) { /* BXJ: a plain BX */
                branch_interwork(r_[rn]);
                return true;
            }
            if ((insn & 0x047F2F00u) == 0x005E0F00u) { /* ERET / SUBS PC, LR, #imm8 */
                if (cur_mode() == MODE_USR)
                    return false;
                exception_return(r_[14] - (insn & 0xFF), get_spsr());
                return true;
            }
            if ((insn & 0x04702FFFu) == 0x04700000u) /* SMC: the hook ignores EXCP_SMC */
                return cur_mode() != MODE_USR;
            /* HVC, UDF and the rest: UNDEF (via B<cond> with cond 111x) */
        }
        u32 cond = (insn >> 22) & 0xF;
        if (cond >= 0xE || in_it)
            return false;
        if (cond_passed(cond)) {
            s32 off = (s32)((s << 20) | (j2 << 19) | (j1 << 18) | (((insn >> 16) & 0x3F) << 12) | ((insn & 0x7FF) << 1));
            off = (off << 11) >> 11;
            r_[15] = pc + 4 + (u32)off;
            branched_ = true;
        }
        return true;
    }
    default: { /* B.W / BLX / BL (branch24) */
        u32 i1 = (~(j1 ^ s)) & 1, i2 = (~(j2 ^ s)) & 1;
        s32 off = (s32)((s << 24) | (i1 << 23) | (i2 << 22) | (((insn >> 16) & 0x3FF) << 12) | ((insn & 0x7FF) << 1));
        off = (off << 7) >> 7;
        if ((insn & 0x5000) == 0x4000) { /* BLX (to ARM): H bit (imm bit 1) must be 0 */
            if (insn & 1)
                return false;
            r_[14] = (pc + 4) | 1;
            cpsr_ &= ~PSR_T;
            r_[15] = ((pc + 4) & ~3u) + (u32)off;
        } else {
            if (insn & 0x4000) /* BL */
                r_[14] = (pc + 4) | 1;
            r_[15] = pc + 4 + (u32)off;
        }
        branched_ = true;
        if (in_it) /* QEMU loses the IT state over a direct branch (gen_goto_tb) */
            cpsr_ &= ~0x0600FC00u;
        return true;
    }
    }
}

/* 11111: load/store single, data processing (register), multiplies,
 * coprocessor 0xFC-0xFF (UNDEF) */
bool ArmCpu::t32_group_f8(u32 insn, u32 pc)
{
    u32 rn = (insn >> 16) & 0xF;
    u32 rt = (insn >> 12) & 0xF;
    u32 r8 = (insn >> 8) & 0xF;
    u32 r0 = insn & 0xF;
    switch ((insn >> 24) & 7) {
    case 0:
    case 1: { /* 0xF8 / 0xF9: load/store single */
        u32 o = (insn >> 20) & 7; /* size:L */
        bool sgn = (insn >> 24) & 1;
        int size = (o >> 1) == 0 ? 1 : (o >> 1) == 1 ? 2 : 4;
        bool load = o & 1;
        if (o >= 6 || (sgn && (!load || size == 4)))
            return false;
        if (!load) {
            if (insn & (1u << 23))
                return t32_ldst(false, size, false, rt, rn, insn & 0xFFF, false, true, true, false);
            switch (insn & 0x900) {
            case 0x000:
                if (insn & 0x6C0)
                    return false;
                return t32_ldst(false, size, false, rt, rn, r_[r0] << ((insn >> 4) & 3), true, true, true, false);
            case 0x800:
                if (((insn >> 9) & 3) == 2) /* 1100: negative imm8 */
                    return t32_ldst(false, size, false, rt, rn, insn & 0xFF, false, true, false, false);
                if (((insn >> 9) & 3) == 3) /* 1110: unprivileged */
                    return t32_ldst(false, size, false, rt, rn, insn & 0xFF, false, true, true, false);
                return false;
            case 0x900: /* 1PUW with W = 1 */
                return t32_ldst(false, size, false, rt, rn, insn & 0xFF, false, (insn >> 10) & 1, (insn >> 9) & 1, true);
            default:
                return false;
            }
        }
        /* loads: Rt = 15 in the byte/halfword forms below is PLD/PLI (NOP) */
        bool hint_ok = size != 4;
        if (rn == 15) { /* literal */
            if (hint_ok && rt == 15)
                return true;
            return t32_ldst(true, size, sgn, rt, 15, insn & 0xFFF, false, true, (insn >> 23) & 1, false);
        }
        if (insn & (1u << 23)) {
            if (hint_ok && rt == 15)
                return true;
            return t32_ldst(true, size, sgn, rt, rn, insn & 0xFFF, false, true, true, false);
        }
        if ((insn & 0x900) == 0x900)
            return t32_ldst(true, size, sgn, rt, rn, insn & 0xFF, false, (insn >> 10) & 1, (insn >> 9) & 1, true);
        if ((insn & 0xF00) == 0xC00) {
            if (hint_ok && rt == 15)
                return true;
            return t32_ldst(true, size, sgn, rt, rn, insn & 0xFF, false, true, false, false);
        }
        if ((insn & 0xF00) == 0xE00)
            return t32_ldst(true, size, sgn, rt, rn, insn & 0xFF, false, true, true, false);
        if ((insn & 0xFC0) == 0x000) {
            if (hint_ok && rt == 15)
                return true;
            return t32_ldst(true, size, sgn, rt, rn, r_[r0] << ((insn >> 4) & 3), true, true, true, false);
        }
        return false;
    }
    case 2: { /* 0xFA: data processing (register) */
        u32 op = (insn >> 20) & 7;
        u32 b54 = (insn >> 4) & 3;
        switch (insn & 0x0080F0C0u) {
        case 0x0000F000u: { /* LSL/LSR/ASR/ROR (register): Rm 19:16 by Rs 3:0 */
            if (b54)
                return false;
            bool c = (cpsr_ & PSR_C) != 0;
            u32 v = shift_c((insn >> 21) & 3, r_[rn], r_[r0] & 0xFF, c, false);
            return t32_alu(13, (insn >> 20) & 1, r8, 0, v, c);
        }
        case 0x0000F080u: { /* SXTAH UXTAH SXTAB16 UXTAB16 SXTAB UXTAB */
            static const int k[8] = {1, 4, 2, 5, 0, 3, -1, -1};
            if (k[op] < 0)
                return false;
            op_xta(r8, rn, r0, b54, k[op]);
            return true;
        }
        case 0x0080F000u:   /* signed parallel add/sub */
        case 0x0080F040u: { /* unsigned */
            static const int pop[8] = {4, 0, 1, -1, 5, 3, 2, -1};
            if (pop[op] < 0 || b54 == 3)
                return false;
            op_par_addsub((insn & 0x40) ? 3 + b54 : b54, pop[op], r8, rn, r0);
            return true;
        }
        case 0x0080F080u:
            switch ((op << 2) | b54) {
            case 0x0:
            case 0x1:
            case 0x2:
            case 0x3: /* QADD QDADD QSUB QDSUB */
                op_qaddsub(r8, rn, r0, !(b54 & 2), b54 & 1);
                return true;
            case 0x4:
                op_rev(r8, r0, 0);
                return true;
            case 0x5:
                op_rev(r8, r0, 1);
                return true;
            case 0x6:
                op_rev(r8, r0, 3);
                return true;
            case 0x7:
                op_rev(r8, r0, 2);
                return true;
            case 0x8:
                op_sel(r8, rn, r0);
                return true;
            case 0xC: { /* CLZ */
                u32 v = r_[r0], n = 0;
                if (v == 0)
                    n = 32;
                else
                    while (!(v & 0x80000000u)) {
                        n++;
                        v <<= 1;
                    }
                wreg(r8, n);
                return true;
            }
            default:
                return false; /* CRC32 (v8) */
            }
        default:
            return false;
        }
    }
    case 3: { /* 0xFB: multiplies, divide */
        u32 ra = rt, rd = r8;
        bool b4 = (insn >> 4) & 1, b5 = (insn >> 5) & 1;
        switch (insn & 0x00F000F0u) {
        case 0x00000000u: /* MUL / MLA */
            op_mla(rd, rn, r0, ra, ra != 15, false);
            return true;
        case 0x00000010u: /* MLS */
            wreg(rd, r_[ra] - r_[rn] * r_[r0]);
            return true;
        case 0x00100000u:
        case 0x00100010u:
        case 0x00100020u:
        case 0x00100030u: /* SMULxy / SMLAxy */
            op_smlaxy(rd, ra, rn, r0, ra == 15 ? 0 : 1, b5, b4);
            return true;
        case 0x00200000u:
        case 0x00200010u: /* SMLAD{X} / SMUAD{X} */
            op_smlad(rd, ra, rn, r0, b4, false);
            return true;
        case 0x00300000u:
        case 0x00300010u: /* SMULWy / SMLAWy */
            op_smlawy(rd, ra, rn, r0, ra != 15, b4);
            return true;
        case 0x00400000u:
        case 0x00400010u: /* SMLSD{X} / SMUSD{X} */
            op_smlad(rd, ra, rn, r0, b4, true);
            return true;
        case 0x00500000u:
        case 0x00500010u: /* SMMLA{R} / SMMUL{R} */
            op_smmla(rd, ra, rn, r0, b4, false);
            return true;
        case 0x00600000u:
        case 0x00600010u: /* SMMLS{R} */
            op_smmla(rd, ra, rn, r0, b4, true);
            return true;
        case 0x00700000u: /* USADA8 / USAD8 */
            op_usada8(rd, ra, rn, r0);
            return true;
        case 0x00800000u: /* SMULL */
            op_mlal(ra, rd, rn, r0, false, false, false);
            return true;
        case 0x009000F0u: /* SDIV */
        case 0x00B000F0u: /* UDIV */
            if (ra != 15)
                return false;
            op_div(rd, rn, r0, (insn & 0x00200000u) != 0);
            return true;
        case 0x00A00000u: /* UMULL */
            op_mlal(ra, rd, rn, r0, true, false, false);
            return true;
        case 0x00C00000u: /* SMLAL */
            op_mlal(ra, rd, rn, r0, false, true, false);
            return true;
        case 0x00C00080u:
        case 0x00C00090u:
        case 0x00C000A0u:
        case 0x00C000B0u: /* SMLALxy */
            op_smlaxy(rd, ra, rn, r0, 2, b5, b4);
            return true;
        case 0x00C000C0u:
        case 0x00C000D0u: /* SMLALD{X} */
            op_smlald(ra, rd, rn, r0, b4, false);
            return true;
        case 0x00D000C0u:
        case 0x00D000D0u: /* SMLSLD{X} */
            op_smlald(ra, rd, rn, r0, b4, true);
            return true;
        case 0x00E00000u: /* UMLAL */
            op_mlal(ra, rd, rn, r0, true, true, false);
            return true;
        case 0x00E00060u: /* UMAAL */
            op_umaal(ra, rd, rn, r0);
            return true;
        default:
            return false;
        }
    }
    default: /* 0xFC-0xFF: coprocessor "2" forms and Advanced SIMD: UNDEF */
        return false;
    }
}

