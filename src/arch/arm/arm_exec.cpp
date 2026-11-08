/* ARM (A32) decode dispatch and the execute helpers for ArmCpu. Split out of
 * arm_cpu.cpp only for file size; these are the same class's members. */
#include "arch/arm/arm_cpu.h"
#include "arch/arm/arm_cpregs.h"

#include <string.h>

void ArmCpu::branch_interwork(u32 value)
{
    /* QEMU gen_bx: PC = value & ~1 in both states (an ARM target with bit 1
     * set really runs misaligned there), T = bit 0 */
    if (value & 1)
        cpsr_ |= PSR_T;
    else
        cpsr_ &= ~PSR_T;
    r_[15] = value & ~1u;
    branched_ = true;
}

void ArmCpu::branch_same(u32 value)
{
    r_[15] = thumb() ? (value & ~1u) : (value & ~3u);
    branched_ = true;
}

bool ArmCpu::step_arm()
{
    u32 pc = r_[15];
    bool ok;
    u32 insn = mem.fetch32(pc, ok);
    if (!ok) {
        /* Unicorn reports UC_MEM_FETCH_UNMAPPED to the unmapped hook (which the
         * emulator declines, like Python's hook_unmapped) and only retries the
         * fetch if the hook mapped the page. */
        if (mem.on_unmapped && mem.on_unmapped(mem.owner, &mem, pc, 4, Memory::UNMAPPED_FETCH))
            insn = mem.fetch32(pc, ok);
        if (!ok) {
            error_ = strf("Invalid memory fetch (UC_ERR_FETCH_UNMAPPED)");
            fault_addr_ = pc;
            return false;
        }
    }
    branched_ = false;
    r_[15] = pc + 8; /* pipeline value visible to the instruction */
    u32 cond = insn >> 28;
    if (cond != 0xF) {
        if (!cond_passed(cond)) {
            r_[15] = pc + 4;
            return true;
        }
    } else {
        /* cond == 0xF: only the unconditional encodings Unicorn's decoder
         * (QEMU a32-uncond, default Cortex-A15 model, no NEON/VFP enabled)
         * accepts; everything else - erased flash 0xFFFFFFFF included - is
         * UC_ERR_INSN_INVALID there. Verified encoding by encoding against
         * Unicorn (scratchpad auditA_nv.py). */
        bool okf = true;
        if ((insn & 0xFFF1FE20u) == 0xF1000000u) {
            /* CPS (a NOP in user mode) */
            op_cps((insn >> 18) & 3, (insn >> 17) & 1, insn & 0x1C0u, insn & 0x1Fu);
        } else if ((insn & 0xFFFFFDFFu) == 0xF1010000u) {
            /* SETEND: only the CPSR.E bit (data endianness is not modelled) */
            cpsr_ = (cpsr_ & ~(1u << 9)) | (insn & (1u << 9));
        } else if (insn == 0xF57FF01Fu) { /* CLREX */
            excl_valid_ = false;
        } else if ((insn & 0xFFFFFFF0u) == 0xF57FF040u    /* DSB */
                   || (insn & 0xFFFFFFF0u) == 0xF57FF050u /* DMB */
                   || (insn & 0xFFFFFFF0u) == 0xF57FF060u /* ISB */
                   || (insn & 0xFF30F000u) == 0xF510F000u /* PLD/PLDW imm */
                   || (insn & 0xFF70F000u) == 0xF450F000u /* PLI imm */
                   || (insn & 0xFF30F010u) == 0xF710F000u /* PLD/PLDW reg */
                   || (insn & 0xFF70F010u) == 0xF650F000u /* PLI reg */
                   || (insn & 0xFF700000u) == 0xF4100000u /* unallocated memory hint */
                   || (insn & 0xFF700010u) == 0xF6100000u /* (imm / reg): NOP with v7MP */) {
            /* NOP */
        } else if ((insn & 0xFE5FFFE0u) == 0xF84D0500u) { /* SRS */
            okf = op_srs(insn & 0x1F, (insn >> 23) & 3, (insn >> 21) & 1);
        } else if ((insn & 0xFE50FFFFu) == 0xF8100A00u) { /* RFE */
            okf = op_rfe((insn >> 16) & 0xF, (insn >> 23) & 3, (insn >> 21) & 1);
        } else if ((insn & 0x0E000000u) == 0x0A000000u) {
            /* BLX (immediate): branch to Thumb */
            u32 imm24 = insn & 0x00FFFFFF;
            s32 off = (imm24 & 0x00800000) ? (s32)(imm24 | 0xFF000000u) : (s32)imm24;
            u32 h = (insn >> 24) & 1;
            r_[14] = pc + 4;
            cpsr_ |= PSR_T;
            r_[15] = ((pc + 8) & ~3u) + (u32)(off << 2) + (h << 1);
            branched_ = true;
        } else {
            okf = false;
        }
        if (!okf) {
            r_[15] = pc;
            return invalid(pc, insn);
        }
        if (!error_.empty()) {
            r_[15] = pc;
            return false;
        }
        if (!branched_)
            r_[15] = pc + 4;
        return true;
    }

    /* Decode follows QEMU 5.0's a32.decode (Unicorn 2.1.4): SBZ/SBO fields and
     * UNPREDICTABLE register choices it rejects are UC_ERR_INSN_INVALID. */
    u32 op = (insn >> 25) & 7;
    ok = true;
    switch (op) {
    case 0:
        if ((insn & 0x90) == 0x90) {
            if ((insn & 0x60) == 0)
                ok = exec_arm_mul(insn, pc);
            else
                ok = exec_arm_ls_extra(insn);
        } else if ((insn & 0x01900000u) == 0x01000000u) {
            ok = exec_arm_misc(insn, pc);
        } else {
            ok = exec_arm_dp(insn);
        }
        break;
    case 1:
        if ((insn & 0x01900000u) == 0x01000000u)
            ok = exec_arm_misc_imm(insn, pc);
        else
            ok = exec_arm_dp(insn);
        break;
    case 2:
        ok = exec_arm_ls(insn);
        break;
    case 3:
        ok = (insn & 0x10) ? exec_arm_media(insn) : exec_arm_ls(insn);
        break;
    case 4:
        ok = exec_arm_block(insn);
        break;
    case 5:
        exec_arm_branch(insn);
        break;
    case 6:
        ok = exec_coproc(insn);
        break;
    case 7:
        if (insn & (1u << 24)) {
            if (on_swi)
                on_swi(watch_ctx);
            u32 c = cpsr_;
            u32 npc = pc + 4;
            switch_mode(MODE_SVC);
            cpsr_ = (c & ~0x3Fu) | 0x13 | 0x80;
            set_spsr(c);
            r_[14] = npc;
            r_[15] = vector_base + 0x08;
            branched_ = true;
        } else {
            ok = exec_coproc(insn);
        }
        break;
    }

    if (!ok) {
        /* YIELD/WFE raise EXCP_YIELD after setting PC to the next insn */
        r_[15] = undef_next_ ? undef_next_pc_ : pc;
        undef_next_ = false;
        return invalid(pc, insn);
    }
    if (!error_.empty()) {
        /* a data access faulted: Unicorn reports the faulting instruction's
         * own address as PC, not the pipeline value */
        r_[15] = pc;
        return false;
    }
    if (!branched_)
        r_[15] = pc + 4;
    return true;
}

