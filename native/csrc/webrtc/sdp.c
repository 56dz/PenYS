/* sdp.c -- SDP offer parsing and answer generation.
 *
 * See sdp.h for the contract. Implementation notes that matter on this device:
 *
 *   - Everything is parsed in one pass over a mutable copy of the text. Lines
 *     are split on \n and a trailing \r is stripped, so CRLF and LF both work.
 *   - No gets/fscanf: the parser is plain pointer walking with explicit
 *     bounds, because the offer arrives from the network.
 *   - The answer is written with a growing cursor and every append checks the
 *     remaining capacity, so a hostile offer cannot overflow the caller.
 *   - Payload types are re-numbered in the answer? No. We reuse the offer's
 *     payload numbers verbatim. Renumbering is legal but pointless here and
 *     risks mismatching the peer's fmtp associations.
 */

#include "sdp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* -------------------------------------------------------------- helpers -- */

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!err || cap == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static int copy_str(char *dst, size_t cap, const char *src, size_t n)
{
    if (!dst || cap == 0)
        return 0;
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
    return (int)n;
}

static int starts_with(const char *s, const char *pfx)
{
    size_t n = strlen(pfx);
    return strncmp(s, pfx, n) == 0;
}

/* --------------------------------------------------------------- struct -- */

struct sdp_offer {
    char session_fingerprint_alg[32];
    char session_fingerprint[160];
    char session_ice_ufrag[64];
    char session_ice_pwd[128];
    int  session_setup;

    char bundle_mids[SDP_MAX_MEDIA][64];
    int  n_bundle;

    sdp_media_t media[SDP_MAX_MEDIA];
    int         n_media;
};

/* A media entry starts with everything inherited from the session. */
static void media_init(sdp_media_t *m, const struct sdp_offer *o, int index)
{
    memset(m, 0, sizeof(*m));
    m->index = index;
    m->h264_pt = -1;
    m->h265_pt = -1;
    m->opus_pt = -1;
    m->h264_packetization_mode = -1;
    m->direction = SDP_DIR_NONE;
    if (o) {
        copy_str(m->ice_ufrag, sizeof(m->ice_ufrag), o->session_ice_ufrag,
                 strlen(o->session_ice_ufrag));
        copy_str(m->ice_pwd, sizeof(m->ice_pwd), o->session_ice_pwd,
                 strlen(o->session_ice_pwd));
        copy_str(m->fingerprint_alg, sizeof(m->fingerprint_alg),
                 o->session_fingerprint_alg,
                 strlen(o->session_fingerprint_alg));
        copy_str(m->fingerprint, sizeof(m->fingerprint),
                 o->session_fingerprint, strlen(o->session_fingerprint));
    }
}

/* ----------------------------------------------------------- attribute -- */

/* Parse "a=fmtp:<pt> key=val;key=val" and fill whichever codec it belongs to.
 * Recognises H.264 (RFC 6184), H.265 (RFC 7798) and Opus (RFC 7587) keys. */
static void parse_fmtp(sdp_media_t *m, int pt, const char *params, size_t n)
{
    char buf[512];
    copy_str(buf, sizeof(buf), params, n);

    if (pt == m->h264_pt) {
        copy_str(m->h264_fmtp, sizeof(m->h264_fmtp), buf, strlen(buf));
        {
            const char *p = strstr(buf, "profile-level-id=");
            if (p) {
                p += 17;
                copy_str(m->h264_profile_level_id,
                         sizeof(m->h264_profile_level_id), p, 6);
            }
        }
        {
            const char *p = strstr(buf, "packetization-mode=");
            if (p) {
                p += 19;
                m->h264_packetization_mode = atoi(p);
            }
        }
    } else if (pt == m->h265_pt) {
        copy_str(m->h265_fmtp, sizeof(m->h265_fmtp), buf, strlen(buf));
    } else if (pt == m->opus_pt) {
        copy_str(m->opus_fmtp, sizeof(m->opus_fmtp), buf, strlen(buf));
    }
}

/* a=rtpmap:<pt> <encoding>/<clock>[/<channels>] */
static void parse_rtpmap(sdp_media_t *m, const char *v, size_t n)
{
    char buf[256];
    int pt;
    char *enc, *clock, *ch;
    copy_str(buf, sizeof(buf), v, n);

    pt = atoi(buf);
    enc = strchr(buf, ' ');
    if (!enc)
        return;
    enc++;
    clock = strchr(enc, '/');
    if (clock) {
        *clock++ = 0;
        ch = strchr(clock, '/');
        if (ch)
            *ch++ = 0;
    } else {
        ch = NULL;
    }

    /* First occurrence wins. A real offer lists several H.264 profiles
     * (Baseline/Main/High) under different PTs; taking the last one would
     * silently switch to High and break decoders that only do Baseline. */
    if (strcasecmp(enc, "H264") == 0) {
        if (m->h264_pt < 0)
            m->h264_pt = pt;
    } else if (strcasecmp(enc, "H265") == 0 || strcasecmp(enc, "HEVC") == 0) {
        if (m->h265_pt < 0)
            m->h265_pt = pt;
    } else if (strcasecmp(enc, "opus") == 0) {
        m->opus_pt = pt;
        m->opus_clock = clock ? atoi(clock) : 48000;
        m->opus_channels = ch ? atoi(ch) : 2;
    }
}

