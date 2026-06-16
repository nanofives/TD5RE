/**
 * td5_upnp.c -- Minimal UPnP IGD port-mapping client (see td5_upnp.h).
 *
 * Implements SSDP discovery + a tiny HTTP/SOAP client over Winsock2 to call
 * AddPortMapping / GetSpecificPortMappingEntry / GetExternalIPAddress /
 * DeletePortMapping on the local router. No third-party library; no XML parser
 * (the device description is scanned with plain substring matching, which is
 * sufficient for the few well-known tags we need).
 *
 * Design notes:
 *   - Every network op has a short timeout (SSDP 2s, TCP connect 3s, recv 3s)
 *     so a missing/uncooperative router can never stall the host-setup path.
 *   - WSAStartup is reference-counted by Winsock, so pairing our own
 *     startup/cleanup per public call is safe even while td5_net.c holds its
 *     own Winsock reference.
 *   - All failures are soft: callers fall back to manual port forwarding.
 */

#include "td5_upnp.h"

/* Winsock headers MUST precede any windows.h (pulled in by td5_platform.h). */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "td5_platform.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define UPNP_LOG "upnp"

#define SSDP_MCAST_ADDR     "239.255.255.250"
#define SSDP_MCAST_PORT     1900
#define SSDP_RECV_MS        2000      /* total SSDP listen window */
#define TCP_CONNECT_MS      3000
#define TCP_RECV_MS         3000

#define DEVXML_MAX          16384
#define HTTP_RESP_MAX       8192

/* ----- cached IGD state (populated by upnp_discover) ----- */
static int  s_have_igd;
static char s_control_url[512];   /* absolute control URL for the WAN service */
static char s_service_type[160];  /* WANIPConnection:1 or WANPPPConnection:1  */
static char s_local_ip[64];       /* our LAN IP on the route to the IGD       */
static char s_external_ip[64];

/* ========================================================================
 * Winsock lifetime (ref-counted; safe alongside td5_net.c's own startup)
 * ======================================================================== */

static int upnp_wsa_begin(void)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        TD5_LOG_E(UPNP_LOG, "WSAStartup failed: %d", (int)WSAGetLastError());
        return 0;
    }
    return 1;
}

static void upnp_wsa_end(void)
{
    WSACleanup();
}

/* ========================================================================
 * Small string helpers
 * ======================================================================== */

/** Case-insensitive substring search. */
static const char *upnp_stristr(const char *hay, const char *needle)
{
    size_t nlen;
    if (!hay || !needle) return NULL;
    nlen = strlen(needle);
    if (nlen == 0) return hay;
    for (; *hay; hay++) {
        if (_strnicmp(hay, needle, nlen) == 0)
            return hay;
    }
    return NULL;
}

/**
 * Extract the text between <tag> and </tag> (first occurrence) into out.
 * Returns 1 on success. Tag name is matched case-insensitively, ignoring any
 * XML namespace prefix or attributes on the opening tag.
 */
static int upnp_xml_value(const char *xml, const char *tag, char *out, int out_len)
{
    char open[64], close[64];
    const char *p, *start, *end;
    int n;

    if (!xml || !tag || !out || out_len <= 0) return 0;
    snprintf(open, sizeof(open), "<%s", tag);
    snprintf(close, sizeof(close), "</%s", tag);

    p = upnp_stristr(xml, open);
    if (!p) return 0;
    start = strchr(p, '>');
    if (!start) return 0;
    start++;                          /* first char after the opening '>' */
    end = upnp_stristr(start, close);
    if (!end) return 0;

    n = (int)(end - start);
    if (n < 0) return 0;
    if (n >= out_len) n = out_len - 1;
    memcpy(out, start, (size_t)n);
    out[n] = '\0';
    return 1;
}