bool ArmCpu::exec_arm_dp(u32 insn)
{
    u32 opcode = (insn >> 21) & 0xF;
    bool S = (insn >> 20) & 1;
    u32 rn = (insn >> 16) & 0xF;
    u32 rd = (insn >> 12) & 0xF;
    /* a32.decode: TST/TEQ/CMP/CMN need Rd = 0000, MOV/MVN need Rn = 0000 */
    if ((opcode & 0xC) == 0x8) {
        if (rd)
            return false;
    } else if ((opcode & 0xD) == 0xD && rn) {
        return false;
    }
    bool carry = (cpsr_ & PSR_C) != 0;
    u32 shifter;

    if (insn & (1u << 25)) {
        u32 imm = insn & 0xFF;
        u32 rot = ((insn >> 8) & 0xF) * 2;
        if (rot == 0)
            shifter = imm;
        else {
            shifter = (imm >> rot) | (imm << (32 - rot));
            carry = (shifter & 0x80000000u) != 0;
        }
    } else {
        u32 rm = insn & 0xF;
        u32 shtype = (insn >> 5) & 3;
        u32 value = r_[rm];
        u32 amount;
        bool imm_form;
        if (insn & (1u << 4)) {
            u32 rs = (insn >> 8) & 0xF;
            amount = r_[rs] & 0xFF;
            imm_form = false;
        } else {
            amount = (insn >> 7) & 0x1F;
            imm_form = true;
        }
        shifter = shift_c(shtype, value, amount, carry, imm_form);
    }

    u32 a = r_[rn];
    u32 result = 0;
    bool logical = false, write = true, vset = false, cflag = carry;
    bool cout = carry;

    switch (opcode) {
    case 0x0:
        result = a & shifter;
        logical = true;
        break;
    case 0x1:
        result = a ^ shifter;
        logical = true;
        break;
    case 0x2:
        result = a - shifter;
        cflag = a >= shifter;
        vset = (((a ^ shifter) & (a ^ result)) & 0x80000000u) != 0;
        break;
    case 0x3:
        result = shifter - a;
        cflag = shifter >= a;
        vset = (((shifter ^ a) & (shifter ^ result)) & 0x80000000u) != 0;
        break;
    case 0x4: {
        u64 sum = (u64)a + (u64)shifter;
        result = (u32)sum;
        cflag = (sum >> 32) != 0;
        vset = ((~(a ^ shifter) & (a ^ result)) & 0x80000000u) != 0;
        break;
    }
    case 0x5: {
        u64 sum = (u64)a + (u64)shifter + (u64)((cpsr_ & PSR_C) ? 1 : 0);
        result = (u32)sum;
        cflag = (sum >> 32) != 0;
        vset = ((~(a ^ shifter) & (a ^ result)) & 0x80000000u) != 0;
        break;
    }
    case 0x6: {
        u64 diff = (u64)a - (u64)shifter - (u64)((cpsr_ & PSR_C) ? 0 : 1);
        result = (u32)diff;
        cflag = (diff >> 32) == 0;
        vset = (((a ^ shifter) & (a ^ result)) & 0x80000000u) != 0;
        break;
    }
    case 0x7: {
        u64 diff = (u64)shifter - (u64)a - (u64)((cpsr_ & PSR_C) ? 0 : 1);
        result = (u32)diff;
        cflag = (diff >> 32) == 0;
        vset = (((shifter ^ a) & (shifter ^ result)) & 0x80000000u) != 0;
        break;
    }
    case 0x8:
        result = a & shifter;
        logical = true;
        write = false;
        break;
    case 0x9:
        result = a ^ shifter;
        logical = true;
        write = false;
        break;
    case 0xA:
        result = a - shifter;
        cflag = a >= shifter;
        vset = (((a ^ shifter) & (a ^ result)) & 0x80000000u) != 0;
        write = false;
        break;
    case 0xB: {
        u64 sum = (u64)a + (u64)shifter;
        result = (u32)sum;
        cflag = (sum >> 32) != 0;
        vset = ((~(a ^ shifter) & (a ^ result)) & 0x80000000u) != 0;
        write = false;
        break;
    }
    case 0xC:
        result = a | shifter;
        logical = true;
        break;
    case 0xD:
        result = shifter;
        logical = true;
        break;
    case 0xE:
        result = a & ~shifter;
        logical = true;
        break;
    case 0xF:
        result = ~shifter;
        logical = true;
        break;
    }

    if (write && rd == 15 && S && (opcode == 0x2 || opcode == 0xD)) {
        /* QEMU 5.0 treats only SUBS/MOVS PC as the exception return (no flag
         * update; UNDEF in USR mode). The other S-forms with Rd = PC set the
         * flags and then do an ordinary interworking write. */
        if (cur_mode() == MODE_USR)
            return false;
        exception_return(result, get_spsr());
        return true;
    }
    if (S) {
        set_nz(result);
        if (logical)
            cpsr_ = (cpsr_ & ~PSR_C) | (cout ? PSR_C : 0);
        else
            cpsr_ = (cpsr_ & ~(PSR_C | PSR_V)) | (cflag ? PSR_C : 0) | (vset ? PSR_V : 0);
    }
    if (write) {
        if (rd == 15)
            branch_interwork(result);
        else
            r_[rd] = result;
    }
    return true;
}

bool ArmCpu::exec_arm_mul(u32 insn, u32 pc)
{
    /* QEMU field names: rd = 19:16, ra = 15:12, rm = 11:8, rn = 3:0 */
    u32 rd = (insn >> 16) & 0xF;
    u32 ra = (insn >> 12) & 0xF;
    u32 rm = (insn >> 8) & 0xF;
    u32 rn = insn & 0xF;
    if (!(insn & (1u << 24))) {
        bool S = (insn >> 20) & 1;
        switch ((insn >> 21) & 7) {
        case 0: /* MUL: Ra field must be 0000 */
            if (ra)
                return false;
            op_mla(rd, rn, rm, 0, false, S);
            return true;
        case 1:
            op_mla(rd, rn, rm, ra, true, S);
            return true;
        case 2: /* UMAAL (no S form) */
            if (S)
                return false;
            op_umaal(ra, rd, rn, rm);
            return true;
        case 3: /* MLS (no S form) */
            if (S)
                return false;
            wreg(rd, r_[ra] - r_[rn] * r_[rm]);
            return true;
        case 4:
            op_mlal(ra, rd, rn, rm, true, false, S);
            return true;
        case 5:
            op_mlal(ra, rd, rn, rm, true, true, S);
            return true;
        case 6:
            op_mlal(ra, rd, rn, rm, false, false, S);
            return true;
        default:
            op_mlal(ra, rd, rn, rm, false, true, S);
            return true;
        }
    }
    u32 o = (insn >> 20) & 0xF;
    if (o == 0x0 || o == 0x4) { /* SWP / SWPB: Rt = 15:12, Rt2 = 3:0, Rn = 19:16 */
        if (insn & 0xF00)
            return false;
        return op_swp(ra, rn, rd, o == 0x4, pc);
    }
    if (!(o & 0x8) || ((insn >> 8) & 0xF) != 0xF)
        return false; /* (LDA/STL/LDAEX/STLEX are ARMv8: UNDEF here) */
    static const int sizes[4] = {4, 8, 1, 2};
    int size = sizes[(o >> 1) & 3];
    if (o & 1) { /* LDREX{,D,B,H} Rt, [Rn]: bits 3:0 must be 1111 */
        if (rn != 0xF)
            return false;
        if (size == 8 && (ra & 1))
            return false;
        return op_ldrex(ra, ra + 1, rd, 0, size, pc);
    }
    /* STREX{,D,B,H} Rd(15:12), Rt(3:0), [Rn(19:16)] */
    if (size == 8 && (rn & 1))
        return false;
    return op_strex(ra, rn, rn + 1, rd, 0, size, pc);
}

bool ArmCpu::exec_arm_ls_extra(u32 insn)
{
    bool P = (insn >> 24) & 1;
    bool U = (insn >> 23) & 1;
    bool I = (insn >> 22) & 1;
    bool W = (insn >> 21) & 1;
    bool L = (insn >> 20) & 1;
    u32 rn = (insn >> 16) & 0xF;
    u32 rd = (insn >> 12) & 0xF;
    u32 sh = (insn >> 5) & 3; /* 1 = H, 2 = SB / LDRD, 3 = SH / STRD */
    if (!I && (insn & 0xF00))
        return false; /* register form: bits 11:8 SBZ */
    if (!P && W && !L && sh != 1)
        return false; /* no LDRD/STRD "T" form */
    u32 offset = I ? ((((insn >> 8) & 0xF) << 4) | (insn & 0xF)) : r_[insn & 0xF];
    /* PC as base: the immediate form uses Align(PC, 4) (QEMU add_reg_for_lit) */
    u32 base = (I && rn == 15) ? (r_[15] & ~3u) : r_[rn];
    u32 moved = U ? base + offset : base - offset;
    u32 addr = P ? moved : base;
    bool wback = (!P) || W;

    if (L) {
        u32 val;
        if (!mem_load(addr, sh == 2 ? 1 : 2, val, sh != 1))
            return true;
        if (wback)
            wreg(rn, moved);
        if (rd == 15)
            branch_interwork(val);
        else
            r_[rd] = val;
        return true;
    }
    if (sh == 1) {
        if (!mem_store(addr, 2, r_[rd]))
            return true;
    } else {
        if (rd & 1)
            return false; /* LDRD/STRD with an odd Rt */
        if (sh == 2) { /* LDRD: Rt written before the second load */
            u32 lo, hi;
            if (!mem_load(addr, 4, lo, false))
                return true;
            r_[rd] = lo;
            if (!mem_load(addr + 4, 4, hi, false))
                return true;
            wreg(rd + 1, hi);
            if (!P && !I) { /* QEMU reads the post-index Rm after the loads */
                u32 o2 = r_[insn & 0xF];
                moved = U ? base + o2 : base - o2;
            }
        } else { /* STRD */
            if (!mem_store(addr, 4, r_[rd]))
                return true;
            if (!mem_store(addr + 4, 4, r_[rd + 1]))
                return true;
        }
    }
    if (wback)
        wreg(rn, moved);
    return true;
}