/* a=extmap:<id>[/<dir>] <uri> */
static void parse_extmap(sdp_media_t *m, const char *v, size_t n)
{
    char buf[256];
    char *sp, *uri;
    int id;
    copy_str(buf, sizeof(buf), v, n);
    sp = strchr(buf, ' ');
    if (!sp)
        return;
    *sp++ = 0;
    id = atoi(buf);
    uri = sp;
    if (id <= 0 || m->n_extmaps >= SDP_MAX_EXTMAPS)
        return;
    m->extmaps[m->n_extmaps].id = id;
    copy_str(m->extmaps[m->n_extmaps].uri,
             sizeof(m->extmaps[m->n_extmaps].uri), uri, strlen(uri));
    m->n_extmaps++;
}

/* a=ssrc:<ssrc> <attribute>  -- we only keep the ssrc id itself. */
static void parse_ssrc(sdp_media_t *m, const char *v, size_t n)
{
    char buf[128];
    char *sp;
    unsigned long id;
    int i;
    copy_str(buf, sizeof(buf), v, n);
    sp = strchr(buf, ' ');
    if (sp)
        *sp = 0;
    id = strtoul(buf, NULL, 10);
    if (id == 0 || m->n_ssrcs >= SDP_MAX_SSRCS)
        return;
    for (i = 0; i < m->n_ssrcs; i++)
        if (m->ssrcs[i] == (unsigned int)id)
            return;
    m->ssrcs[m->n_ssrcs++] = (unsigned int)id;
}

static void parse_direction(sdp_media_t *m, const char *name, size_t n)
{
    if (n == 8 && strncmp(name, "sendrecv", 8) == 0)
        m->direction = SDP_DIR_SENDRECV;
    else if (n == 8 && strncmp(name, "sendonly", 8) == 0)
        m->direction = SDP_DIR_SENDONLY;
    else if (n == 8 && strncmp(name, "recvonly", 8) == 0)
        m->direction = SDP_DIR_RECVONLY;
    else if (n == 8 && strncmp(name, "inactive", 8) == 0)
        m->direction = SDP_DIR_INACTIVE;
}

/* a=fingerprint:<alg> <hex:hex:..> */
static void parse_fingerprint(char *alg, size_t algcap, char *fp, size_t fpcap,
                              const char *v, size_t n)
{
    char *sp;
    char buf[256];
    copy_str(buf, sizeof(buf), v, n);
    sp = strchr(buf, ' ');
    if (!sp)
        return;
    *sp++ = 0;
    copy_str(alg, algcap, buf, strlen(buf));
    /* Strip colons: SDP carries AA:BB:CC, dtls.c stores the same form, so we
     * keep it verbatim. */
    copy_str(fp, fpcap, sp, strlen(sp));
}

static int setup_from(const char *v, size_t n)
{
    if (n == 6 && strncmp(v, "active", 6) == 0)  return SDP_SETUP_ACTIVE;
    if (n == 7 && strncmp(v, "passive", 7) == 0) return SDP_SETUP_PASSIVE;
    if (n == 7 && strncmp(v, "actpass", 7) == 0) return SDP_SETUP_ACTPASS;
    return SDP_SETUP_NONE;
}

/* --------------------------------------------------------------- parse -- */

static void parse_m_line(sdp_media_t *m, const char *v, size_t n)
{
    char buf[512];
    char *p, *save;
    int field;

    copy_str(buf, sizeof(buf), v, n);

    /* m=<media> <port> <proto> <fmt> [<fmt> ...] */
    p = strtok_r(buf, " ", &save);
    if (!p)
        return;
    copy_str(m->media, sizeof(m->media), p, strlen(p));
    m->is_application = (strcasecmp(p, "application") == 0);

    p = strtok_r(NULL, " ", &save);
    if (p) {
        m->port = atoi(p);
        if (m->port == 0)
            m->rejected = 1;
    }

    p = strtok_r(NULL, " ", &save);
    if (p)
        copy_str(m->proto, sizeof(m->proto), p, strlen(p));

    field = 0;
    while ((p = strtok_r(NULL, " ", &save)) != NULL) {
        if (m->is_application) {
            /* m=application 9 UDP/DTLS/SCTP webrtc-datachannel
             * The format token is "webrtc-datachannel" (RFC 8841); the real
             * port arrives later in a=sctp-port. Keep both. */
            if (field == 0)
                copy_str(m->app_fmt, sizeof(m->app_fmt), p, strlen(p));
        } else {
            if (m->n_payloads < SDP_MAX_FORMATS)
                m->payloads[m->n_payloads++] = atoi(p);
        }
        field++;
    }
}

