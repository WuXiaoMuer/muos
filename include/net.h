#ifndef MUOS_NET_H
#define MUOS_NET_H
#include "types.h"

/* Minimal IPv4 stack over an RTL8139 (`-nic user,model=rtl8139`) or an
 * Intel E1000 (`-nic user,model=e1000`; the RTL is probed first).
 * Supported: ARP, IPv4, ICMP echo, UDP, DHCP and a small TCP client /
 * server, plus a socket table exposed through the Linux i386 socket
 * syscalls. The static fallback address is the QEMU user-net default
 * 10.0.2.15/24 with gateway 10.0.2.2 and DNS 10.0.2.3. */

#define NET_IP(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
                         ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define NET_DEFAULT_IP   NET_IP(10,0,2,15)
#define NET_DEFAULT_MASK NET_IP(255,255,255,0)
#define NET_DEFAULT_GW   NET_IP(10,0,2,2)
#define NET_DEFAULT_DNS  NET_IP(10,0,2,3)

/* Socket handles handed to user space sit above the file-fd range. */
#define NET_FD_BASE 16
#define NET_FD_MAX  24

#define NET_SOCK_STREAM 1
#define NET_SOCK_DGRAM  2
#define NET_SOCK_RAW    3

#define NET_EAGAIN (-11)

bool_t   net_init(void);
bool_t   net_present(void);
const char* net_driver_name(void);
void     net_get_mac(uint8_t out[6]);
uint32_t net_get_ip(void);
uint32_t net_get_gateway(void);

/* Socket operations. Socket ids are 0..NET_FD_MAX-NET_FD_BASE-1; the
 * syscall layer adds NET_FD_BASE. Return >=0 on success, negative error. */
int net_socket(int type, int proto);
/* Create a connected AF_UNIX pair (socketpair(2)); no NIC required.
 * Returns 0 with ids in *a/*b, -1 when slots are exhausted, -2 for a
 * supported-type mismatch. */
int net_socketpair(int type, int* a, int* b);
/* SOCK_STREAM/SOCK_DGRAM/SOCK_RAW of a live socket, 0 when invalid. */
int net_socket_kind(int id);
/* True for an AF_UNIX endpoint (socketpair) with no NIC dependence. */
bool_t net_socket_is_unix(int id);
int net_bind(int id, uint32_t ip, uint16_t port);
int net_connect(int id, uint32_t ip, uint16_t port);
int net_listen(int id);
int net_accept(int id);
/* Non-blocking accept: pick an established child of listener `id` now. */
int net_accept_now(int id);
/* Drive the polled NIC from the timer tick so a blocked accept can wake. */
void net_tick(void);
int net_sendto(int id, const void* data, uint32_t len,
               uint32_t ip, uint16_t port);
int net_recvfrom(int id, void* buf, uint32_t maxlen,
                 uint32_t* ip, uint16_t* port);
/* shutdown(2): how is 0=SHUT_RD, 1=SHUT_WR, 2=SHUT_RDWR. */
int net_shutdown(int id, int how);
int net_close(int id);
int net_getsockname(int id, uint32_t* ip, uint16_t* port);
int net_getpeername(int id, uint32_t* ip, uint16_t* port);
bool_t net_socket_ok(int id);

#endif
