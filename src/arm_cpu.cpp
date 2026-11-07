#include <stdlib.h>
#include <stdio.h>
#include "arm_cpu.h"

#include <stdlib.h>
#include <string.h>

/* ================================================================= Memory */
Memory::Memory() : owner(0), on_read(0), on_write(0), on_unmapped(0), map_error(false)
{
    host_ = (u8 **)calloc(NPAGES, sizeof(u8 *));
    fast_rd_ = (u8 **)calloc(NPAGES, sizeof(u8 *));
    fast_wr_ = (u8 **)calloc(NPAGES, sizeof(u8 *));
    rd_hook_ = (u8 *)calloc(NPAGES, 1);
    wr_hook_ = (u8 *)calloc(NPAGES, 1);
}

Memory::~Memory()
{
    for (size_t i = 0; i < blocks_.size(); i++)
        free(blocks_[i]);
    free(host_);
    free(fast_rd_);
    free(fast_wr_);
    free(rd_hook_);
    free(wr_hook_);
}

bool Memory::map(u32 base, u32 size)
{
    if (base & (PAGE - 1))
        return false;
    u32 npages = (size + PAGE - 1) / PAGE;
    u8 *buf = (u8 *)calloc((size_t)npages * PAGE, 1);
    if (!buf)
        return false;
    blocks_.push_back(buf);
    for (u32 i = 0; i < npages; i++) {
        u32 page = (base >> 12) + i;
        if (page >= NPAGES) {
            free(buf);
            blocks_.pop_back();
            return false;
        }
        u8 *p = buf + (size_t)i * PAGE;
        host_[page] = p;
        if (!rd_hook_[page])
            fast_rd_[page] = p;
        if (!wr_hook_[page])
            fast_wr_[page] = p;
    }
    regions_.push_back(std::make_pair(base, npages * PAGE));
    return true;
}

bool Memory::is_mapped(u32 addr) const { return host_[addr >> 12] != 0; }

void Memory::add_read_hook(u32 begin, u32 end)
{
    for (u32 a = begin & ~(PAGE - 1); a < end; a += PAGE) {
        u32 p = a >> 12;
        rd_hook_[p] = 1;
        fast_rd_[p] = 0;
    }
}
void Memory::add_write_hook(u32 begin, u32 end)
{
    for (u32 a = begin & ~(PAGE - 1); a < end; a += PAGE) {
        u32 p = a >> 12;
        wr_hook_[p] = 1;
        fast_wr_[p] = 0;
    }
}

u32 Memory::phys_read_slow(u32 addr, int size) const
{
    u32 v = 0;
    for (int i = 0; i < size; i++)
        v |= (u32)phys_read(addr + (u32)i, 1) << (8 * i);
    return v;
}

void Memory::phys_write_slow(u32 addr, int size, u32 value)
{
    for (int i = 0; i < size; i++)
        phys_write(addr + (u32)i, 1, (value >> (8 * i)) & 0xFF);
}

void Memory::phys_read_block(u32 addr, u8 *dst, u32 len) const
{
    for (u32 i = 0; i < len;) {
        u8 *h = host_[(addr + i) >> 12];
        u32 off = (addr + i) & (PAGE - 1);
        u32 chunk = PAGE - off;
        if (chunk > len - i)
            chunk = len - i;
        if (h)
            memcpy(dst + i, h + off, chunk);
        else
            memset(dst + i, 0, chunk);
        i += chunk;
    }
}

void Memory::phys_write_block(u32 addr, const u8 *src, u32 len)
{
    for (u32 i = 0; i < len;) {
        u8 *h = host_[(addr + i) >> 12];
        u32 off = (addr + i) & (PAGE - 1);
        u32 chunk = PAGE - off;
        if (chunk > len - i)
            chunk = len - i;
        if (h)
            memcpy(h + off, src + i, chunk);
        i += chunk;
    }
}

