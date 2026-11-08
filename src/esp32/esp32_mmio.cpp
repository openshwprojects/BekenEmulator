#include "esp/esp_machine.h"
#include "esp/esp_spi.h"
#include "esp32/esp32_rng.h"
#include "esp32/esp32_cache.h"
#include "esp32/esp32_rtc_io.h"
#include "esp32/esp32_analog.h"
#include "esp32/esp32_phy.h"
#include "esp32/esp_aes.h"
#include "esp/esp_state.h"
#include "crypto_prims.h"

/* Classic ESP32 definitions follow ESP-IDF's soc/esp32/register/soc headers.
 * These are named, bounded devices. Reserved words fail rather than becoming
 * anonymous zero pages. Configuration storage uses documented writable masks.
 * Timing is deterministic instruction time, not host wall time. */
namespace {
struct Register { u32 address, reset, writable; };
const Register configuration[] = {
    {0x3ff00000,0,1}, {0x3ff00004,0,1}, /* PRO/APP boot remap */
    {0x3ff0001c,0,0x1f}, {0x3ff00020,0,0x1f}, /* crypto clock/reset */
    {0x3ff00024,0,0xffffffff}, {0x3ff00028,0,0xffffffff}, /* DPORT Wi-Fi BB configuration */
    {0x3ff0002c,1,1}, {0x3ff00030,0,1}, {0x3ff00034,0,1},
    {0x3ff00038,0,0xffffffff}, /* APPCPU boot address */
    {0x3ff0003c,0,0xf},       /* CPU period and peripheral clock select */
    {0x3ff00044,0x8ff,0x3fff}, {0x3ff0005c,0x8ff,0x3fff},
    {0x3ff0007c,0,3},        /* cache mux mode */
    {0x3ff00094,3,0xff},     /* ROM forced-on overrides; clear permits automatic gating */
    {0x3ff000a0,0xffffffff,0xffffffff}, {0x3ff000a4,1,1}, /* SRAM forced-on overrides */
    {0x3ff000ac,0x101,0x303}, /* PRO/APP cache tag forced-on/power-down controls */
    {0x3ff000bc,0,0xffff},   /* host link selector and peripheral IO swap */
    {0x3ff000c0,0xf9c1e06f,0xffffffff}, /* peripheral clock enables */
    {0x3ff000c4,0,0xffffffff},           /* peripheral reset enables */
    {0x3ff000cc,0xfffce030,0xffffffff}, /* wireless/SDIO/RNG clock enables */
    /* DPORT_CORE_RST_EN: BB/FE/MAC/BT/SDIO/EMAC reset lines. These remain
     * asserted until explicitly released; clocks and SoC reset are separate.
     * The Wi-Fi baseband reset also clears its decoded receiver controls. */
    {0x3ff000d0,0,0x7ff},
    {0x3ff00440,0x100,0x111}, {0x3ff00468,0x100,0x111}, /* CPU record controls */
    {0x3ff005a0,0,0x0fffffff}, /* per-core cache invalid-access interrupt enables */
    {0x3ff48000,0x1c492000,0x7fffffcf}, /* oscillator/PLL power controls */
    {0x3ff4800c,0,0}, {0x3ff48010,0,0}, {0x3ff48014,0,0},
    {0x3ff4801c,0x28140403,0xffffffff}, /* PLL/XTAL/RC startup and CPU-stall wait counts */
    {0x3ff48030,0x00800000,0xdf800000}, /* analog power and PLLA controls */
    {0x3ff48034,0x41,0},     /* power-on reset on both CPUs */
    {0x3ff4803c,0,0x1ff}, {0x3ff48040,0,0},
    {0x3ff4804c,0,0xffffffff}, {0x3ff48050,3495253,0xffffffff},
    {0x3ff48054,0,0xffffffff}, {0x3ff48058,0,0xffffffff},
    {0x3ff48070,0x08002210,0xfffffff0}, /* ROM selected 80 MHz PLL */
    {0x3ff48074,0x02a00000,0xfee00000}, /* SDIO regulator configuration */
    {0x3ff48078,0,0xff000000}, /* analog bias heartbeat/attenuation configuration */
    {0x3ff4807c,0xa9002400,0xffffff80}, /* analog/digital regulator bias */
    {0x3ff48080,0x00012925,0x001fffff}, /* RTC/fast/slow RAM power and isolation overrides */
    {0x3ff48084,0x00155550,0xff1ffff8}, /* digital-domain sleep policy and power overrides */
    {0x3ff48088,0xaaaa5000,0xfffff980}, /* digital isolation and pad-hold configuration */
    {0x3ff4808c,0x4c80,0xffffff80}, /* RTC watchdog stage actions */
    {0x3ff48090,128000,0xffffffff}, {0x3ff48094,80000,0xffffffff},
    {0x3ff48098,4095,0xffffffff}, {0x3ff4809c,4095,0xffffffff},
    {0x3ff480a0,0,0}, {0x3ff480a4,0x50d83aa1,0xffffffff},
    {0x3ff480a8,0,0xe0000000}, /* RTC test/entropy-source mux */
    {0x3ff480ac,0,0xfff00000}, /* per-core software stall upper fields */
    /* ROM handover values: 40 MHz XTAL and 80 MHz APB, each duplicated in
     * both halfwords as required by rtc_clk_xtal_freq_get/apb_freq_get. */
    {0x3ff480b0,0x00280028,0xffffffff}, {0x3ff480b4,0x4c4b4c4b,0xffffffff},
    {0x3ff480b8,0,0xffffffff}, {0x3ff480bc,0,0xffffffff},
    {0x3ff480c8,0,0x3ffff}, /* forced RTC pad hold; release is supported below */
    /* Normal powered operation: DET stays low. Enabling detection, reset or
     * RF/flash shutdown does not itself cause a voltage-drop event. */
    {0x3ff480d4,0x13ff0000,0x7fffc000}, /* brownout threshold, wait and action enables */
    {0x3ff5a0f8,0,0xffffffff}, {0x3ff5a0fc,0,0xffff},
    {0x3ff5a110,0,3}, {0x3ff5a118,0,0xffffffff},
    {0x3ff66000,0x2000,0x3fff}, /* SYSCON system-clock source divider */
    {0x3ff66004,39,0xff}, {0x3ff66008,79,0xff}, {0x3ff6600c,11,0xff},
    {0x3ff6607c,0x96042000,0x7fffffff} /* date plus revision-3 ECO bit */
};

const Register *find_register(u32 address)
{
    for (size_t i = 0; i < sizeof(configuration)/sizeof(configuration[0]); ++i)
        if (configuration[i].address == address) return &configuration[i];
    return 0;
}

u32 stored(const std::map<u32,u32> &registers, u32 address, u32 initial = 0)
{
    std::map<u32,u32>::const_iterator it = registers.find(address);
    return it == registers.end() ? initial : it->second;
}

u32 rtc_slow_hz(const std::map<u32,u32> &registers)
{
    unsigned source = stored(registers,0x3ff48070,0x08002210) >> 30;
    return source == 1 ? 32768 : source == 2 ? 31250 : 150000;
}

/* Host state keys are outside all decoded hardware windows. */
const u32 EFUSE_DEADLINE_LO = 0xffff1000, EFUSE_DEADLINE_HI = 0xffff1004;
const u32 SHA_STATE = esp_state::SHA, SHA_STARTED = esp_state::SHA+0x20;
const u32 RTC_WDT_DEADLINE = 0xffff1200, RTC_WDT_STAGE = 0xffff1208;
const u32 TIMG_WDT_STATE = 0xffff1240;

u32 timer_group_base(unsigned group) { return group?0x3ff60000:0x3ff5f000; }

u32 watchdog_register(u32 address)
{
    u32 base=address>=0x3ff60000?0x3ff60000:0x3ff5f000;
    return address>=base+0x48 && address<=base+0x64 && !(address&3)?base:0;
}

u32 watchdog_reset(u32 offset)
{
    static const u32 defaults[]={0x4c000,0x10000,26000000,0x7ffffff,0xfffff,0xfffff,0,0x50d83aa1};
    return defaults[(offset-0x48)/4];
}

u64 deadline_at(const std::map<u32,u32> &regs,u32 key)
{
    return stored(regs,key)|((u64)stored(regs,key+4)<<32);
}

void arm_timer_watchdog(std::map<u32,u32> &regs,unsigned group,u64 now,unsigned stage)
{
    u32 base=timer_group_base(group),state=TIMG_WDT_STATE+group*16;
    u32 hold=stored(regs,base+0x50+stage*4,watchdog_reset(0x50+stage*4));
    unsigned prescale=stored(regs,base+0x4c,0x10000)>>16;
    u64 deadline=now+(u64)(hold?hold:1)*prescale;
    regs[state]=(u32)deadline;regs[state+4]=(u32)(deadline>>32);regs[state+8]=stage;
}

void arm_rtc_wdt(std::map<u32,u32> &regs,u64 now,unsigned stage)
{
    const u32 defaults[]={128000,80000,4095,4095};
    u32 hold=stored(regs,0x3ff48090+stage*4,defaults[stage]);
    u32 hz=rtc_slow_hz(regs);
    u64 deadline=now+((u64)(hold?hold:1)*1000000+hz-1)/hz;
    regs[RTC_WDT_DEADLINE]=(u32)deadline;
    regs[RTC_WDT_DEADLINE+4]=(u32)(deadline>>32);
    regs[RTC_WDT_STAGE]=stage;
}

u32 efuse_identity(unsigned word)
{
    /* Deterministic locally administered MAC 02:00:00:00:00:32, with Dallas
     * CRC8 matching the ESP32 ROM, rather than another device's identity. */
    const u8 mac[] = {2,0,0,0,0,0x32};
    u8 crc = 0;
    for (unsigned i = 0; i < sizeof(mac); ++i) {
        crc ^= mac[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0x8c : 0);
    }
    if (word == 1) return 0x32;
    if (word == 2) return 0x200 | ((u32)crc << 16);
    if (word == 3) return 1 << 15;
    if (word == 5) return 1 << 20;
    return 0; /* unencrypted dual-core D0WD revision 3, no burned overrides */
}

int flash_entry(u32 address)
{
    if (address >= 0x3f400000 && address < 0x3f800000)
        return (address & 0x3fffff) >> 16;
    if (address >= 0x400d0000 && address < 0x40400000)
        return 64 + ((address & 0x3fffff) >> 16);
    if (address >= 0x40400000 && address < 0x40800000)
        return 128 + ((address & 0x3fffff) >> 16);
    if (address >= 0x40800000 && address < 0x40c00000)
        return 192 + ((address & 0x3fffff) >> 16);
    return -1;
}
}