sdp_offer_t *sdp_offer_parse(const char *sdp, char *err, size_t errcap)
{
    struct sdp_offer *o;
    char *text, *line, *save;
    int in_media = -1;
    int saw_version = 0;
    size_t len;

    if (err && errcap)
        err[0] = 0;
    if (!sdp) {
        set_err(err, errcap, "null offer");
        return NULL;
    }
    len = strlen(sdp);
    if (len == 0) {
        set_err(err, errcap, "empty offer");
        return NULL;
    }
    if (len > SDP_MAX_LEN) {
        set_err(err, errcap, "offer too large (%zu > %d)", len, SDP_MAX_LEN);
        return NULL;
    }

    o = (struct sdp_offer *)calloc(1, sizeof(*o));
    if (!o) {
        set_err(err, errcap, "out of memory");
        return NULL;
    }

    text = (char *)malloc(len + 1);
    if (!text) {
        free(o);
        set_err(err, errcap, "out of memory");
        return NULL;
    }
    memcpy(text, sdp, len + 1);

    line = strtok_r(text, "\n", &save);
    while (line) {
        size_t ln = strlen(line);
        const char *body = line;
        size_t blen = ln;

        while (blen > 0 && (body[blen - 1] == '\r' || body[blen - 1] == ' '))
            blen--;

        if (blen == 0) {
            line = strtok_r(NULL, "\n", &save);
            continue;
        }

        if (!saw_version) {
            if (blen >= 2 && body[0] == 'v' && body[1] == '=') {
                saw_version = 1;
                line = strtok_r(NULL, "\n", &save);
                continue;
            }
            set_err(err, errcap, "not an SDP (first line is not v=)");
            free(text);
            free(o);
            return NULL;
        }

        if (starts_with(body, "m=")) {
            in_media++;
            if (in_media < SDP_MAX_MEDIA) {
                media_init(&o->media[in_media], o, in_media);
                parse_m_line(&o->media[in_media], body + 2, blen - 2);
                o->n_media = in_media + 1;
            }
        } else if (in_media < 0) {
            /* session level */
            if (starts_with(body, "a=ice-ufrag:"))
                copy_str(o->session_ice_ufrag, sizeof(o->session_ice_ufrag),
                         body + 12, blen - 12);
            else if (starts_with(body, "a=ice-pwd:"))
                copy_str(o->session_ice_pwd, sizeof(o->session_ice_pwd),
                         body + 10, blen - 10);
            else if (starts_with(body, "a=fingerprint:"))
                parse_fingerprint(o->session_fingerprint_alg,
                                  sizeof(o->session_fingerprint_alg),
                                  o->session_fingerprint,
                                  sizeof(o->session_fingerprint),
                                  body + 14, blen - 14);
            else if (starts_with(body, "a=setup:"))
                o->session_setup = setup_from(body + 8, blen - 8);
            else if (starts_with(body, "a=group:BUNDLE")) {
                const char *p = body + 15;
                size_t rest = blen - 15;
                char buf[512];
                char *tok, *s2;
                copy_str(buf, sizeof(buf), p, rest);
                tok = strtok_r(buf, " ", &s2);
                while (tok && o->n_bundle < SDP_MAX_MEDIA) {
                    copy_str(o->bundle_mids[o->n_bundle],
                             sizeof(o->bundle_mids[0]), tok, strlen(tok));
                    o->n_bundle++;
                    tok = strtok_r(NULL, " ", &s2);
                }
            }
        } else if (in_media < SDP_MAX_MEDIA) {
            sdp_media_t *m = &o->media[in_media];
            if (starts_with(body, "a=mid:"))
                copy_str(m->mid, sizeof(m->mid), body + 6, blen - 6);
            else if (starts_with(body, "a=rtpmap:"))
                parse_rtpmap(m, body + 9, blen - 9);
            else if (starts_with(body, "a=fmtp:")) {
                const char *fsp = strchr(body + 7, ' ');
                if (fsp)
                    parse_fmtp(m, atoi(body + 7), fsp + 1,
                               blen - (size_t)(fsp - body) - 1);
            }
            else if (starts_with(body, "a=extmap:"))
                parse_extmap(m, body + 9, blen - 9);
            else if (starts_with(body, "a=ssrc:"))
                parse_ssrc(m, body + 7, blen - 7);
            else if (starts_with(body, "a=rtcp-mux"))
                m->has_rtcp_mux = 1;
            else if (starts_with(body, "a=ice-ufrag:"))
                copy_str(m->ice_ufrag, sizeof(m->ice_ufrag), body + 12,
                         blen - 12);
            else if (starts_with(body, "a=ice-pwd:"))
                copy_str(m->ice_pwd, sizeof(m->ice_pwd), body + 10,
                         blen - 10);
            else if (starts_with(body, "a=fingerprint:"))
                parse_fingerprint(m->fingerprint_alg,
                                  sizeof(m->fingerprint_alg),
                                  m->fingerprint, sizeof(m->fingerprint),
                                  body + 14, blen - 14);
            else if (starts_with(body, "a=setup:"))
                m->setup = setup_from(body + 8, blen - 8);
            else if (starts_with(body, "a=sctp-port:"))
                copy_str(m->sctp_port, sizeof(m->sctp_port), body + 12,
                         blen - 12);
            else
                parse_direction(m, body + 2, blen - 2);
        }

        line = strtok_r(NULL, "\n", &save);
    }

    free(text);

    if (!saw_version) {
        set_err(err, errcap, "no v= line");
        free(o);
        return NULL;
    }
    if (o->n_media == 0) {
        set_err(err, errcap, "no m= line");
        free(o);
        return NULL;
    }
    return o;
}