Bytes Memory::read_bytes(u32 addr, u32 len) const
{
    Bytes out(len, '\0');
    if (len)
        phys_read_block(addr, (u8 *)&out[0], len);
    return out;
}
void Memory::write_bytes(u32 addr, const Bytes &data)
{
    if (!data.empty())
        phys_write_block(addr, (const u8 *)data.data(), (u32)data.size());
}
void Memory::fill(u32 addr, u8 value, u32 len)
{
    for (u32 i = 0; i < len;) {
        u8 *h = host_[(addr + i) >> 12];
        u32 off = (addr + i) & (PAGE - 1);
        u32 chunk = PAGE - off;
        if (chunk > len - i)
            chunk = len - i;
        if (h)
            memset(h + off, value, chunk);
        i += chunk;
    }
}

bool Memory::bus_read(u32 addr, int size, u32 &out)
{
    u32 page = addr >> 12;
    if (!host_[page]) {
        if (on_unmapped && on_unmapped(owner, this, addr, size, UNMAPPED_READ)) {
            /* Unicorn (QEMU, 1 KB ARM target pages) cannot complete an access
             * to the last 1 KB of the address space through an on-demand
             * mapping: the hook maps the page, then the access still fails
             * with UC_ERR_MAP. A page that was already mapped works. */
            if (addr >= 0xFFFFFC00u) {
                map_error = true;
                out = 0;
                return false;
            }
            out = phys_read(addr, size);
            return true;
        }
        out = 0;
        return false;
    }
    if ((addr & (PAGE - 1)) + (u32)size > PAGE && !host_[(page + 1) & (NPAGES - 1)]) {
        /* page-crossing access into an unmapped page: QEMU faults on the
         * second page (its start address goes to the unmapped hook) */
        u32 a2 = (addr + PAGE) & ~(PAGE - 1);
        if (!(on_unmapped && on_unmapped(owner, this, a2, size, UNMAPPED_READ))) {
            out = 0;
            return false;
        }
    }
    if (rd_hook_[page] && on_read)
        on_read(owner, this, addr, size);
    out = phys_read(addr, size);
    return true;
}

bool Memory::bus_write(u32 addr, int size, u32 value)
{
    u32 page = addr >> 12;
    if (!host_[page]) {
        if (on_unmapped && on_unmapped(owner, this, addr, size, UNMAPPED_WRITE)) {
            if (addr >= 0xFFFFFC00u) { /* see bus_read */
                map_error = true;
                return false;
            }
            /* a freshly mapped page may still be unhooked: write it */
            if (!wr_hook_[addr >> 12])
                phys_write(addr, size, value);
            else if (on_write)
                on_write(owner, this, addr, size, value);
            return true;
        }
        return false;
    }
    if ((addr & (PAGE - 1)) + (u32)size > PAGE && !host_[(page + 1) & (NPAGES - 1)]) {
        /* page-crossing store into an unmapped page: QEMU stores it byte by
         * byte, so the bytes on the first page land before the fault */
        u32 a2 = (addr + PAGE) & ~(PAGE - 1);
        if (!(on_unmapped && on_unmapped(owner, this, a2, size, UNMAPPED_WRITE))) {
            for (u32 i = 0; addr + i < a2; i++)
                phys_write(addr + i, 1, (value >> (8 * i)) & 0xFF);
            return false;
        }
    }
    if (wr_hook_[page] && on_write) {
        on_write(owner, this, addr, size, value); /* handler does its own backing write */
        return true;
    }
    phys_write(addr, size, value);
    return true;
}

u32 Memory::fetch32_slow(u32 addr, bool &ok) const
{
    u8 *h = host_[addr >> 12];
    if (!h) {
        ok = false;
        return 0;
    }
    ok = true;
    u32 off = addr & (PAGE - 1);
    if (off + 4 <= PAGE)
        return rd32le(h + off);
    if (!host_[((addr >> 12) + 1) & (NPAGES - 1)]) { /* misaligned ARM fetch into a hole */
        ok = false;
        return 0;
    }
    return phys_read(addr, 4);
}

u16 Memory::fetch16_slow(u32 addr, bool &ok) const
{
    u8 *h = host_[addr >> 12];
    if (!h) {
        ok = false;
        return 0;
    }
    ok = true;
    u32 off = addr & (PAGE - 1);
    if (off + 2 <= PAGE)
        return rd16le(h + off);
    return (u16)phys_read(addr, 2);
}

