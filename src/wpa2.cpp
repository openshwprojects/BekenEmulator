#include "wpa2.h"
#include "crypto_prims.h"

#include <stdio.h>
#include <stdlib.h>
#include <set>

namespace wpa2 {

static const u16 EAPOL_VERSION = 2;
static const u16 EAPOL_TYPE_KEY = 3;
static const u16 DESC_TYPE_RSN = 2;
static const int KEY_HDR = 99;
static const int MIC_OFF = 81, MIC_LEN = 16;

static const u16 KI_DESC_VER_CCMP = 0x0002;
static const u16 KI_PAIRWISE = 0x0008;
static const u16 KI_INSTALL = 0x0040;
static const u16 KI_ACK = 0x0080;
static const u16 KI_MIC = 0x0100;
static const u16 KI_SECURE = 0x0200;
static const u16 KI_ENCRYPTED = 0x1000;

static const u16 M1 = KI_DESC_VER_CCMP | KI_PAIRWISE | KI_ACK;
static const u16 M2 = KI_DESC_VER_CCMP | KI_PAIRWISE | KI_MIC;
static const u16 M3 = KI_DESC_VER_CCMP | KI_PAIRWISE | KI_INSTALL | KI_ACK | KI_MIC | KI_SECURE | KI_ENCRYPTED;
static const u16 M4 = KI_DESC_VER_CCMP | KI_PAIRWISE | KI_MIC | KI_SECURE;

static Bytes OUI_RSN() { return H("000fac"); }

const Bytes &RSN_IE_BODY()
{
    static Bytes body;
    if (body.empty()) {
        body = p16le(1) + OUI_RSN() + Bytes(1, (char)4)
               + p16le(1) + OUI_RSN() + Bytes(1, (char)4)
               + p16le(1) + OUI_RSN() + Bytes(1, (char)2)
               + p16le(0);
    }
    return body;
}
const Bytes &RSN_IE()
{
    static Bytes ie;
    if (ie.empty())
        ie = Bytes(1, (char)IE_RSN) + Bytes(1, (char)RSN_IE_BODY().size()) + RSN_IE_BODY();
    return ie;
}

Bytes pmk(const Bytes &passphrase, const Bytes &ssid)
{
    return pbkdf2_hmac_sha1(passphrase, ssid, 4096, 32);
}

Bytes prf(const Bytes &key, const Bytes &label, const Bytes &data, int nbytes)
{
    Bytes out;
    int i = 0;
    while ((int)out.size() < nbytes) {
        Bytes msg = label + Bytes(1, '\0') + data + Bytes(1, (char)(u8)i);
        out += hmac_sha1(key, msg);
        i++;
    }
    return bhead(out, nbytes);
}

void ptk(const Bytes &master, const Bytes &aa, const Bytes &spa, const Bytes &anonce, const Bytes &snonce,
         Bytes &kck, Bytes &kek, Bytes &tk)
{
    Bytes lo_addr = aa < spa ? aa : spa;
    Bytes hi_addr = aa < spa ? spa : aa;
    Bytes lo_n = anonce < snonce ? anonce : snonce;
    Bytes hi_n = anonce < snonce ? snonce : anonce;
    Bytes data = lo_addr + hi_addr + lo_n + hi_n;
    Bytes full = prf(master, Bytes("Pairwise key expansion"), data, 48);
    kck = bslice(full, 0, 16);
    kek = bslice(full, 16, 32);
    tk = bslice(full, 32, 48);
}

Bytes key_mic(const Bytes &kck, const Bytes &frame)
{
    Bytes blank = frame;
    for (int i = 0; i < MIC_LEN; i++)
        blank[MIC_OFF + i] = '\0';
    return bhead(hmac_sha1(kck, blank), MIC_LEN);
}

Bytes aes_key_wrap(const Bytes &kek, const Bytes &plain)
{
    int n = (int)plain.size() / 8;
    u64 a = 0xA6A6A6A6A6A6A6A6ULL;
    std::vector<Bytes> r(n);
    for (int i = 0; i < n; i++)
        r[i] = plain.substr(i * 8, 8);
    Aes enc(kek);
    for (int j = 0; j < 6; j++)
        for (int i = 1; i <= n; i++) {
            Bytes in = p64be(a) + r[i - 1];
            u8 b[16];
            enc.encrypt_block((const u8 *)in.data(), b);
            a = rd64be(b) ^ (u64)(n * j + i);
            r[i - 1] = Bytes((const char *)(b + 8), 8);
        }
    Bytes out = p64be(a);
    for (int i = 0; i < n; i++)
        out += r[i];
    return out;
}

Bytes aes_key_unwrap(const Bytes &kek, const Bytes &wrapped, bool &ok)
{
    ok = false;
    if (wrapped.size() % 8 || wrapped.size() < 16)
        return Bytes();
    int n = (int)wrapped.size() / 8 - 1;
    u64 a = rd64be(wrapped.data());
    std::vector<Bytes> r(n);
    for (int i = 0; i < n; i++)
        r[i] = wrapped.substr(8 * (i + 1), 8);
    Aes dec(kek);
    for (int j = 5; j >= 0; j--)
        for (int i = n; i >= 1; i--) {
            Bytes in = p64be(a ^ (u64)(n * j + i)) + r[i - 1];
            u8 b[16];
            dec.decrypt_block((const u8 *)in.data(), b);
            a = rd64be(b);
            r[i - 1] = Bytes((const char *)(b + 8), 8);
        }
    if (a != 0xA6A6A6A6A6A6A6A6ULL)
        return Bytes();
    ok = true;
    Bytes out;
    for (int i = 0; i < n; i++)
        out += r[i];
    return out;
}

Bytes gtk_kde(const Bytes &gtk, int key_id)
{
    Bytes body = OUI_RSN() + Bytes(1, (char)1) + Bytes(1, (char)(key_id & 3)) + Bytes(1, '\0') + gtk;
    return Bytes(1, (char)0xdd) + Bytes(1, (char)body.size()) + body;
}

Bytes pad_key_data(const Bytes &data)
{
    if (data.size() % 8 == 0)
        return data;
    Bytes out = data + Bytes(1, (char)0xdd);
    while (out.size() % 8)
        out += '\0';
    return out;
}

Bytes build_eapol_key(u16 key_info, u64 replay, const Bytes &nonce_in, const Bytes &key_data,
                      const Bytes *kck, int key_len, const Bytes &rsc_in)
{
    Bytes nonce = ljust(bhead(nonce_in, 32), 32, '\0');
    Bytes rsc = ljust(bhead(rsc_in, 8), 8, '\0');
    Bytes body = Bytes(1, (char)DESC_TYPE_RSN) + p16be(key_info) + p16be((u16)key_len) + p64be(replay)
                 + nonce + bytes_n(16) + rsc + bytes_n(8) + bytes_n(MIC_LEN) + p16be((u16)key_data.size()) + key_data;
    Bytes frame = Bytes(1, (char)EAPOL_VERSION) + Bytes(1, (char)EAPOL_TYPE_KEY) + p16be((u16)body.size()) + body;
    if (kck) {
        Bytes mic = key_mic(*kck, frame);
        frame = bhead(frame, MIC_OFF) + mic + bfrom(frame, MIC_OFF + MIC_LEN);
    }
    return frame;
}

EapolKey parse_eapol_key(const Bytes &data)
{
    EapolKey k;
    if (data.size() < (size_t)KEY_HDR || (u8)data[1] != EAPOL_TYPE_KEY)
        return k;
    if ((u8)data[4] != DESC_TYPE_RSN)
        return k;
    k.valid = true;
    k.key_info = (u16)g16be(data, 5);
    k.key_len = (u16)g16be(data, 7);
    u32 kd_len = g16be(data, 97);
    k.replay = g64be(data, 9);
    k.nonce = bslice(data, 17, 49);
    k.mic = bslice(data, MIC_OFF, MIC_OFF + MIC_LEN);
    k.key_data = bslice(data, KEY_HDR, KEY_HDR + kd_len);
    k.frame = data;
    k.pairwise = (k.key_info & KI_PAIRWISE) != 0;
    k.has_mic = (k.key_info & KI_MIC) != 0;
    k.ack = (k.key_info & KI_ACK) != 0;
    k.secure = (k.key_info & KI_SECURE) != 0;
    k.install = (k.key_info & KI_INSTALL) != 0;
    return k;
}

Authenticator::Authenticator(const std::string &passphrase, const std::string &ssid, const Bytes &ap_mac,
                             const Bytes &sta_mac, const Bytes *anonce_in, const Bytes *gtk_in,
                             void (*log)(void *, const std::string &), void *log_ctx)
    : done(false), state("INIT"), aa_(ap_mac), spa_(sta_mac), replay_(0), log_(log), log_ctx_(log_ctx)
{
    master_ = pmk(Bytes(passphrase), Bytes(ssid));
    if (anonce_in)
        anonce = *anonce_in;
    else
        anonce = os_urandom(32); /* Python: anonce or os.urandom(32) */
    if (gtk_in)
        gtk = *gtk_in;
    else
        gtk = os_urandom(16);
}

Bytes Authenticator::start()
{
    replay_++;
    state = "M1";
    return build_eapol_key(M1, replay_, anonce);
}

Bytes Authenticator::on_message(const Bytes &data, bool &has_reply)
{
    has_reply = false;
    EapolKey msg = parse_eapol_key(data);
    if (!msg.valid || !msg.pairwise)
        return Bytes();
    if (state == "M1" && msg.has_mic && !msg.secure) {
        if (msg.replay != replay_)
            return Bytes();
        snonce_ = msg.nonce;
        Bytes kck, kek, tk;
        ptk(master_, aa_, spa_, anonce, snonce_, kck, kek, tk);
        if (key_mic(kck, msg.frame) != msg.mic) {
            state = "FAILED";
            if (log_)
                log_(log_ctx_, "[WIFI] four-way handshake: the station's key does not match the passphrase this network was given");
            return Bytes();
        }
        kck_ = kck;
        kek_ = kek;
        this->tk = tk;
        replay_++;
        state = "M3";
        Bytes wrapped = aes_key_wrap(kek_, pad_key_data(RSN_IE() + gtk_kde(gtk)));
        has_reply = true;
        return build_eapol_key(M3, replay_, anonce, wrapped, &kck_);
    }
    if (state == "M3" && msg.has_mic && msg.secure) {
        if (msg.replay != replay_)
            return Bytes();
        if (key_mic(kck_, msg.frame) != msg.mic) {
            state = "FAILED";
            return Bytes();
        }
        state = "DONE";
        done = true;
        return Bytes();
    }
    return Bytes();
}

/* ------------------------------------------------------------- self-tests */
static int g_checks;
static void ck(bool cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        exit(1);
    }
    g_checks++;
    printf("  [PASS] %s\n", msg);
}