bool ArmCpu::exec_arm_ls(u32 insn)
{
    bool I = (insn >> 25) & 1;
    bool P = (insn >> 24) & 1;
    bool U = (insn >> 23) & 1;
    bool B = (insn >> 22) & 1;
    bool W = (insn >> 21) & 1;
    bool L = (insn >> 20) & 1;
    u32 rn = (insn >> 16) & 0xF;
    u32 rd = (insn >> 12) & 0xF;
    u32 offset;
    u32 base;
    if (!I) {
        offset = insn & 0xFFF;
        /* immediate form with PC as base: Align(PC, 4) (literal) */
        base = (rn == 15) ? (r_[15] & ~3u) : r_[rn];
    } else {
        u32 rm = insn & 0xF;
        u32 shtype = (insn >> 5) & 3;
        u32 amount = (insn >> 7) & 0x1F;
        bool c = (cpsr_ & PSR_C) != 0;
        offset = shift_c(shtype, r_[rm], amount, c, true);
        base = r_[rn];
    }
    u32 moved = U ? base + offset : base - offset;
    u32 addr = P ? moved : base;
    bool wback = (!P) || W;
    int size = B ? 1 : 4;

    if (L) {
        u32 val;
        if (!mem_load(addr, size, val, false))
            return true;
        if (wback)
            wreg(rn, moved);
        if (rd == 15)
            branch_interwork(val);
        else
            r_[rd] = val;
    } else {
        /* a stored PC reads as PC + 8 (QEMU load_reg) */
        if (!mem_store(addr, size, r_[rd]))
            return true;
        if (wback)
            wreg(rn, moved);
    }
    return true;
}

bool ArmCpu::exec_arm_block(u32 insn)
{
    bool P = (insn >> 24) & 1;
    bool U = (insn >> 23) & 1;
    bool S = (insn >> 22) & 1;
    bool W = (insn >> 21) & 1;
    u32 rn = (insn >> 16) & 0xF;
    u32 list = insn & 0xFFFF;
    if (insn & (1u << 20)) {
        /* v7: LDM with writeback and the base in the list is UNDEF */
        if (W && (list & (1u << rn)))
            return false;
        return op_ldm(rn, list, W, P, U, S, 1);
    }
    return op_stm(rn, list, W, P, U, S, 1);
}

/* QEMU get_user_reg / set_user_reg: the USR bank's r13/r14 slot (even in
 * SYS, where it is the copy saved at the last mode switch), and r8-r12 from
 * the USR bank only while in FIQ. */
u32 ArmCpu::user_reg(u32 i) const
{
    if (i == 13 || i == 14)
        return bank_usr_[i - 8];
    if (i >= 8 && i <= 12 && cur_mode() == MODE_FIQ)
        return bank_usr_[i - 8];
    return r_[i];
}

void ArmCpu::set_user_reg(u32 i, u32 v)
{
    if (i == 13 || i == 14 || (i >= 8 && i <= 12 && cur_mode() == MODE_FIQ))
        bank_usr_[i - 8] = v;
    else
        r_[i] = v;
}

static int popcount16(u32 list)
{
    return __builtin_popcount(list);
}

/* QEMU op_stm: b = before, inc = increment, user = the ^ form */
bool ArmCpu::op_stm(u32 rn, u32 list, bool w, bool b, bool inc, bool user, int min_n)
{
    if (user && cur_mode() == MODE_USR)
        return false;
    int n = popcount16(list);
    if (n < min_n || rn == 15)
        return false;
    u32 addr = r_[rn];
    if (b)
        addr = inc ? addr + 4 : addr - 4u * (u32)n;
    else if (!inc && n != 1)
        addr -= 4u * (u32)(n - 1);
    u8 *fp = (user || (cpsr_ & PSR_E)) ? 0 : mem.fast_ptr(addr, 4 * n, true);
    if (fp) { /* hot path (PUSH / STMDB): one directly addressable page */
        for (u32 l = list; l; l &= l - 1, fp += 4)
            wr32le(fp, r_[__builtin_ctz(l)]); /* base: original value; PC: +8/+4 */
        addr += 4u * (u32)(n - 1);
    } else {
    int j = 0;
    for (u32 i = 0; i < 16; i++) {
        if (!(list & (1u << i)))
            continue;
        /* the base is stored as its original value; PC as PC + 8/4 */
        u32 v = (user && i != 15) ? user_reg(i) : r_[i];
        if (!mem_store(addr, 4, v))
            return true;
        if (++j != n)
            addr += 4;
    }
    }
    if (w) {
        if (!b)
            addr = inc ? addr + 4 : addr - 4u * (u32)n;
        else if (!inc && n != 1)
            addr -= 4u * (u32)(n - 1);
        wreg(rn, addr);
    }
    return true;
}

/* QEMU do_ldm: the ^ form with PC is an exception return, without PC it
 * loads the USR bank (no writeback allowed). */
bool ArmCpu::op_ldm(u32 rn, u32 list, bool w, bool b, bool inc, bool user, int min_n)
{
    bool exc_return = false;
    if (user) {
        if (cur_mode() == MODE_USR)
            return false;
        if (list & 0x8000) {
            exc_return = true;
            user = false;
        } else if (w) {
            return false;
        }
    }
    int n = popcount16(list);
    if (n < min_n || rn == 15)
        return false;
    u32 addr = r_[rn];
    if (b)
        addr = inc ? addr + 4 : addr - 4u * (u32)n;
    else if (!inc && n != 1)
        addr -= 4u * (u32)(n - 1);
    bool loaded_base = false;
    u32 loaded_val = 0;
    u8 *fp = (user || exc_return || (cpsr_ & PSR_E)) ? 0 : mem.fast_ptr(addr, 4 * n, false);
    if (fp) { /* hot path (POP / LDMIA): one directly addressable page */
        for (u32 l = list; l; l &= l - 1, fp += 4) {
            u32 i = (u32)__builtin_ctz(l), v = rd32le(fp);
            if (i == rn) {
                loaded_val = v;
                loaded_base = true;
            } else if (i == 15)
                branch_interwork(v);
            else
                r_[i] = v;
        }
        addr += 4u * (u32)(n - 1);
    } else {
    int j = 0;
    for (u32 i = 0; i < 16; i++) {
        if (!(list & (1u << i)))
            continue;
        u32 v;
        if (!mem_load(addr, 4, v, false))
            return true;
        if (user)
            set_user_reg(i, v);
        else if (i == rn) {
            loaded_val = v;
            loaded_base = true;
        } else if (i == 15 && exc_return) {
            r_[15] = v;
            branched_ = true;
        } else if (i == 15)
            branch_interwork(v);
        else
            r_[i] = v;
        if (++j != n)
            addr += 4;
    }
    }
    if (w) {
        if (!b)
            addr = inc ? addr + 4 : addr - 4u * (u32)n;
        else if (!inc && n != 1)
            addr -= 4u * (u32)(n - 1);
        wreg(rn, addr);
    }
    if (loaded_base)
        wreg(rn, loaded_val);
    if (exc_return) {
        write_cpsr_insn(get_spsr(), 0xFF1FFFFFu);
        r_[15] &= thumb() ? ~1u : ~3u;
        branched_ = true;
    }
    return true;
}

void ArmCpu::exec_arm_branch(u32 insn)
{
    u32 pc8 = r_[15];
    u32 imm24 = insn & 0x00FFFFFF;
    s32 off = (imm24 & 0x00800000) ? (s32)(imm24 | 0xFF000000u) : (s32)imm24;
    if (insn & (1u << 24))
        r_[14] = pc8 - 4;
    r_[15] = pc8 + (u32)(off << 2); /* QEMU gen_jmp: no masking */
    branched_ = true;
}

