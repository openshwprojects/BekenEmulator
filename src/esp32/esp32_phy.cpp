#include "esp32/esp32_phy.h"
#include "esp/esp_state.h"
#include "esp/esp_rf_math.h"
namespace {
const u32 NF_CONTROL=0x3ff5c018,NF_RESULT=0x3ff5c050;
const u32 NF_STATE=esp_state::PHY_ESP32,NF_DEADLINE=NF_STATE+4;
const u32 NF_PENDING=NF_STATE+12,NF_AMPLITUDE=NF_STATE+16;
const u32 PBUS_COMMAND=0x3ff46094,PBUS_STATUS=0x3ff460a0;
const u32 PBUS_DEADLINE=esp_state::PHY_ESP32+0x100,PBUS_PENDING=PBUS_DEADLINE+8;
struct Register { u32 address, supported, reset_line; };
/* Exact controls established by Espressif's classic ESP32 libphy routines
 * disable_wifi_agc, enable_wifi_agc, set_cca and agc_reg_init. They configure
 * the receiver, not measurement-completion or packet-reception status.
 * Unconfigured controls start clear in this functional receiver model;
 * these values are not a claim about undocumented silicon reset defaults. */
const Register controls[] = {
    {0x3ff5c004,0x00010000,1},
    {0x3ff5c018,0xffffffff,1}, /* CCA/AGC plus noise enable23/reset25/auto28; DONE24 is RO */
    {0x3ff5c01c,0x00ff00ff,1}, /* AGC threshold[23:16], signed CCA threshold[7:0] */
    /* agc_reg_init supplies a complete configuration word here; mode[5:4]
     * and force-disable8 are also independently controlled by ram_*_agc. */
    {0x3ff5c030,0xffffffff,1},
    {0x3ff5c038,0x0c000000,1}, /* AGC control bits used by enable/init */
    {0x3ff5c080,0x607fffc9,1}, /* gain-control fields [30:29], [22:17], [16:8], [7:6], [3], and force hold [0] */
    {0x3ff5c0a4,0xffffffff,1}, /* older agc_reg_init writes complete gain config */
    {0x3ff5c0c4,0xffffffff,1}, /* four AGC thresholds, programmed as one word */
    {0x3ff5c094,0xffffffff,1},
    {0x3ff5c02c,0xffffffff,1}, /* receiver configuration including init toggle23 */
    {0x3ff5c028,0x000001ff,1},
    {0x3ff5c0f8,0x0003fe00,1},
    {0x3ff5c088,0x000000ff,1},
    {0x3ff5c104,0xffffffff,1},
    {0x3ff5c07c,0xffffffff,1},
    /* bb_reg_init programs complete digital filter words. The remaining
     * entries expose only fields read/modified by that routine. */
    {0x3ff5cc48,0xffffffff,1},
    {0x3ff5cce4,0x00400000,1},
    {0x3ff5c400,0xffffffff,1},
    {0x3ff5c450,0xffffffff,1},
    {0x3ff5c454,0xffffffff,1},
    {0x3ff5c458,0xffffffff,1},
    {0x3ff5c45c,0xffffffff,1},
    {0x3ff5cd04,0xffffffff,1},
    {0x3ff5cd08,0xffffffff,1},
    {0x3ff5cc0c,0xffffffff,1},
    {0x3ff5cc08,0xffffffff,1},
    {0x3ff5ccdc,0xffffffff,1},
    {0x3ff5c044,0x01ffffff,1},
    {0x3ff5c024,0x0003fe00,1},
    {0x3ff5cc04,0x000001fc,1},
    {0x3ff5ccd8,0xffffffff,1},
    {0x3ff5cd0c,0x000000ff,1},
    {0x3ff5d000,0x1ffff840,1}, /* tx_paon_set PA-on timing fields */
    {0x3ff5d030,0xffffffff,1},
    {0x3ff5d03c,0xffffffff,1}, /* bb_wdg_cfg supplies complete threshold word */
    {0x3ff5d040,0xe0000000,1}, /* watchdog reset/IRQ enables, timeout-clear */
    {0x3ff5d044,1,1}, /* rom_noise_floor_auto_set acquisition mode */
    {0x3ff5d050,1,1}, /* sample acknowledgement/rearm strobe */
    {0x3ff5ccb8,0xe0000000,1}, /* phy_set_bbfreq_init clock selection */
    {0x3ff5c804,0xffffffff,1},
    {0x3ff5c85c,0x0000000f,1},
    {0x3ff5c860,0x00000c00,1},
    {0x3ff5c87c,0x00000800,1},
    {0x3ff5c11c,0xffffffff,1}, /* phy_ant_init complete antenna config words */
    {0x3ff5c120,0xffffffff,1},
    {0x3ff5c010,0xffffffff,1},
    {0x3ff5c014,0xffffffff,1},
    {0x3ff5c074,0xffffffff,1},
    {0x3ff5c084,0xffffffff,1},
    {0x3ff5c090,0xffffffff,1},
    {0x3ff5c0a0,0xffffffff,1}, /* opt_11b_resart receiver thresholds */
    {0x3ff5c0cc,0xffffffff,1},
    {0x3ff5c0d0,0xffffffff,1},
    {0x3ff5c0f0,0x000000ff,1},
    {0x3ff5c0b8,0x00000800,1},
    {0x3ff5c124,0xffffffff,1},
    {0x3ff5d014,0xffffffff,1},
    {0x3ff5d018,0xffffffff,1},
    {0x3ff5d01c,0xffffffff,1},
    {0x3ff5d020,0xffffffff,1},
    /* force_txrxoff, ram_set_rxclk_en/txclk_en, fe_reg_init: force/value
     * pairs[17:8] and configuration bit24. PBUS busy31 is read-only and
     * supplied by the timed command engine, not a retained control field. */
    {0x3ff460a0,0x0103ff00,2},
    {PBUS_COMMAND,0x0001ffff,2},
    /* Remaining exact fields programmed by fe_reg_init; FE and FE2 share
     * DPORT_FE_RST, which is independent of the Wi-Fi baseband reset. */
    {0x3ff45114,0x00000200,2},
    {0x3ff45104,0x0f0f0f00,2},
    {0x3ff45108,0x000f0f0f,2},
    {0x3ff450a8,0x0000000f,2},
    {0x3ff450c4,0xffffffff,2},
    {0x3ff450c8,0xffffffff,2},
    {0x3ff450dc,0xdfffffff,2},
    {0x3ff450d8,0x02000000,2},
    {0x3ff46000,0xffffffff,2},
    {0x3ff46004,0xffffffff,2},
    {0x3ff46008,0xffffffff,2},
    {0x3ff4600c,0xffffffff,2},
    {0x3ff46010,0xffffffff,2},
    {0x3ff46060,0x0c000000,2},
    {0x3ff4607c,0x0019ffff,2},
    {0x3ff460b8,0xffffffff,2},
    {0x3ff460bc,0x0fffffff,2},
    {0x3ff460c4,0x00040000,2},
    {0x3ff460f8,0xffffffff,2},
    {0x3ff46094,0xffffffff,2},
    {0x3ff4609c,0x00004014,2}, /* bit2/4 also used by phy_close_pa */
    {0x3ff4604c,0x0000ff00,2},
    {0x3ff46048,0x0000ff00,2}, /* tx_paon_set front-end delay */
    {0x3ff46030,0xffffffff,2}, /* set_pbus_mem bank range configuration */
    {0x3ff46034,0xffffffff,2},
    {0x3ff46038,0xffffffff,2},
    {0x3ff4603c,0xffffffff,2},
    {0x3ff46040,0xffffffff,2},
    {0x3ff46044,0xffffffff,2},
    {0x3ff45034,0xffffffff,2},
    {0x3ff45038,0xffffffff,2}, /* set_pbus_mem table word and address/strobe */
    {0x3ff4503c,0xffffffff,2},
    {0x3ff45040,0xffffffff,2},
    {0x3ff45044,0xffffffff,2},
    {0x3ff45048,0xffffffff,2},
    {0x3ff4504c,0xffffffff,2},
    {0x3ff45050,0xffffffff,2},
    {0x3ff45054,0xffffffff,2},
    {0x3ff45058,0xffffffff,2},
    {0x3ff4505c,0xffffffff,2},
    {0x3ff45060,0xffffffff,2},
    {0x3ff45064,0xffffffff,2},
    {0x3ff45068,0xffffffff,2},
    {0x3ff4506c,0xffffffff,2},
    {0x3ff45070,0xffffffff,2},
    {0x3ff45074,0xffffffff,2},
    {0x3ff45078,0xffffffff,2},
    {0x3ff450d0,0xffffffff,2},
    {0x3ff450f0,0xffffffff,2},
    {0x3ff460c0,0x00000080,2}, /* ram_en_pwdet front-end route enable */
    /* set_chan_freq_hw_init programs upper16 to0x0c80 after preserving
     * lower16. No active MAC request or status is accepted here. */
    {0x3ff71000,0xffffffff,4},
    {0x3ff71004,0xffffffff,4},
    {0x3ff71008,0xffffffff,4},
    {0x3ff7100c,0xffffffff,4},
    {0x3ff71010,0xffffffff,4},
    {0x3ff71018,0xffffffff,4},
    {0x3ff7101c,0xffffffff,4},
    {0x3ff71020,0xffffffff,4},
    {0x3ff71024,0xffffffff,4},
    {0x3ff71028,0xffffffff,4},
    {0x3ff7102c,0xffffffff,4},
    {0x3ff71030,0xffffffff,4},
    {0x3ff71034,0xffffffff,4},
    {0x3ff71038,0xffffffff,4},
    {0x3ff7103c,0xffffffff,4},
    {0x3ff71040,0xffffffff,4},
    {0x3ff71044,0xffffffff,4},
    {0x3ff71050,0xffffffff,4},
    {0x3ff71060,0xffffffff,4},
    {0x3ff71064,0xffffffff,4},
    {0x3ff71070,0xffffffff,4},
    {0x3ff71074,0xffffffff,4},
    {0x3ff71080,0xffffffff,4},
    {0x3ff71098,0xffffffff,4},
    {0x3ff710a0,0xffffffff,4},
    {0x3ff710a4,0xffffffff,4},
    {0x3ff710b0,0xffffffff,4},
    {0x3ff710b4,0xffffffff,4},
    {0x3ff710b8,0xffffffff,4},
    {0x3ff710bc,0xffffffff,4},
    {0x3ff710c0,0xffffffff,4},
    {0x3ff710c4,0xffffffff,4},
    {0x3ff710c8,0xffffffff,4},
    {0x3ff710d0,0xffffffff,4},
    {0x3ff71140,0xffffffff,4},
    {0x3ff71144,0xffffffff,4},
    {0x3ff71148,0xffffffff,4},
    {0x3ff7114c,0xffffffff,4},
    {0x3ff71150,0xffffffff,4},
    {0x3ff71154,0xffffffff,4},
    {0x3ff71160,0xffffffff,4},
    {0x3ff71180,0xffffffff,4},
    {0x3ff711a0,0xffffffff,4},
    {0x3ff711b0,0xffffffff,4},
    {0x3ff711b4,0xffffffff,4},
    {0x3ff711b8,0xffffffff,4},
    {0x3ff711bc,0xffffffff,4},
    {0x3ff711c0,0xffffffff,4},
    {0x3ff711c4,0xffffffff,4},
    {0x3ff711c8,0xffffffff,4},
    {0x3ff711cc,0xffffffff,4},
    {0x3ff711d0,0xffffffff,4},
    {0x3ff711e0,0xffffffff,4},
    {0x3ff71200,0xffffffff,4},
    {0x3ff71204,0xffffffff,4},
    {0x3ff7120c,0xffffffff,4},
    {0x3ff71210,0xffffffff,4},
    {0x3ff71218,0xffffffff,4},
    {0x3ff7121c,0xffffffff,4},
    {0x3ff71220,0xffffffff,4},
    {0x3ff71224,0xffffffff,4},
    {0x3ff71228,0xffffffff,4},
    {0x3ff7122c,0xffffffff,4},
    {0x3ff71250,0xffffffff,4},
    {0x3ff71254,0xffffffff,4},
    {0x3ff71260,0xffffffff,4},
    {0x3ff71270,0xffffffff,4},
    {0x3ff71274,0xffffffff,4},
    {0x3ff71280,0xffffffff,4},
    {0x3ff71290,0xffffffff,4},
    {0x3ff712a0,0xffffffff,4},
    {0x3ff712b0,0xffffffff,4},
    {0x3ff712b4,0xffffffff,4},
    {0x3ff712b8,0xffffffff,4},
    {0x3ff712c0,0xffffffff,4},
    {0x3ff712c4,0xffffffff,4},
    {0x3ff712c8,0xffffffff,4},
    {0x3ff712cc,0xffffffff,4},
    {0x3ff712d0,0xffffffff,4},
    {0x3ff712d4,0xffffffff,4},
    {0x3ff712e0,0xffffffff,4},
    {0x3ff712f0,0xffffffff,4},
    {0x3ff71300,0xffffffff,4},
    {0x3ff71320,0xffffffff,4},
    {0x3ff71324,0xffffffff,4},
    {0x3ff71328,0xffffffff,4},
    {0x3ff7132c,0xffffffff,4},
    {0x3ff71390,0xffffffff,4},
    {0x3ff720dc,0xffffffff,4},
    {0x3ff720f0,0xffffffff,4},
    {0x3ff720f4,0xffffffff,4},
    {0x3ff72124,0xffffffff,4},
    {0x3ff72130,0xffffffff,4},
    {0x3ff73000,0xffffffff,4},
    {0x3ff73004,0xffffffff,4},
    {0x3ff73008,0xffffffff,4},
    {0x3ff7300c,0xffffffff,4},
    {0x3ff73010,0xffffffff,4},
    {0x3ff73014,0xffffffff,4},
    {0x3ff73018,0xffffffff,4},
    {0x3ff7301c,0xffffffff,4},
    {0x3ff73020,0xffffffff,4},
    {0x3ff73024,0xffffffff,4},
    {0x3ff73028,0xffffffff,4},
    {0x3ff7302c,0xffffffff,4},
    {0x3ff73030,0xffffffff,4},
    {0x3ff73034,0xffffffff,4},
    {0x3ff73038,0xffffffff,4},
    {0x3ff7303c,0xffffffff,4},
    {0x3ff73040,0xffffffff,4},
    {0x3ff73044,0xffffffff,4},
    {0x3ff73048,0xffffffff,4},
    {0x3ff7304c,0xffffffff,4},
    {0x3ff73050,0xffffffff,4},
    {0x3ff73054,0xffffffff,4},
    {0x3ff73058,0xffffffff,4},
    {0x3ff7305c,0xffffffff,4},
    {0x3ff73060,0xffffffff,4},
    {0x3ff73064,0xffffffff,4},
    {0x3ff73068,0xffffffff,4},
    {0x3ff7306c,0xffffffff,4},
    {0x3ff73070,0xffffffff,4},
    {0x3ff73074,0xffffffff,4},
    {0x3ff73078,0xffffffff,4},
    {0x3ff7307c,0xffffffff,4},
    {0x3ff73080,0xffffffff,4},
    {0x3ff73084,0xffffffff,4},
    {0x3ff73088,0xffffffff,4},
    {0x3ff7308c,0xffffffff,4},
    {0x3ff73090,0xffffffff,4},
    {0x3ff7309c,0xffffffff,4},
    {0x3ff730a0,0xffffffff,4},
    {0x3ff730a4,0xffffffff,4},
    {0x3ff730a8,0xffffffff,4},
    {0x3ff730d8,0xffffffff,4},
    {0x3ff730dc,0xffffffff,4},
    {0x3ff730e0,0xffffffff,4},
    {0x3ff730e4,0xffffffff,4},
    {0x3ff730f4,0xffffffff,4},
    {0x3ff730f8,0xffffffff,4},
    {0x3ff730fc,0xffffffff,4},
    {0x3ff73100,0xffffffff,4},
    {0x3ff73104,0xffffffff,4},
    {0x3ff73108,0xffffffff,4},
    {0x3ff7310c,0xffffffff,4},
    {0x3ff73110,0xffffffff,4},
    {0x3ff73114,0xffffffff,4},
    {0x3ff73118,0xffffffff,4},
    {0x3ff7311c,0xffffffff,4},
    {0x3ff73120,0xffffffff,4},
    {0x3ff73124,0xffffffff,4},
    {0x3ff73128,0xffffffff,4},
    {0x3ff7312c,0xffffffff,4},
    {0x3ff73130,0xffffffff,4},
    {0x3ff73134,0xffffffff,4},
    {0x3ff73138,0xffffffff,4},
    {0x3ff7313c,0xffffffff,4},
    {0x3ff73140,0xffffffff,4},
    {0x3ff73144,0xffffffff,4},
    {0x3ff73148,0xffffffff,4},
    {0x3ff7314c,0xffffffff,4},
    {0x3ff73158,0xffffffff,4},
    {0x3ff73164,0xffffffff,4},
    {0x3ff73250,0xffffffff,4},
    {0x3ff73254,0xffffffff,4},
    {0x3ff73258,0xffffffff,4},
    {0x3ff7325c,0xffffffff,4},
    {0x3ff73284,0xffffffff,4},
    {0x3ff73288,0xffffffff,4},
    {0x3ff732f0,0xffffffff,4},
    {0x3ff732f4,0xffffffff,4},
    {0x3ff732f8,0xffffffff,4},
    {0x3ff73400,0xffffffff,4},
    {0x3ff73404,0xffffffff,4},
    {0x3ff73408,0xffffffff,4},
    {0x3ff7340c,0xffffffff,4},
    {0x3ff73410,0xffffffff,4},
    {0x3ff73414,0xffffffff,4},
    {0x3ff73418,0xffffffff,4},
    {0x3ff7341c,0xffffffff,4},
    {0x3ff73800,0xffffffff,4},
    {0x3ff73804,0xffffffff,4},
    {0x3ff73808,0xffffffff,4},
    {0x3ff7380c,0xffffffff,4},
    {0x3ff73810,0xffffffff,4},
    {0x3ff73814,0xffffffff,4},
    {0x3ff73818,0xffffffff,4},
    {0x3ff7381c,0xffffffff,4},
    {0x3ff73820,0xffffffff,4},
    {0x3ff73824,0xffffffff,4},
    {0x3ff73828,0xffffffff,4},
    {0x3ff7382c,0xffffffff,4},
    {0x3ff73830,0xffffffff,4},
    {0x3ff73c1c,0xffffffff,4},
    {0x3ff73c20,0xffffffff,4},
    {0x3ff73c24,0xffffffff,4},
    {0x3ff73c40,0xffffffff,4},
    {0x3ff73c48,0xffffffff,4},
    {0x3ff73c4c,0xffffffff,4},
    {0x3ff73c54,0xffffffff,4},
    {0x3ff73c58,0xffffffff,4},
    {0x3ff73c5c,0xffffffff,4},
    {0x3ff73c68,0xffffffff,4},
    {0x3ff73c6c,0xffffffff,4},
    {0x3ff73c74,0xffffffff,4},
    {0x3ff73c78,0xffffffff,4},
    {0x3ff73c7c,0xffffffff,4},
    {0x3ff73c80,0xffffffff,4},
    {0x3ff73c84,0xffffffff,4},
    {0x3ff73c88,0xffffffff,4},
    {0x3ff73c8c,0xffffffff,4},
    {0x3ff73c90,0xffffffff,4},
    {0x3ff73c94,0xffffffff,4},
    {0x3ff73c98,0xffffffff,4},
    {0x3ff73cac,0xffffffff,4},
    {0x3ff73cb8,0xffffffff,4},
    {0x3ff73cbc,0xffffffff,4},
    {0x3ff73cc0,0xffffffff,4},
    {0x3ff73cc4,0xffffffff,4},
    {0x3ff73cc8,0xffffffff,4},
    {0x3ff73cd0,0xffffffff,4},
    {0x3ff73d18,0xffffffff,4},
    {0x3ff73d1c,0xffffffff,4},
    {0x3ff73d20,0xffffffff,4},
    {0x3ff73d24,0xffffffff,4},
    {0x3ff73d30,0xffffffff,4},
    {0x3ff73d38,0xffff000e,4},
    {0x3ff73d40,0xffffffff,4},
    {0x3ff73d78,0xffffffff,4},
    {0x3ff73d7c,0xffffffff,4},
    {0x3ff73d84,0xffffffff,4},
    {0x3ff73d88,0xffffffff,4},
    {0x3ff74000,0xffffffff,4},
    {0x3ff74008,0xffffffff,4},
    {0x3ff7400c,0xffffffff,4},
    {0x3ff74010,0xffffffff,4},
    {0x3ff740b8,0xffffffff,4},
    {0x3ff740f4,0xffffffff,4},
    {0x3ff74130,0xffffffff,4},
    {0x3ff7416c,0xffffffff,4},
    {0x3ff741a8,0xffffffff,4},
    {0x3ff741e4,0xffffffff,4},
    {0x3ff74220,0xffffffff,4},
    {0x3ff74258,0xffffffff,4},
    {0x3ff7425c,0xffffffff,4},
    {0x3ff74260,0xffffffff,4},
    {0x3ff74264,0xffffffff,4},
    {0x3ff74268,0xffffffff,4},
    {0x3ff74270,0xffffffff,4},
    {0x3ff74274,0xffffffff,4},
    {0x3ff74278,0xffffffff,4},
    {0x3ff74284,0xffffffff,4},
    {0x3ff74400,0xffffffff,4},
    {0x3ff74404,0xffffffff,4},
    {0x3ff74408,0xffffffff,4},
    {0x3ff7440c,0xffffffff,4},
    {0x3ff74410,0xffffffff,4},
    {0x3ff74414,0xffffffff,4},
    {0x3ff74418,0xffffffff,4},
    {0x3ff7441c,0xffffffff,4},
    {0x3ff74420,0xffffffff,4},
    {0x3ff74424,0xffffffff,4},
    {0x3ff74428,0xffffffff,4},
    {0x3ff7442c,0xffffffff,4},
    {0x3ff74430,0xffffffff,4},
    {0x3ff74434,0xffffffff,4},
    {0x3ff74438,0xffffffff,4},
    {0x3ff7443c,0xffffffff,4},
    {0x3ff74440,0xffffffff,4},
    {0x3ff74444,0xffffffff,4},
    {0x3ff74448,0xffffffff,4},
    {0x3ff7444c,0xffffffff,4},
    {0x3ff74450,0xffffffff,4},
    {0x3ff74454,0xffffffff,4},
    {0x3ff74458,0xffffffff,4},
    {0x3ff7445c,0xffffffff,4},
    {0x3ff74460,0xffffffff,4},
    {0x3ff74464,0xffffffff,4},
    {0x3ff74468,0xffffffff,4},
    {0x3ff7446c,0xffffffff,4},
    {0x3ff74470,0xffffffff,4},
    {0x3ff74474,0xffffffff,4},
    {0x3ff74478,0xffffffff,4},
    {0x3ff7447c,0xffffffff,4},
    {0x3ff74540,0xffffffff,4},
    {0x3ff74544,0xffffffff,4},
    {0x3ff74548,0xffffffff,4},
    {0x3ff7454c,0xffffffff,4},
    {0x3ff74550,0xffffffff,4},
    {0x3ff74554,0xffffffff,4},
    {0x3ff74558,0xffffffff,4},
    {0x3ff7455c,0xffffffff,4},
    {0x3ff74560,0xffffffff,4},
    {0x3ff74564,0xffffffff,4},
    {0x3ff74568,0xffffffff,4},
    {0x3ff7456c,0xffffffff,4},
    {0x3ff74570,0xffffffff,4},
    {0x3ff74574,0xffffffff,4},
    {0x3ff74578,0xffffffff,4},
    {0x3ff7457c,0xffffffff,4},
    {0x3ff74580,0xffffffff,4},
    {0x3ff75010,0xffffffff,4},
    {0x3ff75014,0xffffffff,4},
    {0x3ff75018,0xffffffff,4},
    {0x3ff7501c,0xffffffff,4},
    {0x3ff75020,0xffffffff,4},
    {0x3ff75024,0xffffffff,4},
    {0x3ff75028,0xffffffff,4},
    {0x3ff7502c,0xffffffff,4},
    {0x3ff75030,0xffffffff,4},
    {0x3ff75034,0xffffffff,4},
    {0x3ff75038,0xffffffff,4},
    {0x3ff7503c,0xffffffff,4},
    {0x3ff75040,0xffffffff,4},
    {0x3ff75044,0xffffffff,4},
    {0x3ff75048,0xffffffff,4},
    {0x3ff7504c,0xffffffff,4},
    {0x3ff75050,0xffffffff,4},
    {0x3ff75054,0xffffffff,4},
    {0x3ff75058,0xffffffff,4},
    {0x3ff7505c,0xffffffff,4},
    {0x3ff75060,0xffffffff,4},
    {0x3ff75064,0xffffffff,4},
    {0x3ff75068,0xffffffff,4},
    {0x3ff7506c,0xffffffff,4},
    {0x3ff75070,0xffffffff,4},
    {0x3ff75074,0xffffffff,4},
    {0x3ff75078,0xffffffff,4},
    {0x3ff7507c,0xffffffff,4},
    {0x3ff75080,0xffffffff,4},
    {0x3ff75084,0xffffffff,4},
    {0x3ff75088,0xffffffff,4},
    {0x3ff7508c,0xffffffff,4},
    {0x3ff75090,0xffffffff,4},
    {0x3ff75094,0xffffffff,4},
    {0x3ff75098,0xffffffff,4},
    {0x3ff7509c,0xffffffff,4},
    {0x3ff750a0,0xffffffff,4},
    {0x3ff75800,0xffffffff,4},
};
u32 canonical(u32 address) {
    if(address>=0x60000000 && address<0x60040000)return address-0x60000000+0x3ff40000;
    return address;
}
const Register *reg(u32 address) {
    for(unsigned i=0;i<sizeof(controls)/sizeof(controls[0]);++i)if(controls[i].address==address)return &controls[i];
    if(!(address&3) && ((address>=0x3ff73ce4 && address<=0x3ff73d20) ||
                        (address>=0x3ff740b4 && address<=0x3ff74284) ||
                        (address>=0x3ff74400 && address<=0x3ff747fc) ||
                        address==0x3ff73098 || address==0x3ff732fc)) {
        static const Register mac_extra={0,0xffffffff,4};
        return &mac_extra;
    }
    return 0;
}
u32 stored(const std::map<u32,u32> &r,u32 address) {
    std::map<u32,u32>::const_iterator it=r.find(address);return it==r.end()?0:it->second;
}
void noise_update(std::map<u32,u32> &r,u64 now) {
    const u64 deadline=stored(r,NF_DEADLINE)|(u64(stored(r,NF_DEADLINE+4))<<32);
    if(deadline && now>=deadline) {
        r[NF_RESULT]=stored(r,NF_PENDING);r[NF_CONTROL]|=0x01000000;
        r.erase(NF_DEADLINE);r.erase(NF_DEADLINE+4);
    }
}
void noise_cancel(std::map<u32,u32> &r) {
    r[NF_CONTROL]&=~0x01000000u;
    r.erase(NF_DEADLINE);r.erase(NF_DEADLINE+4);
}
void noise_start(std::map<u32,u32> &r,u64 now) {
    noise_cancel(r);
    if((stored(r,NF_CONTROL)&0x02800000)!=0x00800000 || (stored(r,0x3ff000d0)&1))return;
    /* Functional 20MHz receiver: integrate128 deterministic I/Q samples.
     * The virtual board's variance8 is -96dBm. Timing/idle noise are explicit
     * model parameters, not undocumented silicon reset/calibration values. */
    const unsigned amplitude=r.find(NF_AMPLITUDE)==r.end()?3:stored(r,NF_AMPLITUDE);
    const unsigned range=amplitude*2+1;u64 energy=0;
    u32 state=stored(r,NF_STATE);if(!state)state=0x32adc001;
    for(unsigned i=0;i<128;++i) {
        state^=state<<13;state^=state>>17;state^=state<<5;
        const int a=int(state%range)-int(amplitude),b=int((state>>16)%range)-int(amplitude);
        energy+=a*a+b*b;
    }
    r[NF_STATE]=state;
    r[NF_PENDING]=esp_rf_math::logarithmic_power_code(double(energy)/128,8,640,4,1023);
    const u64 deadline=now+7;r[NF_DEADLINE]=(u32)deadline;r[NF_DEADLINE+4]=(u32)(deadline>>32);
}
bool pbus_cell(unsigned block,unsigned selector,u32 &address,unsigned &shift) {
    /* Real mask-ROM pbus_rd_addr/pbus_rd_shift tables. Blocks0/4/5 have
     * one cell; blocks1..3 have two. Do not alias invalid selectors. */
    if(selector<1 || selector>2 || block>5 || (selector==2 && (block==0 || block>=4)))return false;
    if(block==0){address=0x3ff460a8;shift=9;}
    else if(block==1){address=selector==1?0x3ff460a8:0x3ff460ac;shift=selector==1?0:18;}
    else if(block==2){address=0x3ff460ac;shift=selector==1?9:0;}
    else if(block==3){address=0x3ff460b0;shift=selector==1?18:9;}
    else {address=block==4?0x3ff460b0:0x3ff460b4;shift=0;}
    return true;
}
bool pbus_readback(u32 address) {
    return address>=0x3ff460a8 && address<=0x3ff460b4 && !(address&3);
}
bool iq_est_readback(u32 address) {
    return ((address>=0x3ff46080 && address<=0x3ff4608c) ||
            (address>=0x3ff460dc && address<=0x3ff460e4)) && !(address&3);
}
bool pbus_update(std::map<u32,u32> &r,u64 now) {
    const u64 deadline=stored(r,PBUS_DEADLINE)|(u64(stored(r,PBUS_DEADLINE+4))<<32);
    if(!deadline)return false;
    if(now<deadline)return true;
    const u32 command=stored(r,PBUS_PENDING);u32 address=0;unsigned shift=0;
    if(pbus_cell((command>>2)&15,(command>>15)&3,address,shift))
        r[address]=(stored(r,address)&~(511u<<shift))|(((command>>6)&511)<<shift);
    r.erase(PBUS_DEADLINE);r.erase(PBUS_DEADLINE+4);r.erase(PBUS_PENDING);
    return false;
}
}
namespace esp32_phy {
Result read(std::map<u32,u32> &r,u32 address,u32 &value,u64 now) {
    address=canonical(address);
    if(address==PBUS_STATUS || address==PBUS_COMMAND || pbus_readback(address)) {
        const bool busy=pbus_update(r,now);value=stored(r,address);
        if(address==PBUS_STATUS && busy)value|=0x80000000;
        return OK;
    }
    if(address==NF_CONTROL || address==NF_RESULT) {
        noise_update(r,now);value=stored(r,address);return OK;
    }
    /* bb_wdt_get_status: this controller currently accepts configuration,
     * not active RX/TX state-machine commands. It therefore has no pending
     * operation to time out. Unsupported active operations still fail at
     * their command register; this is not an always-successful completion. */
    if(address==0x3ff5d008){value=0;return OK;}
    if(iq_est_readback(address)){value=stored(r,address);return OK;}
    if(!reg(address))return UNHANDLED;
    value=stored(r,address);
    if(address==0x3ff71004 && !value)value=0x08000b00;
    if(address==0x3ff71204 && !value)value=0x08000900;
    if(address==0x3ff4607c && (value&3)==3)value|=0x80000000u;
    if(address==0x3ff73d24)value|=1;
    if(address==0x3ff71030)value|=0x8000;
    return OK;
}
Result write(std::map<u32,u32> &r,u32 address,u32 value,std::string &error,u64 now) {
    address=canonical(address);
    if(address==NF_RESULT){error="ESP32 receiver noise result is read-only";return ERROR;}
    if(pbus_readback(address)){error="ESP32 PBUS readback is read-only";return ERROR;}
    if(iq_est_readback(address)){error="ESP32 IQ estimator readback is read-only";return ERROR;}
    if(address==0x3ff5d008){error="ESP32 baseband watchdog status is read-only";return ERROR;}
    const Register *definition=reg(address);
    if(!definition)return UNHANDLED;
    if(stored(r,0x3ff000d0)&definition->reset_line){error="ESP32 radio controller is held in reset";return ERROR;}
    if(address==NF_CONTROL)value&=~0x01000000u; /* status cannot be supplied by guest */
    if(address==PBUS_STATUS || address==0x3ff4607c)value&=~0x80000000u;
    if((address==0x3ff7101c || address==0x3ff7121c) && (value&0x80000000u)) {
        /* BT/BLE Link Layer clock latch: bit 31 is latch request/busy flag.
         * The 28-bit 3.2 kHz (312.5 us half-slot) counter is latched synchronously. */
        value=(value&0x70000000u)|(u32)(((now*16)/5000)&0x0fffffffu);
        r[address+4]=(u32)(now%312);
    }
    if(value&~definition->supported){error="unsupported ESP32 radio control";return ERROR;}
    if(address==PBUS_COMMAND) {
        const bool busy=pbus_update(r,now);const u32 old=stored(r,address);
        if(busy && value!=old){error="ESP32 PBUS command is busy";return ERROR;}
        if((value&2) && !(old&2)) {
            u32 target=0;unsigned shift=0;
            if(!(value&1)){error="ESP32 PBUS command requires debug mode";return ERROR;}
            if(!pbus_cell((value>>2)&15,(value>>15)&3,target,shift)) {
                error="unsupported ESP32 PBUS block/register";return ERROR;
            }
            /* A functional serial configuration transfer occupies1us. The
             * receiver/transmitter cell changes only at completion. */
            const u64 deadline=now+1;r[PBUS_PENDING]=value;
            r[PBUS_DEADLINE]=(u32)deadline;r[PBUS_DEADLINE+4]=(u32)(deadline>>32);
        }
        r[address]=value;return OK;
    }
    if(address==NF_CONTROL) {
        noise_update(r,now);const u32 old=stored(r,address);
        r[address]=value|(old&0x01000000);
        if((value&0x02000000) || !(value&0x00800000))noise_cancel(r);
        else if(!(old&0x00800000) || (old&0x02000000))noise_start(r,now);
        return OK;
    }
    if(address==0x3ff5d050) {
        noise_update(r,now);if(value&1)noise_start(r,now);
        r[address]=0;return OK;
    }
    /* bb_wdt_timeout_clear writes a one-shot clear command without a second
     * software write. Keep its reset/interrupt enables independently. */
    if(address==0x3ff5d040)value&=~0x20000000u;
    r[address]=value;return OK;
}
void reset_baseband(std::map<u32,u32> &r) {
    for(unsigned i=0;i<sizeof(controls)/sizeof(controls[0]);++i)if(controls[i].reset_line==1)r.erase(controls[i].address);
    r.erase(NF_RESULT);r.erase(NF_STATE);r.erase(NF_DEADLINE);r.erase(NF_DEADLINE+4);r.erase(NF_PENDING);
}
bool set_noise_amplitude(std::map<u32,u32> &r,unsigned amplitude) {
    if(amplitude>511)return false;
    r[NF_AMPLITUDE]=amplitude;return true;
}
bool agc_enabled(const std::map<u32,u32> &r) {
    return !(stored(r,0x3ff000d0)&1) && !(stored(r,0x3ff5c080)&1) && (stored(r,0x3ff5c030)&0x130)==0x10;
}
int cca_threshold(const std::map<u32,u32> &r) {
    unsigned threshold=stored(r,0x3ff5c01c)&255;return threshold>=128?(int)threshold-256:(int)threshold;
}
void reset_frontend(std::map<u32,u32> &r) {
    for(unsigned i=0;i<sizeof(controls)/sizeof(controls[0]);++i)if(controls[i].reset_line==2)r.erase(controls[i].address);
    for(u32 address=0x3ff460a8;address<=0x3ff460b4;address+=4)r.erase(address);
    r.erase(PBUS_DEADLINE);r.erase(PBUS_DEADLINE+4);r.erase(PBUS_PENDING);
}
void reset_mac(std::map<u32,u32> &r) {
    for(unsigned i=0;i<sizeof(controls)/sizeof(controls[0]);++i)if(controls[i].reset_line==4)r.erase(controls[i].address);
    for(u32 addr=0x3ff73ce4;addr<=0x3ff73d20;addr+=4)r.erase(addr);
    for(u32 addr=0x3ff740b4;addr<=0x3ff74284;addr+=4)r.erase(addr);
    for(u32 addr=0x3ff74400;addr<=0x3ff747fc;addr+=4)r.erase(addr);
    r.erase(0x3ff73098);r.erase(0x3ff732fc);
}
bool frontend_signal(const std::map<u32,u32> &r,unsigned shift,bool automatic) {
    if(shift<8 || shift>16 || (shift&1) || (stored(r,0x3ff000d0)&2))return false;
    const u32 field=(stored(r,0x3ff460a0)>>shift)&3;
    return field&2?(field&1)!=0:automatic;
}
}