void Memory::regions(std::vector<std::pair<u32, u32> > &out) const { out = regions_; }

/* ================================================================== ArmCpu */
ArmCpu::ArmCpu(Memory &m)
    : vector_base(0), watch_cb(0), on_swi(0), watch_ctx(0), mem(m), cpsr_(0), spsr_(0), stop_(false), fault_addr_(0),
      untilset_(false), until_(0), hit_until_(false)
{
    reset();
}

void ArmCpu::reset()
{
    memset(r_, 0, sizeof(r_));
    /* Unicorn's power-on CPSR for its ARM core: SVC, A/I/F masked, Z set. */
    cpsr_ = 0x400001D3u;
    spsr_ = 0;
    memset(bank_usr_, 0, sizeof(bank_usr_));
    memset(bank_fiq_, 0, sizeof(bank_fiq_));
    memset(bank_irq_, 0, sizeof(bank_irq_));
    memset(bank_svc_, 0, sizeof(bank_svc_));
    memset(bank_abt_, 0, sizeof(bank_abt_));
    memset(bank_und_, 0, sizeof(bank_und_));
    spsr_fiq_ = spsr_irq_ = spsr_svc_ = spsr_abt_ = spsr_und_ = spsr_usr_ = 0;
    excl_valid_ = false;
    excl_addr_ = 0;
    excl_val_ = 0;
    undef_next_ = false;
    undef_next_pc_ = 0;
    it_set_ = false;
    fpexc_ = 0;
    defer_store_ = false;
    pending_err_.clear();
    dry_run_ = false;
    pend_req_ = false;
    branched_ = false;
    stop_ = false;
    error_.clear();
}

/* Save the seven low banked regs of the mode we are leaving, then load the
 * destination mode's. USR and SYS share one bank. */
static int bank_of(u32 mode)
{
    switch (mode) {
    case MODE_FIQ:
        return 1;
    case MODE_IRQ:
        return 2;
    case MODE_SVC:
        return 3;
    case MODE_ABT:
        return 4;
    case MODE_UND:
        return 5;
    default:
        return 0; /* USR / SYS */
    }
}

void ArmCpu::switch_mode(u32 new_mode)
{
    u32 old = cur_mode();
    if (old == new_mode)
        return;
    int ob = bank_of(old), nb = bank_of(new_mode);
    /* store old */
    if (ob == 1) {
        for (int i = 0; i < 7; i++)
            bank_fiq_[i] = r_[8 + i];
    } else if (ob == 0) {
        for (int i = 0; i < 7; i++)
            bank_usr_[i] = r_[8 + i];
    } else {
        /* only sp(r13), lr(r14) banked; r8..r12 are the user bank */
        for (int i = 0; i < 5; i++)
            bank_usr_[i] = r_[8 + i];
        u32 *b = ob == 2 ? bank_irq_ : ob == 3 ? bank_svc_ : ob == 4 ? bank_abt_ : bank_und_;
        b[0] = r_[13];
        b[1] = r_[14];
    }
    /* load new */
    if (nb == 1) {
        for (int i = 0; i < 7; i++)
            r_[8 + i] = bank_fiq_[i];
    } else if (nb == 0) {
        for (int i = 0; i < 7; i++)
            r_[8 + i] = bank_usr_[i];
    } else {
        for (int i = 0; i < 5; i++)
            r_[8 + i] = bank_usr_[i];
        u32 *b = nb == 2 ? bank_irq_ : nb == 3 ? bank_svc_ : nb == 4 ? bank_abt_ : bank_und_;
        r_[13] = b[0];
        r_[14] = b[1];
    }
}

u32 ArmCpu::get_spsr() const
{
    switch (cur_mode()) {
    case MODE_FIQ:
        return spsr_fiq_;
    case MODE_IRQ:
        return spsr_irq_;
    case MODE_SVC:
        return spsr_svc_;
    case MODE_ABT:
        return spsr_abt_;
    case MODE_UND:
        return spsr_und_;
    default:
        return spsr_usr_; /* USR/SYS: QEMU's banked_spsr[0] slot */
    }
}