/* Media instructions (A32 cond 011x xxxx ... xxx1 xxxx), a32.decode */
bool ArmCpu::exec_arm_media(u32 insn)
{
    u32 op1 = (insn >> 20) & 0x1F;
    u32 op2 = (insn >> 5) & 0x7;
    u32 rd = (insn >> 12) & 0xF;
    u32 rn = (insn >> 16) & 0xF;
    u32 rm = insn & 0xF;
    u32 b118 = (insn >> 8) & 0xF;

    switch (op1 >> 3) {
    case 0: { /* parallel add/subtract: Rn 19:16, Rd 15:12, Rm 3:0 */
        static const int kinds[8] = {-1, 0, 1, 2, -1, 3, 4, 5};
        static const int ops[8] = {0, 1, 2, 3, 4, -1, -1, 5};
        int k = kinds[op1 & 7], o = ops[op2];
        if (k < 0 || o < 0 || b118 != 0xF)
            return false;
        op_par_addsub((u32)k, (u32)o, rd, rn, rm);
        return true;
    }
    case 1: { /* pack / saturate / reverse / extend */
        if (!(op2 & 1)) { /* xx0: PKH (op1 01000) or SSAT/USAT (op1 x101x / x111x) */
            if (op1 == 0x08) {
                op_pkh(rd, rn, rm, (insn >> 7) & 0x1F, (insn >> 6) & 1);
                return true;
            }
            if (op1 & 0x2) { /* 0x0A/0x0B SSAT, 0x0E/0x0F USAT */
                op_sat(rd, rm, (insn >> 7) & 0x1F, (insn >> 6) & 1, (insn >> 16) & 0x1F, (op1 & 4) ? 1 : 0);
                return true;
            }
            return false;
        }
        if (op2 == 3) { /* extend (+add): bits 9:8 SBZ */
            static const int xk[8] = {2, -1, 0, 1, 5, -1, 3, 4};
            int k = xk[op1 & 7];
            if (k < 0 || (insn & 0x300))
                return false;
            op_xta(rd, rn, rm, (insn >> 10) & 3, k);
            return true;
        }
        if (b118 != 0xF)
            return false;
        if (op2 == 5 && op1 == 0x08) {
            op_sel(rd, rn, rm);
            return true;
        }
        if (op2 == 1 && (op1 == 0x0A || op1 == 0x0E)) { /* SSAT16 / USAT16 */
            op_sat(rd, rm, 0, false, (insn >> 16) & 0xF, op1 == 0x0A ? 2 : 3);
            return true;
        }
        if (rn != 0xF)
            return false;
        if (op1 == 0x0B && (op2 == 1 || op2 == 5)) {
            op_rev(rd, rm, op2 == 1 ? 0 : 1); /* REV / REV16 */
            return true;
        }
        if (op1 == 0x0F && (op2 == 1 || op2 == 5)) {
            op_rev(rd, rm, op2 == 1 ? 3 : 2); /* RBIT / REVSH */
            return true;
        }
        return false;
    }
    case 2: { /* signed multiplies, divide: Rd 19:16, Ra 15:12, Rm 11:8, Rn 3:0 */
        u32 mrd = rn, ra = rd, mrm = b118, mrn = rm;
        u32 o = op1 & 7;
        if (o == 0 && op2 < 4) {
            op_smlad(mrd, ra, mrn, mrm, op2 & 1, op2 & 2);
            return true;
        }
        if ((o == 1 || o == 3) && op2 == 0) {
            if (ra != 0xF)
                return false;
            op_div(mrd, mrn, mrm, o == 3);
            return true;
        }
        if (o == 4 && op2 < 4) {
            op_smlald(ra, mrd, mrn, mrm, op2 & 1, op2 & 2);
            return true;
        }
        if (o == 5 && (op2 == 0 || op2 == 1 || op2 == 6 || op2 == 7)) {
            op_smmla(mrd, ra, mrn, mrm, op2 & 1, op2 & 4);
            return true;
        }
        return false;
    }
    default: /* 11xxx: USADA8, bitfield */
        if (op1 == 0x18 && op2 == 0) {
            op_usada8(rn, rd, rm, b118);
            return true;
        }
        if ((op2 & 3) == 2 && (op1 & 0x1E) == 0x1A) /* SBFX */
            return op_bfx(rd, rm, (insn >> 7) & 0x1F, (insn >> 16) & 0x1F, false);
        if ((op2 & 3) == 2 && (op1 & 0x1E) == 0x1E) /* UBFX */
            return op_bfx(rd, rm, (insn >> 7) & 0x1F, (insn >> 16) & 0x1F, true);
        if ((op2 & 3) == 0 && (op1 & 0x1E) == 0x1C) /* BFC / BFI */
            return op_bfci(rd, rm, (insn >> 7) & 0x1F, (insn >> 16) & 0x1F);
        return false;
    }
}


/* cond 00010xx0 (bits 7 and 4 not both set): PSR transfer, BX/BLX/BXJ, CLZ,
 * saturating add/sub, halfword multiplies, BKPT/HLT/HVC/SMC, ERET. */
bool ArmCpu::exec_arm_misc(u32 insn, u32 pc)
{
    u32 op = (insn >> 21) & 3; /* bits 22:21 */
    u32 op2 = (insn >> 4) & 0xF;
    u32 rn = (insn >> 16) & 0xF;
    u32 rd = (insn >> 12) & 0xF;
    u32 rs = (insn >> 8) & 0xF;
    u32 rm = insn & 0xF;
    switch (op2) {
    case 0x0:
        if (!(op & 1)) { /* MRS */
            bool r = op >> 1;
            if (insn & 0x200) { /* MRS (banked): ....001m 0000 0000 */
                if ((insn & 0xEFF) != 0x200)
                    return false;
                return op_mrs_banked(r, (((insn >> 8) & 1) << 4) | rn, rd);
            }
            if ((insn & 0x000F0FFFu) != 0x000F0000u)
                return false;
            if (r && cur_mode() == MODE_USR)
                return false;
            wreg(rd, mrs_value(r));
            return true;
        }
        /* MSR (register): Rd field 1111, bits 11:8 0000 (or 001m banked) */
        if (rd != 0xF)
            return false;
        if (insn & 0x200) {
            if ((insn & 0xC00) != 0)
                return false;
            return op_msr_banked(op >> 1, (((insn >> 8) & 1) << 4) | rn, rm);
        }
        if (rs != 0)
            return false;
        return op_msr(r_[rm], rn, op >> 1);
    case 0x1:
        if (op == 1 && (insn & 0x000FFF00u) == 0x000FFF00u) { /* BX */
            branch_interwork(r_[rm]);
            return true;
        }
        if (op == 3 && (insn & 0x000F0F00u) == 0x000F0F00u) { /* CLZ */
            u32 v = r_[rm], n = 0;
            if (v == 0)
                n = 32;
            else
                while (!(v & 0x80000000u)) {
                    n++;
                    v <<= 1;
                }
            wreg(rd, n);
            return true;
        }
        return false;
    case 0x2: /* BXJ: a plain BX here */
        if (op == 1 && (insn & 0x000FFF00u) == 0x000FFF00u) {
            branch_interwork(r_[rm]);
            return true;
        }
        return false;
    case 0x3: /* BLX (register) */
        if (op == 1 && (insn & 0x000FFF00u) == 0x000FFF00u) {
            u32 target = r_[rm];
            r_[14] = pc + 4;
            branch_interwork(target);
            return true;
        }
        return false;
    case 0x5: /* QADD/QSUB/QDADD/QDSUB Rd, Rm, Rn: bits 11:8 SBZ */
        if (rs != 0)
            return false;
        op_qaddsub(rd, rn, rm, !(op & 1), (op >> 1) != 0);
        return true;
    case 0x6: /* ERET (a returning SUBS PC, LR, #0) */
        if ((insn & 0x0FFFFFFFu) != 0x0160006Eu || cur_mode() == MODE_USR)
            return false;
        exception_return(r_[14], get_spsr());
        return true;
    case 0x7:
        if (op == 1) { /* BKPT: EXCP_BKPT goes to the interrupt hook, which
                        * ignores it, and execution resumes at the BKPT */
            stall(pc);
            return true;
        }
        if (op == 3 && (insn & 0x000FFF00u) == 0) /* SMC: hook ignores EXCP_SMC */
            return cur_mode() != MODE_USR;
        return false; /* HLT, HVC */
    case 0x8:
    case 0xA:
    case 0xC:
    case 0xE: {
        /* halfword multiplies: Rd 19:16, Ra 15:12, Rm 11:8, Rn 3:0 */
        bool nt = (insn >> 5) & 1, mt = (insn >> 6) & 1;
        switch (op) {
        case 0:
            op_smlaxy(rn, rd, rm, rs, 1, nt, mt);
            return true;
        case 1:
            if (nt) { /* SMULWy: Ra SBZ */
                if (rd)
                    return false;
                op_smlawy(rn, 0, rm, rs, false, mt);
            } else {
                op_smlawy(rn, rd, rm, rs, true, mt);
            }
            return true;
        case 2:
            op_smlaxy(rn, rd, rm, rs, 2, nt, mt);
            return true;
        default:
            if (rd)
                return false;
            op_smlaxy(rn, 0, rm, rs, 0, nt, mt);
            return true;
        }
    }
    default:
        return false; /* CRC32 (v8) and the rest */
    }
}

