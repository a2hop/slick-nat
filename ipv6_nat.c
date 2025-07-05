// Define basic types first
typedef unsigned char __u8;
typedef unsigned short __u16;
typedef unsigned int __u32;
typedef unsigned long long __u64;
typedef signed char __s8;
typedef signed short __s16;
typedef signed int __s32;
typedef signed long long __s64;
typedef __u16 __be16;
typedef __u32 __be32;
typedef __u16 __sum16;
typedef __u32 __wsum;

// Now include BPF helpers
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#define ETH_P_IPV6 0x86DD
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define TC_ACT_OK 0

// Define necessary structures
struct ethhdr {
    unsigned char h_dest[6];
    unsigned char h_source[6];
    __be16 h_proto;
} __attribute__((packed));

struct in6_addr {
    union {
        __u8 u6_addr8[16];
        __be16 u6_addr16[8];
        __be32 u6_addr32[4];
    } in6_u;
};

struct ipv6hdr {
    __u8 priority:4, version:4;
    __u8 flow_lbl[3];
    __be16 payload_len;
    __u8 nexthdr;
    __u8 hop_limit;
    struct in6_addr saddr;
    struct in6_addr daddr;
};

struct tcphdr {
    __be16 source;
    __be16 dest;
    __be32 seq;
    __be32 ack_seq;
    __u16 res1:4, doff:4, fin:1, syn:1, rst:1, psh:1, ack:1, urg:1, ece:1, cwr:1;
    __be16 window;
    __sum16 check;
    __be16 urg_ptr;
};

struct udphdr {
    __be16 source;
    __be16 dest;
    __be16 len;
    __sum16 check;
};

// BPF map definitions
#define BPF_MAP_TYPE_LRU_HASH 9
#define BPF_ANY 0

struct __sk_buff {
    __u32 len;
    __u32 pkt_type;
    __u32 mark;
    __u32 queue_mapping;
    __u32 protocol;
    __u32 vlan_present;
    __u32 vlan_tci;
    __u32 vlan_proto;
    __u32 priority;
    __u32 ingress_ifindex;
    __u32 ifindex;
    __u32 tc_index;
    __u32 cb[5];
    __u32 hash;
    __u32 tc_classid;
    __u32 data;
    __u32 data_end;
    __u32 napi_id;
    __u32 family;
    __u32 remote_ip4;
    __u32 local_ip4;
    __u32 remote_ip6[4];
    __u32 local_ip6[4];
    __u32 remote_port;
    __u32 local_port;
    __u32 data_meta;
    __u32 flow_keys;
    __u64 tstamp;
    __u32 wire_len;
    __u32 gso_segs;
    __u32 sk;
};

// Define our IPv6 addresses
struct ipv6_addr {
    __u32 addr[4];
};

// Source network: 7607:af56:ff8:d12::/96
static const struct ipv6_addr src_network = {
    .addr = {bpf_htonl(0x7607af56), bpf_htonl(0x0ff80d12), 0, 0}
};

// NAT source network: 2607:f8f8:631:d601:2000:d12::/96
static const struct ipv6_addr nat_src_network = {
    .addr = {bpf_htonl(0x2607f8f8), bpf_htonl(0x0631d601), bpf_htonl(0x20000d12), 0}
};

// Target network: 7607:af56:abb1:c7::/96
static const struct ipv6_addr target_network = {
    .addr = {bpf_htonl(0x7607af56), bpf_htonl(0xabb100c7), 0, 0}
};

// Actual target network: 2a0a:8dc0:509b:21::/96
static const struct ipv6_addr actual_target_network = {
    .addr = {bpf_htonl(0x2a0a8dc0), bpf_htonl(0x509b0021), 0, 0}
};

// Connection tracking maps
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 65536);
    __type(key, struct ipv6_addr);  // NAT source address
    __type(value, struct ipv6_addr); // Original source address
} outbound_src_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 65536);
    __type(key, struct ipv6_addr);  // Actual target address
    __type(value, struct ipv6_addr); // Presented target address
} outbound_dst_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 65536);
    __type(key, struct ipv6_addr);  // Original source
    __type(value, struct ipv6_addr); // Actual target
} connection_map SEC(".maps");