void sdp_offer_free(sdp_offer_t *o)
{
    free(o);
}

/* ------------------------------------------------------------ accessors -- */

/* Session-level attributes are optional in real offers: browsers emit the
 * ICE credentials and the fingerprint inside every media section, and some
 * servers emit them only there. So each session accessor falls back to the
 * first media section that carries a value. Without this fallback a perfectly
 * valid browser offer looks like it has no ufrag/fingerprint at all. */

const char *sdp_session_fingerprint(const sdp_offer_t *o)
{
    int i;
    if (!o)
        return "";
    if (o->session_fingerprint[0])
        return o->session_fingerprint;
    for (i = 0; i < o->n_media; i++)
        if (o->media[i].fingerprint[0])
            return o->media[i].fingerprint;
    return "";
}

const char *sdp_session_fingerprint_alg(const sdp_offer_t *o)
{
    int i;
    if (!o)
        return "";
    if (o->session_fingerprint_alg[0])
        return o->session_fingerprint_alg;
    for (i = 0; i < o->n_media; i++)
        if (o->media[i].fingerprint_alg[0])
            return o->media[i].fingerprint_alg;
    return "";
}

const char *sdp_session_ice_ufrag(const sdp_offer_t *o)
{
    int i;
    if (!o)
        return "";
    if (o->session_ice_ufrag[0])
        return o->session_ice_ufrag;
    for (i = 0; i < o->n_media; i++)
        if (o->media[i].ice_ufrag[0])
            return o->media[i].ice_ufrag;
    return "";
}

const char *sdp_session_ice_pwd(const sdp_offer_t *o)
{
    int i;
    if (!o)
        return "";
    if (o->session_ice_pwd[0])
        return o->session_ice_pwd;
    for (i = 0; i < o->n_media; i++)
        if (o->media[i].ice_pwd[0])
            return o->media[i].ice_pwd;
    return "";
}

int sdp_session_setup(const sdp_offer_t *o)
{
    int i;
    if (!o)
        return SDP_SETUP_NONE;
    if (o->session_setup != SDP_SETUP_NONE)
        return o->session_setup;
    for (i = 0; i < o->n_media; i++)
        if (o->media[i].setup != SDP_SETUP_NONE)
            return o->media[i].setup;
    return SDP_SETUP_NONE;
}

int sdp_bundle_count(const sdp_offer_t *o)
{
    return o ? o->n_bundle : 0;
}

const char *sdp_bundle_mid(const sdp_offer_t *o, int i)
{
    if (!o || i < 0 || i >= o->n_bundle)
        return "";
    return o->bundle_mids[i];
}

int sdp_media_count(const sdp_offer_t *o)
{
    return o ? o->n_media : 0;
}

const sdp_media_t *sdp_media_at(const sdp_offer_t *o, int i)
{
    if (!o || i < 0 || i >= o->n_media)
        return NULL;
    return &o->media[i];
}

const sdp_media_t *sdp_media_by_mid(const sdp_offer_t *o, const char *mid)
{
    int i;
    if (!o || !mid)
        return NULL;
    for (i = 0; i < o->n_media; i++)
        if (strcmp(o->media[i].mid, mid) == 0)
            return &o->media[i];
    return NULL;
}

const sdp_media_t *sdp_media_first_kind(const sdp_offer_t *o, const char *kind)
{
    int i;
    if (!o || !kind)
        return NULL;
    for (i = 0; i < o->n_media; i++)
        if (strcasecmp(o->media[i].media, kind) == 0)
            return &o->media[i];
    return NULL;
}

int sdp_media_pt_for(const sdp_media_t *m, int codec)
{
    if (!m)
        return -1;
    if (codec == SDP_CODEC_H264) return m->h264_pt;
    if (codec == SDP_CODEC_H265) return m->h265_pt;
    if (codec == SDP_CODEC_OPUS) return m->opus_pt;
    return -1;
}