/* cond 00110xx0: MOVW, MOVT, MSR (immediate) and the hints */
bool ArmCpu::exec_arm_misc_imm(u32 insn, u32 pc)
{
    u32 op = (insn >> 21) & 3;
    u32 rd = (insn >> 12) & 0xF;
    u32 imm16 = ((insn >> 4) & 0xF000) | (insn & 0xFFF);
    if (op == 0) { /* MOVW */
        wreg(rd, imm16);
        return true;
    }
    if (op == 2) { /* MOVT */
        wreg(rd, (r_[rd] & 0xFFFF) | (imm16 << 16));
        return true;
    }
    if (rd != 0xF)
        return false;
    u32 fields = (insn >> 16) & 0xF;
    if (op == 1 && fields == 0)
        return hint(insn & 0xFF, pc + 4);
    u32 imm = insn & 0xFF, rot = ((insn >> 8) & 0xF) * 2;
    u32 val = rot ? ((imm >> rot) | (imm << (32 - rot))) : imm;
    return op_msr(val, fields, op == 3);
}

/* ------------------------------------------------ shared A32/T32 helpers */
void ArmCpu::stall(u32 pc)
{
    r_[15] = pc;
    branched_ = true;
    it_set_ = true; /* re-executed with the same IT state */
}

void ArmCpu::exception_return(u32 pc, u32 spsr)
{
    /* QEMU gen_rfe / cpsr_write_eret: all valid CPSR bits from SPSR, then
     * PC masked by the new state's alignment */
    write_cpsr_insn(spsr, 0xFF1FFFFFu);
    r_[15] = pc & (thumb() ? ~1u : ~3u);
    branched_ = true;
}

bool ArmCpu::hint(u32 op8, u32 next_pc)
{
    if (op8 == 1 || op8 == 2) {
        /* YIELD / WFE: EXCP_YIELD, which Unicorn reports as an invalid
         * instruction - with PC already at the next instruction */
        undef_next_ = true;
        undef_next_pc_ = next_pc;
        return false;
    }
    return true; /* WFI (no pending work: Unicorn returns), NOP, SEV, DBG, ... */
}

u32 ArmCpu::mrs_value(bool spsr) const
{
    /* MRS CPSR reads the execution-state bits (T, IT, J, IL) as zero */
    return spsr ? get_spsr() : (cpsr_ & ~0x0730FC20u);
}

bool ArmCpu::op_msr(u32 val, u32 fields, bool spsr)
{
    u32 mask = ((fields & 1) ? 0x000000FFu : 0) | ((fields & 2) ? 0x0000FF00u : 0)
               | ((fields & 4) ? 0x00FF0000u : 0) | ((fields & 8) ? 0xFF000000u : 0);
    mask &= 0xFF1FFFFFu; /* aarch32_cpsr_valid_mask for the A15 */
    if (!spsr)
        mask &= ~0x0710FC20u; /* CPSR_EXEC */
    if (cur_mode() == MODE_USR) {
        if (spsr)
            return false;
        mask &= 0xF80F0000u; /* CPSR_USER */
    }
    if (spsr)
        set_spsr((get_spsr() & ~mask) | (val & mask));
    else
        write_cpsr_insn(val, mask);
    return true;
}

/* MRS/MSR (banked): QEMU msr_banked_access_decode + the runtime checks.
 * regno 8-14 = r8-r14 of tgtmode, 16 = its SPSR. MON/HYP are UNDEF here. */
bool ArmCpu::banked_decode(bool r, u32 sysm, u32 &tgtmode, u32 &regno) const
{
    u32 cur = cur_mode();
    if (cur == MODE_USR)
        return false;
    if (r) {
        switch (sysm) {
        case 0x0E:
            tgtmode = MODE_FIQ;
            break;
        case 0x10:
            tgtmode = MODE_IRQ;
            break;
        case 0x12:
            tgtmode = MODE_SVC;
            break;
        case 0x14:
            tgtmode = MODE_ABT;
            break;
        case 0x16:
            tgtmode = MODE_UND;
            break;
        default:
            return false;
        }
        regno = 16;
    } else if (sysm <= 0x6) {
        tgtmode = MODE_USR;
        regno = sysm + 8;
    } else if (sysm >= 0x8 && sysm <= 0xE) {
        tgtmode = MODE_FIQ;
        regno = sysm;
    } else if (sysm >= 0x10 && sysm <= 0x17) {
        static const u32 m[4] = {MODE_IRQ, MODE_SVC, MODE_ABT, MODE_UND};
        tgtmode = m[(sysm - 0x10) >> 1];
        regno = (sysm & 1) ? 13 : 14;
    } else {
        return false;
    }
    if (cur == tgtmode)
        return false;
    if (tgtmode == MODE_USR) {
        if (regno >= 8 && regno <= 12 && cur != MODE_FIQ)
            return false;
        if ((regno == 13 || regno == 14) && cur == MODE_SYS)
            return false;
    }
    return true;
}

bool ArmCpu::op_mrs_banked(bool r, u32 sysm, u32 rd)
{
    u32 tgt, regno;
    if (rd == 15 || !banked_decode(r, sysm, tgt, regno))
        return false;
    u32 v;
    if (regno == 16)
        v = tgt == MODE_FIQ ? spsr_fiq_ : tgt == MODE_IRQ ? spsr_irq_ : tgt == MODE_SVC ? spsr_svc_
                                          : tgt == MODE_ABT ? spsr_abt_ : spsr_und_;
    else if (tgt == MODE_USR)
        v = bank_usr_[regno - 8];
    else if (tgt == MODE_FIQ)
        v = bank_fiq_[regno - 8];
    else {
        const u32 *b = tgt == MODE_IRQ ? bank_irq_ : tgt == MODE_SVC ? bank_svc_ : tgt == MODE_ABT ? bank_abt_ : bank_und_;
        v = b[regno - 13];
    }
    r_[rd] = v;
    return true;
}

bool ArmCpu::op_msr_banked(bool r, u32 sysm, u32 rn)
{
    u32 tgt, regno;
    if (rn == 15 || !banked_decode(r, sysm, tgt, regno))
        return false;
    u32 v = r_[rn];
    if (regno == 16) {
        u32 *p = tgt == MODE_FIQ ? &spsr_fiq_ : tgt == MODE_IRQ ? &spsr_irq_ : tgt == MODE_SVC ? &spsr_svc_
                                               : tgt == MODE_ABT ? &spsr_abt_ : &spsr_und_;
        *p = v;
    } else if (tgt == MODE_USR)
        bank_usr_[regno - 8] = v;
    else if (tgt == MODE_FIQ)
        bank_fiq_[regno - 8] = v;
    else {
        u32 *b = tgt == MODE_IRQ ? bank_irq_ : tgt == MODE_SVC ? bank_svc_ : tgt == MODE_ABT ? bank_abt_ : bank_und_;
        b[regno - 13] = v;
    }
    return true;
}

void ArmCpu::op_mla(u32 rd, u32 rn, u32 rm, u32 ra, bool add, bool s)
{
    u32 v = r_[rn] * r_[rm];
    if (add)
        v += r_[ra];
    if (s)
        set_nz(v);
    wreg(rd, v);
}

void ArmCpu::op_mlal(u32 ra, u32 rd, u32 rn, u32 rm, bool uns, bool add, bool s)
{
    u64 p = uns ? (u64)r_[rm] * (u64)r_[rn] : (u64)((s64)(s32)r_[rm] * (s64)(s32)r_[rn]);
    if (add)
        p += ((u64)r_[rd] << 32) | r_[ra];
    if (s)
        cpsr_ = (cpsr_ & ~(PSR_N | PSR_Z)) | ((p >> 63) ? PSR_N : 0) | (p == 0 ? PSR_Z : 0);
    wreg(ra, (u32)p);
    wreg(rd, (u32)(p >> 32));
}

