/* udp_probe.c -- see udp_probe.h.
 *
 * Deliberately tiny: no STUN, no ICE, no parsing. The cloud-gaming service
 * rewrites every candidate to a fixed ip:port, so all we ever need to know is
 * whether a datagram survives the round trip -- and, ideally, whether the far
 * end is closed (fast ICMP answer) or merely filtered (silence).
 *
 * The connect(2) below is the whole trick: an UNCONNECTED UDP socket never
 * learns about ICMP port-unreachable, so a closed port looks exactly like a
 * firewall drop and you must burn the entire timeout to find out. Connecting
 * makes the kernel queue the error on the socket and poll() then reports
 * POLLERR, which turns "closed" into a sub-millisecond answer.
 */

#include "udp_probe.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

const char *udp_probe_result_name(int result)
{
    switch (result) {
    case UDP_PROBE_OPEN:    return "open";
    case UDP_PROBE_TIMEOUT: return "timeout";
    case UDP_PROBE_REFUSED: return "refused";
    case UDP_PROBE_ERROR:   return "error";
    default:                return "?";
    }
}

/* IPv4 literal only, which is all the service ever hands us. Using
 * getaddrinfo with AF_INET matters: AF_UNSPEC makes the resolver try IPv6
 * first on this firmware, which is exactly the 15-second trap we hit in
 * wss.c. A hostname still works through getaddrinfo. */
static int resolve(const char *ip, int port, struct sockaddr_in *addr)
{
    struct addrinfo hints, *res = NULL;
    char portstr[16];

    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &addr->sin_addr) == 1)
        return 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;          /* mandatory, see above */
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(ip, portstr, &hints, &res) != 0 || !res)
        return -1;
    memcpy(addr, res->ai_addr, sizeof(*addr));
    freeaddrinfo(res);
    return 0;
}

int udp_probe(const char *ip, int port, int timeout_ms, int fd,
              udp_probe_result_t *out)
{
    struct sockaddr_in peer;
    struct sockaddr_storage from;
    socklen_t fromlen;
    unsigned char byte = 0x2A;
    char rx[512];
    int created = 0, rc, orig_flags = 0, connected = 0;
    long long t0, t1;

    if (!out || !ip || !*ip || port <= 0 || port > 65535 || timeout_ms < 0)
        return -1;

    memset(out, 0, sizeof(*out));
    out->result = UDP_PROBE_ERROR;

    if (resolve(ip, port, &peer) != 0)
        return -1;
    out->from_port = 0;

    if (fd < 0) {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
            return -1;
        created = 1;
    } else {
        orig_flags = fcntl(fd, F_GETFL, 0);
    }

    /* Non-blocking always: the caller is a single-threaded event loop. */
    if (orig_flags >= 0)
        fcntl(fd, F_SETFL, orig_flags | O_NONBLOCK);

    t0 = now_ms();

    /* connect() so ICMP unreachable becomes visible; fall back to sendto()
     * when the caller already connected the socket to something else and the
     * kernel refuses to retarget it. */
    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) == 0) {
        connected = 1;
        rc = (int)send(fd, &byte, 1, 0);
    } else {
        rc = (int)sendto(fd, &byte, 1, 0, (struct sockaddr *)&peer,
                         sizeof(peer));
    }
    if (rc != 1) {
        if (errno == ECONNREFUSED) {
            out->result = UDP_PROBE_REFUSED;
            out->elapsed_ms = (int)(now_ms() - t0);
        } else if (errno == ENETUNREACH || errno == EHOSTUNREACH) {
            out->result = UDP_PROBE_ERROR;
            out->elapsed_ms = (int)(now_ms() - t0);
        }
        goto done;
    }

    for (;;) {
        struct pollfd p;
        long long remain;
        int pr;

        remain = (long long)timeout_ms - (now_ms() - t0);
        if (remain <= 0)
            break;

        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        pr = poll(&p, 1, (int)remain);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            goto done;
        }
        if (pr == 0)
            break;

        if (p.revents & (POLLERR | POLLHUP)) {
            /* A connected UDP socket reports ICMP unreachable this way. */
            int soerr = 0;
            socklen_t elen = sizeof(soerr);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &elen) == 0 &&
                soerr == ECONNREFUSED) {
                out->result = UDP_PROBE_REFUSED;
                out->elapsed_ms = (int)(now_ms() - t0);
                goto done;
            }
            if (p.revents & POLLHUP)
                goto done;
            /* Other error codes: read it out below and let recvfrom decide. */
        }
        if (!(p.revents & POLLIN))
            continue;

        fromlen = sizeof(from);
        memset(&from, 0, sizeof(from));
        rc = (int)recvfrom(fd, rx, sizeof(rx), 0, (struct sockaddr *)&from,
                           &fromlen);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            if (errno == ECONNREFUSED) {
                out->result = UDP_PROBE_REFUSED;
                out->elapsed_ms = (int)(now_ms() - t0);
            }
            goto done;
        }

        out->result = UDP_PROBE_OPEN;
        out->rx_bytes = rc;
        out->elapsed_ms = (int)(now_ms() - t0);
        if (from.ss_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&from;
            inet_ntop(AF_INET, &sin->sin_addr, out->from_ip,
                      sizeof(out->from_ip));
            out->from_port = ntohs(sin->sin_port);
        } else if (connected) {
            /* A connected socket can report a zeroed source; the peer is known. */
            snprintf(out->from_ip, sizeof(out->from_ip), "%s", ip);
            out->from_port = port;
        }
        goto done;
    }

    /* Silence: either filtered, or the peer does not answer bare datagrams
     * (a DTLS listener will not). Both mean "we cannot prove reachability",
     * which is a TIMEOUT and not an error. */
    out->result = UDP_PROBE_TIMEOUT;
    out->elapsed_ms = (int)(now_ms() - t0);

done:
    t1 = now_ms();
    if (out->elapsed_ms == 0)
        out->elapsed_ms = (int)(t1 - t0);
    if (!created && orig_flags >= 0)
        fcntl(fd, F_SETFL, orig_flags);
    if (created)
        close(fd);
    return 0;
}
