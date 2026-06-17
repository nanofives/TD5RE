/**
 * td5_upnp.h -- Minimal UPnP IGD (Internet Gateway Device) port-mapping client.
 *
 * Self-contained: uses only Winsock2 (already linked as -lws2_32). No external
 * library. Implements just the three SOAP actions the source port needs so a
 * Direct-IP host can open its UDP game port on the router automatically:
 *
 *   1. SSDP M-SEARCH (UDP multicast 239.255.255.250:1900) -> find the IGD and
 *      its device-description URL.
 *   2. HTTP GET the device description XML -> locate the WANIPConnection /
 *      WANPPPConnection service control URL + serviceType.
 *   3. SOAP POST AddPortMapping / GetSpecificPortMappingEntry /
 *      GetExternalIPAddress / DeletePortMapping to that control URL.
 *
 * Everything runs on the calling (host-setup) thread with short timeouts and is
 * completely outside the lockstep/determinism path. Every entry point is a
 * best-effort no-op-on-failure: callers treat a 0 return as "UPnP unavailable"
 * and fall back to a manual-port-forward message.
 */

#ifndef TD5_UPNP_H
#define TD5_UPNP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Add (or refresh) a port mapping on the IGD: external <port> -> this machine's
 * LAN IP <port>. Performs lazy SSDP discovery on first call (result cached).
 *
 * @param port        external == internal port to map.
 * @param udp         1 = UDP, 0 = TCP.
 * @param desc        human-readable mapping description (e.g. "TD5RE").
 * @param lease_secs  lease duration in seconds; 0 = permanent (until deleted).
 * @return 1 on success (router accepted the mapping), 0 on any failure.
 */
int td5_upnp_map_port(uint16_t port, int udp, const char *desc, int lease_secs);

/**
 * Verify a mapping exists on the router via GetSpecificPortMappingEntry.
 * @return 1 if the router reports an active mapping for <port>, 0 otherwise.
 */
int td5_upnp_verify_port(uint16_t port, int udp);

/**
 * Whether SSDP discovery found a UPnP IGD (router) at all. Valid after a
 * td5_upnp_map_port() / discovery attempt. Lets callers tell "no UPnP router"
 * apart from "router found but the mapping was refused".
 * @return 1 if an IGD is known/cached, 0 otherwise.
 */
int td5_upnp_found_igd(void);

/**
 * The UPnP SOAP fault code from the last td5_upnp_map_port() AddPortMapping
 * attempt (0 = success / not attempted). Notable values: 718 the external port
 * is already mapped (often a static forward in the router UI), 714 no such
 * entry, 725/402/501 lease-policy / argument faults.
 */
int td5_upnp_last_map_fault(void);

/**
 * Query the IGD's WAN-side (public) IP via GetExternalIPAddress.
 * @param buf  receives a null-terminated dotted-quad string.
 * @param len  size of buf.
 * @return 1 on success, 0 on failure.
 */
int td5_upnp_get_external_ip(char *buf, int len);

/**
 * Delete a previously added mapping (called on host shutdown). Best-effort.
 */
void td5_upnp_unmap_port(uint16_t port, int udp);

/**
 * Copy the LAN IP this machine uses to reach the IGD (filled during discovery).
 * Falls back to the primary host address if discovery has not run.
 * @return 1 on success, 0 on failure.
 */
int td5_upnp_get_local_ip(char *buf, int len);

/**
 * Forget the cached IGD (control URL / service type / local IP) so the next
 * map/verify call re-runs SSDP discovery. Cheap; safe to call anytime.
 */
void td5_upnp_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* TD5_UPNP_H */
