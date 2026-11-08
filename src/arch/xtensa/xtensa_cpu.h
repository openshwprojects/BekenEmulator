/* Little-endian Xtensa interpreter shared by ESP8266 (LX106/call0) and
 * ESP32 (LX6/windowed). The bus owns memory and peripheral policy; the CPU
 * owns architectural state only. Unknown instructions stop at their PC.
 * Encodings: Espressif's Overview of Xtensa ISA, cross-checked against
 * the upstream QEMU Xtensa decoder (the overview contains errata).
 */
#ifndef BK_XTENSA_CPU_H
#define BK_XTENSA_CPU_H

#include "common.h"

class XtensaBus {
public:
    virtual ~XtensaBus() {}
    virtual bool read(u32 address, unsigned size, u32 &value, bool instruction) = 0;
    virtual bool write(u32 address, unsigned size, u32 value) = 0;
    virtual bool external_read(u32, u32 &) { return false; }
    virtual bool external_write(u32, u32) { return false; }
};

enum XtensaResult {
    XTENSA_OK = 0, XTENSA_WAIT, XTENSA_BREAK, XTENSA_INVALID,
    XTENSA_MEMORY_FAULT, XTENSA_EXCEPTION
};

enum XtensaSpecial {
    XT_SR_LBEG = 0, XT_SR_LEND = 1, XT_SR_LCOUNT = 2, XT_SR_SAR = 3,
    XT_SR_BR = 4, XT_SR_LITBASE = 5, XT_SR_SCOMPARE1 = 12,
    XT_SR_WINDOWBASE = 72, XT_SR_WINDOWSTART = 73,
    XT_SR_EPC1 = 177, XT_SR_DEPC = 192,
    XT_SR_INTERRUPT = 226, XT_SR_INTSET = 226, XT_SR_INTCLEAR = 227,
    XT_SR_INTENABLE = 228, XT_SR_PS = 230, XT_SR_VECBASE = 231,
    XT_SR_EXCCAUSE = 232, XT_SR_CCOUNT = 234, XT_SR_PRID = 235, XT_SR_EXCVADDR = 238
};

class XtensaCpu {
public:
    explicit XtensaCpu(XtensaBus &bus, bool windowed = false);
    void reset(u32 pc);
    void set_processor_id(u32 id) { processor_id_=id;sr_[XT_SR_PRID]=id; }
    XtensaResult step();
    XtensaResult run(u64 count);
    u32 reg(unsigned index) const;
    void set_reg(unsigned index, u32 value);
    u32 pc() const { return pc_; }
    void set_pc(u32 value) { pc_ = value; waiting_ = false; }
    u32 special(unsigned index) const { return sr_[index & 255]; }
    void set_special(unsigned index, u32 value);
    u32 user(unsigned index) const { return ur_[index & 255]; }
    void set_user(unsigned index, u32 value);
    u32 float_bits(unsigned index) const { return fr_[index & 15]; }
    void set_float_bits(unsigned index, u32 value) { fr_[index & 15] = value; }
    u64 instructions() const { return instructions_; }
    u32 fault_address() const { return fault_address_; }
    u32 last_instruction() const { return instruction_; }
    bool windowed() const { return windowed_; }
    /* ROM interception happens before the callee ENTRY instruction. */
    u32 rom_argument(unsigned index) const;
    void rom_return(u32 value);
    unsigned pending_call_increment() const { return pending_call_; }
    /* Deliver an interrupt whose peripheral routing has been resolved by
     * the machine. Returns false if masked. level is 1..7. */
    bool interrupt(unsigned level, u32 vector);
    /* Explicitly deliver an architectural fault after the machine decides
     * it has a real handler. Missing devices and unsupported opcodes remain
     * inspectable stops and are never silently converted to guest success. */
    void enter_exception(unsigned cause, u32 address = 0, bool set_address = false);
    void set_irq(unsigned index, bool active = true);
    bool service_interrupts();
    void advance_cycles(u32 cycles);
    bool cycles_to_next_timer(u32 &cycles) const;
    bool waiting() const { return waiting_; }

private:
    XtensaBus &bus_;
    bool windowed_;
    bool waiting_;
    u32 ar_[64], sr_[256], ur_[256], fr_[16];
    u8 instruction_attr_[8], data_attr_[8];
    u32 pc_, instruction_, fault_address_, processor_id_;
    u64 instructions_;
    unsigned pending_call_;
    XtensaResult data_read(u32 addr, unsigned size, u32 &value);
    XtensaResult data_write(u32 addr, unsigned size, u32 value);
    XtensaResult execute(u32 insn, unsigned length, u32 &next, bool &jump);
    void call(unsigned increment, u32 return_pc);
    XtensaResult retw(u32 &next);
    bool check_window(unsigned highest);
    bool access_permitted(u32 addr, bool instruction, bool store);
    XtensaResult float_operation(u32 instruction, u32 &next, bool &jump);
};

#endif
