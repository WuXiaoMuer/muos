/* net.c - RTL8139 driver + minimal IPv4 stack and socket ABI.
 *
 * Deliberately polling based (no IRQ handler): the ISR ROK bit is checked
 * on every syscall that needs the network. That keeps the driver usable
 * from the kernel shell and from user syscalls without touching IRQ
 * routing, and QEMU's user-mode network responds within a few milliseconds.
 */
#include "net.h"
#include "pci.h"
#include "io.h"
#include "mm.h"
#include "pit.h"
#include "string.h"
#include "vga.h"
#include "serial.h"

#define RX_SIZE 32768u
#define TX_MAX  1792u

/* RTL8139 register offsets */
#define R_IDR0      0x00
#define R_TSD0      0x10
#define R_TSAD0     0x20
#define R_RBSTART   0x30
#define R_CMD       0x37
#define R_CAPR      0x38
#define R_IMR       0x3C
#define R_ISR       0x3E
#define R_TCR       0x40
#define R_RCR       0x44
#define R_CONFIG1   0x52

#define ETH_TYPE_IP  0x0800
#define ETH_TYPE_ARP 0x0806

typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    bool_t   used;
} arp_entry_t;

typedef struct {
    bool_t   used;
    int      type;
    int      proto;
    uint16_t lport;
    bool_t   bound;
    bool_t   connected;
    uint32_t rip;
    uint16_t rport;
    bool_t   has;
    uint32_t sip;
    uint16_t sport;
    uint32_t plen;
    uint8_t  data[512];

    /* TCP (SOCK_STREAM) state */
    uint8_t  tcp_state;
    int      parent;        /* listening socket that spawned us, -1 if none */
    bool_t   echo_mode;     /* kernel demo echo service (port 7) */
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    uint32_t snd_una;       /* first unacknowledged sequence number */
    uint32_t cwnd;          /* congestion window, in bytes */
    uint32_t ssthresh;

    /* Single-outstanding-segment retransmission (RTO with exponential
     * backoff). Enough for the QEMU user-net and a lost-segment demo. */
    bool_t   rtx_pending;
    uint32_t rtx_seq, rtx_end;
    uint32_t rtx_deadline, rto;
    uint8_t  rtx_flags, rtx_retries;
    uint16_t rtx_len;
    uint8_t  rtx_data[480];
} net_sock_t;

#define TCP_MSS 480u
#define TCP_RTO_MIN 100u        /* PIT ticks (10ms each) = 1s */
#define TCP_RTO_MAX 800u
#define TCP_RETRIES 5
#define TCP_CWND_MAX 65535u

#define TCP_CLOSED    0
#define TCP_LISTEN    1
#define TCP_SYN_SENT  2
#define TCP_SYN_RCVD  3
#define TCP_ESTAB     4
#define TCP_FIN_WAIT  5
#define TCP_CLOSE_WAIT 6

#define TCP_F_FIN 0x01
#define TCP_F_SYN 0x02
#define TCP_F_RST 0x04
#define TCP_F_PSH 0x08
#define TCP_F_ACK 0x10

static uint16_t io_base;
static bool_t   present;
static uint8_t  mac_addr[6];
static uint32_t my_ip, my_mask, gateway;
static uint32_t rx_capr;
static int      tx_next;
static arp_entry_t arp_cache[4];
static net_sock_t  socks[NET_FD_MAX - NET_FD_BASE];
static uint16_t    next_port = 40000;

static uint8_t rx_buffer[RX_SIZE + 16] __attribute__((aligned(16)));
static uint8_t tx_buffer[TX_MAX]       __attribute__((aligned(16)));
static uint8_t rx_frame[1600]         __attribute__((aligned(4)));
static uint8_t out_frame[1600]        __attribute__((aligned(4)));
static uint8_t loop_frame[1600]       __attribute__((aligned(4)));
static uint8_t tcp_seg[560]           __attribute__((aligned(4)));
static uint8_t udp_hdr[8 + 512]       __attribute__((aligned(4)));

/* ---------------- helpers ---------------- */

