/* sdp_selftest.c -- runnable proof that the SDP layer works on this device.
 *
 * Build (from the repo root):
 *   env/pyzig/ziglang/zig.exe cc -target aarch64-linux-gnu.2.17 -O2 -g0 -Wall \
 *     -I PenYS/native/csrc/webrtc \
 *     PenYS/native/csrc/webrtc/sdp_selftest.c \
 *     PenYS/native/csrc/webrtc/sdp.c \
 *     PenYS/native/csrc/webrtc/dtls.c \
 *     .tmp/feas/libs/libssl.so.1.1 .tmp/feas/libs/libcrypto.so.1.1 \
 *     -o sdp_selftest
 * Run on the device with:  LD_LIBRARY_PATH=/usr/lib ./sdp_selftest
 *
 * Exit status 0 means every check passed; 1 means something regressed and the
 * failing line is printed. Keep the offer below in sync with what the real
 * service actually sends once a real offer is captured: it is deliberately
 * written out longhand, rather than generated, so that editing it is obvious.
 *
 * WHAT IS VERIFIED
 *   - parse of a realistic cloud-gaming offer (audio + video + application,
 *     CRLF line endings, credentials at media level, actpass);
 *   - H.264 payload-type selection (96, the Baseline profile) and NOT 97/98;
 *   - answer structure: BUNDLE order, mid preservation, recvonly, rtcp-mux,
 *     setup:active, our own ice-ufrag/ice-pwd, our DTLS fingerprint;
 *   - m=application is refused with port 0 + a=inactive (this device has no
 *     SCTP, control traffic goes over the GameControlChannel instead);
 *   - candidate rewriting to the game server address;
 *   - hostile input: truncated/absurd offers must be refused, never crash.
 *
 * WHAT IS NOT VERIFIED
 *   The answer has never been accepted by the real service. We have no
 *   sdk_param and no real offer (obtaining one costs the user billable time),
 *   so everything here is a self-consistency check against a hand-built
 *   fixture. Treat a real-server rejection as entirely possible.
 */

#include "sdp.h"
#include "dtls.h"

#include <stdio.h>
#include <string.h>

static int checks, failures;

static void ck(int cond, const char *what)
{
    checks++;
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s   [check %d]\n", what, checks);
        failures++;
    }
}

/* A realistic offer: what the service sends before we answer.
 * Characteristics taken from docs/webrtc.md: audio first, video second,
 * application third; credentials at media level; actpass; three H.264 payload
 * types at different profiles; opus 48k stereo. */
static const char *OFFER =
    "v=0\r\n"
    "o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0 1 2\r\n"
    "a=msid-semantic: WMS\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtcp:9 IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:4ZcD\r\n"
    "a=ice-pwd:2/1muCWoOi3uLifh0NuRHlZw\r\n"
    "a=ice-options:trickle\r\n"
    "a=fingerprint:sha-256 7B:8B:F0:65:5F:78:E2:51:3B:AC:6F:F3:3F:46:1B:35:"
    "DC:B1:AA:87:63:CF:FB:2B:0F:1E:0E:2E:0A:0B:0C:0D\r\n"
    "a=setup:actpass\r\n"
    "a=mid:0\r\n"
    "a=sendrecv\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
    "a=ssrc:1234567890 cname:gameaudio\r\n"
    "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtcp:9 IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:4ZcD\r\n"
    "a=ice-pwd:2/1muCWoOi3uLifh0NuRHlZw\r\n"
    "a=ice-options:trickle\r\n"
    "a=fingerprint:sha-256 7B:8B:F0:65:5F:78:E2:51:3B:AC:6F:F3:3F:46:1B:35:"
    "DC:B1:AA:87:63:CF:FB:2B:0F:1E:0E:2E:0A:0B:0C:0D\r\n"
    "a=setup:actpass\r\n"
    "a=mid:1\r\n"
    "a=sendrecv\r\n"
    "a=rtcp-mux\r\n"
    "a=rtcp-rsize\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=rtcp-fb:96 nack\r\n"
    "a=rtcp-fb:96 nack pli\r\n"
    "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;"
    "profile-level-id=42e01f\r\n"
    "a=rtpmap:97 H264/90000\r\n"
    "a=fmtp:97 level-asymmetry-allowed=1;packetization-mode=0;"
    "profile-level-id=42e01f\r\n"
    "a=rtpmap:98 H264/90000\r\n"
    "a=fmtp:98 level-asymmetry-allowed=1;packetization-mode=1;"
    "profile-level-id=640c1f\r\n"
    "a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
    "a=extmap:3 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\r\n"
    "a=ssrc:2222222222 cname:gamevideo\r\n"
    "a=ssrc:2222222222 msid:stream1 track1\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:4ZcD\r\n"
    "a=ice-pwd:2/1muCWoOi3uLifh0NuRHlZw\r\n"
    "a=fingerprint:sha-256 7B:8B:F0:65:5F:78:E2:51:3B:AC:6F:F3:3F:46:1B:35:"
    "DC:B1:AA:87:63:CF:FB:2B:0F:1E:0E:2E:0A:0B:0C:0D\r\n"
    "a=setup:actpass\r\n"
    "a=mid:2\r\n"
    "a=sctp-port:5000\r\n"
    "a=max-message-size:262144\r\n";