int sdp_offer_summary(const sdp_offer_t *o, char *out, size_t cap)
{
    size_t used = 0;
    int i;

    if (!out || cap == 0)
        return 0;
    out[0] = 0;
    if (!o)
        return 0;

#define APP(...)                                                              \
    do {                                                                      \
        if (used < cap) {                                                     \
            int _n = snprintf(out + used, cap - used, __VA_ARGS__);           \
            if (_n > 0)                                                       \
                used += (size_t)_n;                                           \
            if (used >= cap)                                                  \
                used = cap - 1;                                               \
        }                                                                     \
    } while (0)

    /* Use the accessors, not the raw fields: they apply the media-level
     * fallback and are what callers actually see. */
    APP("session: ufrag=%s pwd_len=%zu fp_alg=%s setup=%d bundle=%d"
        " (attr level: %s)\n",
        sdp_session_ice_ufrag(o), strlen(sdp_session_ice_pwd(o)),
        sdp_session_fingerprint_alg(o)[0] ? sdp_session_fingerprint_alg(o)
                                          : "none",
        sdp_session_setup(o), o->n_bundle,
        o->session_ice_ufrag[0] ? "session" : "media-fallback");

    for (i = 0; i < o->n_media; i++) {
        const sdp_media_t *m = &o->media[i];
        APP("  m[%d] mid=%s kind=%s proto=%s port=%d%s dir=%d rtcp-mux=%d "
            "pts=%d",
            m->index, m->mid, m->media, m->proto, m->port,
            m->rejected ? "(rejected)" : "", m->direction, m->has_rtcp_mux,
            m->n_payloads);
        if (m->h264_pt >= 0)
            APP(" h264=%d(profile-level-id=%s,pkt=%d)", m->h264_pt,
                m->h264_profile_level_id[0] ? m->h264_profile_level_id : "?",
                m->h264_packetization_mode);
        if (m->h265_pt >= 0)
            APP(" h265=%d", m->h265_pt);
        if (m->opus_pt >= 0)
            APP(" opus=%d(%dHz/%dch)", m->opus_pt, m->opus_clock,
                m->opus_channels);
        APP(" extmaps=%d ssrcs=%d", m->n_extmaps, m->n_ssrcs);
        if (m->sctp_port[0])
            APP(" sctp-port=%s", m->sctp_port);
        APP("\n");
    }
#undef APP
    return (int)used;
}

/* ------------------------------------------------------------- answer ---- */

typedef struct {
    char  *buf;
    size_t cap;
    size_t used;
    int    overflow;
} wbuf_t;

static void wb_printf(wbuf_t *w, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (w->overflow || w->used >= w->cap)
        return;
    va_start(ap, fmt);
    n = vsnprintf(w->buf + w->used, w->cap - w->used, fmt, ap);
    va_end(ap);
    if (n < 0) {
        w->overflow = 1;
        return;
    }
    if ((size_t)n >= w->cap - w->used) {
        w->overflow = 1;
        w->used = w->cap - 1;
        return;
    }
    w->used += (size_t)n;
}

/* Random ICE credentials. 24 hex chars of ufrag, 48 of pwd: comfortably above
 * the RFC 5245 minimums (4 and 22) and cheap to generate. */
static void make_ice_cred(char *ufrag, size_t ufragcap,
                          char *pwd, size_t pwdcap, unsigned int seed_extra)
{
    static const char *hex = "0123456789abcdef";
    unsigned char rnd[32];
    unsigned long seed = (unsigned long)time(NULL) ^
                         ((unsigned long)seed_extra << 16) ^
                         (unsigned long)(size_t)ufrag;
    size_t i;

    /* Prefer real randomness; fall back to a seeded LCG only if RAND fails. */
    {
        extern int RAND_bytes(unsigned char *buf, int num);
        if (RAND_bytes(rnd, (int)sizeof(rnd)) != 1) {
            unsigned long x = seed | 1;
            for (i = 0; i < sizeof(rnd); i++) {
                x = x * 6364136223846793005UL + 1442695040888963407UL;
                rnd[i] = (unsigned char)(x >> 33);
            }
        }
    }

    for (i = 0; i + 1 < ufragcap && i < 24; i += 2) {
        ufrag[i]     = hex[rnd[(i / 2) % sizeof(rnd)] >> 4];
        ufrag[i + 1] = hex[rnd[(i / 2) % sizeof(rnd)] & 0xF];
    }
    ufrag[i < ufragcap ? i : ufragcap - 1] = 0;

    for (i = 0; i + 1 < pwdcap && i < 48; i += 2) {
        unsigned char b = rnd[(i / 2 + 8) % sizeof(rnd)];
        pwd[i]     = hex[b >> 4];
        pwd[i + 1] = hex[b & 0xF];
    }
    pwd[i < pwdcap ? i : pwdcap - 1] = 0;
}

static const char *dir_name(int sendrecv)
{
    return sendrecv ? "sendrecv" : "recvonly";
}

/* Decide whether we accept this media section. */
static int media_acceptable(const sdp_media_t *m, const sdp_answer_cfg_t *cfg,
                            int accept_datachannel, int *chosen)
{
    *chosen = 0;
    if (m->rejected || m->port == 0)
        return 0;

    if (m->is_application) {
        if (accept_datachannel) {
            *chosen = 0;
            return 1;
        }
        return 0;
    }
    if (strcasecmp(m->media, "video") == 0) {
        int want = cfg->codecs ? cfg->codecs : SDP_CODEC_ALL;
        if ((want & SDP_CODEC_H264) && m->h264_pt >= 0) {
            *chosen = SDP_CODEC_H264;
            return 1;
        }
        if ((want & SDP_CODEC_H265) && m->h265_pt >= 0) {
            *chosen = SDP_CODEC_H265;
            return 1;
        }
        return 0;
    }
    if (strcasecmp(m->media, "audio") == 0) {
        int want = cfg->codecs ? cfg->codecs : SDP_CODEC_ALL;
        if ((want & SDP_CODEC_OPUS) && m->opus_pt >= 0) {
            *chosen = SDP_CODEC_OPUS;
            return 1;
        }
        return 0;
    }
    return 0;
}