static inline int ipv6_addr_equal(const struct ipv6_addr *a, const struct ipv6_addr *b) {
    return (a->addr[0] == b->addr[0] && a->addr[1] == b->addr[1] && 
            a->addr[2] == b->addr[2] && a->addr[3] == b->addr[3]);
}

static inline int ipv6_addr_in_network(const struct ipv6_addr *addr, 
                                      const struct ipv6_addr *network, int prefix_len) {
    int bytes = prefix_len / 32;
    int bits = prefix_len % 32;
    
    for (int i = 0; i < bytes; i++) {
        if (addr->addr[i] != network->addr[i])
            return 0;
    }
    
    if (bits > 0) {
        __u32 mask = bpf_htonl(0xFFFFFFFF << (32 - bits));
        if ((addr->addr[bytes] & mask) != (network->addr[bytes] & mask))
            return 0;
    }
    
    return 1;
}

static inline void copy_ipv6_addr(struct ipv6_addr *dst, const struct ipv6_addr *src) {
    dst->addr[0] = src->addr[0];
    dst->addr[1] = src->addr[1];
    dst->addr[2] = src->addr[2];
    dst->addr[3] = src->addr[3];
}

static inline void update_checksum(void *data, __u32 old_val, __u32 new_val) {
    __u32 *csum_ptr = (__u32 *)data;
    __u32 csum = ~bpf_ntohs(*(__u16 *)csum_ptr);
    
    csum = csum + (~old_val & 0xFFFF) + (new_val & 0xFFFF);
    csum = csum + (~(old_val >> 16) & 0xFFFF) + ((new_val >> 16) & 0xFFFF);
    csum = (csum & 0xFFFF) + (csum >> 16);
    csum = (csum & 0xFFFF) + (csum >> 16);
    
    *(__u16 *)csum_ptr = bpf_htons((__u16)~csum);
}

SEC("tc")
int ipv6_nat_egress(struct __sk_buff *skb) {
    void *data = (void *)(long)skb->data;
    void *data_end = (void *)(long)skb->data_end;
    
    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return TC_ACT_OK;
    
    if (eth->h_proto != bpf_htons(ETH_P_IPV6))
        return TC_ACT_OK;
    
    struct ipv6hdr *ip6 = (void *)(eth + 1);
    if ((void *)(ip6 + 1) > data_end)
        return TC_ACT_OK;
    
    struct ipv6_addr src_addr = {
        .addr = {ip6->saddr.in6_u.u6_addr32[0], ip6->saddr.in6_u.u6_addr32[1],
                 ip6->saddr.in6_u.u6_addr32[2], ip6->saddr.in6_u.u6_addr32[3]}
    };
    
    struct ipv6_addr dst_addr = {
        .addr = {ip6->daddr.in6_u.u6_addr32[0], ip6->daddr.in6_u.u6_addr32[1],
                 ip6->daddr.in6_u.u6_addr32[2], ip6->daddr.in6_u.u6_addr32[3]}
    };
    
    // Check if source is from our network (7607:af56:ff8:d12::/96) going to target (7607:af56:abb1:c7::/96)
    if (ipv6_addr_in_network(&src_addr, &src_network, 96) && 
        ipv6_addr_in_network(&dst_addr, &target_network, 96)) {
        
        // SNAT: Change source to NAT network (2607:f8f8:631:d601:2000:d12::/96)
        struct ipv6_addr new_src = nat_src_network;
        new_src.addr[3] = src_addr.addr[3]; // Keep the host part
        
        // DNAT: Change destination to actual target network (2a0a:8dc0:509b:21::/96)
        struct ipv6_addr new_dst = actual_target_network;
        new_dst.addr[3] = dst_addr.addr[3]; // Keep the host part
        
        // Store mappings for return traffic
        bpf_map_update_elem(&outbound_src_map, &new_src, &src_addr, BPF_ANY);
        bpf_map_update_elem(&outbound_dst_map, &new_dst, &dst_addr, BPF_ANY);
        bpf_map_update_elem(&connection_map, &src_addr, &new_dst, BPF_ANY);
        
        // Update addresses
        copy_ipv6_addr((struct ipv6_addr *)&ip6->saddr, &new_src);
        copy_ipv6_addr((struct ipv6_addr *)&ip6->daddr, &new_dst);
        
        // Update checksums
        if (ip6->nexthdr == IPPROTO_TCP) {
            struct tcphdr *tcp = (void *)(ip6 + 1);
            if ((void *)(tcp + 1) <= data_end) {
                for (int i = 0; i < 4; i++) {
                    update_checksum(&tcp->check, src_addr.addr[i], new_src.addr[i]);
                    update_checksum(&tcp->check, dst_addr.addr[i], new_dst.addr[i]);
                }
            }
        } else if (ip6->nexthdr == IPPROTO_UDP) {
            struct udphdr *udp = (void *)(ip6 + 1);
            if ((void *)(udp + 1) <= data_end && udp->check != 0) {
                for (int i = 0; i < 4; i++) {
                    update_checksum(&udp->check, src_addr.addr[i], new_src.addr[i]);
                    update_checksum(&udp->check, dst_addr.addr[i], new_dst.addr[i]);
                }
            }
        }
    }
    
    return TC_ACT_OK;
}

