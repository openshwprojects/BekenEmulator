#include "xtensa_cpu.h"
#include <string.h>
#include <math.h>
#include <fenv.h>

namespace {
const u32 PS_EXCM = 0x10, PS_WOE = 0x40000;
const unsigned timer_irqs[3] = {6,15,16};
const u32 lx6_level_mask[8] = {0,0x000637ff,0x00380000,0x28c08800,0x53000000,0x84010000,0,0x00004000};
const u32 b4const[16] = {0xffffffffu,1,2,3,4,5,6,7,8,10,12,16,32,64,128,256};
const u32 b4constu[16] = {32768,65536,2,3,4,5,6,7,8,10,12,16,32,64,128,256};
u32 sext(u32 value, unsigned bits) {
    const u32 sign = 1u << (bits - 1);
    return (value ^ sign) - sign;
}
u32 asr(u32 value, unsigned amount) {
    if (!amount) return value;
    if (amount >= 32) return (value & 0x80000000u) ? 0xffffffffu : 0;
    return (value >> amount) | ((value & 0x80000000u) ? (~0u << (32 - amount)) : 0);
}
unsigned clz(u32 value) {
    unsigned n = 0;
    if (!value) return 32;
    while (!(value & 0x80000000u)) { ++n; value <<= 1; }
    return n;
}
bool compare(u32 a, u32 b, unsigned condition) {
    switch (condition) {
    case 0: return a == b;
    case 1: return a != b;
    case 2: return (s32)a < (s32)b;
    default: return (s32)a >= (s32)b;
    }
}
float bits_float(u32 bits) { float f; memcpy(&f,&bits,4); return f; }
u32 float_bits(float f) { u32 bits; memcpy(&bits,&f,4); return bits; }
/* Isolate each guest operation's rounding and sticky flags from the host.
 * Volatile operands prevent compile-time folding across guest FCR changes. */
struct FloatEnvironment {
    fenv_t saved;
    FloatEnvironment(unsigned rounding) {
        feholdexcept(&saved);
        static const int modes[4]={FE_TONEAREST,FE_TOWARDZERO,FE_UPWARD,FE_DOWNWARD};
        fesetround(modes[rounding&3]);
    }
    ~FloatEnvironment() { fesetenv(&saved); }
    u32 flags() const {
        int flags=fetestexcept(FE_ALL_EXCEPT);
        return ((flags&FE_INEXACT)?0x80:0) | ((flags&FE_UNDERFLOW)?0x100:0) |
            ((flags&FE_OVERFLOW)?0x200:0) | ((flags&FE_DIVBYZERO)?0x400:0) | ((flags&FE_INVALID)?0x800:0);
    }
};
}

XtensaCpu::XtensaCpu(XtensaBus &bus, bool windowed) : bus_(bus), windowed_(windowed), processor_id_(windowed?0xcdcd:0) { reset(0); }

void XtensaCpu::reset(u32 pc) {
    memset(ar_, 0, sizeof(ar_));
    memset(sr_, 0, sizeof(sr_));
    memset(ur_, 0, sizeof(ur_));
    memset(fr_, 0, sizeof(fr_));
    memset(instruction_attr_, 2, sizeof(instruction_attr_));
    memset(data_attr_, 2, sizeof(data_attr_));
    pc_ = pc; instruction_ = fault_address_ = 0; instructions_ = 0; pending_call_ = 0;
    waiting_ = false;
    sr_[XT_SR_WINDOWSTART] = 1;
    sr_[XT_SR_PS] = windowed_ ? PS_WOE : 0;
    sr_[XT_SR_VECBASE] = 0x40000000;
    /* Fixed IDs from Espressif's LX106/LX6 core-isa configuration headers. */
    sr_[176] = windowed_ ? 0xc2bcfffeu : 0xc28cdafau;
    sr_[208] = windowed_ ? 0x1cc5fe96u : 0x1082b6f6u;
    sr_[XT_SR_PRID] = processor_id_;
}

u32 XtensaCpu::reg(unsigned index) const {
    return ar_[((windowed_ ? sr_[XT_SR_WINDOWBASE] * 4 : 0) + (index & 15)) & 63];
}
void XtensaCpu::set_reg(unsigned index, u32 value) {
    ar_[((windowed_ ? sr_[XT_SR_WINDOWBASE] * 4 : 0) + (index & 15)) & 63] = value;
}
void XtensaCpu::set_special(unsigned index, u32 value) {
    index &= 255;
    if(index==XT_SR_PRID || index==176 || index==208)return; // hardwired identification registers
    if (index == XT_SR_SAR) value &= 63;
    if (index == XT_SR_WINDOWBASE) value &= 15;
    if (index == XT_SR_WINDOWSTART || index == XT_SR_BR) value &= 0xffff;
    if (index == 17) value = sext(value & 255,8); // ACCHI is signed 8 bits (40-bit accumulator).
    if (index == XT_SR_INTSET) { sr_[XT_SR_INTERRUPT] |= value & (windowed_ ? 0x20000080u : 0x80u); return; }
    if (index == XT_SR_INTCLEAR) { sr_[XT_SR_INTERRUPT] &= ~(value & (windowed_ ? 0x70404480u : 0x7f80u)); return; }
    if (index >= 240 && index < 240 + (windowed_ ? 3u : 1u))
        sr_[XT_SR_INTERRUPT] &= ~(1u << timer_irqs[index-240]);
    sr_[index] = value;
}
void XtensaCpu::set_user(unsigned index,u32 value) {
    index &= 255;
    if (index == 232) value &= 0x7f;
    if (index == 233) value &= 0xf80;
    ur_[index] = value;
}