void ArmCpu::op_umaal(u32 ra, u32 rd, u32 rn, u32 rm)
{
    u64 p = (u64)r_[rm] * (u64)r_[rn] + r_[ra] + r_[rd];
    wreg(ra, (u32)p);
    wreg(rd, (u32)(p >> 32));
}

static u32 add_setq(u32 &cpsr, u32 a, u32 b)
{
    u32 res = a + b;
    if (((res ^ a) & 0x80000000u) && !((a ^ b) & 0x80000000u))
        cpsr |= PSR_Q;
    return res;
}

static u32 add_saturate(u32 &cpsr, u32 a, u32 b)
{
    u32 res = a + b;
    if (((res ^ a) & 0x80000000u) && !((a ^ b) & 0x80000000u)) {
        cpsr |= PSR_Q;
        res = ~(((u32)((s32)a >> 31)) ^ 0x80000000u);
    }
    return res;
}

static u32 sub_saturate(u32 &cpsr, u32 a, u32 b)
{
    u32 res = a - b;
    if (((res ^ a) & 0x80000000u) && ((a ^ b) & 0x80000000u)) {
        cpsr |= PSR_Q;
        res = ~(((u32)((s32)a >> 31)) ^ 0x80000000u);
    }
    return res;
}

void ArmCpu::op_qaddsub(u32 rd, u32 rn, u32 rm, bool add, bool doub)
{
    u32 t0 = r_[rm], t1 = r_[rn];
    if (doub)
        t1 = add_saturate(cpsr_, t1, t1);
    t0 = add ? add_saturate(cpsr_, t0, t1) : sub_saturate(cpsr_, t0, t1);
    wreg(rd, t0);
}

void ArmCpu::op_smlaxy(u32 rd, u32 ra, u32 rn, u32 rm, int add_long, bool nt, bool mt)
{
    s32 a = nt ? ((s32)r_[rn] >> 16) : (s32)(s16)(u16)r_[rn];
    s32 b = mt ? ((s32)r_[rm] >> 16) : (s32)(s16)(u16)r_[rm];
    u32 p = (u32)a * (u32)b;
    if (add_long == 0) {
        wreg(rd, p);
    } else if (add_long == 1) {
        wreg(rd, add_setq(cpsr_, p, r_[ra]));
    } else {
        u64 acc = (((u64)r_[rd] << 32) | r_[ra]) + (u64)(s64)(s32)p;
        wreg(ra, (u32)acc);
        wreg(rd, (u32)(acc >> 32));
    }
}

void ArmCpu::op_smlawy(u32 rd, u32 ra, u32 rn, u32 rm, bool add, bool mt)
{
    u32 t1 = mt ? (r_[rm] & 0xFFFF0000u) : (r_[rm] << 16);
    u32 hi = (u32)((u64)((s64)(s32)r_[rn] * (s64)(s32)t1) >> 32);
    if (add)
        hi = add_setq(cpsr_, hi, r_[ra]);
    wreg(rd, hi);
}

/* parallel add/sub. kind: 0 S, 1 Q, 2 SH, 3 U, 4 UQ, 5 UH;
 * op: 0 ADD16, 1 ASX, 2 SAX, 3 SUB16, 4 ADD8, 5 SUB8 (QEMU op_addsub.h) */
static u32 pas_lane(int kind, bool sub, int width, u32 a, u32 b, int n, u32 &ge)
{
    u32 m = width == 16 ? 0xFFFFu : 0xFFu;
    a &= m;
    b &= m;
    u32 sign = width == 16 ? 0x8000u : 0x80u;
    s32 sa = width == 16 ? (s32)(s16)(u16)a : (s32)(s8)(u8)a;
    s32 sb = width == 16 ? (s32)(s16)(u16)b : (s32)(s8)(u8)b;
    switch (kind) {
    case 0: { /* signed modulo, GE when the sum >= 0 */
        s32 sum = sub ? sa - sb : sa + sb;
        if (sum >= 0)
            ge |= (width == 16 ? 3u : 1u) << (width == 16 ? n * 2 : n);
        return (u32)sum & m;
    }
    case 1: { /* signed saturating */
        u32 res = (sub ? a - b : a + b) & m;
        bool ovf = sub ? (((res ^ a) & sign) && ((a ^ b) & sign)) : (((res ^ a) & sign) && !((a ^ b) & sign));
        if (ovf)
            res = (a & sign) ? sign : sign - 1;
        return res;
    }
    case 2: /* signed halving */
        return (u32)((sub ? sa - sb : sa + sb) >> 1) & m;
    case 3: { /* unsigned modulo, GE on carry / no borrow */
        u32 sum = sub ? a - b : a + b;
        bool g = sub ? ((sum >> width) == 0) : ((sum >> width) == 1);
        if (g)
            ge |= (width == 16 ? 3u : 1u) << (width == 16 ? n * 2 : n);
        return sum & m;
    }
    case 4: /* unsigned saturating */
        if (sub)
            return a > b ? a - b : 0;
        return ((a + b) & m) < a ? m : (a + b) & m;
    default: /* unsigned halving */
        return ((sub ? a - b : a + b) >> 1) & m;
    }
}

void ArmCpu::op_par_addsub(u32 kind, u32 op, u32 rd, u32 rn, u32 rm)
{
    u32 a = r_[rn], b = r_[rm], ge = 0, res = 0;
    int k = (int)kind;
    switch (op) {
    case 0: /* ADD16 */
        res = pas_lane(k, false, 16, a, b, 0, ge) | (pas_lane(k, false, 16, a >> 16, b >> 16, 1, ge) << 16);
        break;
    case 1: /* ASX: lo = a.lo - b.hi, hi = a.hi + b.lo */
        res = pas_lane(k, true, 16, a, b >> 16, 0, ge) | (pas_lane(k, false, 16, a >> 16, b, 1, ge) << 16);
        break;
    case 2: /* SAX: lo = a.lo + b.hi, hi = a.hi - b.lo */
        res = pas_lane(k, false, 16, a, b >> 16, 0, ge) | (pas_lane(k, true, 16, a >> 16, b, 1, ge) << 16);
        break;
    case 3: /* SUB16 */
        res = pas_lane(k, true, 16, a, b, 0, ge) | (pas_lane(k, true, 16, a >> 16, b >> 16, 1, ge) << 16);
        break;
    default: { /* ADD8 / SUB8 */
        bool sub = op == 5;
        for (int n = 0; n < 4; n++)
            res |= pas_lane(k, sub, 8, a >> (8 * n), b >> (8 * n), n, ge) << (8 * n);
        break;
    }
    }
    if (kind == 0 || kind == 3)
        cpsr_ = (cpsr_ & ~0x000F0000u) | (ge << 16);
    wreg(rd, res);
}

void ArmCpu::op_pkh(u32 rd, u32 rn, u32 rm, u32 shift, bool tb)
{
    u32 res;
    if (tb) {
        u32 t = (u32)((s32)r_[rm] >> (shift ? shift : 31));
        res = (r_[rn] & 0xFFFF0000u) | (t & 0xFFFF);
    } else {
        u32 t = r_[rm] << shift;
        res = (t & 0xFFFF0000u) | (r_[rn] & 0xFFFF);
    }
    wreg(rd, res);
}

static u32 do_ssat(u32 &cpsr, s32 val, u32 shift)
{
    s32 top = val >> shift;
    u32 mask = (1u << shift) - 1;
    if (top > 0) {
        cpsr |= PSR_Q;
        return mask;
    }
    if (top < -1) {
        cpsr |= PSR_Q;
        return ~mask;
    }
    return (u32)val;
}

static u32 do_usat(u32 &cpsr, s32 val, u32 shift)
{
    u32 max = (1u << shift) - 1;
    if (val < 0) {
        cpsr |= PSR_Q;
        return 0;
    }
    if ((u32)val > max) {
        cpsr |= PSR_Q;
        return max;
    }
    return (u32)val;
}

/* kind: 0 SSAT, 1 USAT, 2 SSAT16, 3 USAT16 */
void ArmCpu::op_sat(u32 rd, u32 rn, u32 shift, bool sh, u32 satimm, int kind)
{
    u32 t = r_[rn];
    if (sh)
        t = (u32)((s32)t >> (shift ? shift : 31));
    else
        t <<= shift;
    u32 res;
    switch (kind) {
    case 0:
        res = do_ssat(cpsr_, (s32)t, satimm);
        break;
    case 1:
        res = do_usat(cpsr_, (s32)t, satimm);
        break;
    case 2:
        res = (do_ssat(cpsr_, (s32)(s16)(u16)t, satimm) & 0xFFFF) | (do_ssat(cpsr_, (s32)t >> 16, satimm) << 16);
        break;
    default:
        res = (do_usat(cpsr_, (s32)(s16)(u16)t, satimm) & 0xFFFF) | (do_usat(cpsr_, (s32)t >> 16, satimm) << 16);
        break;
    }
    wreg(rd, res);
}