bool EspMachine::update_esp32_rtc()
{
    if(chip_!=esp::CHIP_ESP32)return true;
    /* Both watchdog families participate in the machine's idle deadline.
     * TIMG is clocked from the continuous APB clock, RTC from its slow clock. */
    for(unsigned group=0;group<2;++group) {
        const u32 base=timer_group_base(group),state=TIMG_WDT_STATE+group*16;
        const u32 config=stored(registers_,base+0x48,0x4c000);
        if(!(config&0x80000000u))continue;
        u64 deadline=deadline_at(registers_,state);
        for(unsigned n=0;deadline && esp32_apb_ticks()>=deadline && n<4;++n) {
            const unsigned stage=stored(registers_,state+8)&3;
            const unsigned action=(config>>(29-stage*2))&3;
            /* Advance before queuing a reset. CPU-only resets preserve the
             * watchdog and must resume the following stage, not retrigger. */
            arm_timer_watchdog(registers_,group,deadline,(stage+1)&3);
            if(action==1)interrupts_.raise_group_interrupt(group,2);
            else if(action>=2) {
                request_reset(action==2?RESET_CPU:RESET_DIGITAL,action==2?(1u<<group):3,action==2?11:7+group);
                return true;
            }
            deadline=deadline_at(registers_,state);
        }
    }
    u32 config=stored(registers_,0x3ff4808c,0x4c80);
    if(config&0x80000000u) {
        u64 deadline=stored(registers_,RTC_WDT_DEADLINE)|((u64)stored(registers_,RTC_WDT_DEADLINE+4)<<32);
        for(unsigned n=0;deadline && elapsed_us()>=deadline && n<4;++n) {
            unsigned stage=stored(registers_,RTC_WDT_STAGE)&3;
            unsigned action=(config>>(28-stage*3))&7;
            arm_rtc_wdt(registers_,deadline,(stage+1)&3);
            if(action==1)registers_[0x3ff48040]|=8;
            else if(action==2) {
                const unsigned targets=((config>>9)&1)|((config>>7)&2);
                if(targets) {request_reset(RESET_CPU,targets,13);return true;}
            } else if(action==3 || action==4) {
                request_reset(action==3?RESET_DIGITAL:RESET_RTC,3,action==3?9:16);
                return true;
            }
            deadline=stored(registers_,RTC_WDT_DEADLINE)|((u64)stored(registers_,RTC_WDT_DEADLINE+4)<<32);
        }
    }
    interrupts_.set_source(46,(stored(registers_,0x3ff48040)&stored(registers_,0x3ff4803c))!=0);
    return true;
}