XtensaResult XtensaCpu::float_operation(u32 insn,u32 &next,bool &jump) {
    if (!windowed_) return XTENSA_INVALID;
    const unsigned t=(insn>>4)&15,s=(insn>>8)&15,r=(insn>>12)&15,group=(insn>>16)&15,op=insn>>20;
    if (group==10 ? (op==3 || (op==15 && (t==2 || (t==3 && s>=4)))) : (group!=11 || op<1 || op>13)) return XTENSA_INVALID;
    if (!(sr_[224]&1)) { sr_[XT_SR_EXCCAUSE]=32; return XTENSA_EXCEPTION; }
    unsigned highest=0;
    if (group == 10) {
        if ((op >= 8 && op <= 11) || op == 14 || (op == 15 && t == 4)) highest=r;
        else if (op == 12 || op == 13 || (op == 15 && t == 5)) highest=s;
    } else if (op >= 8 && op <= 11) highest=t;
    if (!check_window(highest)) { next=pc_; jump=true; return XTENSA_OK; }
    FloatEnvironment env(ur_[232]&3);
    volatile float x=bits_float(fr_[s]), y=bits_float(fr_[t]);
    float result=0;
    bool write_float=false;
    if (group == 10) {
        if (op == 0) { result=x+y; write_float=true; }
        else if (op == 1) { result=x-y; write_float=true; }
        else if (op == 2) { result=x*y; write_float=true; }
        else if (op == 4 || op == 5) { result=fmaf(op==5?-x:x,y,bits_float(fr_[r])); write_float=true; }
        /* The division/sqrt refinement sequence is evaluated exactly by
         * MKDADJ/MKSADJ. Intermediate estimates carry no independent result;
         * this is the same sequence interpretation as upstream QEMU. */
        else if (op == 6 || op == 7) {}
        else if ((op >= 8 && op <= 11) || op == 14) {
            double scaled=ldexp(double(x),t), integral=0;
            if (op==8) { fesetround(FE_TONEAREST); integral=rint(scaled); }
            else if (op==9 || op==14) integral=scaled<0?ceil(scaled):floor(scaled);
            else integral=op==10?floor(scaled):ceil(scaled);
            const bool nan=scaled!=scaled;
            if (op==14) {
                if (nan || integral>4294967295.0 || integral<0) {
                    set_reg(r,!nan && integral<0 ? 0 : 0xffffffffu); ur_[233]|=0x800;
                } else set_reg(r,u32(integral));
            } else {
                if (nan || integral>2147483647.0 || integral<-2147483648.0) {
                    set_reg(r,!nan && integral<0 ? 0x80000000u : 0x7fffffffu); ur_[233]|=0x800;
                } else set_reg(r,u32(s32(integral)));
            }
            if (!nan && scaled!=integral) ur_[233]|=0x80;
        } else if (op == 12 || op == 13) {
            volatile double source=op==12?double(s32(reg(s))):double(reg(s));
            result=float(ldexp(source,-int(t))); write_float=true;
        } else if (op == 15) {
            switch (t) {
            case 0: case 15: fr_[r]=fr_[s]; break;
            case 1: fr_[r]=fr_[s]&0x7fffffffu; break;
            case 3: {
                static const u32 constants[4]={0,0x3f800000,0x40000000,0x3f000000};
                if (s>=4) return XTENSA_INVALID;
                fr_[r]=constants[s]; break;
            }
            case 4: set_reg(r,fr_[s]); break;
            case 5: fr_[r]=reg(s); break;
            case 6: fr_[r]=fr_[s]^0x80000000u; break;
            case 7: case 8: case 9: case 10: case 11: case 14: break;
            case 12: result=sqrtf(x); write_float=true; break;
            case 13: { volatile float divisor=bits_float(fr_[r]); result=x/divisor; write_float=true; break; }
            default: return XTENSA_INVALID;
            }
        } else return XTENSA_INVALID;
    } else if (group == 11) {
        if (op>=1 && op<=7) {
            bool unordered=x!=x || y!=y;
            bool value=op==1?unordered : op<=3?x==y : op<=5?x<y : x<=y;
            if (op&1) value=value||unordered;
            sr_[XT_SR_BR]=(sr_[XT_SR_BR]&~(1u<<r))|(u32(value)<<r);
        } else if (op>=8 && op<=11) { if (compare(reg(t),0,op-8)) fr_[r]=fr_[s]; }
        else if (op==12 || op==13) { if (((sr_[XT_SR_BR]>>t)&1)==(op&1)) fr_[r]=fr_[s]; }
        else return XTENSA_INVALID;
    } else return XTENSA_INVALID;
    if (write_float) fr_[r]=::float_bits(result);
    ur_[233]|=env.flags();
    return XTENSA_OK;
}

u32 XtensaCpu::rom_argument(unsigned index) const { return reg(pending_call_ * 4 + 2 + index); }
void XtensaCpu::rom_return(u32 value) {
    const u32 target = reg(pending_call_ * 4);
    set_reg(pending_call_ * 4 + 2, value);
    pc_ = pending_call_ ? ((pc_ & 0xc0000000u) | (target & 0x3fffffffu)) : target;
    waiting_ = false;
    pending_call_ = 0;
}