/* kind: 0 SXTAB, 1 SXTAH, 2 SXTAB16, 3 UXTAB, 4 UXTAH, 5 UXTAB16 (Rn = 15: no add) */
void ArmCpu::op_xta(u32 rd, u32 rn, u32 rm, u32 rot, int kind)
{
    u32 v = r_[rm];
    if (rot)
        v = (v >> (rot * 8)) | (v << (32 - rot * 8));
    switch (kind) {
    case 0:
        v = (u32)(s32)(s8)(u8)v;
        break;
    case 1:
        v = (u32)(s32)(s16)(u16)v;
        break;
    case 2:
        v = ((u32)(s32)(s8)(u8)v & 0xFFFF) | ((u32)(s32)(s8)(u8)(v >> 16) << 16);
        break;
    case 3:
        v &= 0xFF;
        break;
    case 4:
        v &= 0xFFFF;
        break;
    default:
        v &= 0x00FF00FFu;
        break;
    }
    if (rn != 15) {
        u32 a = r_[rn];
        if (kind == 2 || kind == 5)
            v = ((v + a) & 0xFFFF) | (((v >> 16) + (a >> 16)) << 16);
        else
            v += a;
    }
    wreg(rd, v);
}

void ArmCpu::op_sel(u32 rd, u32 rn, u32 rm)
{
    u32 ge = (cpsr_ >> 16) & 0xF, mask = 0;
    for (int i = 0; i < 4; i++)
        if (ge & (1u << i))
            mask |= 0xFFu << (8 * i);
    wreg(rd, (r_[rn] & mask) | (r_[rm] & ~mask));
}

/* kind: 0 REV, 1 REV16, 2 REVSH, 3 RBIT */
void ArmCpu::op_rev(u32 rd, u32 rm, int kind)
{
    u32 v = r_[rm], r;
    switch (kind) {
    case 0:
        r = ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
        break;
    case 1:
        r = ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
        break;
    case 2:
        r = (u32)(s32)(s16)(u16)(((v << 8) & 0xFF00) | ((v >> 8) & 0xFF));
        break;
    default:
        r = 0;
        for (int i = 0; i < 32; i++)
            if (v & (1u << i))
                r |= 1u << (31 - i);
        break;
    }
    wreg(rd, r);
}

void ArmCpu::op_smlad(u32 rd, u32 ra, u32 rn, u32 rm, bool swap, bool sub)
{
    u32 a = r_[rn], b = r_[rm];
    if (swap)
        b = (b >> 16) | (b << 16);
    u32 lo = (u32)((s32)(s16)(u16)a * (s32)(s16)(u16)b);
    u32 hi = (u32)(((s32)a >> 16) * ((s32)b >> 16));
    u32 t = sub ? lo - hi : add_setq(cpsr_, lo, hi);
    if (ra != 15)
        t = add_setq(cpsr_, t, r_[ra]);
    wreg(rd, t);
}

void ArmCpu::op_smlald(u32 ra, u32 rd, u32 rn, u32 rm, bool swap, bool sub)
{
    u32 a = r_[rn], b = r_[rm];
    if (swap)
        b = (b >> 16) | (b << 16);
    s64 lo = (s64)((s32)(s16)(u16)a * (s32)(s16)(u16)b);
    s64 hi = (s64)(((s32)a >> 16) * ((s32)b >> 16));
    u64 l = (u64)(sub ? lo - hi : lo + hi);
    l += ((u64)r_[rd] << 32) | r_[ra];
    wreg(ra, (u32)l);
    wreg(rd, (u32)(l >> 32));
}

void ArmCpu::op_smmla(u32 rd, u32 ra, u32 rn, u32 rm, bool round, bool sub)
{
    u64 p = (u64)((s64)(s32)r_[rn] * (s64)(s32)r_[rm]);
    u32 lo = (u32)p, hi = (u32)(p >> 32);
    if (ra != 15) {
        if (sub) {
            u64 d = ((u64)r_[ra] << 32) - p;
            lo = (u32)d;
            hi = (u32)(d >> 32);
        } else {
            hi += r_[ra];
        }
    }
    if (round)
        hi += lo >> 31;
    wreg(rd, hi);
}

void ArmCpu::op_div(u32 rd, u32 rn, u32 rm, bool u)
{
    u32 n = r_[rn], d = r_[rm], q;
    if (d == 0)
        q = 0;
    else if (u)
        q = n / d;
    else if (n == 0x80000000u && d == 0xFFFFFFFFu)
        q = 0x80000000u;
    else
        q = (u32)((s32)n / (s32)d);
    wreg(rd, q);
}

void ArmCpu::op_usada8(u32 rd, u32 ra, u32 rn, u32 rm)
{
    u32 a = r_[rn], b = r_[rm], sum = 0;
    for (int i = 0; i < 4; i++) {
        u32 x = (a >> (8 * i)) & 0xFF, y = (b >> (8 * i)) & 0xFF;
        sum += x > y ? x - y : y - x;
    }
    if (ra != 15)
        sum += r_[ra];
    wreg(rd, sum);
}

bool ArmCpu::op_bfx(u32 rd, u32 rn, u32 lsb, u32 widthm1, bool u)
{
    u32 width = widthm1 + 1;
    if (lsb + width > 32)
        return false;
    u32 v = r_[rn] >> lsb;
    if (width < 32) {
        u32 m = (1u << width) - 1;
        v &= m;
        if (!u && (v & (1u << (width - 1))))
            v |= ~m;
    }
    wreg(rd, v);
    return true;
}

bool ArmCpu::op_bfci(u32 rd, u32 rn, u32 lsb, u32 msb)
{
    if (msb < lsb)
        return false;
    u32 width = msb + 1 - lsb;
    u32 v = rn == 15 ? 0 : r_[rn];
    if (width != 32) {
        u32 m = ((1u << width) - 1) << lsb;
        v = (r_[rd] & ~m) | ((v << lsb) & m);
    }
    wreg(rd, v);
    return true;
}

/* LDREX family (QEMU op_ldrex + gen_load_exclusive). An unaligned address
 * raises an alignment data abort that Unicorn's hook swallows: PC stays. */
bool ArmCpu::op_ldrex(u32 rt, u32 rt2, u32 rn, u32 imm, int size, u32 pc)
{
    bool t = thumb();
    if (rn == 15 || rt == 15 || (t && rt == 13)
        || (size == 8 && (rt2 == 15 || rt == rt2 || (t && rt2 == 13))))
        return false;
    u32 addr = r_[rn] + imm;
    if (addr & (u32)(size - 1)) {
        stall(pc);
        return true;
    }
    if (size == 8) {
        u32 lo, hi;
        if (!mem_load(addr, 4, lo, false) || !mem_load(addr + 4, 4, hi, false))
            return true;
        excl_val_ = ((u64)hi << 32) | lo;
        wreg(rt2, hi);
        wreg(rt, lo);
    } else {
        u32 v;
        if (!mem_load(addr, size, v, false))
            return true;
        excl_val_ = v;
        wreg(rt, v);
    }
    excl_valid_ = true;
    excl_addr_ = addr;
    return true;
}

/* STREX family: QEMU gen_store_exclusive (a non-atomic cmpxchg against the
 * value LDREX saw). Status 0 = stored, 1 = failed; the monitor is cleared. */
bool ArmCpu::op_strex(u32 rd, u32 rt, u32 rt2, u32 rn, u32 imm, int size, u32 pc)
{
    bool t = thumb();
    if (rd == 15 || rn == 15 || rt == 15 || rd == rn || rd == rt || (t && (rd == 13 || rt == 13))
        || (size == 8 && (rt2 == 15 || rd == rt2 || (t && rt2 == 13))))
        return false;
    u32 addr = r_[rn] + imm;
    if (!excl_valid_ || addr != excl_addr_) {
        r_[rd] = 1;
        excl_valid_ = false;
        return true;
    }
    if (addr & (u32)(size - 1)) {
        stall(pc);
        return true;
    }
    u32 status;
    if (size == 8) {
        u32 lo, hi;
        if (!mem_load(addr, 4, lo, false) || !mem_load(addr + 4, 4, hi, false))
            return true;
        u64 old = ((u64)hi << 32) | lo;
        if (old == excl_val_) {
            if (!mem_store(addr, 4, r_[rt]) || !mem_store(addr + 4, 4, r_[rt2]))
                return true;
        }
        status = old != excl_val_;
    } else {
        u32 old;
        if (!mem_load(addr, size, old, false))
            return true;
        u32 expect = (u32)excl_val_;
        u32 m = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
        if (old == (expect & m)) {
            if (!mem_store(addr, size, r_[rt]))
                return true;
        }
        status = old != expect;
    }
    r_[rd] = status;
    excl_valid_ = false;
    return true;
}