/** Parse "http://host[:port][/path]" into components. Defaults: port 80, "/". */
static int upnp_parse_url(const char *url, char *host, int host_len,
                          int *port, char *path, int path_len)
{
    const char *p, *slash, *colon;
    int n;

    if (!url || !host || !port || !path) return 0;
    if (_strnicmp(url, "http://", 7) != 0) return 0;
    p = url + 7;

    slash = strchr(p, '/');
    colon = strchr(p, ':');
    /* a ':' only counts as a port separator if it precedes the path slash */
    if (colon && (!slash || colon < slash)) {
        n = (int)(colon - p);
        if (n >= host_len) n = host_len - 1;
        memcpy(host, p, (size_t)n);
        host[n] = '\0';
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535) *port = 80;
    } else {
        const char *hend = slash ? slash : (p + strlen(p));
        n = (int)(hend - p);
        if (n >= host_len) n = host_len - 1;
        memcpy(host, p, (size_t)n);
        host[n] = '\0';
        *port = 80;
    }

    if (slash) {
        snprintf(path, (size_t)path_len, "%s", slash);
    } else {
        snprintf(path, (size_t)path_len, "/");
    }
    return 1;
}

/* ========================================================================
 * TCP helpers
 * ======================================================================== */

/** Connect to host:port (numeric IP or name) with a timeout. Returns socket or INVALID_SOCKET. */
static SOCKET upnp_tcp_connect(const char *host, int port)
{
    SOCKET s;
    SOCKADDR_IN addr;
    u_long nonblock = 1, block = 0;
    fd_set wfds;
    struct timeval tv;
    int sel;
    DWORD rcv_to = TCP_RECV_MS;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        /* not dotted-quad -- resolve by name */
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
            return INVALID_SOCKET;
        addr.sin_addr = ((SOCKADDR_IN *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    ioctlsocket(s, FIONBIO, &nonblock);
    connect(s, (const struct sockaddr *)&addr, sizeof(addr));

    FD_ZERO(&wfds);
    FD_SET(s, &wfds);
    tv.tv_sec = TCP_CONNECT_MS / 1000;
    tv.tv_usec = (TCP_CONNECT_MS % 1000) * 1000;
    sel = select(0, NULL, &wfds, NULL, &tv);
    if (sel <= 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }

    ioctlsocket(s, FIONBIO, &block);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rcv_to, sizeof(rcv_to));
    return s;
}

/**
 * One HTTP/1.1 exchange with Connection: close. Sends <method> <path> plus the
 * supplied extra headers and optional body, reads the full response into resp.
 * Returns the HTTP status code (e.g. 200), or 0 on transport failure.
 */
static int upnp_http_exchange(const char *host, int port, const char *path,
                              const char *method, const char *extra_headers,
                              const char *body, char *resp, int resp_len)
{
    SOCKET s;
    char req[2048];
    int blen, hlen, total, n, status = 0;
    const char *sp;

    s = upnp_tcp_connect(host, port);
    if (s == INVALID_SOCKET) return 0;

    blen = body ? (int)strlen(body) : 0;
    hlen = snprintf(req, sizeof(req),
                    "%s %s HTTP/1.1\r\n"
                    "HOST: %s:%d\r\n"
                    "CONNECTION: close\r\n"
                    "CONTENT-LENGTH: %d\r\n"
                    "%s"
                    "\r\n",
                    method, path, host, port, blen,
                    extra_headers ? extra_headers : "");
    if (hlen <= 0 || hlen >= (int)sizeof(req)) { closesocket(s); return 0; }

    if (send(s, req, hlen, 0) == SOCKET_ERROR) { closesocket(s); return 0; }
    if (blen > 0 && send(s, body, blen, 0) == SOCKET_ERROR) { closesocket(s); return 0; }

    total = 0;
    while (total < resp_len - 1) {
        n = recv(s, resp + total, resp_len - 1 - total, 0);
        if (n <= 0) break;            /* close, timeout, or error -> done */
        total += n;
    }
    resp[total] = '\0';
    closesocket(s);

    /* parse "HTTP/1.x NNN" */
    sp = strchr(resp, ' ');
    if (sp) status = atoi(sp + 1);
    return status;
}

/* ========================================================================
 * Local-IP discovery (the route to the IGD)
 * ======================================================================== */

/** Determine the LAN IP this machine would use to reach <gw_host>. */
static int upnp_local_ip_for(const char *gw_host, char *out, int out_len)
{
    SOCKET s;
    SOCKADDR_IN gw, local;
    int local_len = (int)sizeof(local);
    const char *str;

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;

    memset(&gw, 0, sizeof(gw));
    gw.sin_family = AF_INET;
    gw.sin_port = htons(SSDP_MCAST_PORT);
    gw.sin_addr.s_addr = inet_addr(gw_host);
    if (gw.sin_addr.s_addr == INADDR_NONE) { closesocket(s); return 0; }

    /* UDP connect sends nothing but selects the outgoing interface/route. */
    if (connect(s, (const struct sockaddr *)&gw, sizeof(gw)) != 0) {
        closesocket(s);
        return 0;
    }
    if (getsockname(s, (struct sockaddr *)&local, &local_len) != 0) {
        closesocket(s);
        return 0;
    }
    closesocket(s);

    str = inet_ntoa(local.sin_addr);
    if (!str) return 0;
    snprintf(out, (size_t)out_len, "%s", str);
    return 1;
}

/* ========================================================================
 * SSDP discovery -> device XML -> WAN service control URL
 * ======================================================================== */

static const char *k_ssdp_search_targets[] = {
    "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
    "urn:schemas-upnp-org:service:WANIPConnection:1",
    "urn:schemas-upnp-org:service:WANPPPConnection:1",
    "upnp:rootdevice",
};

/** Send M-SEARCH datagrams and collect the first usable LOCATION URL. */
static int upnp_ssdp_find_location(char *loc_url, int loc_len, char *gw_ip, int gw_len)
{
    SOCKET s;
    SOCKADDR_IN mcast;
    DWORD recv_to = SSDP_RECV_MS;
    u_long ttl = 2;
    int i, got = 0;
    DWORD start;

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;

    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&recv_to, sizeof(recv_to));

    memset(&mcast, 0, sizeof(mcast));
    mcast.sin_family = AF_INET;
    mcast.sin_port = htons(SSDP_MCAST_PORT);
    mcast.sin_addr.s_addr = inet_addr(SSDP_MCAST_ADDR);

    for (i = 0; i < (int)(sizeof(k_ssdp_search_targets) / sizeof(k_ssdp_search_targets[0])); i++) {
        char msearch[512];
        int n = snprintf(msearch, sizeof(msearch),
                         "M-SEARCH * HTTP/1.1\r\n"
                         "HOST: %s:%d\r\n"
                         "MAN: \"ssdp:discover\"\r\n"
                         "MX: 2\r\n"
                         "ST: %s\r\n"
                         "\r\n",
                         SSDP_MCAST_ADDR, SSDP_MCAST_PORT, k_ssdp_search_targets[i]);
        sendto(s, msearch, n, 0, (const struct sockaddr *)&mcast, (int)sizeof(mcast));
    }

    start = td5_plat_time_ms();
    while ((td5_plat_time_ms() - start) < SSDP_RECV_MS) {
        char buf[2048];
        SOCKADDR_IN from;
        int from_len = (int)sizeof(from);
        int r = recvfrom(s, buf, (int)sizeof(buf) - 1, 0,
                         (struct sockaddr *)&from, &from_len);
        const char *loc, *eol;
        int ln;
        if (r <= 0) break;            /* timeout */
        buf[r] = '\0';

        loc = upnp_stristr(buf, "LOCATION:");
        if (!loc) continue;
        loc += 9;
        while (*loc == ' ' || *loc == '\t') loc++;
        eol = strpbrk(loc, "\r\n");
        ln = eol ? (int)(eol - loc) : (int)strlen(loc);
        if (ln <= 0 || ln >= loc_len) continue;
        memcpy(loc_url, loc, (size_t)ln);
        loc_url[ln] = '\0';

        /* [SEC 2026-06-15] If LOCATION carries an IP literal, it must be the
         * SSDP responder's own address. Otherwise any host answering the
         * M-SEARCH could redirect our device-description GET (and the follow-up
         * SOAP) to an arbitrary address -- an SSDP-spoof -> SSRF lever.
         * Hostnames are left alone (legitimate but rare; not trivially
         * redirectable without DNS control). Off-path source spoofing remains
         * possible, but this blocks the trivial LAN case. */
        {
            char loc_host[128], loc_path[256];
            int  loc_port = 80;
            if (upnp_parse_url(loc_url, loc_host, sizeof(loc_host),
                               &loc_port, loc_path, sizeof(loc_path))) {
                unsigned long loc_ip = inet_addr(loc_host);
                if (loc_ip != INADDR_NONE && loc_ip != from.sin_addr.s_addr) {
                    TD5_LOG_W(UPNP_LOG,
                              "SSDP: LOCATION host %s != responder %s -- ignoring",
                              loc_host, inet_ntoa(from.sin_addr));
                    continue;
                }
            }
        }

        /* remember the responder's IP as the gateway host */
        snprintf(gw_ip, (size_t)gw_len, "%s", inet_ntoa(from.sin_addr));
        got = 1;
        break;
    }

    closesocket(s);
    return got;
}