bool XtensaCpu::interrupt(unsigned level, u32 vector) {
    unsigned masked = sr_[XT_SR_PS] & 15;
    const unsigned exception_level = windowed_ ? 3 : 1;
    if ((sr_[XT_SR_PS] & PS_EXCM) && masked < exception_level) masked = exception_level;
    const unsigned nmi = windowed_ ? 7 : 3;
    if (level < 1 || level > nmi || (level <= masked && level != nmi)) return false;
    sr_[XT_SR_EPC1 + level - 1] = pc_;
    if (level == 1) { sr_[XT_SR_PS] |= PS_EXCM; sr_[XT_SR_EXCCAUSE] = 4; }
    else { sr_[192 + level] = sr_[XT_SR_PS]; sr_[XT_SR_PS] = (sr_[XT_SR_PS] & ~15u) | level | PS_EXCM; }
    pc_ = vector;
    waiting_ = false;
    return true;
}

void XtensaCpu::enter_exception(unsigned cause, u32 address, bool set_address) {
    u32 vector;
    if (sr_[XT_SR_PS] & PS_EXCM) {
        sr_[XT_SR_DEPC] = pc_;
        vector = windowed_ ? 0x3c0 : 0x70;
    } else {
        sr_[XT_SR_EPC1] = pc_;
        const bool user_mode = (sr_[XT_SR_PS] & 0x20) != 0;
        vector = windowed_ ? (user_mode ? 0x340 : 0x300) : (user_mode ? 0x50 : 0x30);
    }
    sr_[XT_SR_EXCCAUSE] = cause;
    if (set_address) sr_[XT_SR_EXCVADDR] = address;
    sr_[XT_SR_PS] |= PS_EXCM;
    pc_ = sr_[XT_SR_VECBASE] + vector;
    waiting_ = false;
}

void XtensaCpu::set_irq(unsigned index, bool active) {
    if (index >= (windowed_ ? 32u : 15u)) return;
    if (active) sr_[XT_SR_INTERRUPT] |= 1u << index;
    else sr_[XT_SR_INTERRUPT] &= ~(1u << index);
}

bool XtensaCpu::service_interrupts() {
    const unsigned nmi = windowed_ ? 7 : 3;
    const u32 pending = sr_[XT_SR_INTERRUPT] & sr_[XT_SR_INTENABLE];
    unsigned level = 0;
    if (sr_[XT_SR_INTERRUPT] & (1u << 14)) level = nmi;
    else if (windowed_) {
        for (unsigned i=6; i>0; --i) if (pending & lx6_level_mask[i]) { level=i; break; }
    } else if (pending & 0x3fff) level = 1;
    if (!level) return false;
    u32 vector;
    if (level == 1) {
        vector = windowed_ ? ((sr_[XT_SR_PS] & 0x20) ? 0x340 : 0x300) : ((sr_[XT_SR_PS] & 0x20) ? 0x50 : 0x30);
    } else vector = windowed_ ? 0x180+(level-2)*0x40 : (level-1)*0x10;
    if (!interrupt(level,sr_[XT_SR_VECBASE]+vector)) return false;
    if (level == nmi) sr_[XT_SR_INTERRUPT] &= ~(1u << 14);
    return true;
}

void XtensaCpu::advance_cycles(u32 cycles) {
    const u32 previous = sr_[XT_SR_CCOUNT];
    sr_[XT_SR_CCOUNT] += cycles;
    for (unsigned i=0; i<(windowed_ ? 3u : 1u); ++i) {
        const u32 distance = sr_[240+i]-previous;
        if (distance && distance <= cycles) sr_[XT_SR_INTERRUPT] |= 1u << timer_irqs[i];
    }
}

bool XtensaCpu::cycles_to_next_timer(u32 &cycles) const {
    bool found = false;
    cycles = 0xffffffffu;
    unsigned mask=sr_[XT_SR_PS]&15;
    if((sr_[XT_SR_PS]&PS_EXCM) && mask<(windowed_?3u:1u)) mask=windowed_?3:1;
    for (unsigned i=0; i<(windowed_ ? 3u : 1u); ++i) {
        const u32 distance = sr_[240+i]-sr_[XT_SR_CCOUNT];
        if (1+i*2>mask && (sr_[XT_SR_INTENABLE] & (1u << timer_irqs[i])) && distance && (!found || distance < cycles)) {
            cycles = distance; found = true;
        }
    }
    return found;
}

bool XtensaCpu::access_permitted(u32 addr, bool instruction, bool store) {
    /* LX106/LX6 use eight fixed 512-MiB protection regions. The ESP flash
     * cache MMU is a separate peripheral, implemented by the machine bus. */
    const unsigned attr = (instruction ? instruction_attr_ : data_attr_)[addr >> 29];
    const bool permitted = instruction ? (attr >= 1 && attr <= 5) : (attr <= 2 || attr == 4 || attr == 5 || attr == 14);
    if (!permitted) {
        fault_address_ = addr; sr_[XT_SR_EXCVADDR] = addr;
        sr_[XT_SR_EXCCAUSE] = instruction ? 20 : store ? 29 : 28;
    }
    return permitted;
}

XtensaResult XtensaCpu::data_read(u32 addr, unsigned size, u32 &value) {
    if (!access_permitted(addr,false,false)) return XTENSA_EXCEPTION;
    if (!windowed_ && (addr & (size - 1))) { fault_address_ = addr; sr_[XT_SR_EXCCAUSE] = 9; sr_[XT_SR_EXCVADDR] = addr; return XTENSA_EXCEPTION; }
    if (!bus_.read(addr, size, value, false)) { fault_address_ = addr; return XTENSA_MEMORY_FAULT; }
    return XTENSA_OK;
}
XtensaResult XtensaCpu::data_write(u32 addr, unsigned size, u32 value) {
    if (!access_permitted(addr,false,true)) return XTENSA_EXCEPTION;
    if (!windowed_ && (addr & (size - 1))) { fault_address_ = addr; sr_[XT_SR_EXCCAUSE] = 9; sr_[XT_SR_EXCVADDR] = addr; return XTENSA_EXCEPTION; }
    if (!bus_.write(addr, size, value)) { fault_address_ = addr; return XTENSA_MEMORY_FAULT; }
    return XTENSA_OK;
}