void ArmCpu::set_spsr(u32 value)
{
    switch (cur_mode()) {
    case MODE_FIQ:
        spsr_fiq_ = value;
        break;
    case MODE_IRQ:
        spsr_irq_ = value;
        break;
    case MODE_SVC:
        spsr_svc_ = value;
        break;
    case MODE_ABT:
        spsr_abt_ = value;
        break;
    case MODE_UND:
        spsr_und_ = value;
        break;
    default:
        spsr_usr_ = value;
        break;
    }
}

static bool valid_mode(u32 m)
{
    return m == MODE_USR || m == MODE_FIQ || m == MODE_IRQ || m == MODE_SVC || m == MODE_ABT || m == MODE_UND
           || m == MODE_SYS;
}

/* QEMU cpsr_write() for guest writes (MSR, CPS, exception return): a mode
 * change from USR, or to a mode this core lacks (MON/HYP/reserved), leaves
 * CPSR.M alone while the other masked bits are still written. */
void ArmCpu::write_cpsr_insn(u32 val, u32 mask)
{
    if ((cpsr_ ^ val) & mask & PSR_MODE) {
        u32 m = val & PSR_MODE;
        if (cur_mode() == MODE_USR || !valid_mode(m))
            mask &= ~PSR_MODE;
        else
            switch_mode(m);
    }
    cpsr_ = (cpsr_ & ~mask) | (val & mask);
}

void ArmCpu::set_cpsr(u32 value, bool privileged)
{
    u32 new_mode = value & PSR_MODE;
    if (privileged) {
        /* Unicorn's reg_write(CPSR): any bits, mode switch even from USR, but
         * an invalid mode number keeps the current mode */
        if (valid_mode(new_mode))
            switch_mode(new_mode);
        else
            value = (value & ~PSR_MODE) | cur_mode();
        cpsr_ = value;
    } else {
        /* user mode: only the flags field is writable */
        cpsr_ = (cpsr_ & ~0xF80F0000u) | (value & 0xF80F0000u);
    }
}

u32 ArmCpu::reg_read(int reg) const
{
    if (reg >= REG_R0 && reg <= REG_PC)
        return r_[reg];
    if (reg == REG_CPSR)
        return cpsr_;
    if (reg == REG_SPSR)
        return get_spsr();
    return 0;
}

void ArmCpu::reg_write(int reg, u32 value)
{
    if (reg >= REG_R0 && reg <= REG_PC)
        r_[reg] = value;
    else if (reg == REG_CPSR)
        set_cpsr(value, true);
    else if (reg == REG_SPSR)
        set_spsr(value);
}

void ArmCpu::context_save(CpuContext &c) const
{
    memcpy(c.r, r_, sizeof(r_));
    c.cpsr = cpsr_;
    /* flush the current mode's live regs into the right bank snapshot */
    u32 mode = cur_mode();
    int b = bank_of(mode);
    u32 usr[7];
    u32 fiq[7];
    for (int i = 0; i < 7; i++) {
        usr[i] = bank_usr_[i];
        fiq[i] = bank_fiq_[i];
    }
    u32 irq[2], svc[2], abt[2], und[2];
    for (int i = 0; i < 2; i++) {
        irq[i] = bank_irq_[i];
        svc[i] = bank_svc_[i];
        abt[i] = bank_abt_[i];
        und[i] = bank_und_[i];
    }
    if (b == 1)
        for (int i = 0; i < 7; i++)
            fiq[i] = r_[8 + i];
    else if (b == 0)
        for (int i = 0; i < 7; i++)
            usr[i] = r_[8 + i];
    else {
        for (int i = 0; i < 5; i++)
            usr[i] = r_[8 + i];
        u32 *dst = b == 2 ? irq : b == 3 ? svc : b == 4 ? abt : und;
        dst[0] = r_[13];
        dst[1] = r_[14];
    }
    c.r8_usr = usr[0];
    c.r9_usr = usr[1];
    c.r10_usr = usr[2];
    c.r11_usr = usr[3];
    c.r12_usr = usr[4];
    c.sp_usr = usr[5];
    c.lr_usr = usr[6];
    c.r8_fiq = fiq[0];
    c.r9_fiq = fiq[1];
    c.r10_fiq = fiq[2];
    c.r11_fiq = fiq[3];
    c.r12_fiq = fiq[4];
    c.sp_fiq = fiq[5];
    c.lr_fiq = fiq[6];
    c.sp_irq = irq[0];
    c.lr_irq = irq[1];
    c.sp_svc = svc[0];
    c.lr_svc = svc[1];
    c.sp_abt = abt[0];
    c.lr_abt = abt[1];
    c.sp_und = und[0];
    c.lr_und = und[1];
    c.spsr_fiq = spsr_fiq_;
    c.spsr_irq = spsr_irq_;
    c.spsr_svc = spsr_svc_;
    c.spsr_abt = spsr_abt_;
    c.spsr_und = spsr_und_;
}