/**
 * From device XML, find the <service> block whose serviceType is WANIP/PPP
 * Connection and extract its controlURL + serviceType.
 */
static int upnp_find_wan_service(const char *xml, const char *loc_url,
                                 char *ctrl_out, int ctrl_len,
                                 char *svc_out, int svc_len)
{
    const char *p = xml;
    char urlbase[256];
    int have_urlbase;

    have_urlbase = upnp_xml_value(xml, "URLBase", urlbase, sizeof(urlbase));

    while ((p = upnp_stristr(p, "<service")) != NULL) {
        const char *svc_end = upnp_stristr(p, "</service>");
        char stype[160], curl[256];
        if (!svc_end) break;

        /* limit value extraction to this <service> block by temporarily
           scanning a bounded copy */
        {
            int blen = (int)(svc_end - p);
            char block[1024];
            if (blen > (int)sizeof(block) - 1) blen = (int)sizeof(block) - 1;
            memcpy(block, p, (size_t)blen);
            block[blen] = '\0';

            if (upnp_xml_value(block, "serviceType", stype, sizeof(stype)) &&
                (upnp_stristr(stype, "WANIPConnection") ||
                 upnp_stristr(stype, "WANPPPConnection")) &&
                upnp_xml_value(block, "controlURL", curl, sizeof(curl)))
            {
                snprintf(svc_out, (size_t)svc_len, "%s", stype);

                if (_strnicmp(curl, "http://", 7) == 0) {
                    snprintf(ctrl_out, (size_t)ctrl_len, "%s", curl);
                } else if (have_urlbase && urlbase[0]) {
                    /* join URLBase + controlURL, collapsing the slash seam so
                       there is exactly one '/' between them */
                    size_t ub = strlen(urlbase);
                    int base_slash = (urlbase[ub - 1] == '/');
                    int ctrl_slash = (curl[0] == '/');
                    if (base_slash && ctrl_slash)
                        snprintf(ctrl_out, (size_t)ctrl_len, "%s%s", urlbase, curl + 1);
                    else if (!base_slash && !ctrl_slash)
                        snprintf(ctrl_out, (size_t)ctrl_len, "%s/%s", urlbase, curl);
                    else
                        snprintf(ctrl_out, (size_t)ctrl_len, "%s%s", urlbase, curl);
                } else {
                    /* derive scheme://host:port from the LOCATION url */
                    char host[128], path[256];
                    int port = 80;
                    if (!upnp_parse_url(loc_url, host, sizeof(host), &port, path, sizeof(path)))
                        return 0;
                    if (curl[0] == '/')
                        snprintf(ctrl_out, (size_t)ctrl_len, "http://%s:%d%s", host, port, curl);
                    else
                        snprintf(ctrl_out, (size_t)ctrl_len, "http://%s:%d/%s", host, port, curl);
                }
                return 1;
            }
        }
        p = svc_end + 1;
    }
    return 0;
}