void XtensaCpu::call(unsigned increment, u32 return_pc) {
    pending_call_ = increment;
    if (increment) {
        sr_[XT_SR_PS] = (sr_[XT_SR_PS] & ~0x30000u) | (increment << 16);
        set_reg(increment * 4, (return_pc & 0x3fffffffu) | (increment << 30));
    } else set_reg(0, return_pc);
}

/* Window exceptions execute the firmware's real spill/fill vectors. The
 * physical register file is never silently expanded or auto-saved. */
bool XtensaCpu::check_window(unsigned highest) {
    if (!windowed_ || (sr_[XT_SR_PS] & (PS_WOE | PS_EXCM)) != PS_WOE) return true;
    const unsigned wb = sr_[XT_SR_WINDOWBASE];
    for (unsigned n = 1; n <= highest / 4; ++n) {
        if (sr_[XT_SR_WINDOWSTART] & (1u << ((wb + n) & 15))) {
            unsigned distance = 1;
            while (distance < 3 && !(sr_[XT_SR_WINDOWSTART] & (1u << ((wb + n + distance) & 15)))) ++distance;
            sr_[XT_SR_WINDOWBASE] = (wb + n) & 15;
            sr_[XT_SR_PS] = (sr_[XT_SR_PS] & ~0xf00u) | (wb << 8) | PS_EXCM;
            sr_[XT_SR_EPC1] = pc_;
            pc_ = sr_[XT_SR_VECBASE] + (distance - 1) * 0x80;
            return false;
        }
    }
    return true;
}

XtensaResult XtensaCpu::retw(u32 &next) {
    if (!windowed_) return XTENSA_INVALID;
    const unsigned n = reg(0) >> 30, wb = sr_[XT_SR_WINDOWBASE];
    if (!n || (sr_[XT_SR_PS] & (PS_WOE | PS_EXCM)) != PS_WOE) return XTENSA_INVALID;
    for (unsigned distance=1; distance<=3; ++distance) {
        if (sr_[XT_SR_WINDOWSTART] & (1u << ((wb-distance)&15))) {
            if (distance != n) return XTENSA_INVALID;
            break;
        }
    }
    next = (pc_ & 0xc0000000u) | (reg(0) & 0x3fffffffu);
    sr_[XT_SR_WINDOWBASE] = (wb - n) & 15;
    if (!(sr_[XT_SR_WINDOWSTART] & (1u << sr_[XT_SR_WINDOWBASE]))) {
        sr_[XT_SR_PS] = (sr_[XT_SR_PS] & ~0xf00u) | (wb << 8) | PS_EXCM;
        sr_[XT_SR_EPC1] = pc_;
        next = sr_[XT_SR_VECBASE] + (n - 1) * 0x80 + 0x40;
    } else sr_[XT_SR_WINDOWSTART] &= ~(1u << wb);
    return XTENSA_OK;
}

XtensaResult XtensaCpu::step() {
    service_interrupts();
    if (waiting_) return XTENSA_WAIT;
    u32 a, b, c = 0;
    instruction_ = 0;
    if (!access_permitted(pc_,true,false)) return XTENSA_EXCEPTION;
    if (!bus_.read(pc_, 1, a, true)) { fault_address_ = pc_; return XTENSA_MEMORY_FAULT; }
    instruction_ = a;
    if (!bus_.read(pc_ + 1, 1, b, true)) { fault_address_ = pc_ + 1; return XTENSA_MEMORY_FAULT; }
    instruction_ |= b << 8;
    const unsigned length = (a & 15) >= 8 ? 2 : 3;
    if (length == 3) {
        if (!bus_.read(pc_ + 2, 1, c, true)) { fault_address_ = pc_ + 2; return XTENSA_MEMORY_FAULT; }
        instruction_ |= c << 16;
    }
    u32 next = pc_ + length;
    bool jump = false;
    const XtensaResult result = execute(instruction_, length, next, jump);
    if (result != XTENSA_OK && result != XTENSA_WAIT) return result;
    ++instructions_;
    advance_cycles(1);
    if (!jump && !(sr_[XT_SR_PS] & PS_EXCM) && next == sr_[XT_SR_LEND] && sr_[XT_SR_LCOUNT]) {
        --sr_[XT_SR_LCOUNT]; next = sr_[XT_SR_LBEG];
    }
    pc_ = next;
    if (result == XTENSA_WAIT && service_interrupts()) return XTENSA_OK;
    return result;
}

XtensaResult XtensaCpu::run(u64 count) {
    while (count--) { const XtensaResult r = step(); if (r != XTENSA_OK) return r; }
    return XTENSA_OK;
}

