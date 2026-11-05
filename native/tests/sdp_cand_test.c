/* sdp_cand_test.c -- host-side check of the answer's a=candidate line.
 *
 * Why: on 2026-11-05 the answer used to advertise the *server's* address as our
 * candidate (cfg.candidate_ip was filled from game_svr_addr), which makes the
 * media edge treat our real ICE checks as an unknown peer and drop them
 * silently. The fix is cfg.local_cand_ip/local_cand_port, filled from
 * getsockname() on the media socket. The pen was powered off before this could
 * be exercised on hardware, so this test locks the behaviour down on the host:
 *
 *   1. local_cand_* set        -> answer must carry OUR ip:port
 *   2. only candidate_ip set   -> legacy fallback still works
 *   3. neither set, port 0     -> no a=candidate line at all (pure trickle)
 *
 * Build (zig, same toolchain as the device build):
 *   zig cc -target x86_64-windows-gnu -O1 -o sdp_cand_test.exe \
 *       sdp_cand_test.c ../../PenYS/native/csrc/webrtc/sdp.c
 * Exit code 0 = all cases pass.
 */

#include <stdio.h>
#include <string.h>

#include "sdp.h"

/* Trimmed but structurally faithful copy of the real YS offer: BUNDLE with
 * audio/video/application, session-level ICE credentials and fingerprint, the
 * media-level codec sets we negotiate against. */
static const char *OFFER =
    "v=0\r\n"
    "o=- 4617245479528912330 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0 1 2\r\n"
    "a=extmap-allow-mixed\r\n"
    "a=msid-semantic: WMS\r\n"
    "a=ice-ufrag:GYTJ\r\n"
    "a=ice-pwd:abcdefghijklmnopqrstuvwx\r\n"
    "a=fingerprint:sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:"
    "11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00\r\n"
    "a=setup:actpass\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 63 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=sendrecv\r\n"
    "a=rtcp-mux\r\n"
    "a=ice-ufrag:GYTJ\r\n"
    "a=ice-pwd:abcdefghijklmnopqrstuvwx\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=rtpmap:63 red/48000/2\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96 97\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:1\r\n"
    "a=sendonly\r\n"
    "a=rtcp-mux\r\n"
    "a=ice-ufrag:GYTJ\r\n"
    "a=ice-pwd:abcdefghijklmnopqrstuvwx\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
    "a=rtpmap:97 rtx/90000\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:2\r\n"
    "a=sctp-port:5000\r\n";


/* sdp.c seeds o= from RAND_bytes (libcrypto on the device). The host test does
 * not link OpenSSL, so give it a deterministic stand-in: the answer's o= line
 * does not affect the candidate checks below. */
int RAND_bytes(unsigned char *buf, int num)
{
    int i;
    unsigned int x = 0x12345678u;
    for (i = 0; i < num; i++) {
        x = x * 1103515245u + 12345u;
        buf[i] = (unsigned char)(x >> 16);
    }
    return 1;
}

static int failures;