/** Run full discovery once and cache the WAN control URL + service type. */
static int upnp_discover(void)
{
    char loc_url[512], gw_ip[64], host[128], path[256];
    char *xml;
    int port = 80, status;

    if (s_have_igd) return 1;

    if (!upnp_ssdp_find_location(loc_url, sizeof(loc_url), gw_ip, sizeof(gw_ip))) {
        TD5_LOG_W(UPNP_LOG, "SSDP: no IGD responded (no router or UPnP disabled)");
        return 0;
    }
    TD5_LOG_I(UPNP_LOG, "SSDP: IGD at %s (desc %s)", gw_ip, loc_url);

    if (!upnp_parse_url(loc_url, host, sizeof(host), &port, path, sizeof(path))) {
        TD5_LOG_W(UPNP_LOG, "could not parse LOCATION url");
        return 0;
    }

    xml = (char *)malloc(DEVXML_MAX);
    if (!xml) return 0;

    status = upnp_http_exchange(host, port, path, "GET", NULL, NULL, xml, DEVXML_MAX);
    if (status != 200) {
        TD5_LOG_W(UPNP_LOG, "device-desc GET %s returned %d", path, status);
        free(xml);
        return 0;
    }

    if (!upnp_find_wan_service(xml, loc_url, s_control_url, sizeof(s_control_url),
                               s_service_type, sizeof(s_service_type))) {
        TD5_LOG_W(UPNP_LOG, "no WANIPConnection/WANPPPConnection service in device desc");
        free(xml);
        return 0;
    }
    free(xml);

    /* our LAN IP on the route to the gateway (the InternalClient value) */
    if (!upnp_local_ip_for(gw_ip, s_local_ip, sizeof(s_local_ip)) &&
        !upnp_local_ip_for(host, s_local_ip, sizeof(s_local_ip))) {
        TD5_LOG_W(UPNP_LOG, "could not determine local IP toward IGD");
        return 0;
    }

    s_have_igd = 1;
    TD5_LOG_I(UPNP_LOG, "IGD ready: svc=%s ctrl=%s localIP=%s",
              s_service_type, s_control_url, s_local_ip);
    return 1;
}