XtensaResult XtensaCpu::execute(u32 insn, unsigned length, u32 &next, bool &jump) {
    const unsigned op0 = insn & 15, t = (insn >> 4) & 15, s = (insn >> 8) & 15,
        r = (insn >> 12) & 15, op1 = (insn >> 16) & 15, op2 = (insn >> 20) & 15,
        imm8 = insn >> 16, n = t & 3, m = t >> 2;
    const u32 av = reg(s), bv = reg(t);
    u32 value = 0, address = 0;
    XtensaResult result;
    bool condition = false;
    /* Operand-aware window checks are below, before any architectural write. */
#define WC1(a) do { if (!check_window(a)) { next = pc_; jump = true; return XTENSA_OK; } } while (0)
#define WC2(a,b) WC1((a) > (b) ? (a) : (b))
#define WC3(a,b,c) WC1((a) > (b) ? ((a) > (c) ? (a) : (c)) : ((b) > (c) ? (b) : (c)))
#define LOAD(at,addr,sz) do { result = data_read(addr, sz, value); if (result != XTENSA_OK) return result; set_reg(at, value); } while (0)
#define STORE(at,addr,sz) do { result = data_write(addr, sz, reg(at)); if (result != XTENSA_OK) return result; } while (0)
    if (length == 2) {
        switch (op0) {
        case 8: WC2(s,t); LOAD(t, av + r * 4, 4); break;
        case 9: WC2(s,t); STORE(t, av + r * 4, 4); break;
        case 10: WC3(r,s,t); set_reg(r, av + bv); break;
        case 11: WC2(r,s); set_reg(r, av + (t ? t : ~0u)); break;
        case 12:
            WC1(s);
            if (t < 8) set_reg(s, r | (t << 4) | ((t & 6) == 6 ? 0xffffff80u : 0));
            else if ((av != 0) == ((t & 4) != 0)) { next = pc_ + 4 + r + ((t & 3) << 4); jump = true; }
            break;
        case 13:
            if (r == 0) { WC2(s,t); set_reg(t, av); }
            else if (r == 15) {
                if (t == 0) { next = reg(0); jump = true; }
                else if (t == 1) { result = retw(next); jump = true; return result; }
                else if (t == 2) return XTENSA_BREAK;
                else if (t != 3) return XTENSA_INVALID;
            } else return XTENSA_INVALID;
            break;
        default: return XTENSA_INVALID;
        }
        return XTENSA_OK;
    }
    switch (op0) {
    case 0:
        switch (op1) {
        case 0:
            if (op2 >= 8) {
                WC3(r,s,t);
                value = av << (op2 & 3);
                set_reg(r, (op2 & 4) ? value - bv : value + bv);
            } else if (op2 >= 1 && op2 <= 3) {
                WC3(r,s,t); set_reg(r, op2 == 1 ? av & bv : op2 == 2 ? av | bv : av ^ bv);
            } else if (op2 == 5) {
                WC2(s,t);
                u8 *attributes = (r & 8) ? data_attr_ : instruction_attr_;
                switch (r & 7) {
                case 3: set_reg(t,av & 0xe0000000u); break;
                case 4: break; // Fixed region entries cannot be invalidated.
                case 5: set_reg(t,(av & 0xe0000000u) | 1); break;
                case 6: attributes[av >> 29] = bv & 15; break;
                case 7: set_reg(t,(av & 0xe0000000u) | attributes[av >> 29]); break;
                default: return XTENSA_INVALID;
                }
            } else if (op2 == 6 && s < 2) {
                WC2(r,t); set_reg(r, s && !(bv & 0x80000000u) ? bv : 0u - bv);
            } else if (op2 == 4) {
                switch (r) {
                case 0: WC1(s); sr_[XT_SR_SAR] = av & 31; break;
                case 1: WC1(s); sr_[XT_SR_SAR] = 32 - (av & 31); break;
                case 2: WC1(s); sr_[XT_SR_SAR] = (av & 3) * 8; break;
                case 3: WC1(s); sr_[XT_SR_SAR] = 32 - (av & 3) * 8; break;
                case 4: sr_[XT_SR_SAR] = s | ((t & 1) << 4); break;
                case 6:
                    WC2(s,t);
                    if(!bus_.external_read(av,value)) { fault_address_=av;return XTENSA_MEMORY_FAULT; }
                    set_reg(t,value);break;
                case 7:
                    WC2(s,t);
                    if(!bus_.external_write(av,bv)) { fault_address_=av;return XTENSA_MEMORY_FAULT; }
                    break;
                case 8: if (!windowed_) return XTENSA_INVALID; sr_[XT_SR_WINDOWBASE] = (sr_[XT_SR_WINDOWBASE] + sext(t,4)) & 15; break;
                case 14: case 15: WC2(s,t); value = r == 14 && (av & 0x80000000u) ? ~av : av; value = clz(value); set_reg(t, r == 14 && value ? value - 1 : value); break;
                default: return XTENSA_INVALID;
                }
            } else if (op2 == 0) {
                switch (r) {
                case 0:
                    if (m == 2 && (n == 0 || n == 2)) { WC1(s); next = av; jump = true; }
                    else if (m == 2 && n == 1) { result = retw(next); jump = true; return result; }
                    else if (m == 3) { if (n && !windowed_) return XTENSA_INVALID; WC2(s,n*4); call(n,next); next = av; jump = true; }
                    else return XTENSA_INVALID;
                    break;
                case 1:
                    if (!windowed_) return XTENSA_INVALID;
                    WC2(s,t);
                    {
                        unsigned wb = sr_[XT_SR_WINDOWBASE];
                        if (!(sr_[XT_SR_WINDOWSTART] & ((1u << ((wb-1)&15)) | (1u << ((wb-2)&15)) | (1u << ((wb-3)&15))))) {
                            sr_[XT_SR_EXCCAUSE] = 5; return XTENSA_EXCEPTION;
                        }
                    }
                    set_reg(t,av); break;
                case 2:
                    if (t != 0 && t != 1 && t != 2 && t != 3 && t != 8 && t != 12 && t != 13 && t != 15) return XTENSA_INVALID;
                    break; /* Uncached bus: sync/cache ordering is immediate. */
                case 3:
                    if (t == 0 && (s == 0 || s == 2)) { if (!s) sr_[XT_SR_PS] &= ~PS_EXCM; next = sr_[s ? XT_SR_DEPC : XT_SR_EPC1]; }
                    else if (t == 0 && (s == 4 || s == 5) && windowed_) {
                        if (s == 4) sr_[XT_SR_WINDOWSTART] &= ~(1u << sr_[XT_SR_WINDOWBASE]);
                        else sr_[XT_SR_WINDOWSTART] |= 1u << sr_[XT_SR_WINDOWBASE];
                        sr_[XT_SR_WINDOWBASE] = (sr_[XT_SR_PS] >> 8) & 15;
                        sr_[XT_SR_PS] &= ~PS_EXCM; next = sr_[XT_SR_EPC1];
                    } else if (t == 1 && s >= 2 && s <= 7) { sr_[XT_SR_PS] = sr_[192+s]; next = sr_[XT_SR_EPC1+s-1]; }
                    else return XTENSA_INVALID;
                    jump = true; break;
                case 4: return XTENSA_BREAK;
                case 5: if (s || t) return XTENSA_INVALID; sr_[XT_SR_EXCCAUSE] = 1; return XTENSA_EXCEPTION;
                case 6: WC1(t); set_reg(t,sr_[XT_SR_PS]); sr_[XT_SR_PS] = (sr_[XT_SR_PS] & ~15u) | s; break;
                case 7: sr_[XT_SR_PS] = (sr_[XT_SR_PS] & ~15u) | s; waiting_ = true; return XTENSA_WAIT;
                case 8: case 9: case 10: case 11:
                    if (!windowed_) return XTENSA_INVALID;
                    value = (1u << ((r & 2) ? 8 : 4)) - 1;
                    condition = (r & 1) ? ((sr_[XT_SR_BR] >> s) & value) == value : ((sr_[XT_SR_BR] >> s) & value) != 0;
                    sr_[XT_SR_BR] = (sr_[XT_SR_BR] & ~(1u << t)) | (u32(condition) << t); break;
                default: return XTENSA_INVALID;
                }
            } else return XTENSA_INVALID;
            break;
        case 1:
            if (op2 <= 1) { WC2(r,s); unsigned sh = 32 - (t | ((op2 & 1) << 4)); set_reg(r, sh == 32 ? 0 : av << sh); }
            else if (op2 <= 4) { WC2(r,t); unsigned sh = s | ((op2 == 3) << 4); set_reg(r, op2 == 4 ? bv >> s : asr(bv,sh)); }
            else if (op2 == 6) {
                WC1(t); value = special((insn >> 8) & 255);
                /* XSR WINDOWBASE writes its AR result in the old physical
                 * window before making the new window visible. */
                set_reg(t,value); set_special((insn >> 8) & 255,bv);
            }
            else if (op2 == 8) { WC3(r,s,t); set_reg(r,u32(((u64(av) << 32) | bv) >> sr_[XT_SR_SAR])); }
            else if (op2 == 9 || op2 == 11) { WC2(r,t); set_reg(r,op2 == 11 ? asr(bv,sr_[XT_SR_SAR]) : sr_[XT_SR_SAR] >= 32 ? 0 : bv >> sr_[XT_SR_SAR]); }
            else if (op2 == 10) { WC2(r,s); unsigned sh = (32-sr_[XT_SR_SAR]) & 63; set_reg(r,sh >= 32 ? 0 : av << sh); }
            else if (op2 == 12 || op2 == 13) { WC3(r,s,t); set_reg(r,op2 == 12 ? (av & 65535)*(bv & 65535) : u32(s32(s16(av))*s32(s16(bv)))); }
            else return XTENSA_INVALID;
            break;
        case 2:
            if (!windowed_ && op2 != 8) return XTENSA_INVALID;
            if (op2 <= 4) {
                unsigned x = (sr_[XT_SR_BR] >> s) & 1, y = (sr_[XT_SR_BR] >> t) & 1;
                value = op2 == 0 ? x & y : op2 == 1 ? x & !y : op2 == 2 ? x | y : op2 == 3 ? x | !y : x ^ y;
                sr_[XT_SR_BR] = (sr_[XT_SR_BR] & ~(1u << r)) | (value << r);
            } else {
                WC3(r,s,t);
                if (op2 == 8) value = av*bv;
                else if (op2 == 10) value = u32((u64(av)*bv) >> 32);
                else if (op2 == 11) value = u32((s64(s32(av))*s32(bv)) >> 32);
                else if (op2 >= 12) {
                    if (!bv) { sr_[XT_SR_EXCCAUSE] = 6; return XTENSA_EXCEPTION; }
                    if (op2 == 12) value = av/bv;
                    if (op2 == 14) value = av%bv;
                    if (op2 == 13 || op2 == 15) {
                        if (av == 0x80000000u && bv == 0xffffffffu) value = op2 == 13 ? av : 0;
                        else value = op2 == 13 ? u32(s32(av)/s32(bv)) : u32(s32(av)%s32(bv));
                    }
                } else return XTENSA_INVALID;
                set_reg(r,value);
            }
            break;
        case 3:
            if (!windowed_ && ((op2 >= 2 && op2 <= 7) || op2 == 12 || op2 == 13)) return XTENSA_INVALID;
            if (op2>=14) {
                const unsigned ur=op2==14?s*16+t:(insn>>8)&255;
                if (ur==232 || ur==233) {
                    if (!windowed_) return XTENSA_INVALID;
                    if (!(sr_[224]&1)) { sr_[XT_SR_EXCCAUSE]=32; return XTENSA_EXCEPTION; }
                }
            }
            if (op2 == 0) { WC1(t); set_reg(t,special((insn >> 8) & 255)); }
            else if (op2 == 1) { WC1(t); set_special((insn >> 8) & 255,bv); }
            else if (op2 == 2) { WC2(r,s); set_reg(r,sext(av & ((1u << (t+8))-1),t+8)); }
            else if (op2 == 3) { WC2(r,s); s32 hi = s32((1u << (t+7))-1), lo = -hi-1; set_reg(r,(s32)av < lo ? u32(lo) : (s32)av > hi ? u32(hi) : av); }
            else if (op2 >= 4 && op2 <= 7) { WC3(r,s,t); condition = op2 < 6 ? (s32)av < (s32)bv : av < bv; if (op2 & 1) condition = !condition; set_reg(r,condition ? av : bv); }
            else if (op2 >= 8 && op2 <= 11) { WC3(r,s,t); if (compare(bv,0,op2-8)) set_reg(r,av); }
            else if (op2 == 12 || op2 == 13) { WC2(r,s); if (((sr_[XT_SR_BR] >> t) & 1) == (op2 & 1)) set_reg(r,av); }
            else if (op2 == 14) { WC1(r); set_reg(r,user(s*16+t)); }
            else { WC1(t); set_user((insn >> 8) & 255,bv); }
            break;
        case 4: case 5: WC2(r,t); set_reg(r,(bv >> (s | ((op1 & 1)<<4))) & ((1u << (op2+1))-1)); break;
        case 8:
            if (!windowed_ || (op2 != 0 && op2 != 1 && op2 != 4 && op2 != 5)) return XTENSA_INVALID;
            if (!(sr_[224]&1)) { sr_[XT_SR_EXCCAUSE]=32; return XTENSA_EXCEPTION; }
            WC2(s,t); address=av+bv;
            if (op2 & 4) result=data_write(address,4,fr_[r]);
            else { result=data_read(address,4,value); if (result==XTENSA_OK) fr_[r]=value; }
            if (result!=XTENSA_OK) return result;
            if (op2&1) set_reg(s,address);
            break;
        case 9:
            if (!windowed_ || (op2 != 0 && op2 != 4 && op2 != 5)) return XTENSA_INVALID;
            /* S32NB bypasses the hardware write buffer. Stores on this bus
             * already complete synchronously; its offset is unsigned. */
            WC2(s,t); address = av + (op2 == 5 ? r * 4 : (0xffffffc0u | (r << 2)));
            if (!op2) { LOAD(t,address,4); } else { STORE(t,address,4); }
            break;
        case 10: case 11: return float_operation(insn,next,jump);
        default: return XTENSA_INVALID;
        }
        break;
    case 1:
        WC1(t); address = (sr_[XT_SR_LITBASE] & 1) ? (sr_[XT_SR_LITBASE] & ~0xfffu) : ((pc_+3) & ~3u);
        address += 0xfffc0000u | ((insn >> 8) << 2); LOAD(t,address,4); break;
    case 2:
        if (r == 10) { WC1(t); set_reg(t,sext((s << 8) | imm8,12)); }
        else if (r == 12 || r == 13) { WC2(s,t); set_reg(t,av + (sext(imm8,8) << (r == 13 ? 8 : 0))); }
        else if (r == 7) {
            /* Valid cache maintenance operations are no-ops on the coherent,
             * uncached interpreter bus. Reserved sub-opcodes still trap. */
            if (t == 9 || t == 10 || t == 11 || (t == 8 && op1 != 0 && op1 != 2 && op1 != 3 && op1 != 4 && op1 != 5) ||
                (t == 13 && op1 != 0 && op1 != 2 && op1 != 3)) return XTENSA_INVALID;
            WC1(s);
        }
        else if (r == 0 || r == 1 || r == 2 || r == 4 || r == 5 || r == 6 || r == 9 || r == 11 || r == 14 || r == 15) {
            if (!windowed_ && (r == 11 || r == 14 || r == 15)) return XTENSA_INVALID;
            WC2(s,t); unsigned size = (r == 0 || r == 4) ? 1 : (r == 1 || r == 5 || r == 9) ? 2 : 4;
            address = av + imm8*size;
            if ((r == 11 || r == 14 || r == 15) && (address & 3)) {
                fault_address_ = address; sr_[XT_SR_EXCVADDR] = address; sr_[XT_SR_EXCCAUSE] = 9; return XTENSA_EXCEPTION;
            }
            if (r == 14) { /* S32C1I: one indivisible step on the serialized CPU bus. */
                if (!access_permitted(address,false,true)) return XTENSA_EXCEPTION;
                result = data_read(address,4,value); if (result != XTENSA_OK) return result;
                if (value == sr_[XT_SR_SCOMPARE1]) { result = data_write(address,4,bv); if (result != XTENSA_OK) return result; }
                set_reg(t,value);
            } else if (r == 4 || r == 5 || r == 6 || r == 15) { STORE(t,address,size); }
            else { LOAD(t,address,size); if (r == 9) set_reg(t,sext(value,16)); }
        } else return XTENSA_INVALID;
        break;
    case 3:
        if (!windowed_ || (r != 0 && r != 4 && r != 8 && r != 12)) return XTENSA_INVALID;
        if (!(sr_[224]&1)) { sr_[XT_SR_EXCCAUSE]=32; return XTENSA_EXCEPTION; }
        WC1(s); address=av+imm8*4;
        if (r&4) result=data_write(address,4,fr_[t]);
        else { result=data_read(address,4,value); if (result==XTENSA_OK) fr_[t]=value; }
        if (result!=XTENSA_OK) return result;
        if (r&8) set_reg(s,address);
        break;
    case 4: {
        if (!windowed_) return XTENSA_INVALID;
        /* MAC16's signed 40-bit accumulator and four 32-bit multiplier
         * registers are special registers. Combined loads read first, so a
         * failed bus operation leaves the accumulator and pointer intact. */
        unsigned operation = op1 & 12;
        bool first_mr = (op2 & 3) == 2, second_mr = (op2 & 12) == 0;
        int delta = 0;
        if (op2 > 9) return XTENSA_INVALID;
        if (!(op2 & 2)) {
            first_mr = true; delta = (op2 & 1) ? -4 : 4;
            if (op2 >= 8) { if (op1) return XTENSA_INVALID; operation = 15; }
            else if (operation != 8) return XTENSA_INVALID;
        } else if (!operation && op2 != 7) return XTENSA_INVALID;
        if (operation != 15) {
            if (!first_mr) { WC1(s); }
            if (!second_mr) { WC1(t); }
        }
        if (delta) {
            WC1(s); address = av + u32(delta);
            result = data_read(address,4,value); if (result != XTENSA_OK) return result;
        }
        if (operation != 15) {
            u32 x = first_mr ? sr_[32+((r>>2)&1)] : av;
            u32 y = second_mr ? sr_[34+((t>>2)&1)] : bv;
            x = (x >> ((op1&1) ? 16 : 0)) & 0xffff;
            y = (y >> ((op1&2) ? 16 : 0)) & 0xffff;
            const s64 product = !operation ? s64(u64(x)*y) : s64(s16(x))*s16(y);
            u64 acc = (u64(sr_[17])<<32)|sr_[16];
            if (operation == 0 || operation == 4) acc = u64(product);
            else if (operation == 8) acc += u64(product);
            else acc -= u64(product);
            sr_[16] = u32(acc); sr_[17] = sext(u32(acc>>32)&255,8);
        }
        if (delta) { set_reg(s,address); sr_[32+(r&3)] = value; }
        break;
    }
    case 5:
        if (n && !windowed_) return XTENSA_INVALID;
        WC1(n*4); call(n,next); next = (pc_ & ~3u) + 4 + (sext(insn >> 6,18) << 2); jump = true; break;
    case 6:
        if (!n) { next = pc_ + 4 + sext(insn >> 6,18); jump = true; }
        else if (n == 1) { WC1(s); if (compare(av,0,m)) { next = pc_+4+sext(insn >> 12,12); jump = true; } }
        else if (n == 2) { WC1(s); if (compare(av,b4const[r],m)) { next = pc_+4+sext(imm8,8); jump = true; } }
        else if (m == 0) {
            if (!windowed_ || s > 3 || (sr_[XT_SR_PS] & (PS_WOE | PS_EXCM)) != PS_WOE) return XTENSA_INVALID;
            unsigned ci = (sr_[XT_SR_PS] >> 16) & 3;
            WC1(ci*4); set_reg(ci*4+s,av-((insn >> 12)*8));
            sr_[XT_SR_WINDOWBASE] = (sr_[XT_SR_WINDOWBASE]+ci) & 15;
            sr_[XT_SR_WINDOWSTART] |= 1u << sr_[XT_SR_WINDOWBASE]; pending_call_ = 0;
        } else if (m >= 2) { WC1(s); if ((av < b4constu[r]) == (m == 2)) { next = pc_+4+sext(imm8,8); jump = true; } }
        else if (r <= 1) { if (!windowed_) return XTENSA_INVALID; if (((sr_[XT_SR_BR] >> s) & 1) == r) { next = pc_+4+sext(imm8,8); jump = true; } }
        else if (r >= 8 && r <= 10) {
            if (!windowed_) return XTENSA_INVALID;
            WC1(s); sr_[XT_SR_LBEG] = next; sr_[XT_SR_LEND] = pc_+4+imm8; sr_[XT_SR_LCOUNT] = av-1;
            if ((r == 9 && !av) || (r == 10 && (s32)av <= 0)) { next = sr_[XT_SR_LEND]; jump = true; }
        } else return XTENSA_INVALID;
        break;
    case 7:
        WC1(s); if ((r & 7) < 6) { WC1(t); }
        switch (r & 7) {
        case 0: condition = (av & bv) == 0; break;
        case 1: condition = av == bv; break;
        case 2: condition = (s32)av < (s32)bv; break;
        case 3: condition = av < bv; break;
        case 4: condition = (av & bv) == bv; break;
        case 5: condition = (av & (1u << (bv & 31))) == 0; break;
        default: condition = (av & (1u << (t | ((r & 1) << 4)))) == 0; break;
        }
        if (r & 8) condition = !condition;
        if (condition) { next = pc_+4+sext(imm8,8); jump = true; }
        break;
    default: return XTENSA_INVALID;
    }
    return XTENSA_OK;
#undef WC1
#undef WC2
#undef WC3
#undef LOAD
#undef STORE
}
