#include "esp8266/esp8266_wdev.h"
#include "esp/esp_state.h"

namespace esp8266_wdev {
namespace {
const u32 MASK=0x3ff20c18,RAW=0x3ff20c1c,STATUS=0x3ff20c20,CLEAR=0x3ff20c24;
const u32 EVENTS=esp_state::WDEV_8266;
const u32 RNG_STATE=esp_state::WDEV_8266+4,RNG=0x3ff20e44;
const u32 ACK_SIGNAL=0x3ff20e04;
const u32 QUEUE_EVENTS=0x3ff20cc0,QUEUE_EVENT_MASK=0x0fff0fff;
u32 get(const Registers &registers,u32 address)
{
    Registers::const_iterator it=registers.find(address);
    return it==registers.end()?0:it->second;
}
bool configuration(u32 address)
{
    /* Each TX queue has backoff, control, PHY, HT and duration words.
     * Queue control writes are intercepted by the DMA device before this
     * configuration bank; reading it reports the actual programmed value. */
    if(address>=0x3ff20cd0 && address<=0x3ff20dd0)for(unsigned queue=0;queue<=10;++queue) {
        if(queue==8 || queue==9)continue;
        const u32 base=0x3ff20dc0-queue*24;
        if(address>=base && address<=base+16 && !(address&3))return true;
    }
    /* Espressif NONOS SDK wDev_Option_Init programs these individual MAC
     * configuration words. None is a packet-completion or ready flag. */
    switch(address) {
    case 0x3ff20000:case 0x3ff20004: /* receive control */
    case 0x3ff20008:case 0x3ff2000c:case 0x3ff20010: /* RX descriptor heads */
    case 0x3ff2007c:case 0x3ff20080: /* RX buffer ring limits */
    case 0x3ff20084:case 0x3ff20088: /* aggregate-length descriptor ring */
    case 0x3ff2006c:case 0x3ff20178: /* RX policy and block-ack configuration */
    case 0x3ff20800:case 0x3ff20804: /* per-interface crypto policy */
    case 0x3ff20808:case 0x3ff2080c: /* key-table selection and valid slots */
    case 0x3ff20400:case 0x3ff20404:case 0x3ff20408: /* ACK rate table */
    case 0x3ff2040c: /* wDevForceAck6M rate selection */
    case 0x3ff20c14: /* MAC option enables and receive timing */
    case 0x3ff20c68:case 0x3ff20c6c: /* receive/transmit timing */
    case 0x3ff20c70: /* PHY/MAC configuration */
    case 0x3ff20c74: /* pm_sleep_set_mac / pm_wakeup_init timing and gates */
    /* NONOS SDK3.0.5 wDev_AppendRxBlocks updates these policy words
     * after replenishing its RX pool, preserving low28 and setting the
     * high nibbles to1/5 when at least two buffers remain. Retaining the
     * configuration neither publishes an RX descriptor nor raises an IRQ. */
    case 0x3ff20ca8:case 0x3ff20cac:
    case 0x3ff20de0: /* SDK 3.x pm_sleep_set_mac power-switch timing */
    case 0x3ff20ce4:case 0x3ff20ce8: /* remaining Tx_Copy2Queue staging words */
    case 0x3ff20c88:case 0x3ff20c90:case 0x3ff20c94:
    case 0x3ff20e08:return true;
    default:
        /* Two BSSID/address values and their compare masks, individually
         * programmed by wDev_SetBssid and wDev_SetMacAddress. */
        return address>=0x3ff20c28 && address<=0x3ff20c64 && !(address&3);
    }
}
bool queue_lifetime(u32 address)
{
    /* NONOS SDK3.0.5 lmacSetTxFrame writes queue0..7's remaining lifetime
     * in units of1024us into bits23:12 of the sixth queue word. Queue10
     * shares addresses with the TX staging registers and is not decoded
     * as an ordinary lifetime register. Expiry is a TX-device event. */
    return address>=0x3ff20d2c && address<=0x3ff20dd4 &&
        (0x3ff20dd4-address)%24==0;
}
}
bool handles(u32 address)
{
    return address==0x3ff20c00 || address==MASK || address==RAW || address==STATUS || address==CLEAR ||
        address==0x3ff20c84 || address==0x3ff20410 || address==0x3ff201d0 ||
        address==RNG || address==ACK_SIGNAL || address==QUEUE_EVENTS || queue_lifetime(address) || configuration(address);
}
bool interrupt(const Registers &registers)
{
    return (get(registers,EVENTS)&get(registers,MASK))!=0;
}
void raise(Registers &registers,u32 events){registers[EVENTS]|=events;}
void raise_queue_events(Registers &registers,u32 collisions,u32 expirations)
{
    collisions&=0xfff;expirations&=0xfff;
    registers[QUEUE_EVENTS]|=collisions|(expirations<<16);
    if(collisions)raise(registers,1u<<20);
    if(expirations)raise(registers,1u<<29);
}
bool read(Registers &registers,u32 address,u64 microseconds,u32 &value)
{
    if(address==0x3ff20c00){value=(u32)microseconds;return true;}
    /* wDev_ProcessFiq tests bit24 before using the signal byte at bits23:16
     * for TX completion. A broadcast/no-ACK exchange has no ACK sample.
     * The functional Air interface supplies ACK outcome but no measured
     * RSSI, so that measurement remains explicitly invalid in all cases. */
    if(address==ACK_SIGNAL){value=0;return true;}
    if(address==QUEUE_EVENTS){value=get(registers,address)&QUEUE_EVENT_MASK;return true;}
    if(address==RNG) {
        /* RANDOM_REG32 in the ESP8266 core's esp8266_peri.h. A repeatable
         * PRNG models its readable stream, not physical or secure entropy. */
        u32 state=get(registers,RNG_STATE);
        if(!state)state=0x8266febau;
        state^=state<<13;state^=state>>17;state^=state<<5;
        registers[RNG_STATE]=value=state;return true;
    }
    /* SDK wDev_ProcessFiq also inspects raw pending events while individual
     * sources are masked. Only c24 acknowledges those latched events. */
    if(address==RAW){value=get(registers,EVENTS);return true;}
    if(address==STATUS){value=get(registers,EVENTS)&get(registers,MASK);return true;}
    /* pm_check_mac_idle polls TX state[3:0] at20410 and RX state[15:12] at201d0.
     * Both are idle at reset; active DMA owns their state transitions. */
    if(address==MASK || address==0x3ff20c84 || address==0x3ff20410 || address==0x3ff201d0 ||
       queue_lifetime(address) || configuration(address)) {
        value=get(registers,address);return true;
    }
    return false;
}
bool write(Registers &registers,u32 address,u32 value)
{
    /* wDev_ProcessFiq reads c20 and acknowledges those bits at c24. An
     * interrupt masked during acknowledgement must retain unrelated events. */
    if(address==CLEAR){registers[EVENTS]&=~value;return true;}
    /* wDev_ClearTxqCollisions and lmacProcessTxTimeout acknowledge cc0
     * using read/modify/write clears. Software cannot manufacture collision
     * or expiry events, and c24 remains the separate MAC IRQ acknowledge. */
    if(address==QUEUE_EVENTS) {
        registers[address]&=value&QUEUE_EVENT_MASK;return true;
    }
    if(queue_lifetime(address)) {
        if(value&~0x00fff000u)return false;
        registers[address]=value;return true;
    }
    if(address==MASK || configuration(address)){registers[address]=value;return true;}
    return false;
}
bool key_memory(u32 address,unsigned size)
{
    /* SDK wDev_Insert_KeyEntry and wDev_remove_KeyEntry index 25 entries
     * of ten words: two address/type words followed by a 32-byte key. */
    return (size==1 || size==2 || size==4) && address>=0x3ff21400 &&
        (u64)address+size<=0x3ff21400+25*40;
}
bool read_key(const Registers &registers,u32 address,unsigned size,u32 &value)
{
    if(!key_memory(address,size))return false;
    value=0;
    for(unsigned i=0;i<size;++i)
        value|=((get(registers,(address+i)&~3u)>>(((address+i)&3)*8))&255u)<<(i*8);
    return true;
}
bool write_key(Registers &registers,u32 address,unsigned size,u32 value)
{
    if(!key_memory(address,size))return false;
    for(unsigned i=0;i<size;++i) {
        const u32 word=(address+i)&~3u,shift=((address+i)&3)*8;
        registers[word]=(get(registers,word)&~(255u<<shift))|(((value>>(i*8))&255)<<shift);
    }
    return true;
}
}