/* ========================================================================
 * SOAP action helper
 * ======================================================================== */

/**
 * POST a SOAP action body to the cached control URL.
 * @return HTTP status code, or 0 on failure. Response copied to resp.
 */
static int upnp_soap(const char *action, const char *args_xml, char *resp, int resp_len)
{
    char host[128], path[256], headers[320], body[1024];
    int port = 80, blen;

    if (!s_have_igd) return 0;
    if (!upnp_parse_url(s_control_url, host, sizeof(host), &port, path, sizeof(path)))
        return 0;

    blen = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>",
        action, s_service_type, args_xml ? args_xml : "", action);
    if (blen <= 0 || blen >= (int)sizeof(body)) return 0;

    snprintf(headers, sizeof(headers),
             "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
             "SOAPACTION: \"%s#%s\"\r\n",
             s_service_type, action);

    return upnp_http_exchange(host, port, path, "POST", headers, body, resp, resp_len);
}

/* ========================================================================
 * Public API
 * ======================================================================== */

int td5_upnp_map_port(uint16_t port, int udp, const char *desc, int lease_secs)
{
    char args[512], resp[HTTP_RESP_MAX];
    int status;

    if (!upnp_wsa_begin()) return 0;

    if (!upnp_discover()) { upnp_wsa_end(); return 0; }

    snprintf(args, sizeof(args),
        "<NewRemoteHost></NewRemoteHost>"
        "<NewExternalPort>%u</NewExternalPort>"
        "<NewProtocol>%s</NewProtocol>"
        "<NewInternalPort>%u</NewInternalPort>"
        "<NewInternalClient>%s</NewInternalClient>"
        "<NewEnabled>1</NewEnabled>"
        "<NewPortMappingDescription>%s</NewPortMappingDescription>"
        "<NewLeaseDuration>%d</NewLeaseDuration>",
        (unsigned)port, udp ? "UDP" : "TCP", (unsigned)port,
        s_local_ip, desc ? desc : "TD5RE", lease_secs);

    status = upnp_soap("AddPortMapping", args, resp, sizeof(resp));
    upnp_wsa_end();

    if (status == 200) {
        TD5_LOG_I(UPNP_LOG, "AddPortMapping %s/%u -> %s:%u OK",
                  udp ? "UDP" : "TCP", (unsigned)port, s_local_ip, (unsigned)port);
        return 1;
    }
    TD5_LOG_W(UPNP_LOG, "AddPortMapping %u failed (HTTP %d)", (unsigned)port, status);
    return 0;
}