void ArmCpu::context_restore(const CpuContext &c)
{
    memcpy(r_, c.r, sizeof(r_));
    cpsr_ = c.cpsr;
    bank_usr_[0] = c.r8_usr;
    bank_usr_[1] = c.r9_usr;
    bank_usr_[2] = c.r10_usr;
    bank_usr_[3] = c.r11_usr;
    bank_usr_[4] = c.r12_usr;
    bank_usr_[5] = c.sp_usr;
    bank_usr_[6] = c.lr_usr;
    bank_fiq_[0] = c.r8_fiq;
    bank_fiq_[1] = c.r9_fiq;
    bank_fiq_[2] = c.r10_fiq;
    bank_fiq_[3] = c.r11_fiq;
    bank_fiq_[4] = c.r12_fiq;
    bank_fiq_[5] = c.sp_fiq;
    bank_fiq_[6] = c.lr_fiq;
    bank_irq_[0] = c.sp_irq;
    bank_irq_[1] = c.lr_irq;
    bank_svc_[0] = c.sp_svc;
    bank_svc_[1] = c.lr_svc;
    bank_abt_[0] = c.sp_abt;
    bank_abt_[1] = c.lr_abt;
    bank_und_[0] = c.sp_und;
    bank_und_[1] = c.lr_und;
    spsr_fiq_ = c.spsr_fiq;
    spsr_irq_ = c.spsr_irq;
    spsr_svc_ = c.spsr_svc;
    spsr_abt_ = c.spsr_abt;
    spsr_und_ = c.spsr_und;
    /* the live r_[] already holds the current mode's view (saved by whoever
     * built the context from this same CPU); nothing to reload. */
}

bool ArmCpu::take_irq()
{
    /* mirror of trigger_irq: the emulator checks the ICU global enable first */
    u32 cpsr = cpsr_;
    if (cpsr & PSR_I)
        return false;
    u32 pc = r_[15];
    u32 new_cpsr = (cpsr & ~0x3Fu) | 0x92; /* IRQ mode, I set, Thumb cleared */
    switch_mode(MODE_IRQ);
    cpsr_ = new_cpsr;
    set_spsr(cpsr);
    r_[14] = pc + 4;
    r_[15] = vector_base + 0x18;
    return true;
}

bool ArmCpu::take_fiq()
{
    u32 cpsr = cpsr_;
    if (cpsr & PSR_F)
        return false;
    u32 pc = r_[15];
    u32 new_cpsr = (cpsr & ~0x3Fu) | 0x11 | 0xC0; /* FIQ mode, F+I set */
    switch_mode(MODE_FIQ);
    cpsr_ = new_cpsr;
    set_spsr(cpsr);
    r_[14] = pc + 4;
    r_[15] = vector_base + 0x1C;
    return true;
}

bool ArmCpu::invalid(u32 pc, u32 insn)
{
    error_ = strf("Invalid instruction (UC_ERR_INSN_INVALID)");
    fault_addr_ = pc;
    (void)insn;
    return false;
}