void sdp_answer_cfg_init(sdp_answer_cfg_t *cfg)
{
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->setup = "active";     /* we are the client: we send the ClientHello */
    cfg->codecs = 0;           /* accept every codec we understand          */
    cfg->sendrecv = 0;         /* recvonly: we render, we do not publish    */
    cfg->echo_extmaps = 0;     /* we implement no RTP header extension      */
    cfg->accept_datachannel = 0; /* no SCTP on this device                  */
}

int sdp_build_answer(const sdp_offer_t *offer, const sdp_answer_cfg_t *cfg,
                     char *out, size_t cap, char *err, size_t errcap)
{
    wbuf_t w;
    int i, first_mid_done = 0;
    int accepted[SDP_MAX_MEDIA];
    char ufrag[32], pwd[64];
    unsigned long sid, sver;
    const char *fp;
    const char *setup;

    if (err && errcap)
        err[0] = 0;
    if (!offer || !out || cap < 128) {
        set_err(err, errcap, "bad arguments");
        return -1;
    }
    if (!cfg || !cfg->fingerprint || !cfg->fingerprint[0]) {
        set_err(err, errcap, "cfg->fingerprint is required (dtls_local_fingerprint)");
        return -1;
    }

    fp = cfg->fingerprint;
    /* Accept both "sha-256 AA:BB" and a bare "AA:BB" (algorithm defaulted). */
    if (strncasecmp(fp, "sha-256 ", 8) != 0) {
        static char fixed[200];
        snprintf(fixed, sizeof(fixed), "sha-256 %s", fp);
        fp = fixed;
    }

    setup = cfg->setup ? cfg->setup : "active";
    if (strcasecmp(setup, "active") != 0 && strcasecmp(setup, "passive") != 0 &&
        strcasecmp(setup, "actpass") != 0) {
        set_err(err, errcap, "cfg->setup must be active/passive/actpass");
        return -1;
    }

    sid  = cfg->session_id ? cfg->session_id
                           : (unsigned long)time(NULL) * 1000UL + 7UL;
    sver = cfg->session_version ? cfg->session_version : sid;

    make_ice_cred(ufrag, sizeof(ufrag), pwd, sizeof(pwd), (unsigned int)sid);
    if (cfg->ice_ufrag_out && cfg->ice_ufrag_out_cap)
        copy_str(cfg->ice_ufrag_out, cfg->ice_ufrag_out_cap, ufrag, strlen(ufrag));
    if (cfg->ice_pwd_out && cfg->ice_pwd_out_cap)
        copy_str(cfg->ice_pwd_out, cfg->ice_pwd_out_cap, pwd, strlen(pwd));

    w.buf = out;
    w.cap = cap;
    w.used = 0;
    w.overflow = 0;
    out[0] = 0;

    wb_printf(&w, "v=0\r\n");
    wb_printf(&w, "o=- %lu %lu IN IP4 %s\r\n", sid, sver,
              cfg->local_ip ? cfg->local_ip : "0.0.0.0");
    wb_printf(&w, "s=-\r\n");
    wb_printf(&w, "t=0 0\r\n");

    /* Which m-lines do we accept? Needed twice: once for the BUNDLE group,
     * once for the m-line loop below. */
    for (i = 0; i < offer->n_media && i < SDP_MAX_MEDIA; i++) {
        int chosen;
        accepted[i] = media_acceptable(&offer->media[i], cfg,
                                       cfg->accept_datachannel, &chosen);
    }

    /* BUNDLE: RFC 8843 7.3.3 forbids listing an m-line we rejected (the
     * answer carries it with port 0). Echoing the offer's group verbatim
     * kept a rejected m=application in the group, which strict peers reject. */
    wb_printf(&w, "a=group:BUNDLE");
    if (offer->n_bundle > 0) {
        int b2, m2;
        for (b2 = 0; b2 < offer->n_bundle; b2++) {
            for (m2 = 0; m2 < offer->n_media && m2 < SDP_MAX_MEDIA; m2++) {
                if (strcmp(offer->media[m2].mid, offer->bundle_mids[b2]) != 0)
                    continue;
                if (accepted[m2])
                    wb_printf(&w, " %s", offer->bundle_mids[b2]);
                break;
            }
        }
    } else {
        for (i = 0; i < offer->n_media && i < SDP_MAX_MEDIA; i++)
            if (offer->media[i].mid[0] && accepted[i])
                wb_printf(&w, " %s", offer->media[i].mid);
    }
    wb_printf(&w, "\r\n");

    wb_printf(&w, "a=msid-semantic: WMS\r\n");

    for (i = 0; i < offer->n_media; i++) {
        const sdp_media_t *m = &offer->media[i];
        int chosen = 0;
        int accept = media_acceptable(m, cfg, cfg->accept_datachannel, &chosen);

        if (m->is_application) {
            /* We have no SCTP. Reject explicitly: port 0 + mid, keeps the
             * BUNDLE ordering and tells the peer we are not offering a data
             * channel. RTCDataChannel is replaced by GameControlChannel. */
            wb_printf(&w, "m=application 0 %s %s\r\n",
                      m->proto[0] ? m->proto : "UDP/DTLS/SCTP",
                      m->app_fmt[0] ? m->app_fmt : "webrtc-datachannel");
            if (m->mid[0])
                wb_printf(&w, "a=mid:%s\r\n", m->mid);
            wb_printf(&w, "a=inactive\r\n");
            /* OUR credentials, same as the bundled audio/video m-lines. A
             * rejected section carrying the PEER's ufrag makes a naive peer
             * believe the BUNDLE transport still covers this m-line. */
            wb_printf(&w, "a=ice-ufrag:%s\r\n", ufrag);
            wb_printf(&w, "a=ice-pwd:%s\r\n", pwd);
            wb_printf(&w, "a=fingerprint:%s\r\n", fp);
            if (m->has_rtcp_mux)
                wb_printf(&w, "a=rtcp-mux\r\n");
            (void)accept;
            first_mid_done = 1;
            continue;
        }

        if (!accept) {
            /* Refuse but keep the m-line slot so later mids still line up. */
            wb_printf(&w, "m=%s 0 %s", m->media, m->proto);
            if (m->n_payloads > 0)
                wb_printf(&w, " %d", m->payloads[0]);
            else
                wb_printf(&w, " 0");
            wb_printf(&w, "\r\n");
            if (m->mid[0])
                wb_printf(&w, "a=mid:%s\r\n", m->mid);
            wb_printf(&w, "a=inactive\r\n");
            continue;
        }

        /* Chosen payload type. */
        {
            int pt = -1;
            if (chosen == SDP_CODEC_H264) pt = m->h264_pt;
            else if (chosen == SDP_CODEC_H265) pt = m->h265_pt;
            else if (chosen == SDP_CODEC_OPUS) pt = m->opus_pt;

            wb_printf(&w, "m=%s %d %s %d\r\n", m->media,
                      cfg->candidate_port > 0 ? cfg->candidate_port : 9,
                      m->proto[0] ? m->proto : "UDP/TLS/RTP/SAVPF", pt);

            wb_printf(&w, "c=IN IP4 %s\r\n",
                      cfg->local_ip ? cfg->local_ip : "0.0.0.0");

            if (m->mid[0])
                wb_printf(&w, "a=mid:%s\r\n", m->mid);

            wb_printf(&w, "a=%s\r\n", dir_name(cfg->sendrecv));
            wb_printf(&w, "a=rtcp:9 IN IP4 0.0.0.0\r\n");
            if (m->has_rtcp_mux)
                wb_printf(&w, "a=rtcp-mux\r\n");

            /* OUR credentials, not the offer's: we are the ICE controlling
             * side toward the service's fixed UDP port. */
            wb_printf(&w, "a=ice-ufrag:%s\r\n", ufrag);
            wb_printf(&w, "a=ice-pwd:%s\r\n", pwd);

            wb_printf(&w, "a=fingerprint:%s\r\n", fp);
            wb_printf(&w, "a=setup:%s\r\n", setup);

            /* rtpmap/fmtp for the accepted codec, copied from the offer so the
             * parameter sets match byte for byte. */
            if (chosen == SDP_CODEC_H264) {
                wb_printf(&w, "a=rtpmap:%d H264/90000\r\n", pt);
                if (m->h264_fmtp[0])
                    wb_printf(&w, "a=fmtp:%d %s\r\n", pt, m->h264_fmtp);
            } else if (chosen == SDP_CODEC_H265) {
                wb_printf(&w, "a=rtpmap:%d H265/90000\r\n", pt);
                if (m->h265_fmtp[0])
                    wb_printf(&w, "a=fmtp:%d %s\r\n", pt, m->h265_fmtp);
            } else if (chosen == SDP_CODEC_OPUS) {
                wb_printf(&w, "a=rtpmap:%d opus/%d/%d\r\n", pt,
                          m->opus_clock ? m->opus_clock : 48000,
                          m->opus_channels ? m->opus_channels : 2);
                if (m->opus_fmtp[0])
                    wb_printf(&w, "a=fmtp:%d %s\r\n", pt, m->opus_fmtp);
                else
                    wb_printf(&w, "a=fmtp:%d minptime=10;useinbandfec=1\r\n",
                              pt);
            }

            if (cfg->echo_extmaps) {
                int e;
                for (e = 0; e < m->n_extmaps; e++) {
                    /* Our parsers do not implement any extension, and echoing
                     * one makes the peer emit it; that is safe only because
                     * RTP header extensions are length-delimited. Kept for
                     * fidelity with the JS client, which echoes them. */
                    wb_printf(&w, "a=extmap:%d %s\r\n", m->extmaps[e].id,
                              m->extmaps[e].uri);
                }
            }

            if (m->n_ssrcs > 0)
                wb_printf(&w, "a=ssrc:%u cname:pen\r\n",
                          (unsigned)m->ssrcs[0]);

            /* SBEM: a single candidate line keeps non-trickle peers happy.
             * Prefer our real local endpoint: advertising the server's own
             * address is an ICE lie that makes our checks look spoofed. */
            if (cfg->local_cand_ip && cfg->local_cand_port > 0) {
                wb_printf(&w, "a=candidate:1 1 udp 2130706431 %s %d typ host\r\n",
                          cfg->local_cand_ip, cfg->local_cand_port);
            } else if (cfg->candidate_ip && cfg->candidate_port > 0) {
                wb_printf(&w, "a=candidate:1 1 udp 2130706431 %s %d typ host\r\n",
                          cfg->candidate_ip, cfg->candidate_port);
            }

            if (!first_mid_done && m->n_ssrcs > 0)
                first_mid_done = 1;
        }
    }

    if (w.overflow) {
        set_err(err, errcap, "answer buffer too small (%zu bytes)", cap);
        return -1;
    }
    out[w.used] = 0;
    return (int)w.used;
}