int td5_upnp_verify_port(uint16_t port, int udp)
{
    char args[256], resp[HTTP_RESP_MAX];
    char client[64];
    int status;

    if (!upnp_wsa_begin()) return 0;
    if (!upnp_discover()) { upnp_wsa_end(); return 0; }

    snprintf(args, sizeof(args),
        "<NewRemoteHost></NewRemoteHost>"
        "<NewExternalPort>%u</NewExternalPort>"
        "<NewProtocol>%s</NewProtocol>",
        (unsigned)port, udp ? "UDP" : "TCP");

    status = upnp_soap("GetSpecificPortMappingEntry", args, resp, sizeof(resp));
    upnp_wsa_end();

    if (status == 200 && upnp_xml_value(resp, "NewInternalClient", client, sizeof(client))) {
        TD5_LOG_I(UPNP_LOG, "verify: router maps %s/%u -> %s", udp ? "UDP" : "TCP",
                  (unsigned)port, client);
        return 1;
    }
    TD5_LOG_W(UPNP_LOG, "verify: no active mapping for %u (HTTP %d)", (unsigned)port, status);
    return 0;
}

int td5_upnp_get_external_ip(char *buf, int len)
{
    char resp[HTTP_RESP_MAX];
    int status;

    if (!buf || len <= 0) return 0;

    if (s_external_ip[0]) {           /* cached from a prior call */
        snprintf(buf, (size_t)len, "%s", s_external_ip);
        return 1;
    }

    if (!upnp_wsa_begin()) return 0;
    if (!upnp_discover()) { upnp_wsa_end(); return 0; }

    status = upnp_soap("GetExternalIPAddress", NULL, resp, sizeof(resp));
    upnp_wsa_end();

    if (status == 200 &&
        upnp_xml_value(resp, "NewExternalIPAddress", s_external_ip, sizeof(s_external_ip)) &&
        s_external_ip[0]) {
        snprintf(buf, (size_t)len, "%s", s_external_ip);
        TD5_LOG_I(UPNP_LOG, "external IP: %s", s_external_ip);
        return 1;
    }
    TD5_LOG_W(UPNP_LOG, "GetExternalIPAddress failed (HTTP %d)", status);
    return 0;
}

void td5_upnp_unmap_port(uint16_t port, int udp)
{
    char args[256], resp[HTTP_RESP_MAX];
    int status;

    if (!upnp_wsa_begin()) return;
    if (!s_have_igd && !upnp_discover()) { upnp_wsa_end(); return; }

    snprintf(args, sizeof(args),
        "<NewRemoteHost></NewRemoteHost>"
        "<NewExternalPort>%u</NewExternalPort>"
        "<NewProtocol>%s</NewProtocol>",
        (unsigned)port, udp ? "UDP" : "TCP");

    status = upnp_soap("DeletePortMapping", args, resp, sizeof(resp));
    upnp_wsa_end();

    if (status == 200)
        TD5_LOG_I(UPNP_LOG, "DeletePortMapping %u OK", (unsigned)port);
    else
        TD5_LOG_W(UPNP_LOG, "DeletePortMapping %u failed (HTTP %d)", (unsigned)port, status);
}

int td5_upnp_get_local_ip(char *buf, int len)
{
    if (!buf || len <= 0) return 0;
    if (s_local_ip[0]) {
        snprintf(buf, (size_t)len, "%s", s_local_ip);
        return 1;
    }
    /* discovery has not run -- fall back to the primary host address */
    if (upnp_wsa_begin()) {
        char hostname[128];
        struct addrinfo hints, *res = NULL;
        int ok = 0;
        if (gethostname(hostname, sizeof(hostname)) == 0) {
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_INET;
            if (getaddrinfo(hostname, NULL, &hints, &res) == 0 && res) {
                const char *str = inet_ntoa(((SOCKADDR_IN *)res->ai_addr)->sin_addr);
                if (str) { snprintf(buf, (size_t)len, "%s", str); ok = 1; }
                freeaddrinfo(res);
            }
        }
        upnp_wsa_end();
        if (ok) return 1;
    }
    return 0;
}

void td5_upnp_reset(void)
{
    s_have_igd = 0;
    s_control_url[0] = '\0';
    s_service_type[0] = '\0';
    s_local_ip[0] = '\0';
    s_external_ip[0] = '\0';
}