bool EspMachine::cycles_to_esp32_rtc(u64 &cycles) const
{
    if(chip_!=esp::CHIP_ESP32)return false;
    bool found=false;
    const u64 fraction=(cpu_instructions()-clock_origin_instructions_+clock_skipped_cycles_)%cpu_mhz_;
    if(stored(registers_,0x3ff4808c,0x4c80)&0x80000000u) {
        u64 deadline=deadline_at(registers_,RTC_WDT_DEADLINE);
        u64 remaining=deadline>elapsed_us()?deadline-elapsed_us():0;
        if(deadline && remaining<=~u64(0)/cpu_mhz_) {
            cycles=remaining*cpu_mhz_;cycles=cycles>fraction?cycles-fraction:1;found=true;
        }
    }
    for(unsigned group=0;group<2;++group) {
        if(!(stored(registers_,timer_group_base(group)+0x48,0x4c000)&0x80000000u))continue;
        u64 deadline=deadline_at(registers_,TIMG_WDT_STATE+group*16);
        if(!deadline)continue;
        u64 now=esp32_apb_ticks(),ticks=deadline>now?deadline-now:0;
        u64 candidate;
        if(!cycles_for_apb_ticks(ticks,candidate))continue;
        if(!found || candidate<cycles){cycles=candidate;found=true;}
    }
    return found;
}