static void check(int cond, const char *what)
{
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

/* Return a pointer to the "a=candidate:" line inside `answer`, or NULL. */
static const char *find_cand(const char *s)
{
    return strstr(s, "a=candidate:");
}

static int build(sdp_offer_t *o, sdp_answer_cfg_t *cfg, char *out, size_t cap)
{
    char err[256];
    int n = sdp_build_answer(o, cfg, out, cap, err, sizeof(err));
    if (n < 0) {
        printf("  build_answer failed: %s\n", err);
        return -1;
    }
    return n;
}

int main(void)
{
    char err[256];
    sdp_offer_t *o;
    sdp_answer_cfg_t cfg;
    static char answer[SDP_MAX_LEN];
    const char *cand;
    int n;

    o = sdp_offer_parse(OFFER, err, sizeof(err));
    if (!o) {
        printf("parse failed: %s\n", err);
        return 2;
    }
    printf("offer: media=%d bundle=%d ufrag=%s\n", sdp_media_count(o),
           sdp_bundle_count(o), sdp_session_ice_ufrag(o));

    /* ---- case 1: the fix -- our real endpoint wins ------------------- */
    printf("case 1: local_cand_ip/port set\n");
    sdp_answer_cfg_init(&cfg);
    cfg.local_ip = "192.168.0.198";   /* session.c: W.udp_local_ip */
    cfg.fingerprint = "sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:"
                      "11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00";
    cfg.local_cand_ip = "192.168.0.198";
    cfg.local_cand_port = 45789;
    n = build(o, &cfg, answer, sizeof(answer));
    if (n < 0)
        return 2;
    cand = find_cand(answer);
    check(cand != NULL, "answer has an a=candidate line");
    if (cand) {
        char line[256];
        size_t i = 0;
        while (cand[i] && cand[i] != '\r' && cand[i] != '\n' && i + 1 < sizeof(line)) {
            line[i] = cand[i];
            i++;
        }
        line[i] = 0;
        printf("       %s\n", line);
        check(strstr(line, "192.168.0.198 45789") != NULL,
              "candidate carries our own ip:port");
        check(strstr(line, "typ host") != NULL, "candidate is typ host");
    }
    check(strstr(answer, "m=audio 45789 ") != NULL,
          "accepted m-line carries our real media port, not the offer's 9");
    check(strstr(answer, "c=IN IP4 192.168.0.198") != NULL,
          "c= names the address we actually send from");
    check(strstr(answer, "a=ice-options:trickle") != NULL,
          "answer advertises trickle (we do send a candidate over signalling)");

    /* ---- case 2: legacy fallback still honoured ---------------------- */
    printf("case 2: only candidate_ip/port set (legacy path)\n");
    sdp_answer_cfg_init(&cfg);
    cfg.local_ip = "0.0.0.0";
    cfg.fingerprint = "sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:"
                      "11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00";
    cfg.candidate_ip = "10.1.2.3";
    cfg.candidate_port = 40000;
    n = build(o, &cfg, answer, sizeof(answer));
    if (n < 0)
        return 2;
    cand = find_cand(answer);
    check(cand != NULL, "legacy candidate still emitted");
    check(cand && strstr(cand, "10.1.2.3 40000") != NULL,
          "legacy ip:port preserved");

    /* ---- case 3: nothing but trickle -------------------------------- */
    printf("case 3: no candidate configured (pure trickle)\n");
    sdp_answer_cfg_init(&cfg);
    cfg.local_ip = "0.0.0.0";
    cfg.fingerprint = "sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:"
                      "11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00";
    n = build(o, &cfg, answer, sizeof(answer));
    if (n < 0)
        return 2;
    check(find_cand(answer) == NULL, "no a=candidate in the answer");

    /* ---- sanity: answer shape --------------------------------------- */
    printf("case 4: answer shape\n");
    check(strstr(answer, "m=audio ") != NULL, "audio m-line kept");
    check(strstr(answer, "m=video ") != NULL, "video m-line kept");
    check(strstr(answer, "m=application 0 ") != NULL,
          "application rejected with port 0 (no SCTP)");
    check(strstr(answer, "a=setup:active") != NULL, "a=setup:active");
    check(strstr(answer, "a=mid:1") != NULL, "mid values preserved");


    /* ---- case 5: the answer must authenticate our ICE checks --------- */
    printf("case 5: answer carries our ICE/DTLS credentials\n");
    sdp_answer_cfg_init(&cfg);
    cfg.local_ip = "0.0.0.0";
    cfg.fingerprint = "sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:"
                      "11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00";
    cfg.local_cand_ip = "192.168.0.198";
    cfg.local_cand_port = 45789;
    {
        char uf[64], pw[128];
        memset(uf, 0, sizeof(uf));
        memset(pw, 0, sizeof(pw));
        cfg.ice_ufrag_out = uf;
        cfg.ice_ufrag_out_cap = sizeof(uf);
        cfg.ice_pwd_out = pw;
        cfg.ice_pwd_out_cap = sizeof(pw);
        n = build(o, &cfg, answer, sizeof(answer));
        if (n < 0)
            return 2;
        printf("       our ufrag=%s pwd=%s\n", uf, pw);
        check(uf[0] != 0, "we generated a local ufrag");
        check(pw[0] != 0, "we generated a local pwd");
        {
            char want[200];
            snprintf(want, sizeof(want), "a=ice-ufrag:%s", uf);
            check(strstr(answer, want) != NULL,
                  "answer advertises OUR ufrag (server verifies USERNAME with it)");
            snprintf(want, sizeof(want), "a=ice-pwd:%s", pw);
            check(strstr(answer, want) != NULL, "answer advertises OUR pwd");
        }
        check(strstr(answer, "a=group:BUNDLE") != NULL, "answer keeps a=group:BUNDLE");
        check(strstr(answer, "a=fingerprint:sha-256") != NULL, "answer keeps fingerprint");
        printf("---- answer ----\n%s\n----------------\n", answer);
    }

    printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