static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put16be(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }
static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static void put32be(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Implemented below; declared here because ip_handle dispatches to them. */
static bool_t net_send_ip_raw(uint8_t proto, uint32_t dst,
                              const uint8_t* payload, uint32_t plen);
static void tcp_send_seg(net_sock_t* s, uint32_t seq, uint32_t ack,
                         uint8_t flags, const uint8_t* data, uint32_t len);
static void tcp_input(uint32_t src, uint32_t dst,
                      const uint8_t* seg, uint32_t len);
static void tcp_timers(void);
static void dhcp_input(const uint8_t* b, uint32_t len);

static uint16_t net_cksum(const uint8_t* data, uint32_t len, uint32_t sum0) {
    uint32_t sum = sum0;
    while (len > 1) {
        sum += (uint16_t)((data[0] << 8) | data[1]);
        data += 2;
        len -= 2;
    }
    if (len) sum += (uint16_t)(data[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

static void mac_read(void) {
    for (int i = 0; i < 6; i++) mac_addr[i] = inb(io_base + R_IDR0 + i);
}

/* ---------------- receive path ---------------- */

static void net_send_frame(const uint8_t* frame, uint32_t len);

static void arp_store(uint32_t ip, const uint8_t* mac) {
    int free_slot = 0;
    for (int i = 0; i < 4; i++) {
        if (arp_cache[i].used && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
        if (!arp_cache[i].used) free_slot = i;
    }
    arp_cache[free_slot].ip = ip;
    memcpy(arp_cache[free_slot].mac, mac, 6);
    arp_cache[free_slot].used = true;
}

static bool_t arp_lookup(uint32_t ip, uint8_t* mac) {
    for (int i = 0; i < 4; i++) {
        if (arp_cache[i].used && arp_cache[i].ip == ip) {
            memcpy(mac, arp_cache[i].mac, 6);
            return true;
        }
    }
    return false;
}

static void arp_send(uint16_t op, uint32_t target_ip, const uint8_t* target_mac) {
    uint8_t* f = out_frame;
    memcpy(f, target_mac, 6);
    memcpy(f + 6, mac_addr, 6);
    put16be(f + 12, ETH_TYPE_ARP);

    uint8_t* a = f + 14;
    put16be(a + 0, 1);              /* Ethernet */
    put16be(a + 2, ETH_TYPE_IP);
    a[4] = 6;
    a[5] = 4;
    put16be(a + 6, op);
    memcpy(a + 8, mac_addr, 6);
    memcpy(a + 14, &my_ip, 4);      /* sender protocol address */
    memcpy(a + 18, target_mac, 6);
    memcpy(a + 24, &target_ip, 4);
    net_send_frame(f, 14 + 28);
}

static void arp_handle(const uint8_t* a, uint32_t len) {
    if (len < 28) return;
    uint32_t sender_ip;
    memcpy(&sender_ip, a + 14, 4);
    const uint8_t* sender_mac = a + 8;
    arp_store(sender_ip, sender_mac);
    if (be16(a + 6) == 1 && sender_ip != my_ip) {
        /* Someone wants us: answer with our MAC. */
        uint32_t target;
        memcpy(&target, a + 24, 4);
        if (target == my_ip) {
            arp_send(2, sender_ip, sender_mac);
        }
    }
}

static void udp_deliver(uint16_t dport, uint16_t sport, uint32_t sip,
                        const uint8_t* payload, uint32_t len) {
    for (int i = 0; i < NET_FD_MAX - NET_FD_BASE; i++) {
        net_sock_t* s = &socks[i];
        if (!s->used || s->type != NET_SOCK_DGRAM) continue;
        if (!s->bound || s->lport != dport) continue;
        if (s->connected && (s->rip != sip || s->rport != sport)) continue;
        if (s->has) continue;               /* one pending packet per socket */
        if (len > sizeof(s->data)) len = sizeof(s->data);
        memcpy(s->data, payload, len);
        s->plen = len;
        s->sip = sip;
        s->sport = sport;
        s->has = true;
    }
}

static void raw_deliver(int proto, uint32_t sip, const uint8_t* payload,
                        uint32_t len) {
    for (int i = 0; i < NET_FD_MAX - NET_FD_BASE; i++) {
        net_sock_t* s = &socks[i];
        if (!s->used || s->type != NET_SOCK_RAW || s->proto != proto) continue;
        if (s->has) continue;
        if (len > sizeof(s->data)) len = sizeof(s->data);
        memcpy(s->data, payload, len);
        s->plen = len;
        s->sip = sip;
        s->sport = 0;
        s->has = true;
    }
}

static void ip_handle(const uint8_t* ip, uint32_t len) {
    if (len < 20) return;
    uint8_t ihl = (uint8_t)(ip[0] & 0x0F) * 4;
    if (ihl < 20 || ihl > len) return;
    uint32_t total = be16(ip + 2);
    if (total > len) total = len;
    uint32_t dst;
    memcpy(&dst, ip + 16, 4);
    if (dst != my_ip && dst != 0xFFFFFFFFu && dst != NET_IP(127,0,0,1)) return;

    uint8_t proto = ip[9];
    const uint8_t* payload = ip + ihl;
    uint32_t plen = total > ihl ? total - ihl : 0;

    if (proto == 1) {                       /* ICMP */
        if (plen < 8) return;
        if (payload[0] == 8) {              /* echo request -> reply */
            uint32_t src;
            memcpy(&src, ip + 12, 4);
            uint8_t* f = out_frame;
            uint8_t dmac[6];
            uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
            if (!arp_lookup(src, dmac)) {
                /* Reply needs a destination MAC: resolve the sender. */
                arp_send(1, src, bcast);
                uint32_t deadline = pit_get_ticks() + 50;
                while (pit_get_ticks() < deadline && !arp_lookup(src, dmac)) {
                    __asm__ volatile ("sti; hlt; cli");
                }
            }
            memcpy(f, dmac, 6);
            memcpy(f + 6, mac_addr, 6);
            put16be(f + 12, ETH_TYPE_IP);
            uint8_t* oip = f + 14;
            memset(oip, 0, 20);
            oip[0] = 0x45;
            oip[8] = 64;
            oip[9] = 1;
            memcpy(oip + 12, &my_ip, 4);
            memcpy(oip + 16, &src, 4);
            uint32_t olen = 20 + plen;
            put16be(oip + 2, (uint16_t)olen);
            put16be(oip + 10, net_cksum(oip, 20, 0));
            uint8_t* icmp = oip + 20;
            memcpy(icmp, payload, plen);
            icmp[0] = 0;                    /* echo reply */
            put16be(icmp + 2, 0);
            put16be(icmp + 2, net_cksum(icmp, plen, 0));
            net_send_frame(f, 14 + olen);
            return;
        }
        uint32_t src;
        memcpy(&src, ip + 12, 4);
        raw_deliver(1, src, payload, plen);
    } else if (proto == 17) {               /* UDP */
        if (plen < 8) return;
        uint16_t sport = be16(payload);
        uint16_t dport = be16(payload + 2);
        uint32_t ulen = be16(payload + 4);
        if (ulen < 8) return;
        if (ulen > plen) ulen = plen;
        uint32_t src;
        memcpy(&src, ip + 12, 4);
        if (dport == 68) dhcp_input(payload + 8, ulen - 8);
        else udp_deliver(dport, sport, src, payload + 8, ulen - 8);
    } else if (proto == 6) {                /* TCP */
        uint32_t src;
        memcpy(&src, ip + 12, 4);
        tcp_input(src, dst, payload, plen);
    } else {
        uint32_t src;
        memcpy(&src, ip + 12, 4);
        raw_deliver(proto, src, payload, plen);
    }
}

static void net_handle_frame(const uint8_t* f, uint32_t len) {
    if (len < 14) return;
    uint16_t type = be16(f + 12);
    if (type == ETH_TYPE_ARP) {
        arp_handle(f + 14, len - 14);
    } else if (type == ETH_TYPE_IP) {
        ip_handle(f + 14, len - 14);
    }
}

static void ring_read(uint32_t start, uint8_t* dst, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) dst[i] = rx_buffer[(start + i) % RX_SIZE];
}

static void net_poll(void) {
    if (!present) return;
    for (uint32_t guard = 0; guard < 64; guard++) {
        uint16_t isr = inw(io_base + R_ISR);
        if (!(isr & 0x0001)) break;          /* ROK */
        /* Acknowledge before reading, as QEMU requires. */
        outw(io_base + R_ISR, 0xFFFF);

        uint8_t hdr[4];
        ring_read(rx_capr, hdr, 4);
        uint16_t status = (uint16_t)(hdr[0] | (hdr[1] << 8));
        uint16_t plen   = (uint16_t)(hdr[2] | (hdr[3] << 8));
        if (!(status & 0x0001) || plen < 4 || plen > 1600) {
            rx_capr = (rx_capr + 4) & ~3u;
            if (rx_capr >= RX_SIZE) rx_capr -= RX_SIZE;
            continue;
        }
        uint32_t flen = plen - 4;            /* strip CRC */
        if (flen > sizeof(rx_frame)) flen = sizeof(rx_frame);
        ring_read(rx_capr + 4, rx_frame, flen);
        rx_capr = (rx_capr + 4 + plen + 3) & ~3u;
        if (rx_capr >= RX_SIZE) rx_capr -= RX_SIZE;

        net_handle_frame(rx_frame, flen);
    }
    tcp_timers();
    /* CAPR is 16 bytes ahead of the internal read pointer on the RTL8139
     * (QEMU mirrors the chip): writing x makes the chip read from x+16,
     * so compensate to publish our actual read offset. */
    outw(io_base + R_CAPR, (uint16_t)(rx_capr - 0x10));
}

/* ---------------- transmit path ---------------- */

static void net_send_frame(const uint8_t* frame, uint32_t len) {
    if (!present || len < 14 || len > TX_MAX) return;
    memcpy(tx_buffer, frame, len);
    uint32_t phys = mm_virt_to_phys((uint32_t)tx_buffer);
    int slot = tx_next;
    tx_next = (tx_next + 1) & 3;

    outl(io_base + R_TSAD0 + (uint32_t)slot * 4, phys);
    outl(io_base + R_TSD0 + (uint32_t)slot * 4, len);
    /* Wait for transmit OK (bit 15) so the buffer can be reused. */
    for (uint32_t n = 0; n < 200000; n++) {
        if (inl(io_base + R_TSD0 + (uint32_t)slot * 4) & 0x8000u) break;
    }
}

static bool_t arp_resolve(uint32_t ip, uint8_t* out) {
    if (arp_lookup(ip, out)) return true;
    uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    arp_send(1, ip, bcast);
    uint32_t deadline = pit_get_ticks() + 100;   /* ~1 second */
    while (pit_get_ticks() < deadline) {
        __asm__ volatile ("sti; hlt; cli");
        net_poll();
        if (arp_lookup(ip, out)) return true;
    }
    return false;
}

static uint32_t local_ip_for(uint32_t dst) {
    if (dst == NET_IP(127,0,0,1)) return dst;
    if (dst == 0xFFFFFFFFu) return 0;       /* DHCP: source 0.0.0.0 */
    return my_ip;
}

/* Build Ethernet + IPv4 around an already-formed transport payload. */
static bool_t net_send_ip_raw(uint8_t proto, uint32_t dst,
                              const uint8_t* payload, uint32_t plen) {
    if (!present) return false;

    uint32_t tot = 20 + plen;
    uint32_t srcip = local_ip_for(dst);

    if (dst == 0xFFFFFFFFu) {
        /* Broadcast (DHCP): Ethernet broadcast, no ARP, source 0.0.0.0. */
        uint8_t* f = out_frame;
        for (int i = 0; i < 6; i++) f[i] = 0xFF;
        memcpy(f + 6, mac_addr, 6);
        put16be(f + 12, ETH_TYPE_IP);
        uint8_t* ip = f + 14;
        memset(ip, 0, 20);
        ip[0] = 0x45;
        ip[8] = 64;
        ip[9] = proto;
        memcpy(ip + 12, &srcip, 4);
        memcpy(ip + 16, &dst, 4);
        put16be(ip + 2, (uint16_t)tot);
        put16be(ip + 4, 0x1234);
        put16be(ip + 10, net_cksum(ip, 20, 0));
        if (plen) memcpy(ip + 20, payload, plen);
        net_send_frame(f, 14 + tot);
        return true;
    }

    if (dst == my_ip || dst == NET_IP(127,0,0,1)) {
        /* Loopback: hand the datagram to our own receive path. Copy first
         * because ip_handle (and the replies it sends) reuse out_frame. */
        uint8_t* ip = loop_frame;
        memset(ip, 0, 20);
        ip[0] = 0x45;
        ip[8] = 64;
        ip[9] = proto;
        memcpy(ip + 12, &srcip, 4);
        memcpy(ip + 16, &dst, 4);
        put16be(ip + 2, (uint16_t)tot);
        put16be(ip + 4, 0x1234);
        put16be(ip + 10, net_cksum(ip, 20, 0));
        if (plen) memcpy(ip + 20, payload, plen);
        ip_handle(ip, tot);
        return true;
    }

    uint8_t next_hop = ((dst & my_mask) == (my_ip & my_mask)) ? 0 : 1;
    uint32_t hop = next_hop ? gateway : dst;
    uint8_t dmac[6];
    if (!arp_resolve(hop, dmac)) {
        /* QEMU's user-mode network (slirp) accepts IP frames addressed to
         * the virtual gateway without ARP; broadcast is a safe fallback. */
        uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        memcpy(dmac, bcast, 6);
    }

    uint8_t* f = out_frame;
    memcpy(f, dmac, 6);
    memcpy(f + 6, mac_addr, 6);
    put16be(f + 12, ETH_TYPE_IP);

    uint8_t* ip = f + 14;
    memset(ip, 0, 20);
    ip[0] = 0x45;
    ip[8] = 64;
    ip[9] = proto;
    memcpy(ip + 12, &srcip, 4);
    memcpy(ip + 16, &dst, 4);
    put16be(ip + 2, (uint16_t)tot);
    put16be(ip + 4, 0x1234);                /* id */
    put16be(ip + 10, net_cksum(ip, 20, 0));
    if (plen) memcpy((void*)(ip + 20), payload, plen);

    net_send_frame(f, 14 + tot);
    return true;
}

/* UDP convenience: prepend the 8-byte header, then send the IPv4 packet. */
static bool_t net_send_ip(uint8_t proto, uint32_t dst,
                          const uint8_t* payload, uint32_t plen,
                          uint16_t src_port, uint16_t dst_port) {
    if (proto != 17)                       /* raw ICMP and friends */
        return net_send_ip_raw(proto, dst, payload, plen);
    if (plen > 512) plen = 512;
    put16be(udp_hdr + 0, src_port);
    put16be(udp_hdr + 2, dst_port);
    put16be(udp_hdr + 4, (uint16_t)(8 + plen));
    put16be(udp_hdr + 6, 0);                /* checksum optional in IPv4 */
    if (plen) memcpy(udp_hdr + 8, payload, plen);
    return net_send_ip_raw(proto, dst, udp_hdr, 8 + plen);
}

/* ---------------- DHCP client ---------------- */

static bool_t   dhcp_seen;
static uint8_t  dhcp_type;
static uint32_t dhcp_yiaddr, dhcp_server, dhcp_mask, dhcp_gw, dhcp_dns;

static void dhcp_input(const uint8_t* b, uint32_t len) {
    if (len < 240) return;
    if (memcmp(b + 236, "\x63\x82\x53\x63", 4) != 0) return;
    memcpy(&dhcp_yiaddr, b + 16, 4);
    dhcp_type = 0;
    dhcp_server = dhcp_mask = dhcp_gw = dhcp_dns = 0;

    uint32_t i = 240;
    while (i + 2 <= len) {
        uint8_t code = b[i];
        if (code == 0) { i++; continue; }
        if (code == 255) break;
        uint8_t olen = b[i + 1];
        if (i + 2 + olen > len) break;
        const uint8_t* v = b + i + 2;
        if (code == 53 && olen >= 1) dhcp_type = v[0];
        else if (code == 1 && olen >= 4) memcpy(&dhcp_mask, v, 4);
        else if (code == 3 && olen >= 4) memcpy(&dhcp_gw, v, 4);
        else if (code == 6 && olen >= 4) memcpy(&dhcp_dns, v, 4);
        else if (code == 54 && olen >= 4) memcpy(&dhcp_server, v, 4);
        i += 2 + olen;
    }
    dhcp_seen = true;
}

static void dhcp_build(uint8_t* p, uint8_t msgtype,
                       uint32_t reqip, uint32_t server) {
    memset(p, 0, 300);
    p[0] = 1;                  /* BOOTREQUEST */
    p[1] = 1;                  /* Ethernet */
    p[2] = 6;                  /* hardware address length */
    uint32_t xid = 0x4D554F53u;
    memcpy(p + 4, &xid, 4);
    p[10] = 0x80;              /* broadcast flag */
    memcpy(p + 28, mac_addr, 6);
    memcpy(p + 236, "\x63\x82\x53\x63", 4);

    int i = 240;
    p[i++] = 53; p[i++] = 1; p[i++] = msgtype;
    if (reqip) {
        p[i++] = 50; p[i++] = 4; memcpy(p + i, &reqip, 4); i += 4;
    }
    if (server) {
        p[i++] = 54; p[i++] = 4; memcpy(p + i, &server, 4); i += 4;
    }
    p[i++] = 55; p[i++] = 3; p[i++] = 1; p[i++] = 3; p[i++] = 6;
    p[i++] = 255;
}

static bool_t dhcp_run(void) {
    uint8_t bootp[300];

    dhcp_seen = false;
    dhcp_build(bootp, 1, 0, 0);              /* DISCOVER */
    net_send_ip(17, 0xFFFFFFFFu, bootp, 300, 68, 67);
    for (int i = 0; i < 120000 && !dhcp_seen; i++) net_poll();
    if (!dhcp_seen || dhcp_type != 2 || dhcp_yiaddr == 0) return false;

    uint32_t offer = dhcp_yiaddr;
    uint32_t server = dhcp_server;

    dhcp_seen = false;
    dhcp_build(bootp, 3, offer, server);     /* REQUEST */
    net_send_ip(17, 0xFFFFFFFFu, bootp, 300, 68, 67);
    for (int i = 0; i < 120000 && !dhcp_seen; i++) net_poll();
    if (!dhcp_seen || dhcp_type != 5) return false;

    my_ip = dhcp_yiaddr;
    if (dhcp_mask) my_mask = dhcp_mask;
    if (dhcp_gw) gateway = dhcp_gw;
    return true;
}

/* ---------------- TCP (minimal client + loopback server) ---------------- */

static uint16_t tcp_cksum(uint32_t src, uint32_t dst,
                          const uint8_t* seg, uint32_t len) {
    uint8_t ph[12];
    memcpy(ph, &src, 4);
    memcpy(ph + 4, &dst, 4);
    ph[8] = 0;
    ph[9] = 6;
    ph[10] = (uint8_t)(len >> 8);
    ph[11] = (uint8_t)len;
    uint32_t sum = 0;
    for (int i = 0; i < 12; i += 2)
        sum += (uint16_t)((ph[i] << 8) | ph[i + 1]);
    return net_cksum(seg, len, sum);
}

/* Build and transmit one TCP segment without touching retransmission state. */
static void tcp_emit(net_sock_t* s, uint32_t seq, uint32_t ack,
                     uint8_t flags, const uint8_t* data, uint32_t len) {
    if (len > 512) len = 512;
    uint8_t* t = tcp_seg;
    put16be(t + 0, s->lport);
    put16be(t + 2, s->rport);
    put32be(t + 4, seq);
    put32be(t + 8, ack);
    t[12] = 0x50;                       /* data offset 5 words */
    t[13] = flags;
    put16be(t + 14, 2048);              /* window */
    put16be(t + 16, 0);
    put16be(t + 18, 0);
    if (len) memcpy(t + 20, data, len);
    put16be(t + 16, tcp_cksum(local_ip_for(s->rip), s->rip, t, 20 + len));
    net_send_ip_raw(6, s->rip, t, 20 + len);
}

/* Send a segment that carries sequence-space (SYN/FIN/data) and arm the
 * retransmission timer for it. Pure ACKs are sent without arming. */
static void tcp_send_seg(net_sock_t* s, uint32_t seq, uint32_t ack,
                         uint8_t flags, const uint8_t* data, uint32_t len) {
    tcp_emit(s, seq, ack, flags, data, len);

    if (len == 0 && !(flags & (TCP_F_SYN | TCP_F_FIN))) return;
    s->rtx_pending = true;
    s->rtx_seq  = seq;
    s->rtx_end  = seq + len +
                  ((flags & TCP_F_SYN) ? 1u : 0u) +
                  ((flags & TCP_F_FIN) ? 1u : 0u);
    s->rtx_flags = flags;
    s->rtx_len = (uint16_t)(len > TCP_MSS ? TCP_MSS : len);
    if (s->rtx_len) memcpy(s->rtx_data, data, s->rtx_len);
    s->rtx_retries = 0;
    s->rto = TCP_RTO_MIN;
    s->rtx_deadline = pit_get_ticks() + s->rto;
}

/* ACK processing: advance snd_una, clear the retransmission entry and
 * grow the congestion window (slow start / congestion avoidance). */
static void tcp_ack(net_sock_t* s, uint32_t ack) {
    if ((int32_t)(ack - s->snd_una) <= 0) return;
    s->snd_una = ack;
    if (s->rtx_pending && (int32_t)(ack - s->rtx_end) >= 0)
        s->rtx_pending = false;

    if (s->cwnd >= s->ssthresh)
        s->cwnd += (TCP_MSS * TCP_MSS) / (s->cwnd ? s->cwnd : TCP_MSS);
    else
        s->cwnd += TCP_MSS;
    if (s->cwnd > TCP_CWND_MAX) s->cwnd = TCP_CWND_MAX;
}

/* RTO timer: retransmit the single outstanding segment with exponential
 * backoff; collapse the congestion window on every timeout. */
static void tcp_timers(void) {
    uint32_t now = pit_get_ticks();
    int n = NET_FD_MAX - NET_FD_BASE;
    for (int i = 0; i < n; i++) {
        net_sock_t* s = &socks[i];
        if (!s->used || s->type != NET_SOCK_STREAM || !s->rtx_pending) continue;
        if ((int32_t)(now - s->rtx_deadline) < 0) continue;

        if (s->rtx_retries >= TCP_RETRIES) {
            s->rtx_pending = false;
            s->tcp_state = TCP_CLOSED;
            s->used = false;
            continue;
        }
        s->rtx_retries++;
        s->ssthresh = s->cwnd > TCP_MSS * 2 ? s->cwnd / 2 : TCP_MSS * 2;
        s->cwnd = TCP_MSS;
        s->rto = s->rto < TCP_RTO_MAX / 2 ? s->rto * 2 : TCP_RTO_MAX;

        uint32_t seq = s->rtx_seq;
        uint32_t ack = (s->rtx_flags & TCP_F_SYN) ? 0 : s->rcv_nxt;
        s->rtx_deadline = now + s->rto;
        tcp_emit(s, seq, ack, s->rtx_flags, s->rtx_data, s->rtx_len);
    }
}

static net_sock_t* tcp_find(uint32_t dst, uint16_t sport, uint16_t dport,
                            int* listener_id) {
    net_sock_t* found = NULL;
    if (listener_id) *listener_id = -1;
    for (int i = 0; i < NET_FD_MAX - NET_FD_BASE; i++) {
        net_sock_t* s = &socks[i];
        if (!s->used || s->type != NET_SOCK_STREAM || s->lport != dport) continue;
        if (s->tcp_state == TCP_LISTEN) {
            if (listener_id) *listener_id = i;
            continue;
        }
        if (s->rport == sport && s->rip == dst) found = s;
    }
    return found;
}

static net_sock_t* tcp_new_sock(void) {
    for (int i = 0; i < NET_FD_MAX - NET_FD_BASE; i++) {
        if (socks[i].used) continue;
        memset(&socks[i], 0, sizeof(socks[i]));
        socks[i].used = true;
        socks[i].type = NET_SOCK_STREAM;
        socks[i].proto = 6;
        socks[i].parent = -1;
        return &socks[i];
    }
    return NULL;
}

static void tcp_input(uint32_t src, uint32_t dst,
                      const uint8_t* seg, uint32_t len) {
    (void)dst;
    if (len < 20) return;
    uint16_t sport = be16(seg + 0);
    uint16_t dport = be16(seg + 2);
    uint32_t seq = be32(seg + 4);
    uint32_t ack = be32(seg + 8);
    uint32_t off = (uint32_t)(seg[12] >> 4) * 4;
    if (off < 20 || off > len) return;
    uint8_t flags = seg[13];
    const uint8_t* data = seg + off;
    uint32_t dlen = len - off;

    int listener_id = -1;
    net_sock_t* s = tcp_find(src, sport, dport, &listener_id);

    if (!s) {
        bool_t echo = (dport == 7);
        if (listener_id >= 0 && (flags & TCP_F_SYN) && !(flags & TCP_F_ACK)) {
            net_sock_t* child = tcp_new_sock();
            if (!child) return;
            child->lport = dport;
            child->bound = true;
            child->connected = true;
            child->rip = src;
            child->rport = sport;
            child->parent = listener_id;
            child->rcv_nxt = seq + 1;
            child->snd_nxt = 0x20000000u + (uint32_t)(child - socks) * 0x1000u;
            child->snd_una = child->snd_nxt;
            child->cwnd = TCP_MSS;
            child->ssthresh = TCP_CWND_MAX;
            child->tcp_state = TCP_SYN_RCVD;
            /* Consume the SYN sequence number before transmitting: loopback
             * delivers the reply synchronously, so the peer's ACK can arrive
             * inside this call and must see the updated snd_nxt. */
            uint32_t cseq = child->snd_nxt;
            child->snd_nxt = cseq + 1;
            tcp_send_seg(child, cseq, child->rcv_nxt,
                         TCP_F_SYN | TCP_F_ACK, NULL, 0);
        } else if (echo && (flags & TCP_F_SYN) && !(flags & TCP_F_ACK)) {
            net_sock_t* child = tcp_new_sock();
            if (!child) return;
            child->lport = dport;
            child->bound = true;
            child->connected = true;
            child->rip = src;
            child->rport = sport;
            child->parent = -1;
            child->echo_mode = true;
            child->rcv_nxt = seq + 1;
            child->snd_nxt = 0x30000000u + (uint32_t)(child - socks) * 0x1000u;
            child->snd_una = child->snd_nxt;
            child->cwnd = TCP_MSS;
            child->ssthresh = TCP_CWND_MAX;
            child->tcp_state = TCP_SYN_RCVD;
            uint32_t cseq = child->snd_nxt;
            child->snd_nxt = cseq + 1;
            tcp_send_seg(child, cseq, child->rcv_nxt,
                         TCP_F_SYN | TCP_F_ACK, NULL, 0);
        }
        return;
    }

    if (flags & TCP_F_RST) {
        s->used = false;
        return;
    }

    if (flags & TCP_F_ACK) tcp_ack(s, ack);

    if (s->tcp_state == TCP_SYN_SENT && (flags & TCP_F_SYN) && (flags & TCP_F_ACK)) {
        if (ack == s->snd_nxt) {
            s->rcv_nxt = seq + 1;
            s->tcp_state = TCP_ESTAB;
            tcp_send_seg(s, s->snd_nxt, s->rcv_nxt, TCP_F_ACK, NULL, 0);
        }
        return;
    }

    if (s->tcp_state == TCP_SYN_RCVD && (flags & TCP_F_ACK)) {
        if (ack == s->snd_nxt) s->tcp_state = TCP_ESTAB;
    }

    if ((s->tcp_state == TCP_ESTAB || s->tcp_state == TCP_CLOSE_WAIT) &&
        dlen > 0) {
        if (seq == s->rcv_nxt) {
            if (s->echo_mode) {
                /* Kernel demo echo service: send the bytes straight back. */
                tcp_send_seg(s, s->snd_nxt, s->rcv_nxt,
                             TCP_F_PSH | TCP_F_ACK, data, dlen);
                s->snd_nxt += dlen;
            } else if (!s->has) {
                uint32_t n = dlen > sizeof(s->data) ? sizeof(s->data) : dlen;
                memcpy(s->data, data, n);
                s->plen = n;
                s->sip = src;
                s->sport = sport;
                s->has = true;
            }
            s->rcv_nxt += dlen;
        }
        tcp_send_seg(s, s->snd_nxt, s->rcv_nxt, TCP_F_ACK, NULL, 0);
    }

    if ((flags & TCP_F_FIN) && seq + dlen == s->rcv_nxt) {
        s->rcv_nxt++;
        s->tcp_state = TCP_CLOSE_WAIT;
        tcp_send_seg(s, s->snd_nxt, s->rcv_nxt, TCP_F_ACK, NULL, 0);
    }
}

static int tcp_send_data(net_sock_t* s, const void* data, uint32_t len) {
    if (s->tcp_state != TCP_ESTAB && s->tcp_state != TCP_CLOSE_WAIT) return -1;
    uint32_t sent = 0;
    uint32_t guard = 0;
    while (sent < len) {
        uint32_t inflight = s->snd_nxt - s->snd_una;
        uint32_t window = s->cwnd ? s->cwnd : TCP_MSS;

        if (inflight >= window) {
            /* Congestion window is full: wait for ACKs (poll also runs
             * the RTO timer and may abort the connection). */
            net_poll();
            if (s->tcp_state == TCP_CLOSED) {
                return sent ? (int)sent : -1;
            }
            if (++guard > 2000) return -1;      /* ~20 s */
            __asm__ volatile ("sti; hlt; cli");
            continue;
        }

        uint32_t chunk = len - sent;
        if (chunk > TCP_MSS) chunk = TCP_MSS;
        if (chunk > window - inflight) chunk = window - inflight;
        tcp_send_seg(s, s->snd_nxt, s->rcv_nxt,
                     TCP_F_PSH | TCP_F_ACK,
                     (const uint8_t*)data + sent, chunk);
        s->snd_nxt += chunk;
        sent += chunk;
    }
    return (int)sent;
}

/* ---------------- sockets ---------------- */

bool_t net_socket_ok(int id) {
    return id >= 0 && id < NET_FD_MAX - NET_FD_BASE && socks[id].used;
}

int net_socket(int type, int proto) {
    if (!present) return -1;
    if (type != NET_SOCK_DGRAM && type != NET_SOCK_RAW &&
        type != NET_SOCK_STREAM) return -2;
    if (type == NET_SOCK_RAW && proto == 0) return -2;
    for (int i = 0; i < NET_FD_MAX - NET_FD_BASE; i++) {
        if (socks[i].used) continue;
        memset(&socks[i], 0, sizeof(socks[i]));
        socks[i].used = true;
        socks[i].type = type;
        if (proto) socks[i].proto = proto;
        else socks[i].proto = (type == NET_SOCK_DGRAM) ? 17 :
                              (type == NET_SOCK_STREAM) ? 6 : 1;
        socks[i].parent = -1;
        return i;
    }
    return -1;
}

int net_bind(int id, uint32_t ip, uint16_t port) {
    if (!net_socket_ok(id)) return -1;
    net_sock_t* s = &socks[id];
    (void)ip;
    if (!port) port = next_port++;
    s->lport = port;
    s->bound = true;
    return 0;
}

int net_connect(int id, uint32_t ip, uint16_t port) {
    if (!net_socket_ok(id)) return -1;
    net_sock_t* s = &socks[id];
    s->rip = ip;
    s->rport = port;
    s->connected = true;
    if (!s->bound) {
        s->lport = next_port++;
        s->bound = true;
    }
    if (s->type != NET_SOCK_STREAM) return 0;

    s->tcp_state = TCP_SYN_SENT;
    s->rcv_nxt = 0;
    s->snd_nxt = 0x10000000u + (uint32_t)id * 0x1000u;
    s->snd_una = s->snd_nxt;
    s->cwnd = TCP_MSS;
    s->ssthresh = TCP_CWND_MAX;
    uint32_t seq = s->snd_nxt;
    s->snd_nxt = seq + 1;            /* SYN consumes one sequence number */
    s->rtx_pending = false;
    tcp_send_seg(s, seq, 0, TCP_F_SYN, NULL, 0);
    /* SYN retransmission is handled by tcp_timers (up to TCP_RETRIES). */
    for (uint32_t loops = 0; loops < 1000; loops++) {
        if (s->tcp_state == TCP_ESTAB && s->used) return 0;
        if (!s->used) return -1;
        net_poll();
        __asm__ volatile ("sti; hlt; cli");
    }
    return (s->tcp_state == TCP_ESTAB) ? 0 : -1;
}

int net_sendto(int id, const void* data, uint32_t len,
               uint32_t ip, uint16_t port) {
    if (!net_socket_ok(id)) return -1;
    if (!present) return -1;
    net_sock_t* s = &socks[id];
    if (!s->bound) {
        s->lport = (s->type == NET_SOCK_RAW) ? 0 : next_port++;
        s->bound = true;
    }
    uint32_t dst = ip;
    uint16_t dport = port;
    if (!dst && s->connected) { dst = s->rip; dport = s->rport; }

    uint8_t proto = (uint8_t)s->proto;
    if (s->type == NET_SOCK_STREAM)
        return tcp_send_data(s, data, len);
    if (!net_send_ip(proto, dst, (const uint8_t*)data, len,
                     s->lport, dport)) return -1;
    return (int)len;
}

int net_recvfrom(int id, void* buf, uint32_t maxlen,
                 uint32_t* ip, uint16_t* port) {
    if (!net_socket_ok(id)) return -1;
    if (!present) return -1;
    net_sock_t* s = &socks[id];

    if (s->type == NET_SOCK_STREAM) {
        for (uint32_t loops = 0; loops < 300; loops++) {   /* ~3 seconds */
            net_poll();
            if (s->has) {
                uint32_t n = s->plen;
                if (n > maxlen) n = maxlen;
                memcpy(buf, s->data, n);
                if (ip) *ip = s->sip;
                if (port) *port = s->sport;
                s->has = false;
                s->plen = 0;
                return (int)n;
            }
            if (s->tcp_state == TCP_CLOSE_WAIT) return 0;   /* EOF */
            __asm__ volatile ("sti; hlt; cli");
        }
        return NET_EAGAIN;
    }

    for (uint32_t loops = 0; loops < 300; loops++) {   /* ~3 seconds */
        net_poll();
        if (s->has) {
            uint32_t n = s->plen;
            if (n > maxlen) n = maxlen;
            memcpy(buf, s->data, n);
            if (ip) *ip = s->sip;
            if (port) *port = s->sport;
            s->has = false;
            s->plen = 0;
            return (int)n;
        }
        __asm__ volatile ("sti; hlt; cli");
    }
    return NET_EAGAIN;
}

int net_close(int id) {
    if (!net_socket_ok(id)) return -1;
    net_sock_t* s = &socks[id];
    if (s->type == NET_SOCK_STREAM &&
        (s->tcp_state == TCP_ESTAB || s->tcp_state == TCP_CLOSE_WAIT)) {
        tcp_send_seg(s, s->snd_nxt, s->rcv_nxt, TCP_F_FIN | TCP_F_ACK, NULL, 0);
        s->snd_nxt++;
    }
    memset(s, 0, sizeof(*s));
    return 0;
}

/* getsockname/getpeername: report the real bind/connect tuple instead of
 * the 0.0.0.0:0 placeholders the old stub returned. */
int net_getsockname(int id, uint32_t* ip, uint16_t* port) {
    if (!net_socket_ok(id)) return -1;
    net_sock_t* s = &socks[id];
    if (ip)   *ip   = s->bound ? local_ip_for(s->rip) : 0;
    if (port) *port = s->bound ? s->lport : 0;
    return 0;
}

int net_getpeername(int id, uint32_t* ip, uint16_t* port) {
    if (!net_socket_ok(id)) return -1;
    net_sock_t* s = &socks[id];
    if (!s->connected) return -1;
    if (ip)   *ip   = s->rip;
    if (port) *port = s->rport;
    return 0;
}

int net_listen(int id) {
    if (!net_socket_ok(id)) return -1;
    net_sock_t* s = &socks[id];
    if (s->type != NET_SOCK_STREAM) return -1;
    if (!s->bound) {
        s->lport = next_port++;
        s->bound = true;
    }
    s->tcp_state = TCP_LISTEN;
    return 0;
}

int net_accept(int id) {
    if (!net_socket_ok(id)) return -1;
    net_sock_t* l = &socks[id];
    if (l->type != NET_SOCK_STREAM || l->tcp_state != TCP_LISTEN) return -1;
    for (uint32_t loops = 0; loops < 300; loops++) {   /* ~3 seconds */
        net_poll();
        for (int i = 0; i < NET_FD_MAX - NET_FD_BASE; i++) {
            net_sock_t* c = &socks[i];
            if (c->used && c->parent == id && c->tcp_state == TCP_ESTAB) {
                c->parent = -1;
                return i;
            }
        }
        __asm__ volatile ("sti; hlt; cli");
    }
    return NET_EAGAIN;
}

/* ---------------- init ---------------- */

bool_t net_init(void) {
    uint8_t bus = 0, slot = 0, func = 0;
    if (!pci_find(PCI_VENDOR_REALTEK, PCI_DEVICE_RTL8139,
                  &bus, &slot, &func)) return false;

    uint32_t bar = pci_read32(bus, slot, func, 0x10);
    if (!(bar & 1)) return false;              /* need an I/O base */
    io_base = (uint16_t)(bar & 0xFFFC);
    pci_enable_io_busmaster(bus, slot, func);

    outb(io_base + R_CONFIG1, 0x00);           /* power on */
    outb(io_base + R_CMD, 0x10);               /* software reset */
    for (uint32_t n = 0; n < 1000000; n++) {
        if (!(inb(io_base + R_CMD) & 0x10)) break;
    }

    mac_read();
    my_ip = NET_DEFAULT_IP;
    my_mask = NET_DEFAULT_MASK;
    gateway = NET_DEFAULT_GW;

    memset(arp_cache, 0, sizeof(arp_cache));
    memset(socks, 0, sizeof(socks));
    rx_capr = 0;
    tx_next = 0;

    outw(io_base + R_IMR, 0x0000);             /* polling only */
    outw(io_base + R_ISR, 0xFFFF);
    outl(io_base + R_RBSTART, mm_virt_to_phys((uint32_t)rx_buffer));
    /* RCR: accept broadcast/multicast/physical/all, no wrap,
     * 32K ring (bits 11-12 = 2). */
    outl(io_base + R_RCR, 0x100F);
    outl(io_base + R_TCR, 0x0300);
    outw(io_base + R_CAPR, 0xFFF0);            /* internal read pointer = 0 */
    outb(io_base + R_CMD, 0x0C);               /* RE|TE */
    present = true;

    if (dhcp_run()) {
        serial_write("[net] DHCP configured\n");
    } else {
        serial_write("[net] DHCP unavailable, using static 10.0.2.15\n");
    }
    serial_write("[net] RTL8139 initialized\n");
    return true;
}

bool_t net_present(void) { return present; }
const char* net_driver_name(void) { return "RTL8139"; }
void net_get_mac(uint8_t out[6]) { memcpy(out, mac_addr, 6); }
uint32_t net_get_ip(void) { return my_ip; }
uint32_t net_get_gateway(void) { return gateway; }