bool ArmCpu::op_swp(u32 rt, u32 rt2, u32 rn, bool byte, u32 pc)
{
    u32 addr = r_[rn];
    if (!byte && (addr & 3)) { /* MO_ALIGN: swallowed alignment abort */
        stall(pc);
        return true;
    }
    u32 old;
    if (!mem_load(addr, byte ? 1 : 4, old, false))
        return true;
    if (!mem_store(addr, byte ? 1 : 4, r_[rt2]))
        return true;
    wreg(rt, old);
    return true;
}

/* SRS: store LR and SPSR to the given mode's stack (QEMU gen_srs) */
bool ArmCpu::op_srs(u32 mode, u32 pu, bool w)
{
    u32 cur = cur_mode();
    bool ok_mode = mode == MODE_USR || mode == MODE_FIQ || mode == MODE_IRQ || mode == MODE_SVC
                   || mode == MODE_ABT || mode == MODE_UND || mode == MODE_SYS;
    if (cur == MODE_USR || !ok_mode || cur == MODE_SYS)
        return false;
    u32 sp;
    u32 *slot = 0;
    if (mode == cur)
        slot = &r_[13];
    else if (mode == MODE_USR || mode == MODE_SYS)
        slot = &bank_usr_[5];
    else if (mode == MODE_FIQ)
        slot = &bank_fiq_[5];
    else
        slot = mode == MODE_IRQ ? &bank_irq_[0] : mode == MODE_SVC ? &bank_svc_[0] : mode == MODE_ABT ? &bank_abt_[0]
                                                                                            : &bank_und_[0];
    sp = *slot;
    static const s32 pre[4] = {-4, 0, -8, 4};
    static const s32 post[4] = {-8, 4, -4, 0};
    u32 addr = sp + (u32)pre[pu & 3];
    if (!mem_store(addr, 4, r_[14]))
        return true;
    addr += 4;
    if (!mem_store(addr, 4, get_spsr()))
        return true;
    if (w)
        *slot = addr + (u32)post[pu & 3];
    return true;
}

/* RFE: load PC and CPSR from memory (QEMU trans_RFE) */
bool ArmCpu::op_rfe(u32 rn, u32 pu, bool w)
{
    if (cur_mode() == MODE_USR)
        return false;
    static const s32 pre[4] = {-4, 0, -8, 4};
    static const s32 post[4] = {-8, 4, -4, 0};
    u32 addr = r_[rn] + (u32)pre[pu & 3];
    u32 npc, ncpsr;
    if (!mem_load(addr, 4, npc, false))
        return true;
    addr += 4;
    if (!mem_load(addr, 4, ncpsr, false))
        return true;
    if (w)
        wreg(rn, addr + (u32)post[pu & 3]);
    exception_return(npc, ncpsr);
    return true;
}

void ArmCpu::op_cps(u32 imod, bool m, u32 aif, u32 mode)
{
    if (cur_mode() == MODE_USR)
        return; /* a NOP in user mode */
    u32 mask = 0, val = 0;
    if (imod & 2) {
        mask = aif & 0x1C0u;
        if (imod & 1)
            val = mask;
    }
    if (m) {
        mask |= PSR_MODE;
        val |= mode;
    }
    if (mask)
        write_cpsr_insn(val, mask);
}

/* Coprocessor transfers, as QEMU's disas_coproc_insn: MRC/MCR (bit 25 set,
 * bit 4 set; CDP is UNDEF) and MRRC/MCRR - which is also how QEMU reads
 * LDC/STC (any 110x encoding looks up a 64-bit register with opc1 = bits
 * 7:4). cp14/cp15 come from the probed register table; cp10/cp11 are VFP,
 * disabled (FPEXC.EN = 0) except the VMRS/VMSR system registers. */
static const CpRegInfo *cp_lookup(u32 key, CpRegInfo &tmp)
{
    size_t lo = 0, hi = sizeof(CP_REGS) / sizeof(CP_REGS[0]);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (CP_REGS[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < sizeof(CP_REGS) / sizeof(CP_REGS[0]) && CP_REGS[lo].key == key)
        return &CP_REGS[lo];
    u32 cp = key >> 24, is64 = (key >> 20) & 1, opc1 = (key >> 16) & 0xF, crn = (key >> 12) & 0xF;
    u32 crm = (key >> 8) & 0xF, opc2 = (key >> 4) & 0xF, flags = 0;
    if (cp != 15 || is64)
        return 0;
    if (crn == 3)
        flags = CP_WILD_C3_FLAGS;
    else if (crn == 15)
        flags = CP_WILD_C15_FLAGS;
    else if (crn == 10 && (crm == 0 || crm == 1 || crm == 4 || crm == 8))
        flags = CP_WILD_C10_FLAGS;
    else if (crn == 7 && opc1 == 0 && !(crm == 8 && opc2 >= 4))
        flags = CP_WILD_C7_FLAGS;
    else
        return 0;
    tmp.key = key;
    tmp.flags = flags;
    tmp.lo = tmp.hi = 0;
    return &tmp;
}

bool ArmCpu::exec_coproc(u32 insn)
{
    u32 cp = (insn >> 8) & 0xF;
    bool is64 = !(insn & (1u << 25));
    bool isread = (insn >> 20) & 1;
    u32 rt = (insn >> 12) & 0xF;
    bool user = cur_mode() == MODE_USR;
    if ((cp & 0xE) == 0xA) {
        /* VFP: only VMRS/VMSR of FPSID, MVFR0/1 and FPEXC (privileged, they
         * ignore FPEXC.EN); FPSCR and every data instruction need EN */
        if ((insn & 0x0FE00FFFu) != 0x0EE00A10u)
            return false;
        u32 reg = (insn >> 16) & 0xF, v;
        switch (reg) {
        case 0:
            v = 0x410430F0u; /* FPSID */
            break;
        case 6:
            v = 0x11111111u; /* MVFR1 */
            break;
        case 7:
            v = 0x10110222u; /* MVFR0 */
            break;
        case 8:
            v = fpexc_; /* FPEXC */
            break;
        default:
            return false; /* FPSCR (VFP off), MVFR2 (v8), FPINST/FPINST2 (pre-v3) */
        }
        if (user)
            return false;
        if (isread) {
            if (rt == 15)
                cpsr_ = (cpsr_ & 0x0FFFFFFFu) | (v & 0xF0000000u);
            else
                wreg(rt, v);
        } else if (reg == 8) {
            fpexc_ = r_[rt] & 0x40000000u; /* EN only (VFP itself is not modelled) */
        }
        return true;
    }
    if (!is64 && !(insn & 0x10))
        return false; /* CDP */
    u32 key;
    if (is64)
        key = (cp << 24) | (1u << 20) | (((insn >> 4) & 0xF) << 16) | ((insn & 0xF) << 8);
    else
        key = (cp << 24) | (((insn >> 21) & 7) << 16) | (((insn >> 16) & 0xF) << 12) | ((insn & 0xF) << 8)
              | (((insn >> 5) & 7) << 4);
    CpRegInfo tmp;
    const CpRegInfo *ri = cp_lookup(key, tmp);
    if (!ri)
        return false;
    u32 need = isread ? (user ? CPR_R0 : CPR_R1) : (user ? CPR_W0 : CPR_W1);
    if (!(ri->flags & need))
        return false;
    if (ri->flags & CPR_NOP)
        return true; /* ARM_CP_NOP: nothing happens, even to Rt */
    if (!isread)
        return true; /* accepted; register side effects are not modelled */
    if (is64) {
        wreg(rt, ri->lo);
        wreg((insn >> 16) & 0xF, ri->hi);
    } else if (rt == 15) {
        cpsr_ = (cpsr_ & 0x0FFFFFFFu) | (ri->lo & 0xF0000000u); /* APSR_nzcv */
    } else {
        wreg(rt, ri->lo);
    }
    return true;
}
