/* WPA2-PSK (RSN) key derivation and the four-way handshake. Port of src/wpa2.py. */
#ifndef BK_WPA2_H
#define BK_WPA2_H

#include "common.h"

namespace wpa2 {

static const u16 ETH_EAPOL = 0x888E;
static const u32 DECR_CCMP_SUCCESS = 0x1C;
static const u32 CAP_PRIVACY = 0x0010;
static const int IE_RSN = 48;

extern const Bytes &RSN_IE_BODY();
extern const Bytes &RSN_IE();

Bytes pmk(const Bytes &passphrase, const Bytes &ssid);
Bytes prf(const Bytes &key, const Bytes &label, const Bytes &data, int nbytes);
void ptk(const Bytes &master, const Bytes &aa, const Bytes &spa, const Bytes &anonce, const Bytes &snonce,
         Bytes &kck, Bytes &kek, Bytes &tk);
Bytes key_mic(const Bytes &kck, const Bytes &frame);
Bytes aes_key_wrap(const Bytes &kek, const Bytes &plain);
Bytes aes_key_unwrap(const Bytes &kek, const Bytes &wrapped, bool &ok);
Bytes gtk_kde(const Bytes &gtk, int key_id = 1);
Bytes pad_key_data(const Bytes &data);
Bytes build_eapol_key(u16 key_info, u64 replay, const Bytes &nonce = Bytes(), const Bytes &key_data = Bytes(),
                      const Bytes *kck = 0, int key_len = 16, const Bytes &rsc = Bytes());

struct EapolKey {
    bool valid;
    u16 key_info;
    u16 key_len;
    u64 replay;
    Bytes nonce;
    Bytes mic;
    Bytes key_data;
    Bytes frame;
    bool pairwise, has_mic, ack, secure, install;
    EapolKey() : valid(false) {}
};
EapolKey parse_eapol_key(const Bytes &data);

class Authenticator {
public:
    Authenticator(const std::string &passphrase, const std::string &ssid, const Bytes &ap_mac,
                  const Bytes &sta_mac, const Bytes *anonce = 0, const Bytes *gtk = 0,
                  void (*log)(void *, const std::string &) = 0, void *log_ctx = 0);
    Bytes start();
    /* returns reply (empty = none); sets has_reply */
    Bytes on_message(const Bytes &data, bool &has_reply);

    bool done;
    std::string state;
    Bytes tk;
    Bytes anonce, gtk;
    const Bytes &kek() const { return kek_; } /* empty until message 2 verified */

private:
    Bytes master_, aa_, spa_;
    u64 replay_;
    Bytes snonce_, kck_, kek_;
    void (*log_)(void *, const std::string &);
    void *log_ctx_;
};

int wpa2_selftests();

} /* namespace wpa2 */

#endif