RunResult ArmCpu::run(u32 start, u32 until, u64 count)
{
    stop_ = false;
    hit_until_ = false;
    error_.clear();
    untilset_ = (until != 0xFFFFFFFFu);
    until_ = until & ~1u;
    /* bit 0 of the start address selects Thumb state (Unicorn convention) */
    if (start & 1)
        cpsr_ |= PSR_T;
    else
        cpsr_ &= ~PSR_T;
    r_[15] = start & ~1u;
    /* IT bits with IT[3:0] = 0 (e.g. left by an exception return into ARM
     * state) are dropped by QEMU at the next TB start; step_thumb handles the
     * Thumb case per instruction, ARM state does it here, off the hot path */
    if ((cpsr_ & 0x0600FC00u) && !(cpsr_ & 0x06000C00u) && mem.is_mapped(r_[15]))
        cpsr_ &= ~0x0600FC00u; /* (not when the first fetch faults: no TB starts) */

    static int pclog = -1;
    if (pclog < 0)
        pclog = getenv("EMU_PCLOG") ? 1 : 0;
    static u32 ring[64];
    static int ring_i = 0;
    bool has_watch = !watch_pcs.empty() && watch_cb != 0;
    if (!untilset_ && !has_watch && !pclog) {
        for (u64 i = 0; i < count; i++) {
            if (stop_)
                return RUN_STOPPED;
            bool cont = thumb() ? step_thumb() : step_arm();
            if (!cont) {
                if (hit_until_)
                    return RUN_HIT_UNTIL;
                if (stop_)
                    return RUN_STOPPED;
                return error_.empty() ? RUN_STOPPED : RUN_ERR_INVALID;
            }
        }
    } else {
        for (u64 i = 0; i < count; i++) {
            if (stop_)
                return RUN_STOPPED;
            if (untilset_ && r_[15] == until_) {
                return RUN_HIT_UNTIL;
            }
            if (has_watch) {
                u32 wpc = r_[15];
                for (size_t w = 0; w < watch_pcs.size(); w++)
                    if (watch_pcs[w] == wpc) {
                        watch_cb(watch_ctx, wpc);
                        break;
                    }
            }
            if (pclog) {
                if (r_[15] < 0x8000 && ring[(ring_i + 63) % 64] >= 0x10000) {
                    fprintf(stderr, "PCLOG: entered 0x%08x lr=%08x cpsr=%08x; recent:\n", r_[15], r_[14], cpsr_);
                    for (int k = 0; k < 64; k++)
                        fprintf(stderr, " %08x", ring[(ring_i + k) % 64]);
                    fprintf(stderr, "\n");
                    pclog = 0;
                }
                ring[ring_i] = r_[15];
                ring_i = (ring_i + 1) % 64;
            }
            bool cont = thumb() ? step_thumb() : step_arm();
            if (!cont) {
                if (hit_until_)
                    return RUN_HIT_UNTIL;
                if (stop_)
                    return RUN_STOPPED;
                return error_.empty() ? RUN_STOPPED : RUN_ERR_INVALID;
            }
        }
    }
    /* Unicorn never stops a counted run inside an IT block (the code hook
     * carries UC_HOOK_FLAG_NO_STOP there): finish the block first. */
    for (int k = 0; k < 8 && thumb() && (cpsr_ & 0x06000C00u); k++) { /* (a stalled insn
                                                                         * would spin Unicorn forever) */
        if (untilset_ && r_[15] == until_)
            return RUN_HIT_UNTIL;
        if (!step_thumb())
            return error_.empty() ? RUN_STOPPED : RUN_ERR_INVALID;
    }
    return RUN_COUNT_DONE;
}