SEC("tc")
int ipv6_nat_ingress(struct __sk_buff *skb) {
    void *data = (void *)(long)skb->data;
    void *data_end = (void *)(long)skb->data_end;
    
    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return TC_ACT_OK;
    
    if (eth->h_proto != bpf_htons(ETH_P_IPV6))
        return TC_ACT_OK;
    
    struct ipv6hdr *ip6 = (void *)(eth + 1);
    if ((void *)(ip6 + 1) > data_end)
        return TC_ACT_OK;
    
    struct ipv6_addr src_addr = {
        .addr = {ip6->saddr.in6_u.u6_addr32[0], ip6->saddr.in6_u.u6_addr32[1],
                 ip6->saddr.in6_u.u6_addr32[2], ip6->saddr.in6_u.u6_addr32[3]}
    };
    
    struct ipv6_addr dst_addr = {
        .addr = {ip6->daddr.in6_u.u6_addr32[0], ip6->daddr.in6_u.u6_addr32[1],
                 ip6->daddr.in6_u.u6_addr32[2], ip6->daddr.in6_u.u6_addr32[3]}
    };
    
    // Check if this is return traffic from actual target network (2a0a:8dc0:509b:21::/96)
    // to NAT source network (2607:f8f8:631:d601:2000:d12::/96)
    if (ipv6_addr_in_network(&src_addr, &actual_target_network, 96) && 
        ipv6_addr_in_network(&dst_addr, &nat_src_network, 96)) {
        
        // Look up original mappings
        struct ipv6_addr *orig_src = bpf_map_lookup_elem(&outbound_src_map, &dst_addr);
        struct ipv6_addr *orig_dst = bpf_map_lookup_elem(&outbound_dst_map, &src_addr);
        
        if (orig_src && orig_dst) {
            // Reverse SNAT: Change destination back to original source
            copy_ipv6_addr((struct ipv6_addr *)&ip6->daddr, orig_src);
            
            // Reverse DNAT: Change source back to presented target
            copy_ipv6_addr((struct ipv6_addr *)&ip6->saddr, orig_dst);
            
            // Update checksums
            if (ip6->nexthdr == IPPROTO_TCP) {
                struct tcphdr *tcp = (void *)(ip6 + 1);
                if ((void *)(tcp + 1) <= data_end) {
                    for (int i = 0; i < 4; i++) {
                        update_checksum(&tcp->check, dst_addr.addr[i], orig_src->addr[i]);
                        update_checksum(&tcp->check, src_addr.addr[i], orig_dst->addr[i]);
                    }
                }
            } else if (ip6->nexthdr == IPPROTO_UDP) {
                struct udphdr *udp = (void *)(ip6 + 1);
                if ((void *)(udp + 1) <= data_end && udp->check != 0) {
                    for (int i = 0; i < 4; i++) {
                        update_checksum(&udp->check, dst_addr.addr[i], orig_src->addr[i]);
                        update_checksum(&udp->check, src_addr.addr[i], orig_dst->addr[i]);
                    }
                }
            }
        }
    }
    
    return TC_ACT_OK;
}

char _license[] SEC("license") = "GPL";
