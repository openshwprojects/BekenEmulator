#include "xtensa_cpu.h"
#include <stdio.h>
#include <string.h>

namespace {
int checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { fprintf(stderr,"Xtensa check failed at %s:%d: %s\n",__FILE__,__LINE__,#condition); return false; } } while (0)

struct Bus : XtensaBus {
    u8 bytes[4096];
    unsigned reads, writes, fetches;
    Bus() : reads(0), writes(0), fetches(0) { memset(bytes,0,sizeof(bytes)); }
    virtual bool read(u32 addr,unsigned size,u32 &value,bool instruction) {
        if (instruction) ++fetches; else ++reads;
        if (addr >= sizeof(bytes) || size > sizeof(bytes)-addr) return false;
        value=0; for (unsigned i=0;i<size;++i) value|=u32(bytes[addr+i])<<(i*8);
        return true;
    }
    virtual bool write(u32 addr,unsigned size,u32 value) {
        ++writes;
        if (addr >= sizeof(bytes) || size > sizeof(bytes)-addr) return false;
        put(addr,value,size); return true;
    }
    void put(u32 addr,u32 value,unsigned size=3) { for (unsigned i=0;i<size;++i) bytes[addr+i]=u8(value>>(i*8)); }
};
u32 rrr(unsigned op2,unsigned op1,unsigned r,unsigned s,unsigned t) {
    return (op2<<20)|(op1<<16)|(r<<12)|(s<<8)|(t<<4);
}
u32 rri(unsigned r,unsigned s,unsigned t,unsigned imm) { return (imm<<16)|(r<<12)|(s<<8)|(t<<4)|2; }
XtensaResult execute(Bus &bus,XtensaCpu &cpu,u32 instruction,unsigned size=3) {
    bus.put(0x100,instruction,size); cpu.set_pc(0x100); return cpu.step();
}

bool external_registers_and_identity() {
    struct ExternalBus : Bus {
        u32 external;
        ExternalBus():external(0x12345678) {}
        virtual bool external_read(u32 address,u32 &value) {
            if(address!=0x10200c)return false;
            value=external;return true;
        }
        virtual bool external_write(u32 address,u32 value) {
            if(address!=0x10200c)return false;
            external=value;return true;
        }
    } bus;
    XtensaCpu cpu(bus,true);
    CHECK(cpu.special(XT_SR_PRID)==0xcdcd);
    cpu.set_processor_id(0xabab);cpu.reset(0x100);
    CHECK(cpu.special(XT_SR_PRID)==0xabab && execute(bus,cpu,0x03eb20)==XTENSA_OK && cpu.reg(2)==0xabab);
    cpu.set_special(XT_SR_PRID,0);
    CHECK(cpu.special(XT_SR_PRID)==0xabab);
    cpu.set_reg(3,0x10200c);
    CHECK(execute(bus,cpu,0x406320)==XTENSA_OK && cpu.reg(2)==0x12345678);
    CHECK(bus.reads==0 && bus.writes==0); // separateexternalbus, neverordinarymemory
    cpu.set_reg(2,0x87654321);
    CHECK(execute(bus,cpu,0x407320)==XTENSA_OK && bus.external==0x87654321);
    cpu.set_reg(2,0x10200c);
    CHECK(execute(bus,cpu,0x406220)==XTENSA_OK && cpu.reg(2)==0x87654321); // address/destalias
    cpu.set_reg(3,0xdeadbeef);
    CHECK(execute(bus,cpu,0x406320)==XTENSA_MEMORY_FAULT && cpu.pc()==0x100 && cpu.fault_address()==0xdeadbeef);
    CHECK(cpu.reg(2)==0x87654321);
    XtensaCpu lx106(bus);lx106.set_reg(3,0x10200c);
    CHECK(execute(bus,lx106,0x406320)==XTENSA_OK); // bothcoresconfigureexternalregisterISA
    Bus missing;XtensaCpu nomodule(missing,true);nomodule.set_reg(3,0x10200c);
    CHECK(execute(missing,nomodule,0x406320)==XTENSA_MEMORY_FAULT);
    return true;
}

bool density() {
    Bus bus; XtensaCpu cpu(bus);
    /* These byte vectors include actual firmware prologue/epilogue forms. */
    CHECK(execute(bus,cpu,0x423c,2)==XTENSA_OK); // movi.n a2,52
    CHECK(cpu.reg(2)==52 && cpu.pc()==0x102 && bus.fetches==2);
    CHECK(execute(bus,cpu,0xf27c,2)==XTENSA_OK && cpu.reg(2)==0xffffffffu);
    CHECK(execute(bus,cpu,0xf25c,2)==XTENSA_OK && cpu.reg(2)==95);
    cpu.set_reg(3,0x12345678);
    CHECK(execute(bus,cpu,0x032d,2)==XTENSA_OK && cpu.reg(2)==0x12345678);
    CHECK(execute(bus,cpu,0x230b,2)==XTENSA_OK && cpu.reg(2)==0x12345677);
    cpu.set_reg(2,9); cpu.set_reg(3,11);
    CHECK(execute(bus,cpu,0x432a,2)==XTENSA_OK && cpu.reg(4)==20);
    cpu.set_reg(1,0x300); cpu.set_reg(2,0x12345678);
    CHECK(execute(bus,cpu,0x2129,2)==XTENSA_OK && rd32le(bus.bytes+0x308)==0x12345678);
    CHECK(execute(bus,cpu,0x2138,2)==XTENSA_OK && cpu.reg(3)==0x12345678);
    cpu.set_reg(2,0);
    CHECK(execute(bus,cpu,0xf2bc,2)==XTENSA_OK && cpu.pc()==0x143); // beqz.n +63
    CHECK(execute(bus,cpu,0xf2fc,2)==XTENSA_OK && cpu.pc()==0x102); // bnez.n
    cpu.set_reg(0,0x555);
    CHECK(execute(bus,cpu,0xf00d,2)==XTENSA_OK && cpu.pc()==0x555);
    CHECK(execute(bus,cpu,0xf03d,2)==XTENSA_OK);
    CHECK(execute(bus,cpu,0xf06d,2)==XTENSA_INVALID && cpu.pc()==0x100);
    CHECK(execute(bus,cpu,0xffff,2)==XTENSA_INVALID);
    return true;
}

bool arithmetic() {
    Bus bus; XtensaCpu cpu(bus,true);
    cpu.set_reg(2,7); cpu.set_reg(3,11);
    for (unsigned shift=0;shift<4;++shift) {
        CHECK(execute(bus,cpu,rrr(8+shift,0,4,2,3))==XTENSA_OK && cpu.reg(4)==(7u<<shift)+11);
        CHECK(execute(bus,cpu,rrr(12+shift,0,4,2,3))==XTENSA_OK && cpu.reg(4)==(7u<<shift)-11);
    }
    CHECK(execute(bus,cpu,rrr(1,0,4,2,3))==XTENSA_OK && cpu.reg(4)==3);
    CHECK(execute(bus,cpu,rrr(2,0,4,2,3))==XTENSA_OK && cpu.reg(4)==15);
    CHECK(execute(bus,cpu,rrr(3,0,4,2,3))==XTENSA_OK && cpu.reg(4)==12);
    CHECK(execute(bus,cpu,rri(12,2,4,128))==XTENSA_OK && cpu.reg(4)==u32(7-128));
    CHECK(execute(bus,cpu,rri(13,2,4,255))==XTENSA_OK && cpu.reg(4)==u32(7-256));
    CHECK(execute(bus,cpu,rri(10,8,4,0))==XTENSA_OK && cpu.reg(4)==0xfffff800u);
    CHECK(execute(bus,cpu,rri(10,7,4,255))==XTENSA_OK && cpu.reg(4)==2047);
    CHECK(execute(bus,cpu,rrr(6,0,4,0,2))==XTENSA_OK && cpu.reg(4)==u32(-7));
    cpu.set_reg(2,0x80000000u);
    CHECK(execute(bus,cpu,rrr(6,0,4,1,2))==XTENSA_OK && cpu.reg(4)==0x80000000u);
    CHECK(execute(bus,cpu,rrr(8,2,4,2,3))==XTENSA_OK && cpu.reg(4)==0x80000000u);
    cpu.set_reg(2,0xffffffffu); cpu.set_reg(3,2);
    CHECK(execute(bus,cpu,rrr(10,2,4,2,3))==XTENSA_OK && cpu.reg(4)==1);
    CHECK(execute(bus,cpu,rrr(11,2,4,2,3))==XTENSA_OK && cpu.reg(4)==0xffffffffu);
    CHECK(execute(bus,cpu,rrr(12,1,4,2,3))==XTENSA_OK && cpu.reg(4)==131070);
    CHECK(execute(bus,cpu,rrr(13,1,4,2,3))==XTENSA_OK && cpu.reg(4)==u32(-2));
    cpu.set_reg(2,u32(-23)); cpu.set_reg(3,4);
    CHECK(execute(bus,cpu,rrr(13,2,4,2,3))==XTENSA_OK && cpu.reg(4)==u32(-5));
    CHECK(execute(bus,cpu,rrr(15,2,4,2,3))==XTENSA_OK && cpu.reg(4)==u32(-3));
    cpu.set_reg(2,0x80000000u); cpu.set_reg(3,0xffffffffu);
    CHECK(execute(bus,cpu,rrr(13,2,4,2,3))==XTENSA_OK && cpu.reg(4)==0x80000000u);
    CHECK(execute(bus,cpu,rrr(15,2,4,2,3))==XTENSA_OK && cpu.reg(4)==0);
    cpu.set_reg(3,0); cpu.set_reg(4,123);
    CHECK(execute(bus,cpu,rrr(13,2,4,2,3))==XTENSA_EXCEPTION && cpu.special(XT_SR_EXCCAUSE)==6 && cpu.reg(4)==123);
    cpu.set_reg(2,0xabcdef80);
    CHECK(execute(bus,cpu,rrr(2,3,4,2,0))==XTENSA_OK && cpu.reg(4)==0xffffff80u);
    CHECK(execute(bus,cpu,rrr(3,3,4,2,0))==XTENSA_OK && cpu.reg(4)==0xffffff80u);
    cpu.set_reg(2,300);
    CHECK(execute(bus,cpu,rrr(3,3,4,2,0))==XTENSA_OK && cpu.reg(4)==127);
    return true;
}

bool shifts() {
    Bus bus; XtensaCpu cpu(bus);
    cpu.set_reg(2,0x89abcdefu); cpu.set_reg(3,4);
    CHECK(execute(bus,cpu,rrr(4,0,0,3,0))==XTENSA_OK && cpu.special(XT_SR_SAR)==4);
    CHECK(execute(bus,cpu,rrr(9,1,4,0,2))==XTENSA_OK && cpu.reg(4)==0x089abcde);
    CHECK(execute(bus,cpu,rrr(11,1,4,0,2))==XTENSA_OK && cpu.reg(4)==0xf89abcde);
    CHECK(execute(bus,cpu,rrr(4,0,1,3,0))==XTENSA_OK && cpu.special(XT_SR_SAR)==28);
    CHECK(execute(bus,cpu,rrr(10,1,4,2,0))==XTENSA_OK && cpu.reg(4)==0x9abcdef0);
    CHECK(execute(bus,cpu,rrr(1,1,4,2,12))==XTENSA_OK && cpu.reg(4)==0x9abcdef0); // SLLI encoding 32-sh
    CHECK(execute(bus,cpu,rrr(2,1,4,4,2))==XTENSA_OK && cpu.reg(4)==0xf89abcde);
    CHECK(execute(bus,cpu,rrr(7,4,4,8,2))==XTENSA_OK && cpu.reg(4)==0xcd); // extui 8,8
    CHECK(execute(bus,cpu,rrr(7,5,4,0,2))==XTENSA_OK && cpu.reg(4)==0xab);
    cpu.set_special(XT_SR_SAR,32);
    CHECK(execute(bus,cpu,rrr(9,1,4,0,2))==XTENSA_OK && cpu.reg(4)==0);
    CHECK(execute(bus,cpu,rrr(11,1,4,0,2))==XTENSA_OK && cpu.reg(4)==0xffffffffu);
    CHECK(execute(bus,cpu,rrr(10,1,4,2,0))==XTENSA_OK && cpu.reg(4)==0x89abcdefu);
    cpu.set_special(XT_SR_SAR,4); cpu.set_reg(3,0x12345678);
    CHECK(execute(bus,cpu,rrr(8,1,4,3,2))==XTENSA_OK && cpu.reg(4)==0x889abcde);
    cpu.set_reg(2,0);
    CHECK(execute(bus,cpu,rrr(4,0,15,2,4))==XTENSA_OK && cpu.reg(4)==32);
    CHECK(execute(bus,cpu,rrr(4,0,14,2,4))==XTENSA_OK && cpu.reg(4)==31);
    return true;
}

bool memory() {
    Bus bus; XtensaCpu cpu(bus);
    cpu.set_reg(2,0x200); bus.put(0x3fe,0x89ab,2);
    CHECK(execute(bus,cpu,rri(1,2,3,255))==XTENSA_OK && cpu.reg(3)==0x89ab); // unsigned, scaled offset
    CHECK(execute(bus,cpu,rri(9,2,3,255))==XTENSA_OK && cpu.reg(3)==0xffff89abu);
    cpu.set_reg(3,0x12345678);
    CHECK(execute(bus,cpu,rri(4,2,3,255))==XTENSA_OK && bus.bytes[0x2ff]==0x78);
    CHECK(execute(bus,cpu,rri(6,2,3,255))==XTENSA_OK && rd32le(bus.bytes+0x5fc)==0x12345678);
    bus.put(0xfc,0xdeadbeef,4);
    CHECK(execute(bus,cpu,0xffff21)==XTENSA_OK && cpu.reg(2)==0xdeadbeef); // l32r a2,pc-4
    bus.put(0x100,0xffff21); cpu.set_pc(0x101); bus.put(0x101,0xfffe21);
    CHECK(cpu.step()==XTENSA_OK && cpu.reg(2)==0xdeadbeef); // pc+3 alignment
    cpu.set_reg(2,0x201); cpu.set_reg(3,77);
    CHECK(execute(bus,cpu,rri(2,2,3,0))==XTENSA_EXCEPTION && cpu.special(XT_SR_EXCCAUSE)==9 && cpu.reg(3)==77);
    cpu.set_reg(2,0x2000);
    CHECK(execute(bus,cpu,rri(2,2,3,0))==XTENSA_MEMORY_FAULT && cpu.fault_address()==0x2000 && cpu.pc()==0x100);
    cpu.set_pc(4095); bus.bytes[4095]=0x0d;
    CHECK(cpu.step()==XTENSA_MEMORY_FAULT && cpu.fault_address()==4096);
    XtensaCpu lx6(bus,true);
    lx6.set_reg(2,0x300); lx6.set_reg(3,99); lx6.set_special(XT_SR_SCOMPARE1,7); bus.put(0x300,7,4);
    CHECK(execute(bus,lx6,rri(14,2,3,0))==XTENSA_OK && lx6.reg(3)==7 && rd32le(bus.bytes+0x300)==99);
    lx6.set_reg(3,100);
    CHECK(execute(bus,lx6,rri(14,2,3,0))==XTENSA_OK && lx6.reg(3)==99 && rd32le(bus.bytes+0x300)==99);
    return true;
}

bool nonbuffered_stores() {
    /* S32NB is available on this LX6 configuration, but not LX106.
     * Encodings checked using both chip assemblers. Its positive imm4
     * offset shares a format with the negative-offset spill instruction. */
    Bus bus;XtensaCpu cpu(bus,true),lx106(bus);
    cpu.set_reg(2,0x300);cpu.set_reg(3,0x12345678);
    CHECK(execute(bus,cpu,0x590230)==XTENSA_OK && rd32le(bus.bytes+0x300)==0x12345678);
    cpu.set_reg(3,0xabcdef01);
    CHECK(execute(bus,cpu,0x59f230)==XTENSA_OK && rd32le(bus.bytes+0x33c)==0xabcdef01);
    CHECK(cpu.reg(2)==0x300 && cpu.reg(3)==0xabcdef01 && rd32le(bus.bytes+0x2fc)==0);
    CHECK(execute(bus,lx106,0x590230)==XTENSA_INVALID && lx106.instructions()==0);
    cpu.set_reg(2,0x1000);
    CHECK(execute(bus,cpu,0x590230)==XTENSA_MEMORY_FAULT && cpu.pc()==0x100 && cpu.fault_address()==0x1000);
    CHECK(cpu.instructions()==2 && cpu.reg(2)==0x1000 && cpu.reg(3)==0xabcdef01);
    cpu.set_reg(2,0x340);cpu.set_reg(12,99);
    cpu.set_special(XT_SR_VECBASE,0x300);cpu.set_special(XT_SR_WINDOWSTART,3);
    CHECK(execute(bus,cpu,0x5902c0)==XTENSA_OK && cpu.pc()==0x400 && cpu.special(XT_SR_EPC1)==0x100);
    CHECK(rd32le(bus.bytes+0x340)==0); // spill occurs before any store side effect
    return true;
}

bool branches_calls_loops() {
    Bus bus; XtensaCpu cpu(bus,true);
    cpu.set_reg(2,0xffffffffu); cpu.set_reg(3,0);
    CHECK(execute(bus,cpu,0xfc2327)==XTENSA_OK && cpu.pc()==0x103); // blt a3,a2 false
    CHECK(execute(bus,cpu,0xfc2237)==XTENSA_OK && cpu.pc()==0x100); // blt a2,a3 true
    CHECK(execute(bus,cpu,0xfc3237)==XTENSA_OK && cpu.pc()==0x103); // bltu a2,a3 false
    CHECK(execute(bus,cpu,0xffc206)==XTENSA_OK && cpu.pc()==0x0c); // signed J offset -248
    CHECK(execute(bus,cpu,0x000005)==XTENSA_OK && cpu.reg(0)==0x103 && cpu.pc()==0x104);
    cpu.set_reg(0,0x333);
    CHECK(execute(bus,cpu,0x0000c0)==XTENSA_OK && cpu.pc()==0x333 && cpu.reg(0)==0x103); // callx0 a0 alias
    cpu.set_reg(2,3); cpu.set_reg(3,0);
    bus.put(0x100,0x048276); // loop a2,0x108
    bus.put(0x103,0x331b,2); // addi.n a3,a3,1
    bus.put(0x105,0x0020f0); // nop, ends at 0x108
    cpu.set_pc(0x100);
    CHECK(cpu.run(7)==XTENSA_OK && cpu.reg(3)==3 && cpu.pc()==0x108 && cpu.special(XT_SR_LCOUNT)==0);
    cpu.set_reg(2,0);
    CHECK(execute(bus,cpu,0x049276)==XTENSA_OK && cpu.pc()==0x108); // loopnez skipped
    return true;
}

bool special_and_interrupts() {
    Bus bus; XtensaCpu cpu(bus,true);
    cpu.set_reg(2,0xff);
    CHECK(execute(bus,cpu,0x130320)==XTENSA_OK && cpu.special(XT_SR_SAR)==63); // wsr SAR,a2
    CHECK(execute(bus,cpu,0x030330)==XTENSA_OK && cpu.reg(3)==63);
    cpu.set_reg(3,7);
    CHECK(execute(bus,cpu,0x610330)==XTENSA_OK && cpu.reg(3)==63 && cpu.special(XT_SR_SAR)==7);
    cpu.set_special(XT_SR_PS,0x40000);
    CHECK(execute(bus,cpu,0x006520)==XTENSA_OK && cpu.reg(2)==0x40000 && cpu.special(XT_SR_PS)==0x40005);
    CHECK(!cpu.interrupt(5,0x300));
    CHECK(cpu.interrupt(6,0x300) && cpu.pc()==0x300 && cpu.special(XT_SR_EPC1+5)==0x103);
    CHECK(execute(bus,cpu,0x003610)==XTENSA_OK && cpu.pc()==0x103 && cpu.special(XT_SR_PS)==0x40005);
    CHECK(execute(bus,cpu,0x007000)==XTENSA_WAIT && cpu.pc()==0x103);
    CHECK(cpu.interrupt(1,0x300) && (cpu.special(XT_SR_PS)&16));
    CHECK(execute(bus,cpu,0x003000)==XTENSA_OK && cpu.pc()==0x103 && !(cpu.special(XT_SR_PS)&16));
    return true;
}

bool mac16() {
    Bus bus; XtensaCpu cpu(bus,true);
    cpu.set_reg(2,0xfffe0003); cpu.set_reg(3,0xfffc0005);
    CHECK(execute(bus,cpu,0x740234)==XTENSA_OK && cpu.special(16)==15 && cpu.special(17)==0); // mul.aa.ll
    CHECK(execute(bus,cpu,0x770234)==XTENSA_OK && cpu.special(16)==8); // mul.aa.hh
    CHECK(execute(bus,cpu,0x750234)==XTENSA_OK && cpu.special(16)==u32(-10) && cpu.special(17)==0xffffffffu);
    CHECK(execute(bus,cpu,0x780234)==XTENSA_OK && cpu.special(16)==5 && cpu.special(17)==0); // mula.aa.ll
    CHECK(execute(bus,cpu,0x7c0234)==XTENSA_OK && cpu.special(16)==u32(-10) && cpu.special(17)==0xffffffffu);
    CHECK(execute(bus,cpu,0x700234)==XTENSA_OK && cpu.special(16)==15); // umul.aa.ll
    cpu.set_reg(2,0x300); bus.put(0x304,0x98765432,4);
    CHECK(execute(bus,cpu,0x800204)==XTENSA_OK && cpu.reg(2)==0x304 && cpu.special(32)==0x98765432); // ldinc m0,a2
    bus.put(0x300,0x12345678,4);
    CHECK(execute(bus,cpu,0x900204)==XTENSA_OK && cpu.reg(2)==0x300 && cpu.special(32)==0x12345678); // lddec
    cpu.set_reg(2,0x3ffc);
    CHECK(execute(bus,cpu,0x800204)==XTENSA_MEMORY_FAULT && cpu.reg(2)==0x3ffc);
    CHECK(execute(bus,cpu,0x810204)==XTENSA_INVALID); // reserved ldinc encoding
    return true;
}

bool region_protection() {
    Bus bus; XtensaCpu cpu(bus,true);
    cpu.set_reg(9,0x20000000); cpu.set_reg(10,15);
    CHECK(execute(bus,cpu,0x50e9a0)==XTENSA_OK); // actual NSPanel WDTLB a10,a9
    CHECK(execute(bus,cpu,0x50f9b0)==XTENSA_OK && cpu.reg(11)==0x2000000fu); // RDTLB1
    CHECK(execute(bus,cpu,0x50b9b0)==XTENSA_OK && cpu.reg(11)==0x20000000u); // RDTLB0
    CHECK(execute(bus,cpu,0x50d9b0)==XTENSA_OK && cpu.reg(11)==0x20000001u); // PDTLB region hit
    cpu.set_reg(9,0); cpu.set_reg(10,15);
    CHECK(execute(bus,cpu,0x50e9a0)==XTENSA_OK);
    cpu.set_reg(2,0x300);
    CHECK(execute(bus,cpu,rri(2,2,3,0))==XTENSA_EXCEPTION && cpu.special(XT_SR_EXCCAUSE)==28);
    CHECK(execute(bus,cpu,rri(6,2,3,0))==XTENSA_EXCEPTION && cpu.special(XT_SR_EXCCAUSE)==29);
    cpu.set_reg(10,2);
    CHECK(execute(bus,cpu,0x50e9a0)==XTENSA_OK);
    bus.put(0x300,0x12345678,4);
    CHECK(execute(bus,cpu,rri(11,2,3,0))==XTENSA_OK && cpu.reg(3)==0x12345678); // L32AI
    cpu.set_reg(10,15);
    CHECK(execute(bus,cpu,0x5069a0)==XTENSA_OK); // WITLB deny execution in region0
    CHECK(cpu.step()==XTENSA_EXCEPTION && cpu.special(XT_SR_EXCCAUSE)==20 && cpu.fault_address()==0x103);
    return true;
}

bool chip_alignment_and_exception_level() {
    Bus bus; XtensaCpu cpu(bus,true);
    cpu.set_reg(2,0x301); bus.put(0x301,0x12345678,4);
    CHECK(execute(bus,cpu,rri(2,2,3,0))==XTENSA_OK && cpu.reg(3)==0x12345678);
    cpu.set_reg(3,0xaabbccdd);
    CHECK(execute(bus,cpu,rri(6,2,3,0))==XTENSA_OK && rd32le(bus.bytes+0x301)==0xaabbccddu);
    CHECK(execute(bus,cpu,rri(14,2,3,0))==XTENSA_EXCEPTION && cpu.special(XT_SR_EXCCAUSE)==9);
    CHECK(execute(bus,cpu,rri(11,2,3,0))==XTENSA_EXCEPTION && cpu.special(XT_SR_EXCCAUSE)==9);
    CHECK(execute(bus,cpu,rri(15,2,3,0))==XTENSA_EXCEPTION && cpu.special(XT_SR_EXCCAUSE)==9);
    cpu.set_special(XT_SR_PS,0x40010);
    CHECK(!cpu.interrupt(2,0x300) && !cpu.interrupt(3,0x300));
    CHECK(cpu.interrupt(4,0x300));
    return true;
}

bool timers_and_irq_delivery() {
    Bus bus; XtensaCpu cpu(bus,true);
    cpu.set_special(XT_SR_VECBASE,0x400);
    cpu.set_special(XT_SR_CCOUNT,0xfffffffe);
    cpu.set_special(240,1); cpu.set_special(XT_SR_INTENABLE,1u<<6);
    u32 distance=0;
    CHECK(cpu.cycles_to_next_timer(distance) && distance==3);
    cpu.advance_cycles(2);
    CHECK(cpu.special(XT_SR_CCOUNT)==0 && !(cpu.special(XT_SR_INTERRUPT)&(1u<<6)));
    cpu.advance_cycles(1);
    CHECK(cpu.special(XT_SR_INTERRUPT)&(1u<<6));
    cpu.set_pc(0x100);
    CHECK(cpu.service_interrupts() && cpu.pc()==0x700 && cpu.special(XT_SR_EPC1)==0x100 && cpu.special(XT_SR_EXCCAUSE)==4);
    cpu.set_special(240,100);
    CHECK(!(cpu.special(XT_SR_INTERRUPT)&(1u<<6)));
    CHECK(execute(bus,cpu,0x003000)==XTENSA_OK && cpu.pc()==0x100); // rfe restores EPC1
    cpu.set_special(XT_SR_PS,0x40020); // user mode chooses user vector
    cpu.set_irq(6);
    CHECK(cpu.service_interrupts() && cpu.pc()==0x740);
    cpu.set_irq(6,false); cpu.set_special(XT_SR_PS,0x40000);
    cpu.set_special(241,20); cpu.set_special(XT_SR_INTENABLE,1u<<15);
    cpu.set_special(XT_SR_CCOUNT,10);
    CHECK(execute(bus,cpu,0x007000)==XTENSA_WAIT && cpu.waiting());
    CHECK(cpu.step()==XTENSA_WAIT && cpu.pc()==0x103); // stalled WAIT must not execute next memory
    CHECK(cpu.cycles_to_next_timer(distance) && distance==9);
    cpu.advance_cycles(distance);
    CHECK(cpu.service_interrupts() && !cpu.waiting() && cpu.pc()==0x5c0 && cpu.special(XT_SR_EPC1+2)==0x103);
    cpu.set_special(241,50);
    CHECK(!(cpu.special(XT_SR_INTERRUPT)&(1u<<15)));
    CHECK(execute(bus,cpu,0x003310)==XTENSA_OK && cpu.pc()==0x103); // rfi3
    cpu.set_special(XT_SR_INTENABLE,0); cpu.set_special(XT_SR_PS,15);
    cpu.set_irq(14);
    CHECK(cpu.service_interrupts() && cpu.pc()==0x6c0 && !(cpu.special(XT_SR_INTERRUPT)&(1u<<14))); // NMI
    cpu.reset(0x100); cpu.set_special(XT_SR_VECBASE,0x400); cpu.set_special(XT_SR_INTENABLE,1);
    cpu.set_irq(0); bus.put(0x700,0x0020f0);
    CHECK(cpu.step()==XTENSA_OK && cpu.pc()==0x703 && cpu.special(XT_SR_EPC1)==0x100); // automatic delivery
    XtensaCpu lx106(bus); lx106.set_special(XT_SR_VECBASE,0x400); lx106.set_special(XT_SR_INTENABLE,1u<<6); lx106.set_irq(6);
    CHECK(lx106.service_interrupts() && lx106.pc()==0x430);
    lx106.set_special(XT_SR_PS,0x20); lx106.set_irq(6);
    CHECK(lx106.service_interrupts() && lx106.pc()==0x450);
    cpu.reset(0); cpu.set_special(XT_SR_INTSET,0xffffffffu);
    CHECK(cpu.special(XT_SR_INTERRUPT)==0x20000080u); // only software interrupt inputs are writable through INTSET
    cpu.set_irq(0); cpu.set_irq(6); cpu.set_irq(10);
    cpu.set_special(XT_SR_INTCLEAR,0xffffffffu);
    CHECK(cpu.special(XT_SR_INTERRUPT)==0x41); // level inputs and timers require device/CCOMPARE acknowledgement
    cpu.set_special(240,10); cpu.set_special(241,20); cpu.set_special(242,30);
    cpu.set_special(XT_SR_INTENABLE,(1u<<6)|(1u<<15)|(1u<<16)); cpu.set_special(XT_SR_PS,1);
    CHECK(cpu.cycles_to_next_timer(distance) && distance==20); // masked earlier timer cannot hide later wake
    cpu.set_special(XT_SR_PS,16);
    CHECK(cpu.cycles_to_next_timer(distance) && distance==30); // EXCM masks levels1..3
    cpu.set_special(XT_SR_PS,5);
    CHECK(!cpu.cycles_to_next_timer(distance));
    return true;
}

bool synchronous_exceptions() {
    Bus bus; XtensaCpu cpu(bus,true);
    cpu.set_pc(0x123); cpu.set_special(XT_SR_VECBASE,0x400);
    cpu.enter_exception(9,0x301,true);
    CHECK(cpu.pc()==0x700 && cpu.special(XT_SR_EPC1)==0x123 && cpu.special(XT_SR_EXCVADDR)==0x301 && cpu.special(XT_SR_EXCCAUSE)==9);
    cpu.set_pc(0x704); cpu.enter_exception(28,0x10000,true);
    CHECK(cpu.pc()==0x7c0 && cpu.special(XT_SR_DEPC)==0x704 && cpu.special(XT_SR_EPC1)==0x123);
    CHECK(execute(bus,cpu,0x003200)==XTENSA_OK && cpu.pc()==0x704 && (cpu.special(XT_SR_PS)&16)); // rfde
    CHECK(execute(bus,cpu,0x003000)==XTENSA_OK && cpu.pc()==0x123 && !(cpu.special(XT_SR_PS)&16));
    cpu.set_special(XT_SR_PS,0x40020); cpu.set_pc(0x234); cpu.enter_exception(1);
    CHECK(cpu.pc()==0x740 && cpu.special(XT_SR_EPC1)==0x234);
    XtensaCpu lx106(bus); lx106.set_special(XT_SR_VECBASE,0x400); lx106.set_pc(0x123); lx106.enter_exception(9,0x301,true);
    CHECK(lx106.pc()==0x430 && lx106.special(XT_SR_EPC1)==0x123);
    lx106.enter_exception(9,0x303,true);
    CHECK(lx106.pc()==0x470 && lx106.special(XT_SR_DEPC)==0x430);
    return true;
}

bool chip_instruction_options() {
    /* These are different physical Xtensa configurations, not merely two
     * calling conventions. Unsupported instructions must stop unchanged. */
    Bus bus; XtensaCpu lx106(bus), lx6(bus,true);
    const u32 lx6_only[] = {
        0x740234,             // MAC16 mul.aa.ll
        0x048276,             // hardware loop
        0xa24230, 0xb24230,   // muluh, mulsh
        0xc24230, 0xd24230,   // quou, quos
        0xe24230, 0xf24230,   // remu, rems
        0x234200, 0x334200,   // sext, clamps
        0x434230, 0x534230,   // min, max
        0x634230, 0x734230,   // minu, maxu
        0x022230,             // andb
        0x00b230,             // all8
        0x00b232, 0x00f232,   // l32ai, s32ri
        0x00e232              // s32c1i
    };
    lx106.set_reg(2,0x300); lx106.set_reg(3,7);
    for (unsigned i=0;i<sizeof(lx6_only)/sizeof(lx6_only[0]);++i) {
        CHECK(execute(bus,lx106,lx6_only[i])==XTENSA_INVALID && lx106.pc()==0x100 && lx106.reg(3)==7);
    }
    CHECK(execute(bus,lx106,0x03b020)==XTENSA_OK && lx106.reg(2)==0xc28cdafau); // rsr CONFIGID0
    CHECK(execute(bus,lx106,0x03d020)==XTENSA_OK && lx106.reg(2)==0x1082b6f6u); // rsr CONFIGID1
    CHECK(execute(bus,lx6,0x03b020)==XTENSA_OK && lx6.reg(2)==0xc2bcfffeu);
    CHECK(execute(bus,lx6,0x03d020)==XTENSA_OK && lx6.reg(2)==0x1cc5fe96u);
    CHECK(execute(bus,lx106,0x005100)==XTENSA_INVALID); // SIMCALL requires a separately configured semihosting handler
    CHECK(execute(bus,lx106,0x005200)==XTENSA_INVALID); // reserved SYSCALL subopcode
    CHECK(execute(bus,lx106,0x005000)==XTENSA_EXCEPTION && lx106.special(XT_SR_EXCCAUSE)==1);
    return true;
}

bool floating_point() {
    Bus bus; XtensaCpu cpu(bus,true), lx106(bus);
    CHECK(execute(bus,lx106,0x0a4320)==XTENSA_INVALID);
    CHECK(execute(bus,cpu,0x0a4320)==XTENSA_EXCEPTION && cpu.special(XT_SR_EXCCAUSE)==32); // CP0 disabled
    CHECK(execute(bus,cpu,rrr(3,10,4,2,3))==XTENSA_INVALID); // reserved FP opcode remains illegal with CP disabled
    CHECK(execute(bus,cpu,0xe34e80)==XTENSA_EXCEPTION && cpu.special(XT_SR_EXCCAUSE)==32); // RUR FCR
    cpu.set_special(224,1);
    cpu.set_float_bits(2,0x3fc00000); cpu.set_float_bits(3,0x40100000); // 1.5,2.25
    CHECK(execute(bus,cpu,rrr(0,10,4,2,3))==XTENSA_OK && cpu.float_bits(4)==0x40700000); // add=3.75
    CHECK(execute(bus,cpu,rrr(1,10,4,2,3))==XTENSA_OK && cpu.float_bits(4)==0xbf400000); // sub=-.75
    CHECK(execute(bus,cpu,rrr(2,10,4,2,3))==XTENSA_OK && cpu.float_bits(4)==0x40580000); // mul=3.375
    cpu.set_float_bits(4,0x3f800000);
    CHECK(execute(bus,cpu,rrr(4,10,4,2,3))==XTENSA_OK && cpu.float_bits(4)==0x408c0000); // fused 1+3.375
    CHECK(execute(bus,cpu,rrr(5,10,4,2,3))==XTENSA_OK && cpu.float_bits(4)==0x3f800000);
    cpu.set_float_bits(2,0x3f800001); cpu.set_float_bits(3,0x3f7ffffe); cpu.set_float_bits(4,0xbf800000);
    CHECK(execute(bus,cpu,rrr(4,10,4,2,3))==XTENSA_OK && cpu.float_bits(4)==0xa8800000); // fused cancellation -2^-46
    cpu.set_float_bits(2,0x3f800000); cpu.set_float_bits(3,0x33800000); // 1+half ULP
    cpu.set_user(232,0);
    CHECK(execute(bus,cpu,rrr(0,10,4,2,3))==XTENSA_OK && cpu.float_bits(4)==0x3f800000 && (cpu.user(233)&0x80));
    cpu.set_user(232,2); // round towards +infinity
    CHECK(execute(bus,cpu,rrr(0,10,4,2,3))==XTENSA_OK && cpu.float_bits(4)==0x3f800001);
    cpu.set_user(232,0); cpu.set_float_bits(2,0x40200000); // 2.5, ties to even
    CHECK(execute(bus,cpu,rrr(8,10,4,2,0))==XTENSA_OK && cpu.reg(4)==2);
    cpu.set_float_bits(2,0xc0300000); // -2.75
    CHECK(execute(bus,cpu,rrr(9,10,4,2,0))==XTENSA_OK && cpu.reg(4)==u32(-2));
    CHECK(execute(bus,cpu,rrr(10,10,4,2,0))==XTENSA_OK && cpu.reg(4)==u32(-3));
    CHECK(execute(bus,cpu,rrr(11,10,4,2,0))==XTENSA_OK && cpu.reg(4)==u32(-2));
    cpu.set_reg(2,u32(-5));
    CHECK(execute(bus,cpu,rrr(12,10,4,2,1))==XTENSA_OK && cpu.float_bits(4)==0xc0200000); // FLOAT scale1
    cpu.set_reg(2,10);
    CHECK(execute(bus,cpu,rrr(13,10,4,2,2))==XTENSA_OK && cpu.float_bits(4)==0x40200000);
    cpu.set_float_bits(2,0x7fc00001); cpu.set_float_bits(3,0x3f800000);
    CHECK(execute(bus,cpu,rrr(1,11,4,2,3))==XTENSA_OK && (cpu.special(XT_SR_BR)&16)); // unordered
    CHECK(execute(bus,cpu,rrr(2,11,4,2,3))==XTENSA_OK && !(cpu.special(XT_SR_BR)&16)); // ordered equal
    CHECK(execute(bus,cpu,rrr(3,11,4,2,3))==XTENSA_OK && (cpu.special(XT_SR_BR)&16));
    cpu.set_reg(2,0x80000000);
    CHECK(execute(bus,cpu,rrr(15,10,4,2,5))==XTENSA_OK && cpu.float_bits(4)==0x80000000); // WFR preserves -0
    CHECK(execute(bus,cpu,rrr(15,10,2,4,4))==XTENSA_OK && cpu.reg(2)==0x80000000);
    CHECK(execute(bus,cpu,rrr(15,10,3,4,1))==XTENSA_OK && cpu.float_bits(3)==0); // ABS -0
    cpu.set_reg(2,0x300); bus.put(0x304,0x7fc01234,4);
    CHECK(execute(bus,cpu,0x010243)==XTENSA_OK && cpu.float_bits(4)==0x7fc01234); // LSI preserves payload
    CHECK(execute(bus,cpu,0x02c243)==XTENSA_OK && rd32le(bus.bytes+0x308)==0x7fc01234 && cpu.reg(2)==0x308); // SSIU
    cpu.set_reg(3,4);
    CHECK(execute(bus,cpu,rrr(4,8,4,2,3))==XTENSA_OK && rd32le(bus.bytes+0x30c)==0x7fc01234); // SSX
    cpu.set_reg(2,0x2000);
    CHECK(execute(bus,cpu,0x018243)==XTENSA_MEMORY_FAULT && cpu.reg(2)==0x2000 && cpu.float_bits(4)==0x7fc01234); // failed LSIU atomicity
    cpu.set_float_bits(2,0x40800000); cpu.set_float_bits(4,0x40000000);
    CHECK(execute(bus,cpu,rrr(15,10,4,2,13))==XTENSA_OK && cpu.float_bits(4)==0x40000000); // division adjustment
    CHECK(execute(bus,cpu,rrr(15,10,4,2,12))==XTENSA_OK && cpu.float_bits(4)==0x40000000); // sqrt adjustment
    cpu.set_user(233,0); cpu.set_float_bits(2,0xbf800000);
    CHECK(execute(bus,cpu,rrr(15,10,4,2,12))==XTENSA_OK && (cpu.float_bits(4)&0x7f800000)==0x7f800000 && (cpu.user(233)&0x800));
    cpu.set_user(233,0); cpu.set_float_bits(2,0x7fc00000);
    CHECK(execute(bus,cpu,rrr(9,10,4,2,0))==XTENSA_OK && cpu.reg(4)==0x7fffffffu && (cpu.user(233)&0x800)); // saturated invalid conversion
    cpu.set_float_bits(2,0x4f800000); // 2^32 out of unsigned range
    CHECK(execute(bus,cpu,rrr(14,10,4,2,0))==XTENSA_OK && cpu.reg(4)==0xffffffffu);
    cpu.set_reg(2,0xffffffffu);
    CHECK(execute(bus,cpu,0xf3e820)==XTENSA_OK && cpu.user(232)==0x7f); // WUR FCR masks reserved bits
    CHECK(execute(bus,cpu,0xe34e80)==XTENSA_OK && cpu.reg(4)==0x7f);
    cpu.set_user(232,0); cpu.set_special(XT_SR_VECBASE,0x300); cpu.set_special(XT_SR_WINDOWSTART,3);
    cpu.set_float_bits(12,0x3f800000); cpu.set_float_bits(13,0x40000000);
    CHECK(execute(bus,cpu,rrr(0,10,14,12,13))==XTENSA_OK && cpu.pc()==0x103 && cpu.float_bits(14)==0x40400000); // FPR numbers never cause AR window overflow
    CHECK(execute(bus,cpu,rrr(15,10,4,2,4))==XTENSA_OK && cpu.pc()==0x400 && cpu.special(XT_SR_EPC1)==0x100); // RFR does require its AR window
    return true;
}

bool windows_and_rom() {
    Bus bus; XtensaCpu cpu(bus,true);
    cpu.set_reg(1,0x800); cpu.set_reg(10,123);
    CHECK(execute(bus,cpu,0x000025)==XTENSA_OK && cpu.pc()==0x104 && cpu.reg(8)==0x80000103u);
    CHECK(cpu.rom_argument(0)==123 && cpu.pending_call_increment()==2);
    cpu.rom_return(99);
    CHECK(cpu.pc()==0x103 && cpu.reg(10)==99 && cpu.special(XT_SR_WINDOWBASE)==0);
    CHECK(execute(bus,cpu,0x000025)==XTENSA_OK);
    CHECK(execute(bus,cpu,0x004136)==XTENSA_OK); // entry a1,32
    CHECK(cpu.special(XT_SR_WINDOWBASE)==2 && cpu.reg(1)==0x7e0 && cpu.reg(0)==0x80000103u && cpu.reg(2)==99);
    cpu.set_reg(2,456);
    CHECK(execute(bus,cpu,0xf01d,2)==XTENSA_OK && cpu.pc()==0x103 && cpu.special(XT_SR_WINDOWBASE)==0 && cpu.reg(10)==456);
    CHECK(cpu.reg(1)==0x800 && cpu.special(XT_SR_WINDOWSTART)==1);
    /* A live next window must spill through firmware's overflow vector. */
    cpu.set_special(XT_SR_VECBASE,0x300); cpu.set_special(XT_SR_WINDOWSTART,7);
    cpu.set_reg(4,0xaabbccdd);
    CHECK(execute(bus,cpu,0x000015)==XTENSA_OK && cpu.pc()==0x300);
    CHECK(cpu.special(XT_SR_WINDOWBASE)==1 && cpu.special(XT_SR_EPC1)==0x100 && (cpu.special(XT_SR_PS)&16));
    CHECK(execute(bus,cpu,0x003400)==XTENSA_OK && cpu.pc()==0x100 && cpu.special(XT_SR_WINDOWBASE)==0 && cpu.special(XT_SR_WINDOWSTART)==5);
    /* Underflow rotates to the missing caller and records old window. */
    cpu.set_special(XT_SR_WINDOWBASE,2); cpu.set_special(XT_SR_WINDOWSTART,4); cpu.set_reg(0,0x80000200u);
    CHECK(execute(bus,cpu,0xf01d,2)==XTENSA_OK && cpu.pc()==0x3c0 && cpu.special(XT_SR_WINDOWBASE)==0);
    CHECK(execute(bus,cpu,0x003500)==XTENSA_OK && cpu.pc()==0x100 && cpu.special(XT_SR_WINDOWBASE)==2 && cpu.special(XT_SR_WINDOWSTART)==5);
    CHECK(execute(bus,cpu,0xf01d,2)==XTENSA_OK && cpu.pc()==0x200 && cpu.special(XT_SR_WINDOWBASE)==0 && cpu.special(XT_SR_WINDOWSTART)==1);
    XtensaCpu lx106(bus);
    CHECK(execute(bus,lx106,0x000025)==XTENSA_INVALID);
    CHECK(execute(bus,lx106,0xf01d,2)==XTENSA_INVALID);
    cpu.reset(0x100); cpu.set_reg(2,2); cpu.set_reg(10,0xabcdef);
    CHECK(execute(bus,cpu,0x614820)==XTENSA_OK && cpu.special(XT_SR_WINDOWBASE)==2 && cpu.reg(2)==0xabcdef); // xsr a2,WINDOWBASE
    cpu.set_special(XT_SR_WINDOWBASE,0);
    CHECK(cpu.reg(2)==0); // XSR result belongs to the old physical window.
    return true;
}

bool nested_interrupt_returns() {
    Bus bus; XtensaCpu cpu(bus,true);
    const u32 original_ps=0x70020; // windowed user task with CALLINC=3
    cpu.set_pc(0x100); cpu.set_special(XT_SR_PS,original_ps);
    cpu.set_special(XT_SR_WINDOWBASE,6); cpu.set_special(XT_SR_WINDOWSTART,1u<<6);
    cpu.set_reg(1,0xe00); cpu.set_reg(12,0x12345678);
    CHECK(cpu.interrupt(2,0x500));
    CHECK(cpu.special(178)==0x100 && cpu.special(194)==original_ps);
    const u32 level2_ps=cpu.special(XT_SR_PS);
    cpu.set_pc(0x503);
    CHECK(!cpu.interrupt(3,0x600)); // EXCM masks the medium-priority levels.
    CHECK(cpu.interrupt(5,0x700));
    CHECK(cpu.special(181)==0x503 && cpu.special(197)==level2_ps);
    bus.put(0x700,0x003510); // RFI 5: resume the interrupted handler.
    CHECK(cpu.step()==XTENSA_OK && cpu.pc()==0x503 && cpu.special(XT_SR_PS)==level2_ps);
    bus.put(0x503,0x003210); // RFI 2: resume the original user task.
    CHECK(cpu.step()==XTENSA_OK && cpu.pc()==0x100 && cpu.special(XT_SR_PS)==original_ps);
    CHECK(cpu.special(XT_SR_WINDOWBASE)==6 && cpu.special(XT_SR_WINDOWSTART)==(1u<<6));
    CHECK(cpu.reg(1)==0xe00 && cpu.reg(12)==0x12345678);

    /* A high-priority interrupt can preempt a window spill. Its return must
     * preserve EPC1 and PS.OWB so RFWO can retry the original instruction. */
    cpu.reset(0x100); cpu.set_special(XT_SR_VECBASE,0x300);
    cpu.set_special(XT_SR_WINDOWSTART,7); cpu.set_reg(4,0xabcdef01);
    CHECK(execute(bus,cpu,0x000015)==XTENSA_OK && cpu.pc()==0x300);
    const u32 spill_ps=cpu.special(XT_SR_PS);
    CHECK(cpu.interrupt(4,0x700));
    bus.put(0x700,0x003410); // RFI 4
    CHECK(cpu.step()==XTENSA_OK && cpu.pc()==0x300 && cpu.special(XT_SR_PS)==spill_ps);
    CHECK(cpu.special(XT_SR_EPC1)==0x100 && cpu.special(XT_SR_WINDOWBASE)==1);
    bus.put(0x300,0x003400); // RFWO
    CHECK(cpu.step()==XTENSA_OK && cpu.pc()==0x100 && cpu.special(XT_SR_WINDOWBASE)==0);
    CHECK(cpu.special(XT_SR_WINDOWSTART)==5 && cpu.reg(4)==0xabcdef01);
    return true;
}

bool shared_atomic_counter() {
    /* Two cores execute the same guest CAS loop against shared RAM. Every
     * failed compare retries the load; no host-side lock increments the
     * counter. Encodings were checked with the ESP32 assembler. */
    static const u32 program[]={
        0x00a322,0x28a032, // MOVI a2,0x300; MOVI a3,40
        0x002242,0x130c40, // retry: L32I a4,a2,0; WSR a4,SCOMPARE1
        0x01c452,0x00e252, // ADDI a5,a4,1; S32C1I a5,a2,0
        0xf09457,0xffc332, // BNE a4,a5,retry; ADDI a3,a3,-1
        0xfea356,0x007000  // BNEZ a3,retry; WAITI 0
    };
    Bus bus; XtensaCpu pro(bus,true),app(bus,true);
    for(unsigned i=0;i<sizeof(program)/sizeof(program[0]);++i)bus.put(0x100+i*3,program[i]);
    pro.set_pc(0x100);app.set_pc(0x100);
    unsigned attempts=0,steps=0;bool valid=true;
    while(steps<4000 && (!pro.waiting() || !app.waiting())) {
        XtensaCpu &cpu=(steps&1)?app:pro;
        if(!cpu.waiting()) {
            if(cpu.pc()==0x10f)++attempts;
            const XtensaResult result=cpu.step();
            if(result!=XTENSA_OK && result!=XTENSA_WAIT){valid=false;break;}
        }
        ++steps;
    }
    CHECK(valid && pro.waiting() && app.waiting());
    CHECK(rd32le(bus.bytes+0x300)==80 && pro.reg(3)==0 && app.reg(3)==0);
    CHECK(attempts>80); // The fixture actually exercised contended retries.

    struct WriteFaultBus : Bus {
        bool reject;
        WriteFaultBus():reject(true) {}
        virtual bool write(u32 addr,unsigned size,u32 value) {
            return !reject && Bus::write(addr,size,value);
        }
    } fault_bus;
    XtensaCpu cpu(fault_bus,true);
    fault_bus.put(0x300,7,4);cpu.set_reg(2,0x300);cpu.set_reg(3,99);
    cpu.set_special(XT_SR_SCOMPARE1,7);
    CHECK(execute(fault_bus,cpu,0x00e232)==XTENSA_MEMORY_FAULT);
    CHECK(cpu.pc()==0x100 && cpu.reg(3)==99 && cpu.instructions()==0 && rd32le(fault_bus.bytes+0x300)==7);
    fault_bus.reject=false;
    CHECK(cpu.step()==XTENSA_OK && cpu.reg(3)==7 && rd32le(fault_bus.bytes+0x300)==99);
    return true;
}

bool recursive_window_spills() {
    /* Recursive CALL4 and CALL12 exercise both other Xtensa ABI frame
     * sizes, including wraparound and guest stack-backed spill/fill. */
    for(unsigned increment=1;increment<=3;increment+=2) {
        Bus bus;XtensaCpu cpu(bus,true);
        const unsigned outgoing=increment*4+2,depth=increment==1?20:10;
        bus.put(0x100,0x008136); // ENTRY a1,64
        bus.put(0x103,rri(10,0,outgoing,depth));
        bus.put(0x106,0x000085|(increment<<4)); // CALLn recurse at0x110
        bus.put(0x109,rrr(2,0,2,outgoing,outgoing));
        bus.put(0x10c,0x007000);
        bus.put(0x110,0x008136); // recurse: ENTRY a1,64
        bus.put(0x113,0x00a216); // BEQZ a2,base
        bus.put(0x116,rri(12,2,outgoing,255));
        bus.put(0x119,0xffff45|(increment<<4));
        bus.put(0x11c,rri(12,outgoing,2,1));
        bus.put(0x11f,0xf01d,2);
        bus.put(0x121,rri(10,0,2,7)); // base returns7
        bus.put(0x124,0xf01d,2);
        const u32 overflow=0x400+(increment-1)*0x80,underflow=overflow+0x40;
        const unsigned callee_sp=increment*4+1;
        u32 save=overflow,restore=underflow;
        for(unsigned r=0;r<4;++r) {
            bus.put(save,rrr(4,9,12+r,callee_sp,r));save+=3; // S32E into caller base-save area
            bus.put(restore,rrr(0,9,12+r,callee_sp,r));restore+=3;
            if(increment==3 && r==0){bus.put(save,rrr(0,9,13,1,0));save+=3;}
            if(increment==3 && r==2){bus.put(restore,rrr(0,9,13,1,11));restore+=3;}
        }
        if(increment==3) {
            for(unsigned r=4;r<12;++r) {
                bus.put(save,rrr(4,9,r,0,r));save+=3; // S32E a4..a11 at ancestorSP-48..-20
                bus.put(restore,rrr(0,9,r,11,r));restore+=3;
            }
        }
        bus.put(save,0x003400);bus.put(restore,0x003500); // RFWO/RFWU
        cpu.set_special(XT_SR_VECBASE,0x400);cpu.set_special(XT_SR_PS,0x40000|(increment<<16));
        cpu.set_reg(1,0xf00);cpu.set_reg(increment*4,(increment<<30)|0x80);
        bus.put(0xf00-12,0xf40,4); // Incoming call's stack backlink.
        cpu.set_pc(0x100);unsigned spills=0,fills=0;XtensaResult result=XTENSA_OK;
        for(unsigned steps=0;steps<2000 && result==XTENSA_OK;++steps) {
            if(cpu.pc()==overflow)++spills;
            if(cpu.pc()==underflow)++fills;
            result=cpu.step();
        }
        CHECK(result==XTENSA_WAIT && cpu.reg(2)==depth+7);
        CHECK(cpu.reg(1)==0xec0 && cpu.special(XT_SR_WINDOWBASE)==increment);
        CHECK(spills>0 && fills>0);
    }
    return true;
}
}

int xtensa_tests() {
    checks=0;
    if (!external_registers_and_identity() || !nested_interrupt_returns() || !shared_atomic_counter() || !recursive_window_spills())return -1;
    if (!density() || !arithmetic() || !shifts() || !memory() || !nonbuffered_stores() || !branches_calls_loops() || !special_and_interrupts() || !mac16() || !region_protection() || !chip_alignment_and_exception_level() || !timers_and_irq_delivery() || !synchronous_exceptions() || !chip_instruction_options() || !floating_point() || !windows_and_rom()) return -1;
    printf("All Xtensa CPU tests passed (%d checks).\n",checks);
    return checks;
}

#ifdef XTENSA_TEST_MAIN
int main() { return xtensa_tests()<0 ? 1 : 0; }
#endif