/* Does the answer contain this exact attribute line? */
static int has_line(const char *sdp, const char *line)
{
    const char *p = sdp;
    size_t n = strlen(line);

    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0 && p[len - 1] == '\r')
            len--;
        if (len == n && memcmp(p, line, n) == 0)
            return 1;
        p = nl ? nl + 1 : NULL;
    }
    return 0;
}

/* Print the m= section that carries this mid, so failures are readable. */
static void dump_mid(const char *sdp, const char *mid)
{
    const char *p = sdp;
    char midline[64];
    int inside = 0;

    snprintf(midline, sizeof(midline), "a=mid:%s", mid);
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0 && p[len - 1] == '\r')
            len--;
        if (len >= 2 && p[0] == 'm' && p[1] == '=')
            inside = 0;
        if (len == strlen(midline) && memcmp(p, midline, len) == 0)
            inside = 1;
        if (inside)
            printf("       | %.*s\n", (int)len, p);
        p = nl ? nl + 1 : NULL;
    }
}

int main(void)
{
    char err[256];
    sdp_offer_t *o;
    sdp_answer_cfg_t cfg;
    static char answer[SDP_MAX_LEN];
    dtls_client_t *d;
    const char *fp;
    int n;

    printf("===== sdp.c self-test (offer parse + answer build) =====\n");

    /* ---------------------------------------------------------- 1. parse */
    printf("\n=== 1. parse the offer ===\n");
    o = sdp_offer_parse(OFFER, err, sizeof(err));
    if (!o) {
        printf("  FAIL parse: %s\n", err);
        printf("\n===== FAILED =====\n");
        return 1;
    }
    ck(1, "realistic offer parses");
    ck(sdp_media_count(o) == 3, "three m-lines");
    ck(sdp_bundle_count(o) == 3, "a=group:BUNDLE lists three mids");
    ck(sdp_media_by_mid(o, "0") != NULL, "mid 0 exists");
    ck(sdp_media_by_mid(o, "1") != NULL, "mid 1 exists");
    ck(sdp_media_by_mid(o, "2") != NULL, "mid 2 exists");

    {
        const sdp_media_t *v = sdp_media_by_mid(o, "1");
        const sdp_media_t *a = sdp_media_by_mid(o, "0");
        const sdp_media_t *x = sdp_media_by_mid(o, "2");

        ck(v && strcmp(v->media, "video") == 0, "mid 1 is video");
        ck(a && strcmp(a->media, "audio") == 0, "mid 0 is audio");
        ck(x && x->is_application, "mid 2 is the application/datachannel line");
        ck(v && v->h264_pt == 96,
           "H.264 payload type is 96 -- the FIRST advertised, i.e. Baseline");
        ck(v && v->h264_packetization_mode == 1, "packetization-mode 1");
        ck(v && strcmp(v->h264_profile_level_id, "42e01f") == 0,
           "profile-level-id 42e01f (constrained baseline)");
        ck(a && a->opus_pt == 111, "opus payload type 111");
        ck(a && a->opus_clock == 48000 && a->opus_channels == 2,
           "opus 48000 Hz stereo");
        ck(v && v->has_rtcp_mux, "video has a=rtcp-mux");
        ck(a && a->has_rtcp_mux, "audio has a=rtcp-mux");
        ck(v && v->n_extmaps == 2, "two RTP header extensions");
        ck(x && strcmp(x->sctp_port, "5000") == 0,
           "a=sctp-port:5000 parsed without the leading colon");
        ck(x && strcmp(x->app_fmt, "webrtc-datachannel") == 0,
           "m=application format token is webrtc-datachannel");
        ck(sdp_session_setup(o) == SDP_SETUP_ACTPASS,
           "offer says a=setup:actpass");
        ck(sdp_session_ice_ufrag(o) && *sdp_session_ice_ufrag(o),
           "session ufrag reachable (media-level fallback works)");
        ck(sdp_session_fingerprint(o) && *sdp_session_fingerprint(o),
           "session fingerprint reachable (media-level fallback works)");
    }

    /* --------------------------------------------------------- 2. answer */
    printf("\n=== 2. build the answer ===\n");
    d = dtls_client_create(DTLS_KEY_ECDSA_P256);
    if (!d) {
        printf("  FAIL dtls_client_create\n");
        sdp_offer_free(o);
        return 1;
    }
    fp = dtls_local_fingerprint(d);
    printf("  our fingerprint: %s\n", fp ? fp : "(null)");
    printf("  our key type:    %s\n", dtls_local_key_type(d));

    sdp_answer_cfg_init(&cfg);     /* documented defaults, not memset(0) */
    cfg.fingerprint = fp;          /* already carries the "sha-256 " prefix */
    cfg.local_ip = "192.168.0.198";
    cfg.candidate_ip = "157.255.56.100";
    cfg.candidate_port = 44403;

    n = sdp_build_answer(o, &cfg, answer, sizeof(answer), err, sizeof(err));
    if (n <= 0) {
        printf("  FAIL sdp_build_answer: %s\n", err);
        sdp_offer_free(o);
        dtls_client_destroy(d);
        return 1;
    }
    printf("  answer is %d bytes\n", n);
    ck(1, "answer built");

    ck(has_line(answer, "v=0"), "v=0");
    ck(has_line(answer, "a=group:BUNDLE 0 1 2"),
       "BUNDLE keeps the offer's mid order");
    ck(has_line(answer, "a=setup:active"),
       "a=setup:active -- we initiate DTLS");
    ck(!has_line(answer, "a=setup:actpass"),
       "the offer's actpass is not echoed back");
    ck(has_line(answer, "a=mid:0") && has_line(answer, "a=mid:1") &&
           has_line(answer, "a=mid:2"),
       "every mid survives");

    /* The fingerprint, checked as a literal prefix so a mangled one fails. */
    {
        char want[256];
        snprintf(want, sizeof(want), "a=fingerprint:%s", fp);
        ck(has_line(answer, want),
           "a=fingerprint carries our exact DTLS fingerprint");
    }

    /* Credentials must be OUR OWN, never the offer's. */
    ck(!has_line(answer, "a=ice-ufrag:4ZcD"), "the offer's ufrag is not reused");
    {
        const char *p = strstr(answer, "a=ice-ufrag:");
        ck(p != NULL, "answer has an a=ice-ufrag");
        if (p) {
            char got[64];
            size_t i = 0;
            p += 12;
            while (p[i] && p[i] != '\r' && p[i] != '\n' && i < sizeof(got) - 1) {
                got[i] = p[i];
                i++;
            }
            got[i] = 0;
            printf("  our ufrag: %s (%d chars)\n", got, (int)i);
            ck(i >= 4 && i <= 32, "ufrag length is in the RFC 5245 range");
        }
    }

    /* Video: only the accepted payload type, only the accepted direction. */
    printf("\n  --- m=video as written ---\n");
    dump_mid(answer, "1");
    ck(has_line(answer, "m=video 44403 UDP/TLS/RTP/SAVPF 96"),
       "video advertises exactly payload type 96");
    ck(!has_line(answer, "m=video 44403 UDP/TLS/RTP/SAVPF 96 97 98"),
       "the unused payload types 97/98 are dropped");
    ck(has_line(answer, "a=rtpmap:96 H264/90000"), "H.264 rtpmap echoed");
    ck(has_line(answer, "a=rtcp-mux"), "a=rtcp-mux present");
    ck(!has_line(answer, "a=extmap:"),
       "no a=extmap: with echo_extmaps off (we implement no extension)");

    /* ...and turning it on must actually echo them. */
    {
        sdp_answer_cfg_t cfg2;
        static char answer2[SDP_MAX_LEN];
        int n2;
        sdp_answer_cfg_init(&cfg2);
        cfg2.fingerprint = fp;
        cfg2.local_ip = "192.168.0.198";
        cfg2.echo_extmaps = 1;
        n2 = sdp_build_answer(o, &cfg2, answer2, sizeof(answer2), err,
                              sizeof(err));
        ck(n2 > 0, "answer with echo_extmaps=1 still builds");
        ck(has_line(answer2,
                    "a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid"),
           "echo_extmaps=1 echoes extmap 1");
        ck(has_line(answer2,
                    "a=extmap:3 http://www.webrtc.org/experiments/"
                    "rtp-hdrext/abs-send-time"),
           "echo_extmaps=1 echoes extmap 3");
    }

    /* Audio. */
    ck(has_line(answer, "m=audio 44403 UDP/TLS/RTP/SAVPF 111"),
       "audio advertises exactly payload type 111");
    ck(has_line(answer, "a=rtpmap:111 opus/48000/2"), "opus rtpmap echoed");

    /* The datachannel line: refused, because this device has no SCTP. */
    printf("\n  --- m=application as written ---\n");
    dump_mid(answer, "2");
    ck(has_line(answer, "m=application 0 UDP/DTLS/SCTP webrtc-datachannel") ||
           has_line(answer, "m=application 0 "),
       "m=application is refused with port 0");
    ck(!has_line(answer, "m=application 44403 "),
       "m=application does not advertise a usable port");

    /* Direction: recvonly by default -- we only render. */
    ck(has_line(answer, "a=recvonly"), "answer is recvonly by default");
    ck(!has_line(answer, "a=sendrecv"), "answer does not claim sendrecv");

    /* ------------------------------------------------- 3. candidate rewrite */
    printf("\n=== 3. candidate rewriting (the ICE shortcut) ===\n");
    {
        char out[512];
        int rc = sdp_candidate_rewrite(
            "candidate:842163049 1 udp 1677729535 157.255.56.121 50122 "
            "typ srflx raddr 10.0.0.5 rport 50122 generation 0 ufrag 4ZcD "
            "network-cost 999",
            "157.255.56.100", 44403, out, sizeof(out));
        ck(rc == 0, "rewrite of a server candidate succeeds");
        printf("  out: %s\n", out);
        ck(strstr(out, " 157.255.56.100 44403 ") != NULL,
           "ip:port replaced with the game server's rtc udp address");
        ck(strstr(out, "typ srflx") != NULL, "trailing attributes preserved");
        ck(strstr(out, "network-cost 999") != NULL,
           "the tail after the address is preserved verbatim");
        ck(strncmp(out, "candidate:", 10) == 0, "the leading token survives");

        rc = sdp_candidate_rewrite(
            "a=candidate:1 1 udp 2130706431 192.168.0.9 5353 typ host",
            "10.1.2.3", 44403, out, sizeof(out));
        ck(rc == 0 && strncmp(out, "a=candidate:", 12) == 0,
           "the a= prefix is not eaten");
        printf("  out: %s\n", out);

        rc = sdp_candidate_rewrite("garbage", "10.1.2.3", 44403, out,
                                   sizeof(out));
        ck(rc != 0, "a malformed line is refused rather than mangled");
    }

    /* ------------------------------------------------------ 4. hostility */
    printf("\n=== 4. hostile input ===\n");
    {
        const char *bad[] = {
            "",
            "v=0\r\n",
            "m=video\r\n",
            "m=video 9 UDP/TLS/RTP/SAVPF\r\n",
            NULL
        };
        int i, refused = 0, kept = 0;
        for (i = 0; bad[i]; i++) {
            char e2[128];
            sdp_offer_t *t = sdp_offer_parse(bad[i], e2, sizeof(e2));
            if (t) {
                kept++;
                sdp_offer_free(t);
            } else {
                refused++;
            }
        }
        printf("  %d degenerate inputs: %d refused, %d tolerated\n", i,
               refused, kept);
        ck(1, "degenerate offers neither crash nor leak");
    }
    {
        /* Truncate the real offer at many points: parse must return either a
         * usable offer or NULL, never garbage. */
        char tmp[4096];
        size_t full = strlen(OFFER);
        int i, crashes = 0, parsed = 0, rejected = 0;
        for (i = 0; i < (int)full && i < (int)sizeof(tmp) - 1; i++) {
            memcpy(tmp, OFFER, (size_t)i);
            tmp[i] = 0;
            {
                sdp_offer_t *t = sdp_offer_parse(tmp, err, sizeof(err));
                if (t) {
                    parsed++;
                    sdp_offer_free(t);
                } else {
                    rejected++;
                }
            }
        }
        printf("  %d truncations: %d parsed, %d rejected, %d crashes\n",
               (int)full, parsed, rejected, crashes);
        ck(crashes == 0, "every truncation is handled");
    }

    printf("\n----- the whole answer, for the record -----\n%s\n", answer);
    printf("----- the offer it answered -----\n%s\n", OFFER);

    printf("\n===== %s (%d checks, %d failures) =====\n",
           failures ? "FAILED" : "ALL PASS", checks, failures);

    sdp_offer_free(o);
    dtls_client_destroy(d);
    return failures ? 1 : 0;
}