bool EspMachine::esp32_mmu_read(u32 address, unsigned size, u32 &value,
                               bool instruction, bool &handled)
{
    int entry = flash_entry(address);
    const u32 table=0x3ff10000+active_core_*0x2000;
    const u32 control=active_core_?0x3ff00058:0x3ff00040;
    if(entry>=0 && !(stored(registers_,control,active_core_?0x10:8)&8)) {
        esp32_cache::record_disabled_access(registers_,interrupts_,active_core_,address);
        handled=true;return fault("ESP32 flash cache disabled",address);
    }
    handled = entry >= 0 && registers_.find(table + entry * 4) != registers_.end();
    if (!handled) return false;
    if (instruction && address < 0x40000000) return fault("ESP32 DROM is not executable",address);
    value = 0;
    for (unsigned i = 0; i < size; ++i) {
        int current = flash_entry(address + i);
        if (current < 0) return fault("ESP32 MMU read crosses mapping window",address + i);
        u32 mapping = stored(registers_,table + current * 4,0x100);
        esp32_cache::record_mmu_entry(registers_,active_core_,mapping);
        if (mapping & 0x100) return fault("invalid ESP32 flash MMU entry",address + i);
        size_t physical = ((size_t)(mapping & 0xff) << 16) | ((address + i) & 0xffff);
        if (physical >= flash_.size()) return fault("ESP32 MMU mapping exceeds flash",address + i);
        value |= (u32)(u8)flash_[physical] << (i * 8);
    }
    return true;
}