/* ---------------------------------------------------------- candidates --- */

/* Locate the ip field. A candidate line is
 *   candidate:<foundation> <component> <transport> <priority> <ip> <port> typ <type>
 * optionally prefixed by "a=". We accept both and preserve the prefix. */
int sdp_candidate_parse(const char *line, char *ip, size_t ipcap,
                        int *port, char *foundation, size_t fcap,
                        char *type, size_t tcap)
{
    const char *p;
    int field = 0;
    const char *field_start[8] = {0};
    size_t field_len[8] = {0};

    if (!line)
        return -1;
    p = line;
    if (p[0] == 'a' && p[1] == '=')
        p += 2;
    if (!starts_with(p, "candidate:"))
        return -1;
    p += 10;

    /* Tokenise on spaces, but the trailing attributes ("typ host",
     * "generation 0") are free-form; we only need fields 0..5 and the token
     * after "typ". */
    while (*p && field < 8) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        field_start[field] = p;
        while (*p && *p != ' ')
            p++;
        field_len[field] = (size_t)(p - field_start[field]);
        field++;
    }
    if (field < 6)
        return -1;

    if (foundation && fcap) {
        /* field 0 is "<foundation>" already split from "candidate:" */
        copy_str(foundation, fcap, field_start[0], field_len[0]);
    }
    if (ip && ipcap)
        copy_str(ip, ipcap, field_start[4], field_len[4]);
    if (port)
        *port = atoi(field_start[5]);

    if (type && tcap) {
        /* Scan the WHOLE attribute tail for " typ <value> ". Fields 6+ were
         * already consumed by the tokenizer above, so re-scan from field 6's
         * position; falling back to a full scan is harmless and simpler. */
        const char *q = field_start[5] + field_len[5];
        int found = 0;
        while (*q && !found) {
            const char *tok;
            size_t tl;
            while (*q == ' ')
                q++;
            tok = q;
            while (*q && *q != ' ')
                q++;
            tl = (size_t)(q - tok);
            if (tl == 3 && strncmp(tok, "typ", 3) == 0) {
                while (*q == ' ')
                    q++;
                tok = q;
                while (*q && *q != ' ')
                    q++;
                copy_str(type, tcap, tok, (size_t)(q - tok));
                found = 1;
            }
        }
        if (!found)
            type[0] = 0;
    }
    return 0;
}

