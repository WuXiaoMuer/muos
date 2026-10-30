#ifndef MUOS_NET_H
#define MUOS_NET_H
#include "types.h"

/* Minimal IPv4 stack over an RTL8139 (QEMU `-nic user,model=rtl8139`).
 * Supported: ARP, IPv4, ICMP echo, UDP, plus a small socket table exposed
 * through the Linux i386 socket syscalls. There is no TCP, no DHCP and no
 * dynamic configuration: the address is the QEMU user-net default
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
int net_bind(int id, uint32_t ip, uint16_t port);
int net_connect(int id, uint32_t ip, uint16_t port);
int net_listen(int id);
int net_accept(int id);
int net_sendto(int id, const void* data, uint32_t len,
               uint32_t ip, uint16_t port);
int net_recvfrom(int id, void* buf, uint32_t maxlen,
                 uint32_t* ip, uint16_t* port);
int net_close(int id);
int net_getsockname(int id, uint32_t* ip, uint16_t* port);
int net_getpeername(int id, uint32_t* ip, uint16_t* port);
bool_t net_socket_ok(int id);

#endif
