/* udp_probe.h -- "can I send a UDP datagram to this ip:port and get anything
 * back?" == the entire UDP reachability story for this product.
 *
 * The cloud-gaming service has no STUN and no TURN: the game server rewrites
 * the ICE candidate to a fixed, known ip:port that it hands us out of band, so
 * ICE degenerates into a direct link. There is therefore nothing for a STUN
 * parser to do -- we only need to prove the path is open before spending a
 * DTLS handshake on it.
 *
 * Design notes:
 *   - non-blocking socket + poll(), so the caller's single worker thread is
 *     never stuck;
 *   - never allocates after the call returns;
 *   - returns timing so callers can distinguish "closed" (ICMP refused, ~0 ms)
 *     from "filtered" (silence, full timeout).
 */
#ifndef WEBRTC_UDP_PROBE_H
#define WEBRTC_UDP_PROBE_H

#include <stddef.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UDP_PROBE_OPEN       0   /* something came back                       */
#define UDP_PROBE_TIMEOUT    1   /* silence: filtered, or peer does not answer */
#define UDP_PROBE_REFUSED    2   /* ICMP port unreachable: nothing listens     */
#define UDP_PROBE_ERROR     -1   /* socket/DNS failure                         */

typedef struct {
    int  result;      /* UDP_PROBE_*                                     */
    int  elapsed_ms;  /* wall time spent                                 */
    int  rx_bytes;    /* size of the reply, 0 when none arrived          */
    /* The reply's source address, as a printable "ip:port". A DTLS server
     * that answers from a different port than we probed is normal. */
    char from_ip[64];
    int  from_port;
} udp_probe_result_t;

/* Send a 1-byte probe to ip:port and wait up to timeout_ms for ANY datagram
 * back. `fd` may be -1 to create a throwaway socket (closed before return), or
 * an existing connected/unconnected socket the caller wants to reuse -- in
 * that case the socket is left open and its blocking mode is restored.
 * Returns 0 and fills `out` (which always gets a result), or -1 on bad args. */
int udp_probe(const char *ip, int port, int timeout_ms, int fd,
              udp_probe_result_t *out);

const char *udp_probe_result_name(int result);

#ifdef __cplusplus
}
#endif

#endif /* WEBRTC_UDP_PROBE_H */