/* Token spans of a candidate line, for byte-exact rewriting. */
typedef struct {
    const char *start;
    size_t      len;
} tok_t;

/* Split `p` on single spaces into up to max tokens. Returns the token count.
 * Leading spaces are skipped; runs of spaces collapse (the service never emits
 * them, and collapsing keeps the output canonical). */
static int tokenize_spans(const char *p, tok_t *t, int max)
{
    int n = 0;
    while (*p && n < max) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        t[n].start = p;
        while (*p && *p != ' ')
            p++;
        t[n].len = (size_t)(p - t[n].start);
        n++;
    }
    return n;
}

int sdp_candidate_rewrite(const char *line, const char *ip, int port,
                          char *out, size_t cap)
{
    const char *p;
    tok_t t[8];
    int n, i;
    size_t off, need;
    char portbuf[16];
    int pn;

    if (!line || !out || cap == 0 || !ip || !*ip || port <= 0 || port > 65535)
        return -1;

    /* Keep any "a=" prefix: the caller may pass the attribute line verbatim. */
    p = line;
    if (p[0] == 'a' && p[1] == '=')
        p += 2;
    if (!starts_with(p, "candidate:"))
        return -1;
    p += 10;

    /* t[0]=foundation t[1]=component t[2]=transport t[3]=priority
     * t[4]=ip t[5]=port, then "typ" and friends which we copy untouched. */
    n = tokenize_spans(p, t, 8);
    if (n < 6)
        return -1;

    pn = snprintf(portbuf, sizeof(portbuf), "%d", port);
    if (pn <= 0)
        return -1;

    /* prefix = everything before the ip token, counted from the raw line so
     * "a=candidate:" survives byte for byte. */
    off = (size_t)(t[4].start - line);
    need = off + strlen(ip) + 1 + (size_t)pn +
           strlen(t[5].start + t[5].len) + 1;
    if (need > cap)
        return -1;

    memcpy(out, line, off);
    i = (int)off;
    memcpy(out + i, ip, strlen(ip));
    i += (int)strlen(ip);
    out[i++] = ' ';
    memcpy(out + i, portbuf, (size_t)pn);
    i += pn;
    /* Everything after the original port token, including the separator. */
    {
        const char *rest = t[5].start + t[5].len;
        size_t rl = strlen(rest);
        memcpy(out + i, rest, rl);
        i += (int)rl;
    }
    out[i] = 0;
    return 0;
}