bool EspMachine::esp32_mmio_read(u32 address, u32 &value)
{
    /* Espressif libpp hal_now and lmacAdjustTimestamp use this 32-bit MAC
     * microsecond counter. PHY code uses its APB alias. Derive both views
     * from continuous simulated time; neither reads nor CPU clock changes
     * advance it artificially. Neighboring MAC registers remain undecoded. */
    if(address==0x3ff73c00 || address==0x60033c00){value=(u32)elapsed_us();return true;}
    const esp32_analog::Result analog=esp32_analog::read(registers_,address,value);
    if(analog!=esp32_analog::UNHANDLED)return analog==esp32_analog::OK;
    const esp32_phy::Result phy=esp32_phy::read(registers_,address,value,elapsed_us());
    if(phy!=esp32_phy::UNHANDLED)return phy==esp32_phy::OK;
    if(address==0x3ff480c8 || (address>=0x3ff48400 && address<=0x3ff484b0)) {
        u32 inputs=0;
        if(address==0x3ff48424)for(unsigned i=0;i<18;++i) {
            bool input,output,enabled;
            if(gpio_state(esp32_rtc_io::gpio(i),input,output,enabled) && input)inputs|=1u<<(i+14);
        }
        const esp32_rtc_io::Result rtc=esp32_rtc_io::read(registers_,address,inputs,value);
        if(rtc!=esp32_rtc_io::UNHANDLED)return rtc==esp32_rtc_io::OK;
    }
    if(esp32_cache::read(registers_,address,value))return true;
    if(address>=0x3ff01000 && address<=0x3ff01040)
        return esp_aes::read(registers_,address,esp32_apb_ticks(),value);
    if(!update_esp32_rtc())return false;
    u32 wdt_base=watchdog_register(address);
    if(wdt_base){value=stored(registers_,address,watchdog_reset(address-wdt_base));return true;}
    esp_spi::Result spi = esp_spi::read(chip_,registers_,flash_,address,value,&diagnostic_);
    if (spi != esp_spi::UNHANDLED) return spi == esp_spi::OK;
    esp32_rng::Result rng=esp32_rng::read(registers_,address,value);
    if(rng!=esp32_rng::UNHANDLED)return rng==esp32_rng::OK;
    if (address >= 0x3ff03000 && address < 0x3ff03080 && !(address&3)) {
        value = stored(registers_,address); return true; /* SHA message/digest words */
    }
    if (address >= 0x3ff03080 && address <= 0x3ff030bc && !(address&3)) {
        value = 0; return true; /* four engines' command/busy words, synchronous work */
    }
    if (address == 0x3ff5f068 || address == 0x3ff60068 ||
        address == 0x3ff5f06c || address == 0x3ff6006c) {
        u32 config = address & ~4u;
        u32 state = 0xffff1100 + (config == 0x3ff60068 ? 16 : 0);
        u64 deadline = stored(registers_,state) | ((u64)stored(registers_,state+4)<<32);
        if (deadline && elapsed_us() >= deadline) {
            registers_[config] |= 0x8000;
            registers_[config+4] = stored(registers_,state+8);
        }
        value = stored(registers_,address,address == config ? 0x13000 : 0);
        return true;
    }
    if (address >= 0x3ff10000 && address < 0x3ff14000 && !(address & 3)) {
        value = stored(registers_,address,0x100); return true;
    }
    if (address == 0x3ff00040 || address == 0x3ff00058) {
        value = stored(registers_,address,address == 0x3ff00040 ? 8 : 0x10);
        /* Invalidation completes synchronously: this model has no dirty
         * cache lines or outstanding flash bus transactions. DONE tracks the
         * asserted invalidation request, rather than being writable. */
        if (value & 0x10) value |= 0x20;
        return true;
    }
    if (address == 0x3ff48044) {
        value = stored(registers_,0x3ff48040) & stored(registers_,0x3ff4803c);
        return true;
    }
    if (address == 0x3ff48048) { value = 0; return true; }
    if (address == 0x3ff480b0) {
        value=stored(registers_,address,xtal_frequency()*0x10001u);return true;
    }
    if (address == 0x3ff003f0 || address == 0x3ff00418) {
        value = 1 << 7; return true; /* cache state machine idle */
    }
    if (address >= 0x3ff5a000 && address <= 0x3ff5a018 && !(address & 3)) {
        value = efuse_identity((address - 0x3ff5a000) / 4); return true;
    }
    if (address >= 0x3ff5a038 && address <= 0x3ff5a094 && !(address & 3)) {
        value = 0; return true; /* blank OTP key/calibration/user blocks */
    }
    if (address == 0x3ff5a104 || address == 0x3ff5a108 || address == 0x3ff5a10c) {
        u64 end = stored(registers_,EFUSE_DEADLINE_LO) |
            ((u64)stored(registers_,EFUSE_DEADLINE_HI) << 32);
        if (stored(registers_,0x3ff5a104) && elapsed_us() >= end) {
            registers_[0x3ff5a104] = 0;
            registers_[0x3ff5a108] |= 1;
        }
        value = address == 0x3ff5a10c ? stored(registers_,0x3ff5a108) & stored(registers_,0x3ff5a110) :
            stored(registers_,address);
        return true;
    }
    if (address == 0x3ff5a100 || address == 0x3ff5a114 || address == 0x3ff5a11c) {
        value = 0; return true; /* no controller error or coding error */
    }
    const Register *reg = find_register(address);
    if (reg) { value = stored(registers_,address,reg->reset); return true; }
    return false;
}