/* The station's half, enough to drive the authenticator honestly. */
struct StationState {
    Bytes snonce, kck, kek, tk;
};
static Bytes station(const char *passphrase, const char *ssid, const Bytes &aa_, const Bytes &spa_,
                     const Bytes &sn, const Bytes &msg1, const Bytes *msg3, StationState &state)
{
    EapolKey p = parse_eapol_key(msg1);
    if (!msg3) {
        state.snonce = sn;
        Bytes m_ = pmk(Bytes(passphrase), Bytes(ssid));
        ptk(m_, aa_, spa_, p.nonce, sn, state.kck, state.kek, state.tk);
        return build_eapol_key(M2, p.replay, sn, Bytes(), &state.kck);
    }
    return build_eapol_key(M4, parse_eapol_key(*msg3).replay, Bytes(), Bytes(), &state.kck);
}

static void collect_log(void *ctx, const std::string &line)
{
    ((std::vector<std::string> *)ctx)->push_back(line);
}

int wpa2_selftests()
{
    g_checks = 0;
    printf("== master key, against the published IEEE 802.11i vectors ==\n");
    ck(hexstr(pmk(Bytes("password"), Bytes("IEEE"))) == "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e",
       "passphrase 'password' on network 'IEEE' gives the documented key");
    ck(hexstr(pmk(Bytes("ThisIsAPassword"), Bytes("ThisIsASSID"))) == "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af",
       "...and so does the second published vector");
    ck(pmk(Bytes("password"), Bytes("IEEE")) != pmk(Bytes("password"), Bytes("OTHER")),
       "the network name is part of the key, so the same passphrase elsewhere differs");

    printf("== session keys ==\n");
    Bytes aa = H("020000000001"), spa = H("c8478c000000");
    Bytes an = bytes_n(32), sn = bytes_n(32);
    for (int i = 0; i < 32; i++) {
        an[i] = (char)i;
        sn[i] = (char)(32 + i);
    }
    Bytes m = pmk(Bytes("secret123"), Bytes("BekenSim"));
    Bytes kck, kek, tk;
    ptk(m, aa, spa, an, sn, kck, kek, tk);
    ck(kck.size() == 16 && kek.size() == 16 && tk.size() == 16, "the derivation yields three 16-byte keys");
    Bytes kck2, kek2, tk2;
    ptk(m, spa, aa, sn, an, kck2, kek2, tk2);
    ck(kck == kck2 && kek == kek2 && tk == tk2,
       "both ends derive the same keys although they see the addresses and nonces "
       "in the opposite order");
    Bytes kck3, kek3, tk3;
    ptk(m, aa, spa, an, bytes_n(32), kck3, kek3, tk3);
    ck(!(kck == kck3 && kek == kek3 && tk == tk3),
       "a different station nonce gives different session keys");
    {
        std::set<Bytes> outs;
        const int lens[3] = {16, 32, 48};
        for (int i = 0; i < 3; i++)
            outs.insert(prf(m, Bytes("Pairwise key expansion"), Bytes("x"), lens[i]));
        ck(outs.size() == 3, "the function stretches to whatever length is asked for");
    }

    printf("== key wrap (RFC 3394) ==\n");
    /* The RFC's own first test vector: 128-bit key, 128-bit data. */
    Bytes kek_v = H("000102030405060708090A0B0C0D0E0F");
    Bytes plain_v = H("00112233445566778899AABBCCDDEEFF");
    ck(upper(hexstr(aes_key_wrap(kek_v, plain_v))) == "1FA68B0A8112B447AEF34BD8FB5A7B829D3E862371D2CFE5",
       "wrapping the RFC's test data gives the RFC's answer");
    bool ok;
    Bytes unwrapped = aes_key_unwrap(kek_v, aes_key_wrap(kek_v, plain_v), ok);
    ck(ok && unwrapped == plain_v, "and unwrapping returns the original");
    Bytes bad = aes_key_wrap(kek_v, plain_v);
    bad[3] = (char)((u8)bad[3] ^ 1);
    aes_key_unwrap(kek_v, bad, ok);
    ck(!ok, "a tampered wrap is rejected rather than returning rubbish");

    printf("== message framing ==\n");
    Bytes f = build_eapol_key(M1, 1, an);
    ck((u8)f[0] == EAPOL_VERSION && (u8)f[1] == EAPOL_TYPE_KEY, "a message is an EAPOL frame of type key");
    ck(g16be(f, 2) == f.size() - 4, "its length field counts everything after the header");
    EapolKey p = parse_eapol_key(f);
    ck(p.valid && p.nonce == an && p.replay == 1 && p.ack && !p.has_mic,
       "message 1 is read back: nonce, counter, asking for a reply, no MIC yet");
    ck(!parse_eapol_key(H("02000005") + Bytes("hello")).valid && !parse_eapol_key(Bytes()).valid,
       "something that is not one of these messages is not read as one");
    Bytes signed_ = build_eapol_key(M2, 1, sn, Bytes(), &kck);
    ck(key_mic(kck, signed_) == parse_eapol_key(signed_).mic,
       "a signed message carries a MIC computed over itself with the field blanked");
    ck(key_mic(kck, signed_) != key_mic(bytes_n(16), signed_),
       "and that MIC depends on the key, which is what makes it proof");

    printf("== the four-way handshake ==\n");
    Bytes gtk16 = bytes_n(16);
    for (int i = 0; i < 16; i++)
        gtk16[i] = (char)i;
    std::vector<std::string> logs;
    Authenticator auth("secret123", "BekenSim", aa, spa, &an, &gtk16, collect_log, &logs);
    Bytes one = auth.start();
    ck(parse_eapol_key(one).nonce == an && !auth.done, "it opens with its own nonce and is not yet satisfied");
    StationState st;
    Bytes two = station("secret123", "BekenSim", aa, spa, sn, one, 0, st);
    bool hr;
    Bytes three = auth.on_message(two, hr);
    ck(hr && parse_eapol_key(three).install,
       "a station that holds the passphrase gets message 3, telling it to install the key");
    Bytes kd = aes_key_unwrap(auth.kek(), parse_eapol_key(three).key_data, ok);
    ck(ok && startswith(kd, RSN_IE()),
       "...which repeats the advertisement the station saw in the beacon, so it can "
       "satisfy itself nobody downgraded the network underneath it");
    Bytes gtk_part = bfrom(kd, (int)RSN_IE().size());
    ck(startswith(gtk_part, H("dd16000fac01")), "and then the group key, wrapped so only that station can read it");
    /* dd, length, 3-byte OUI, type, then two flag bytes, then the key itself. */
    ck(bslice(gtk_part, 8, 24) == gtk16, "which is the group key the access point chose");
    ck(parse_eapol_key(three).replay > parse_eapol_key(one).replay,
       "each message carries a higher counter, so an old one cannot be replayed");
    Bytes four = station("secret123", "BekenSim", aa, spa, sn, one, &three, st);
    bool hr2;
    auth.on_message(four, hr2);
    ck(auth.done && auth.tk == st.tk, "after message 4 both sides hold the identical traffic key");

    printf("== a station with the wrong passphrase ==\n");
    std::vector<std::string> logs2;
    Authenticator auth2("secret123", "BekenSim", aa, spa, &an, 0, collect_log, &logs2);
    Bytes one2 = auth2.start();
    StationState st_wrong;
    Bytes wrong = station("not-the-password", "BekenSim", aa, spa, sn, one2, 0, st_wrong);
    bool hr3;
    Bytes r3 = auth2.on_message(wrong, hr3);
    ck(!hr3 && r3.empty() && !auth2.done, "it gets no message 3 and the handshake does not complete");
    bool reported = false;
    for (size_t i = 0; i < logs2.size(); i++)
        if (logs2[i].find("does not match the passphrase") != std::string::npos)
            reported = true;
    ck(auth2.state == "FAILED" && reported, "and the mismatch is reported rather than passing silently");
    ck(auth2.tk.empty(), "a failed handshake leaves no traffic key behind, so .tk set always means success");

    printf("== a message with the wrong replay counter is ignored ==\n");
    Authenticator auth3("secret123", "BekenSim", aa, spa, &an);
    Bytes one3 = auth3.start();
    StationState st3;
    EapolKey stale = parse_eapol_key(station("secret123", "BekenSim", aa, spa, sn, one3, 0, st3));
    Bytes fk, fe, ft;
    ptk(pmk(Bytes("secret123"), Bytes("BekenSim")), aa, spa, an, stale.nonce, fk, fe, ft);
    Bytes forged = build_eapol_key(M2, 999, stale.nonce, Bytes(), &fk);
    bool hr4;
    Bytes r4 = auth3.on_message(forged, hr4);
    ck(!hr4 && r4.empty() && auth3.state == "M1" && !auth3.done,
       "a message 2 that does not echo the counter we sent is dropped, even with a valid MIC, "
       "and the handshake stays where it was");

    printf("== what the access point advertises ==\n");
    ck(hexstr(RSN_IE_BODY()) == std::string("0100000fac04") + "0100000fac04" + "0100000fac02" + "0000",
       "the advertisement offers CCMP for both cipher types and passphrase authentication");
    ck(RSN_IE_BODY().size() == 20, "which is 20 bytes, the usual length for this element");

    printf("\nAll %d WPA2 self-tests passed.\n", g_checks);
    return g_checks;
}

} /* namespace wpa2 */