/* --------------------------------------------------------- shifter / mem */
u32 ArmCpu::shift_c_slow(u32 type, u32 value, u32 amount, bool &carry, bool imm_form) const
{
    /* carry is in/out: input = current C flag */
    switch (type) {
    case 0: /* LSL */
        if (amount == 0)
            return value;
        if (amount < 32) {
            carry = ((value >> (32 - amount)) & 1) != 0;
            return value << amount;
        }
        if (amount == 32)
            carry = (value & 1) != 0;
        else
            carry = false;
        return 0;
    case 1: /* LSR */
        if (amount == 0) {
            if (imm_form) { /* LSR #0 means LSR #32 */
                carry = (value & 0x80000000u) != 0;
                return 0;
            }
            return value;
        }
        if (amount < 32) {
            carry = ((value >> (amount - 1)) & 1) != 0;
            return value >> amount;
        }
        if (amount == 32)
            carry = (value & 0x80000000u) != 0;
        else
            carry = false;
        return 0;
    case 2: /* ASR */
        if (amount == 0) {
            if (imm_form) {
                carry = (value & 0x80000000u) != 0;
                return (value & 0x80000000u) ? 0xFFFFFFFFu : 0;
            }
            return value;
        }
        if (amount < 32) {
            carry = ((value >> (amount - 1)) & 1) != 0;
            return (u32)((s32)value >> amount);
        }
        carry = (value & 0x80000000u) != 0;
        return (value & 0x80000000u) ? 0xFFFFFFFFu : 0;
    default: /* ROR / RRX */
        if (amount == 0) {
            if (imm_form) { /* RRX */
                bool old_c = carry;
                carry = (value & 1) != 0;
                return (value >> 1) | (old_c ? 0x80000000u : 0);
            }
            return value;
        }
        {
            u32 a = amount & 31;
            if (a == 0) {
                carry = (value & 0x80000000u) != 0;
                return value;
            }
            carry = ((value >> (a - 1)) & 1) != 0;
            return (value >> a) | (value << (32 - a));
        }
    }
}

bool ArmCpu::mem_load_slow(u32 addr, int size, u32 &out, bool sign)
{
    u8 *p = mem.fast_ptr(addr, size, false);
    if (p) {
        if (size == 1)
            out = p[0];
        else if (size == 2)
            out = rd16le(p);
        else
            out = rd32le(p);
    } else {
        if (!mem.bus_read(addr, size, out)) {
            error_ = mem.map_error ? "Invalid memory mapping (UC_ERR_MAP)"
                                   : "Invalid memory read (UC_ERR_READ_UNMAPPED)";
            mem.map_error = false;
            fault_addr_ = addr;
            return false;
        }
    }
    if (cpsr_ & PSR_E) /* CPSR.E: big-endian data accesses (BE8) */
        out = size == 4 ? ((out >> 24) | ((out >> 8) & 0xFF00u) | ((out << 8) & 0xFF0000u) | (out << 24))
                        : size == 2 ? (((out >> 8) & 0xFFu) | ((out & 0xFFu) << 8)) : out;
    if (sign) {
        if (size == 1)
            out = (u32)(s32)(s8)(u8)out;
        else if (size == 2)
            out = (u32)(s32)(s16)(u16)out;
    }
    return true;
}

bool ArmCpu::mem_store_slow(u32 addr, int size, u32 value)
{
    if (cpsr_ & PSR_E) { /* CPSR.E: big-endian data accesses (BE8) */
        if (dry_run_) /* dry_run_undef sets E as well: one test on the fast path */
            return true;
        value = size == 4 ? ((value >> 24) | ((value >> 8) & 0xFF00u) | ((value << 8) & 0xFF0000u) | (value << 24))
                          : size == 2 ? (((value >> 8) & 0xFFu) | ((value & 0xFFu) << 8)) : value;
    }
    u8 *p = mem.fast_ptr(addr, size, true);
    if (p) {
        if (size == 1)
            p[0] = (u8)value;
        else if (size == 2)
            wr16le(p, (u16)value);
        else
            wr32le(p, value);
        return true;
    }
    if (!mem.bus_write(addr, size, value)) {
        const char *e = mem.map_error ? "Invalid memory mapping (UC_ERR_MAP)"
                                      : "Invalid memory write (UC_ERR_WRITE_UNMAPPED)";
        mem.map_error = false;
        if (defer_store_) { /* IT block: skip the store, raise it after the block */
            if (pending_err_.empty())
                pending_err_ = e;
            pend_req_ = true; /* cpu_exit(): honoured at the next TB start */
            return true;
        }
        error_ = e;
        fault_addr_ = addr;
        return false;
    }
    return true;
}