bool EspMachine::esp32_mmio_write(u32 address, u32 value)
{
    const esp32_analog::Result analog=esp32_analog::write(registers_,address,value,diagnostic_,elapsed_us(),xtal_frequency()*1000);
    if(analog!=esp32_analog::UNHANDLED)return analog==esp32_analog::OK;
    const esp32_phy::Result phy=esp32_phy::write(registers_,address,value,diagnostic_,elapsed_us());
    if(phy!=esp32_phy::UNHANDLED)return phy==esp32_phy::OK;
    if(address==0x3ff480c8 || (address>=0x3ff48400 && address<=0x3ff484b0)) {
        update_gpio_interrupts();
        const esp32_rtc_io::Result rtc=esp32_rtc_io::write(registers_,address,value,diagnostic_);
        if(rtc!=esp32_rtc_io::UNHANDLED){update_gpio_interrupts();return rtc==esp32_rtc_io::OK;}
    }
    if(address>=0x3ff01000 && address<=0x3ff01040)
        return esp_aes::write(registers_,address,esp32_apb_ticks(),value,diagnostic_);
    if(!update_esp32_rtc())return false;
    if(address==0x3ff48030 && (value&0x02000000))
        return fault("ESP32 sleep PLL calibration is unsupported",address);
    if(address==0x3ff000ac && (value&0x202))
        return fault("ESP32 cache tag power-down is unsupported",address);
    /* The awake state machine keeps powered resources accessible after
     * forced-on overrides are removed. Explicit shutdown/isolation needs
     * a separate sleep/power transition model; never silently keep running. */
    if((address==0x3ff48080 && (value&0x0004949a)) ||
       (address==0x3ff48084 && (value&0x000aaaa0)) ||
       (address==0x3ff48088 && (value&0x5555a100)))
        return fault("ESP32 forced power-down or isolation is unsupported",address);
    u32 wdt_base=watchdog_register(address);
    if(wdt_base) {
        const unsigned group=wdt_base==0x3ff60000?1:0;
        const u32 offset=address-wdt_base;
        if(offset==0x64){registers_[address]=value;return true;}
        if(stored(registers_,wdt_base+0x64,0x50d83aa1)!=0x50d83aa1)return true;
        const u32 prospective_config=offset==0x48?value:stored(registers_,wdt_base+0x48,0x4c000);
        const u32 prospective_prescaler=offset==0x4c?value:stored(registers_,wdt_base+0x4c,0x10000);
        /* Espressif documents the MWDT prescaler range as 1..65535. */
        if((prospective_config&0x80000000u) && !(prospective_prescaler>>16))
            return fault("unsupported zero ESP32 watchdog prescaler",address);
        if(offset!=0x60)registers_[address]=value&(offset==0x48?0xffffc000u:offset==0x4c?0xffff0000u:0xffffffffu);
        const u32 config=stored(registers_,wdt_base+0x48,0x4c000);
        interrupts_.configure_group_interrupt(group,2,(config&(1u<<21))!=0,(config&(1u<<22))!=0);
        if(config&0x80000000u)arm_timer_watchdog(registers_,group,esp32_apb_ticks(),0);
        return true;
    }
    if(address>=0x3ff4808c && address<=0x3ff480a0 && !(address&3)) {
        if(stored(registers_,0x3ff480a4,0x50d83aa1)!=0x50d83aa1)return true;
        if(address==0x3ff480a0) {
            if(value&0x80000000u)arm_rtc_wdt(registers_,elapsed_us(),0);
        } else {
            if(address==0x3ff4808c)
                for(unsigned stage=0;stage<4;++stage)
                    if(((value>>(28-stage*3))&7)>4)return fault("reserved ESP32 RTC watchdog stage action",address);
            u32 mask=address==0x3ff4808c?0xffffff80u:0xffffffffu;
            registers_[address]=value&mask;
            if(stored(registers_,0x3ff4808c,0x4c80)&0x80000000u)
                arm_rtc_wdt(registers_,elapsed_us(),0);
        }
        return true;
    }
    esp_spi::Result spi = esp_spi::write(chip_,registers_,flash_,address,value,&diagnostic_,&psram_);
    if (spi != esp_spi::UNHANDLED) return spi == esp_spi::OK;
    esp32_rng::Result rng=esp32_rng::write(registers_,address,value,&diagnostic_);
    if(rng!=esp32_rng::UNHANDLED)return rng==esp32_rng::OK;
    if (address >= 0x3ff03000 && address < 0x3ff03080 && !(address&3)) {
        if (!(stored(registers_,0x3ff0001c)&2) || (stored(registers_,0x3ff00020)&10))
            return fault("ESP32 SHA clock disabled or engine held in reset",address);
        registers_[address] = value; return true;
    }
    if (address == 0x3ff03090 || address == 0x3ff03094 || address == 0x3ff03098) {
        if (!(value&1)) return true;
        if (!(stored(registers_,0x3ff0001c)&2) || (stored(registers_,0x3ff00020)&10))
            return fault("ESP32 SHA clock disabled or engine held in reset",address);
        if (address != 0x3ff03090 && !stored(registers_,SHA_STARTED))
            return fault("ESP32 SHA256 context has not been started",address);
        if (address == 0x3ff03098) {
            for (unsigned i=0;i<8;++i) registers_[0x3ff03000+i*4] = stored(registers_,SHA_STATE+i*4);
            return true;
        }
        static const u32 initial[]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
        u32 state[8]; u8 block[64];
        for (unsigned i=0;i<8;++i) state[i] = address == 0x3ff03090 ? initial[i] : stored(registers_,SHA_STATE+i*4);
        for (unsigned i=0;i<16;++i) wr32be(block+i*4,stored(registers_,0x3ff03000+i*4));
        sha256_compress(state,block);
        for (unsigned i=0;i<8;++i) registers_[SHA_STATE+i*4] = state[i];
        registers_[SHA_STARTED] = 1;
        return true;
    }
    if (address == 0x3ff48000 && (value & 0x80000030)) {
        if(value&0x80000000u)request_reset(RESET_DIGITAL,3,3);
        else request_reset(RESET_CPU,((value>>5)&1)|((value>>3)&2),12);
        /* Reset request bits are self-clearing and excluded by the normal
         * OPTIONS0 writable mask. Other oscillator/stall bits still latch. */
    }
    if (address == 0x3ff5f068 || address == 0x3ff60068) {
        u32 old = stored(registers_,address,0x13000);
        registers_[address] = (value & 0xffff7000) | (old & 0x8000);
        if ((value & 0x80000000u) && !(old & 0x80000000u)) {
            unsigned count = (value>>16)&0x7fff, source = (value>>13)&3;
            if (source == 3) return fault("reserved ESP32 calibration clock",address);
            u32 hz = source == 1 ? 31250 : source == 2 ? 32768 : rtc_slow_hz(registers_);
            u64 deadline = elapsed_us() + ((u64)(count ? count : 1)*1000000 + hz-1)/hz;
            u32 state = 0xffff1100 + (address == 0x3ff60068 ? 16 : 0);
            registers_[state] = (u32)deadline;
            registers_[state+4] = (u32)(deadline>>32);
            registers_[state+8] = (u32)((u64)count*xtal_frequency()*1000000/hz) << 7;
            registers_[address] &= ~0x8000u;
        }
        return true;
    }
    if (address >= 0x3ff10000 && address < 0x3ff14000 && !(address & 3)) {
        registers_[address] = value & 0x1ff; return true;
    }
    if (address == 0x3ff00040 || address == 0x3ff00058) {
        registers_[address] = value & 0x10fdc; /* exclude request/status RO bits */
        if (address == 0x3ff00040) cache_enabled_ = (value & 8) != 0;
        return true;
    }
    if (address == 0x3ff4800c) {
        if (value & 0x80000000) {
            u64 ticks = (elapsed_us()-rtc_epoch_us_) * rtc_slow_hz(registers_) / 1000000;
            registers_[0x3ff48010] = (u32)ticks;
            registers_[0x3ff48014] = (u32)(ticks >> 32) & 0xffff;
            registers_[address] = 0x40000000; /* UPDATE auto-clears, VALID sets */
            registers_[0x3ff48040] |= 16;
        }
        return true;
    }
    if (address == 0x3ff48048) {
        registers_[0x3ff48040] &= ~(value & 0x1ff);
        interrupts_.set_source(46,(stored(registers_,0x3ff48040)&stored(registers_,0x3ff4803c))!=0);
        return true;
    }
    if (address == 0x3ff5a104) {
        if (value & ~1u) return false; /* programming requires a separate OTP model */
        if (value & 1) {
            u64 deadline = elapsed_us() + 4;
            registers_[EFUSE_DEADLINE_LO] = (u32)deadline;
            registers_[EFUSE_DEADLINE_HI] = (u32)(deadline >> 32);
            registers_[address] = 1;
            registers_[0x3ff5a108] &= ~1u;
        }
        return true;
    }
    if (address == 0x3ff5a114) { registers_[0x3ff5a108] &= ~(value & 3); return true; }
    const Register *reg = find_register(address);
    if (reg && reg->writable) {
        if(address==0x3ff0001c || address==0x3ff00020)
            esp_aes::synchronize(registers_,esp32_apb_ticks());
        registers_[address] = (stored(registers_,address,reg->reset) & ~reg->writable) |
                              (value & reg->writable);
        if(address==0x3ff005a0)esp32_cache::update_interrupt(registers_,interrupts_);
        if(address==0x3ff00020 && (value&0x19))esp_aes::reset(registers_);
        if (address == 0x3ff00020 && (value&10)) {
            for (unsigned i=0;i<32;++i) registers_.erase(0x3ff03000+i*4);
            for (unsigned i=0;i<8;++i) registers_.erase(SHA_STATE+i*4);
            registers_.erase(SHA_STARTED);
        }
        if(address==0x3ff000c4 && (value&16))esp32_rng::reset_i2s(registers_);
        if(address==0x3ff000c4 && (value&(1u<<21)))esp32_rng::reset_i2s(registers_,1);
        if(address==0x3ff000d0 && (value&1))esp32_phy::reset_baseband(registers_);
        if(address==0x3ff000d0 && (value&2))esp32_phy::reset_frontend(registers_);
        if(address==0x3ff000d0 && (value&4))esp32_phy::reset_mac(registers_);
        if(address==0x3ff4803c)interrupts_.set_source(46,(stored(registers_,0x3ff48040)&value)!=0);
        if (address == 0x3ff0003c || address == 0x3ff48070 || address == 0x3ff66000) {
            unsigned source = (stored(registers_,0x3ff48070,0x08002210) >> 27) & 3;
            unsigned period = stored(registers_,0x3ff0003c) & 3;
            unsigned mhz = source == 0 ? xtal_frequency() : source == 2 ? 8 :
                           period == 1 ? 160 : period == 2 ? 240 : 80;
            if (source == 0 || source == 2) mhz /= (stored(registers_,0x3ff66000) & 0x3ff) + 1;
            set_cpu_frequency(mhz);
        }
        return true;
    }
    return false;
}
