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
} net_sock_t;

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

/* ---------------- helpers ---------------- */

static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put16be(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }

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
    if (dst != my_ip && dst != 0xFFFFFFFFu) return;

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
        udp_deliver(dport, sport, src, payload + 8, ulen - 8);
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

/* Send one IPv4 packet carrying `payload`. dst may be off-subnet, in which
 * case the frame goes to the gateway but the IP header keeps dst. */
static bool_t net_send_ip(uint8_t proto, uint32_t dst,
                          const uint8_t* payload, uint32_t plen,
                          uint16_t src_port, uint16_t dst_port) {
    if (!present) return false;
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
    uint32_t tot = 20 + 8 * (proto == 17 ? 1 : 0) + plen;
    memset(ip, 0, 20);
    ip[0] = 0x45;
    ip[8] = 64;
    ip[9] = proto;
    memcpy(ip + 12, &my_ip, 4);
    memcpy(ip + 16, &dst, 4);
    put16be(ip + 2, (uint16_t)tot);
    put16be(ip + 4, 0x1234);                /* id */
    put16be(ip + 10, net_cksum(ip, 20, 0));

    const uint8_t* body;
    if (proto == 17) {
        uint8_t* udp = ip + 20;
        put16be(udp + 0, src_port);
        put16be(udp + 2, dst_port);
        put16be(udp + 4, (uint16_t)(8 + plen));
        put16be(udp + 6, 0);                /* checksum optional in IPv4 */
        body = udp + 8;
    } else {
        body = ip + 20;
    }
    if (plen) memcpy((void*)body, payload, plen);

    net_send_frame(f, 14 + tot);
    return true;
}

/* ---------------- sockets ---------------- */

bool_t net_socket_ok(int id) {
    return id >= 0 && id < NET_FD_MAX - NET_FD_BASE && socks[id].used;
}

int net_socket(int type, int proto) {
    if (!present) return -1;
    if (type != NET_SOCK_DGRAM && type != NET_SOCK_RAW) return -2;
    if (type == NET_SOCK_RAW && proto == 0) return -2;
    for (int i = 0; i < NET_FD_MAX - NET_FD_BASE; i++) {
        if (socks[i].used) continue;
        memset(&socks[i], 0, sizeof(socks[i]));
        socks[i].used = true;
        socks[i].type = type;
        socks[i].proto = proto ? proto : (type == NET_SOCK_DGRAM ? 17 : 1);
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
    return 0;
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
    if (!net_send_ip(proto, dst, (const uint8_t*)data, len,
                     s->lport, dport)) return -1;
    return (int)len;
}

int net_recvfrom(int id, void* buf, uint32_t maxlen,
                 uint32_t* ip, uint16_t* port) {
    if (!net_socket_ok(id)) return -1;
    if (!present) return -1;
    net_sock_t* s = &socks[id];
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
    memset(&socks[id], 0, sizeof(socks[id]));
    return 0;
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
    serial_write("[net] RTL8139 initialized\n");
    return true;
}

bool_t net_present(void) { return present; }
const char* net_driver_name(void) { return "RTL8139"; }
void net_get_mac(uint8_t out[6]) { memcpy(out, mac_addr, 6); }
uint32_t net_get_ip(void) { return my_ip; }
uint32_t net_get_gateway(void) { return gateway; }
