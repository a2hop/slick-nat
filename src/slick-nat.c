#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv6.h>
#include <linux/ipv6.h>
#include <linux/udp.h>
#include <linux/tcp.h>
#include <linux/icmpv6.h>
#include <net/ipv6.h>
#include <net/ip6_route.h>
#include <net/dst.h>
#include <linux/netdevice.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/inet.h>
#include <linux/radix-tree.h>
#include <net/addrconf.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include "ndp.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lukasz Xu-Kafarski");
MODULE_DESCRIPTION("Slick NAT - Bidirectional IPv6 NAT Kernel Module");
MODULE_VERSION("0.0.3");

#define PACKET_MARK 0xDEADBEEF
#define PROC_FILENAME "slick_nat_mappings"
#define PROC_BATCH_FILENAME "slick_nat_batch"

// Per-namespace data structure
struct slick_nat_net {
    struct list_head mapping_list;
    spinlock_t mapping_lock;
    struct proc_dir_entry *proc_entry;
    struct proc_dir_entry *proc_batch_entry;
    struct radix_tree_root internal_tree;  // For internal prefix lookups
    struct radix_tree_root external_tree;  // For external prefix lookups
};

// Dynamic mapping structure
struct nat_mapping {
    struct list_head list;
    char interface[IFNAMSIZ];
    struct in6_addr internal_prefix;
    struct in6_addr external_prefix;
    int prefix_len;
    unsigned long internal_key;  // Radix tree key for internal prefix
    unsigned long external_key;  // Radix tree key for external prefix
};

// Batch operation structure
struct batch_operation {
    char operation[8];  // "add", "del", or "drop"
    char interface[IFNAMSIZ];
    char internal_prefix[64];
    char external_prefix[64];
};

static unsigned int slick_nat_net_id __read_mostly;

static struct slick_nat_net *slick_nat_pernet(struct net *net)
{
    return net_generic(net, slick_nat_net_id);
}

static bool compare_prefix_with_len(const struct in6_addr *addr, const struct in6_addr *prefix, int prefix_len) {
    int bytes = prefix_len / 8;
    int bits = prefix_len % 8;
    int i;
    
    for (i = 0; i < bytes; i++) {
        if (addr->s6_addr[i] != prefix->s6_addr[i])
            return false;
    }
    
    if (bits > 0 && i < 16) {
        unsigned char mask = (0xFF << (8 - bits)) & 0xFF;
        if ((addr->s6_addr[i] & mask) != (prefix->s6_addr[i] & mask))
            return false;
    }
    
    return true;
}

static void remap_address_with_len(struct in6_addr *addr, const struct in6_addr *new_prefix, int prefix_len) {
    int bytes = prefix_len / 8;
    int bits = prefix_len % 8;
    int i;
    
    for (i = 0; i < bytes && i < 16; i++) {
        addr->s6_addr[i] = new_prefix->s6_addr[i];
    }
    
    if (bits > 0 && i < 16) {
        unsigned char mask = (0xFF << (8 - bits)) & 0xFF;
        addr->s6_addr[i] = (new_prefix->s6_addr[i] & mask) | (addr->s6_addr[i] & ~mask);
    }
}

// Generate a radix tree key from IPv6 prefix - optimized for tail segment matching
static unsigned long generate_radix_key(const struct in6_addr *prefix, int prefix_len) {
    unsigned long key = 0;
    int i;
    
    // For longer prefixes (/80, /96, /112), use the tail segments for better distribution
    if (prefix_len >= 80) {
        // Use bytes 8-15 (tail 64 bits) for longer prefixes
        for (i = 8; i < 16; i++) {
            key = (key << 8) | prefix->s6_addr[i];
        }
    } else {
        // Use bytes 0-7 (head 64 bits) for shorter prefixes
        for (i = 0; i < 8; i++) {
            key = (key << 8) | prefix->s6_addr[i];
        }
    }
    
    // Include prefix length in the key to handle overlapping prefixes
    key = (key << 8) | (prefix_len & 0xFF);
    
    return key;
}

static struct nat_mapping *find_mapping_by_internal_fast(struct net *net, const struct in6_addr *addr) {
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    struct nat_mapping *mapping;
    unsigned long key;
    void **slot;
    struct radix_tree_iter iter;
    
    // Try exact match first with common prefix lengths - prioritize tail segments
    int common_lens[] = {112, 96, 80, 64, 48, 56, 32, 128, 0};
    int i;
    
    for (i = 0; common_lens[i] != 0; i++) {
        key = generate_radix_key(addr, common_lens[i]);
        mapping = radix_tree_lookup(&sn_net->internal_tree, key);
        if (mapping && compare_prefix_with_len(addr, &mapping->internal_prefix, mapping->prefix_len)) {
            return mapping;
        }
    }
    
    // Fallback to radix tree iteration for less common prefix lengths
    radix_tree_for_each_slot(slot, &sn_net->internal_tree, &iter, 0) {
        mapping = radix_tree_deref_slot(slot);
        if (mapping && compare_prefix_with_len(addr, &mapping->internal_prefix, mapping->prefix_len)) {
            return mapping;
        }
    }
    
    return NULL;
}

static struct nat_mapping *find_mapping_by_external_fast(struct net *net, const struct in6_addr *addr, const char *ifname) {
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    struct nat_mapping *mapping;
    unsigned long key;
    void **slot;
    struct radix_tree_iter iter;
    
    // Try exact match first with common prefix lengths - prioritize tail segments
    int common_lens[] = {112, 96, 80, 64, 48, 56, 32, 128, 0};
    int i;
    
    for (i = 0; common_lens[i] != 0; i++) {
        key = generate_radix_key(addr, common_lens[i]);
        mapping = radix_tree_lookup(&sn_net->external_tree, key);
        if (mapping && 
            strncmp(mapping->interface, ifname, IFNAMSIZ) == 0 &&
            compare_prefix_with_len(addr, &mapping->external_prefix, mapping->prefix_len)) {
            return mapping;
        }
    }
    
    // Fallback to radix tree iteration for interface-specific lookups
    radix_tree_for_each_slot(slot, &sn_net->external_tree, &iter, 0) {
        mapping = radix_tree_deref_slot(slot);
        if (mapping && 
            strncmp(mapping->interface, ifname, IFNAMSIZ) == 0 &&
            compare_prefix_with_len(addr, &mapping->external_prefix, mapping->prefix_len)) {
            return mapping;
        }
    }
    
    return NULL;
}

// Compatibility wrappers for existing code
static struct nat_mapping *find_mapping_by_internal(struct net *net, const struct in6_addr *addr) {
    return find_mapping_by_internal_fast(net, addr);
}

static struct nat_mapping *find_mapping_by_external(struct net *net, const struct in6_addr *addr, const char *ifname) {
    return find_mapping_by_external_fast(net, addr, ifname);
}

static void update_csum(struct sk_buff *skb, struct ipv6hdr *iph, const struct in6_addr *old_addr, const struct in6_addr *new_addr) {
    struct tcphdr *tcph;
    struct udphdr *udph;
    struct icmp6hdr *icmp6h;
    int i;

    if (iph->nexthdr == IPPROTO_TCP) {
        tcph = tcp_hdr(skb);
        for (i = 0; i < 4; i++) {
            inet_proto_csum_replace4(&tcph->check, skb, old_addr->s6_addr32[i], new_addr->s6_addr32[i], true);
        }
    } else if (iph->nexthdr == IPPROTO_UDP) {
        udph = udp_hdr(skb);
        if (udph->check != 0) {
            for (i = 0; i < 4; i++) {
                inet_proto_csum_replace4(&udph->check, skb, old_addr->s6_addr32[i], new_addr->s6_addr32[i], true);
            }
        }
    } else if (iph->nexthdr == IPPROTO_ICMPV6) {
        icmp6h = icmp6_hdr(skb);
        for (i = 0; i < 4; i++) {
            inet_proto_csum_replace4(&icmp6h->icmp6_cksum, skb, old_addr->s6_addr32[i], new_addr->s6_addr32[i], true);
        }
    }
}

static bool handle_icmp_error_embedded_packet(struct sk_buff *skb, struct net *net, bool is_external_if, const char *ifname) {
    struct ipv6hdr *outer_iph, *embedded_iph;
    struct icmp6hdr *icmp6h;
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    struct nat_mapping *mapping_src = NULL, *mapping_dst = NULL;
    struct in6_addr old_saddr, old_daddr;
    unsigned long flags;
    bool translated = false;
    
    outer_iph = ipv6_hdr(skb);
    icmp6h = icmp6_hdr(skb);
    
    // Check if we have enough data for the embedded packet
    if (skb->len < sizeof(struct ipv6hdr) + sizeof(struct icmp6hdr) + sizeof(struct ipv6hdr))
        return false;
    
    // Get the embedded IPv6 header
    embedded_iph = (struct ipv6hdr *)((u8 *)icmp6h + sizeof(struct icmp6hdr));
    
    // Find mappings for the embedded packet
    spin_lock_irqsave(&sn_net->mapping_lock, flags);
    if (is_external_if) {
        // For external interfaces, we need to translate from external to internal
        mapping_src = find_mapping_by_external(net, &embedded_iph->saddr, ifname);
        mapping_dst = find_mapping_by_external(net, &embedded_iph->daddr, ifname);
    } else {
        // For internal interfaces, we need to translate from internal to external
        mapping_src = find_mapping_by_internal(net, &embedded_iph->saddr);
        mapping_dst = find_mapping_by_internal(net, &embedded_iph->daddr);
    }
    spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
    
    // Translate the embedded packet addresses
    if (mapping_src && compare_prefix_with_len(&embedded_iph->saddr, 
                                              is_external_if ? &mapping_src->external_prefix : &mapping_src->internal_prefix, 
                                              mapping_src->prefix_len)) {
        old_saddr = embedded_iph->saddr;
        remap_address_with_len(&embedded_iph->saddr, 
                              is_external_if ? &mapping_src->internal_prefix : &mapping_src->external_prefix, 
                              mapping_src->prefix_len);
        
        // Update the outer ICMPv6 checksum for the embedded source address change
        int i;
        for (i = 0; i < 4; i++) {
            inet_proto_csum_replace4(&icmp6h->icmp6_cksum, skb, 
                                   old_saddr.s6_addr32[i], embedded_iph->saddr.s6_addr32[i], true);
        }
        translated = true;
    }
    
    if (mapping_dst && compare_prefix_with_len(&embedded_iph->daddr,
                                              is_external_if ? &mapping_dst->external_prefix : &mapping_dst->internal_prefix,
                                              mapping_dst->prefix_len)) {
        old_daddr = embedded_iph->daddr;
        remap_address_with_len(&embedded_iph->daddr,
                              is_external_if ? &mapping_dst->internal_prefix : &mapping_dst->external_prefix,
                              mapping_dst->prefix_len);
        
        // Update the outer ICMPv6 checksum for the embedded destination address change
        int i;
        for (i = 0; i < 4; i++) {
            inet_proto_csum_replace4(&icmp6h->icmp6_cksum, skb,
                                   old_daddr.s6_addr32[i], embedded_iph->daddr.s6_addr32[i], true);
        }
        translated = true;
    }
    
    return translated;
}

static bool is_external_interface(struct net *net, const char *ifname) {
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    struct nat_mapping *mapping;
    unsigned long flags;
    
    spin_lock_irqsave(&sn_net->mapping_lock, flags);
    list_for_each_entry(mapping, &sn_net->mapping_list, list) {
        if (strncmp(mapping->interface, ifname, IFNAMSIZ) == 0) {
            spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
            return true;
        }
    }
    spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
    return false;
}

static void send_time_exceeded(struct sk_buff *orig_skb, struct net_device *dev, 
                              const struct in6_addr *src_addr) {
    struct sk_buff *reply_skb;
    struct ipv6hdr *orig_iph, *reply_iph;
    struct icmp6hdr *icmp6h;
    int total_len;
    int orig_len;
    struct ethhdr *eth;
    
    orig_iph = ipv6_hdr(orig_skb);
    
    // For MTR, we need to include enough of the original packet for proper matching
    // Include IPv6 header + at least 8 bytes of transport header (UDP/ICMP)
    orig_len = min_t(int, orig_skb->len - sizeof(struct ethhdr), 
                    1280 - sizeof(struct ipv6hdr) - sizeof(struct icmp6hdr));
    
    // Ensure we have at least IPv6 header + 8 bytes
    if (orig_len < sizeof(struct ipv6hdr) + 8)
        orig_len = sizeof(struct ipv6hdr) + 8;
    
    total_len = sizeof(struct ipv6hdr) + sizeof(struct icmp6hdr) + orig_len;
    
    reply_skb = alloc_skb(total_len + LL_MAX_HEADER, GFP_ATOMIC);
    if (!reply_skb) {
        pr_err("Slick NAT: Failed to allocate skb for time exceeded\n");
        return;
    }
    
    skb_reserve(reply_skb, LL_MAX_HEADER);
    skb_put(reply_skb, total_len);
    skb_reset_network_header(reply_skb);
    
    reply_iph = ipv6_hdr(reply_skb);
    reply_iph->version = 6;
    reply_iph->priority = 0;
    memset(reply_iph->flow_lbl, 0, sizeof(reply_iph->flow_lbl));
    reply_iph->payload_len = htons(sizeof(struct icmp6hdr) + orig_len);
    reply_iph->nexthdr = IPPROTO_ICMPV6;
    reply_iph->hop_limit = 64;
    reply_iph->saddr = *src_addr;
    reply_iph->daddr = orig_iph->saddr;
    
    skb_set_transport_header(reply_skb, sizeof(struct ipv6hdr));
    icmp6h = (struct icmp6hdr *)skb_transport_header(reply_skb);
    
    icmp6h->icmp6_type = ICMPV6_TIME_EXCEED;
    icmp6h->icmp6_code = ICMPV6_EXC_HOPLIMIT;
    icmp6h->icmp6_cksum = 0;
    icmp6h->icmp6_unused = 0;
    
    // Copy the original packet starting from the IPv6 header
    if (skb_copy_bits(orig_skb, skb_network_offset(orig_skb), 
                      (u8 *)icmp6h + sizeof(struct icmp6hdr), orig_len) < 0) {
        kfree_skb(reply_skb);
        pr_err("Slick NAT: Failed to copy original packet data\n");
        return;
    }
    
    // Calculate checksum
    icmp6h->icmp6_cksum = csum_ipv6_magic(&reply_iph->saddr, &reply_iph->daddr,
                                         ntohs(reply_iph->payload_len), IPPROTO_ICMPV6,
                                         csum_partial(icmp6h, ntohs(reply_iph->payload_len), 0));
    
    // Set up Ethernet header
    skb_push(reply_skb, ETH_HLEN);
    skb_reset_mac_header(reply_skb);
    eth = eth_hdr(reply_skb);
    
    // Get the source MAC from the original packet and swap src/dst
    if (skb_mac_header_was_set(orig_skb)) {
        memcpy(eth->h_dest, eth_hdr(orig_skb)->h_source, ETH_ALEN);
        memcpy(eth->h_source, dev->dev_addr, ETH_ALEN);
    } else {
        // Fallback - use device's MAC as source
        memcpy(eth->h_source, dev->dev_addr, ETH_ALEN);
        memset(eth->h_dest, 0xff, ETH_ALEN); // broadcast
    }
    eth->h_proto = htons(ETH_P_IPV6);
    
    reply_skb->dev = dev;
    reply_skb->protocol = htons(ETH_P_IPV6);
    reply_skb->mark = PACKET_MARK;
    reply_skb->pkt_type = PACKET_OUTGOING;
    
    // Send via dev_queue_xmit
    if (dev_queue_xmit(reply_skb) < 0) {
        pr_err("Slick NAT: Failed to send time exceeded\n");
    }
}

static struct in6_addr *get_interface_global_addr(struct net_device *dev) {
    struct inet6_dev *idev;
    struct inet6_ifaddr *ifp;
    struct in6_addr *addr = NULL;
    
    rcu_read_lock();
    idev = __in6_dev_get(dev);
    if (idev) {
        list_for_each_entry(ifp, &idev->addr_list, if_list) {
            if (ifp->scope == RT_SCOPE_UNIVERSE && 
                !(ifp->flags & (IFA_F_TENTATIVE | IFA_F_DEPRECATED)) &&
                !(ipv6_addr_type(&ifp->addr) & IPV6_ADDR_LINKLOCAL)) {
                addr = &ifp->addr;
                break;
            }
        }
    }
    rcu_read_unlock();
    
    return addr;
}

static unsigned int nat_hook_func(void *priv, struct sk_buff *skb, const struct nf_hook_state *state) {
    struct ipv6hdr *iph;
    struct in6_addr old_saddr, old_daddr;
    bool translated = false;
    bool is_icmp_error = false;
    bool is_external_if;
    struct nat_mapping *mapping_src = NULL, *mapping_dst = NULL;
    struct slick_nat_net *sn_net;
    unsigned long flags;
    const char *ifname;
    struct net *net = state->net;

    if (!skb) return NF_ACCEPT;
    
    if (skb->mark == PACKET_MARK) return NF_ACCEPT;

    iph = ipv6_hdr(skb);
    if (!iph) return NF_ACCEPT;

    sn_net = slick_nat_pernet(net);
    
    // Use the appropriate interface based on hook point
    ifname = (state->in) ? state->in->name : 
             (state->out) ? state->out->name : NULL;
    
    if (!ifname) return NF_ACCEPT;
    
    is_external_if = is_external_interface(net, ifname);

    // Check for hop limit expiration on external interface
    if (state->in && is_external_if && iph->hop_limit <= 1) {
        // Get the interface's actual global IPv6 address
        struct in6_addr *interface_addr = get_interface_global_addr(state->in);
        
        if (interface_addr) {
            send_time_exceeded(skb, state->in, interface_addr);
            return NF_DROP;
        }
    }

    // Don't skip link-local packets completely - we need to process some of them
    // Skip only if both source AND destination are link-local
    if ((ipv6_addr_type(&iph->saddr) & IPV6_ADDR_LINKLOCAL) &&
        (ipv6_addr_type(&iph->daddr) & IPV6_ADDR_LINKLOCAL)) {
        return NF_ACCEPT;
    }

    // Look up dynamic mappings using fast radix tree lookup
    spin_lock_irqsave(&sn_net->mapping_lock, flags);
    if (is_external_if) {
        // For external interfaces, use interface-specific lookups
        mapping_src = find_mapping_by_external_fast(net, &iph->saddr, ifname);
        mapping_dst = find_mapping_by_external_fast(net, &iph->daddr, ifname);
    } else {
        // For internal interfaces, use generic lookups (no interface restriction)
        mapping_src = find_mapping_by_internal_fast(net, &iph->saddr);
        mapping_dst = find_mapping_by_internal_fast(net, &iph->daddr);
    }
    spin_unlock_irqrestore(&sn_net->mapping_lock, flags);

    if (iph->nexthdr == IPPROTO_ICMPV6) {
        if (skb->len < sizeof(struct ipv6hdr) + sizeof(struct icmp6hdr))
            return NF_ACCEPT;
        
        struct icmp6hdr *icmp6h = icmp6_hdr(skb);
        if (!icmp6h) return NF_ACCEPT;
        
        if (icmp6h->icmp6_type == NDISC_NEIGHBOUR_SOLICITATION ||
            icmp6h->icmp6_type == NDISC_NEIGHBOUR_ADVERTISEMENT ||
            icmp6h->icmp6_type == NDISC_ROUTER_SOLICITATION ||
            icmp6h->icmp6_type == NDISC_ROUTER_ADVERTISEMENT ||
            icmp6h->icmp6_type == NDISC_REDIRECT) {
            
            // For neighbor solicitations, check the target address in the message
            if (icmp6h->icmp6_type == NDISC_NEIGHBOUR_SOLICITATION) {
                if (skb->len >= sizeof(struct ipv6hdr) + sizeof(struct nd_msg)) {
                    struct nd_msg *ns_msg;
                    struct nat_mapping *mapping;
                    unsigned long flags;
                    
                    // Make sure we can access the data
                    if (skb_linearize(skb) < 0) {
                        return NF_ACCEPT;
                    }
                    
                    iph = ipv6_hdr(skb);
                    icmp6h = icmp6_hdr(skb);
                    ns_msg = (struct nd_msg *)icmp6h;
                    
                    // Check if the target matches any of our prefixes
                    spin_lock_irqsave(&sn_net->mapping_lock, flags);
                    list_for_each_entry(mapping, &sn_net->mapping_list, list) {
                        // For external interfaces, check external prefixes
                        if (is_external_if && strncmp(mapping->interface, ifname, IFNAMSIZ) == 0 &&
                            compare_prefix_with_len(&ns_msg->target, &mapping->external_prefix, mapping->prefix_len)) {
                            spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
                            
                            // Send neighbor advertisement
                            send_neighbor_advertisement(skb, state, &ns_msg->target, &iph->saddr);
                            
                            // Don't process this packet further
                            return NF_DROP;
                        }
                        // For internal interfaces, check external prefixes of ANY interface
                        else if (!is_external_if &&
                                compare_prefix_with_len(&ns_msg->target, &mapping->external_prefix, mapping->prefix_len)) {
                            spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
                            
                            // Send neighbor advertisement for the translated address
                            send_neighbor_advertisement(skb, state, &ns_msg->target, &iph->saddr);
                            
                            // Don't process this packet further
                            return NF_DROP;
                        }
                    }
                    spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
                }
            }
            
            // Allow other neighbor discovery packets through
            return NF_ACCEPT;
        }
        
        if (icmp6h->icmp6_type == ICMPV6_DEST_UNREACH ||
            icmp6h->icmp6_type == ICMPV6_PKT_TOOBIG ||
            icmp6h->icmp6_type == ICMPV6_TIME_EXCEED ||
            icmp6h->icmp6_type == ICMPV6_PARAMPROB) {
            is_icmp_error = true;
        }
        
        if (!is_icmp_error && 
            icmp6h->icmp6_type != ICMPV6_ECHO_REQUEST &&
            icmp6h->icmp6_type != ICMPV6_ECHO_REPLY) {
            return NF_ACCEPT;
        }
    }

    // Handle ingress traffic (packets coming into the interface)
    if (state->in && is_external_if) {
        if (is_icmp_error) {
            // Handle ICMP error messages from external interface
            if (mapping_dst && compare_prefix_with_len(&iph->daddr, &mapping_dst->external_prefix, mapping_dst->prefix_len)) {
                if (skb_ensure_writable(skb, skb->len)) return NF_DROP;
                iph = ipv6_hdr(skb);

                // Translate the embedded packet first
                if (handle_icmp_error_embedded_packet(skb, net, is_external_if, ifname)) {
                    translated = true;
                }

                // Then translate the outer header
                old_daddr = iph->daddr;
                remap_address_with_len(&iph->daddr, &mapping_dst->internal_prefix, mapping_dst->prefix_len);
                update_csum(skb, iph, &old_daddr, &iph->daddr);
                translated = true;
            }
        } else {
            // Handle regular traffic from external interface
            if (mapping_dst && compare_prefix_with_len(&iph->daddr, &mapping_dst->external_prefix, mapping_dst->prefix_len)) {
                // Check hop limit BEFORE translation for packets destined to our NAT'd addresses
                if (iph->hop_limit <= 1) {
                    struct in6_addr *interface_addr = get_interface_global_addr(state->in);
                    if (interface_addr) {
                        send_time_exceeded(skb, state->in, interface_addr);
                        return NF_DROP;
                    }
                }

                // Find corresponding source mapping for the response path
                struct nat_mapping *src_mapping = NULL;
                spin_lock_irqsave(&sn_net->mapping_lock, flags);
                list_for_each_entry(src_mapping, &sn_net->mapping_list, list) {
                    if (strncmp(src_mapping->interface, ifname, IFNAMSIZ) == 0 &&
                        compare_prefix_with_len(&iph->saddr, &src_mapping->external_prefix, src_mapping->prefix_len)) {
                        break;
                    }
                }
                spin_unlock_irqrestore(&sn_net->mapping_lock, flags);

                if (skb_ensure_writable(skb, skb->len)) return NF_DROP;
                iph = ipv6_hdr(skb);

                old_daddr = iph->daddr;
                remap_address_with_len(&iph->daddr, &mapping_dst->internal_prefix, mapping_dst->prefix_len);
                update_csum(skb, iph, &old_daddr, &iph->daddr);

                if (src_mapping) {
                    old_saddr = iph->saddr;
                    remap_address_with_len(&iph->saddr, &src_mapping->internal_prefix, src_mapping->prefix_len);
                    update_csum(skb, iph, &old_saddr, &iph->saddr);
                }
                translated = true;
            } else if (mapping_dst && compare_prefix_with_len(&iph->daddr, &mapping_dst->external_prefix, mapping_dst->prefix_len)) {
                return NF_DROP;
            }
        }
    } 
    // Handle egress traffic (packets going out of internal interfaces)
    else if (state->in && !is_external_if) {
        if (is_icmp_error) {
            // Handle ICMP error messages from internal interface
            if (mapping_src && mapping_dst &&
                compare_prefix_with_len(&iph->saddr, &mapping_src->internal_prefix, mapping_src->prefix_len) &&
                compare_prefix_with_len(&iph->daddr, &mapping_dst->internal_prefix, mapping_dst->prefix_len)) {
                
                if (skb_ensure_writable(skb, skb->len)) return NF_DROP;
                iph = ipv6_hdr(skb);

                // Translate the embedded packet first
                if (handle_icmp_error_embedded_packet(skb, net, is_external_if, ifname)) {
                    translated = true;
                }

                // Then translate the outer header
                old_saddr = iph->saddr;
                old_daddr = iph->daddr;
                
                remap_address_with_len(&iph->saddr, &mapping_src->external_prefix, mapping_src->prefix_len);
                remap_address_with_len(&iph->daddr, &mapping_dst->external_prefix, mapping_dst->prefix_len);
                
                update_csum(skb, iph, &old_saddr, &iph->saddr);
                update_csum(skb, iph, &old_daddr, &iph->daddr);
                translated = true;
            }
        } else {
            // Handle regular traffic from internal interface
            if (mapping_src && mapping_dst &&
                compare_prefix_with_len(&iph->saddr, &mapping_src->internal_prefix, mapping_src->prefix_len) &&
                compare_prefix_with_len(&iph->daddr, &mapping_dst->internal_prefix, mapping_dst->prefix_len)) {

                if (skb_ensure_writable(skb, skb->len)) return NF_DROP;
                iph = ipv6_hdr(skb);

                old_saddr = iph->saddr;
                old_daddr = iph->daddr;

                remap_address_with_len(&iph->saddr, &mapping_src->external_prefix, mapping_src->prefix_len);
                remap_address_with_len(&iph->daddr, &mapping_dst->external_prefix, mapping_dst->prefix_len);

                update_csum(skb, iph, &old_saddr, &iph->saddr);
                update_csum(skb, iph, &old_daddr, &iph->daddr);
                translated = true;
            }
        }
    }

    if (translated) {
        skb->mark = PACKET_MARK;
    }

    return NF_ACCEPT;
}

static unsigned int nat_post_hook_func(void *priv, struct sk_buff *skb, const struct nf_hook_state *state) {
    if (!skb || skb->mark != PACKET_MARK) return NF_ACCEPT;
    
    skb->mark = 0;
    return NF_ACCEPT;
}

static int mapping_show(struct seq_file *m, void *v) {
    struct net *net = m->private;
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    struct nat_mapping *mapping;
    unsigned long flags;
    
    seq_printf(m, "# IPv6 NAT Mappings (netns: %p)\n", net);
    seq_printf(m, "# Format: interface internal_prefix/len -> external_prefix/len\n\n");
    
    spin_lock_irqsave(&sn_net->mapping_lock, flags);
    list_for_each_entry(mapping, &sn_net->mapping_list, list) {
        seq_printf(m, "%s %pI6c/%d -> %pI6c/%d\n", 
                   mapping->interface,
                   &mapping->internal_prefix, mapping->prefix_len, 
                   &mapping->external_prefix, mapping->prefix_len);
    }
    spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
    
    return 0;
}

static int mapping_open(struct inode *inode, struct file *file) {
    return single_open(file, mapping_show, pde_data(inode));
}

static int parse_ipv6_prefix(const char *str, struct in6_addr *addr, int *prefix_len) {
    char buf[128];
    char *prefix_str;
    char *len_str;
    int ret;
    
    if (strlen(str) >= sizeof(buf))
        return -EINVAL;
    
    strcpy(buf, str);
    prefix_str = buf;
    
    len_str = strchr(buf, '/');
    if (!len_str)
        return -EINVAL;
    
    *len_str = '\0';
    len_str++;
    
    ret = in6_pton(prefix_str, -1, (u8 *)addr->s6_addr, -1, NULL);
    if (ret != 1)
        return -EINVAL;
    
    ret = kstrtoint(len_str, 10, prefix_len);
    if (ret < 0)
        return -EINVAL;
    
    if (*prefix_len < 0 || *prefix_len > 128)
        return -EINVAL;
    
    return 0;
}

static int add_mapping_internal_unlocked(struct net *net, const char *interface, 
                                        const struct in6_addr *internal_prefix, int internal_prefix_len,
                                        const struct in6_addr *external_prefix, int external_prefix_len) {
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    struct nat_mapping *mapping, *tmp;
    int ret;
    
    // Both prefixes must have the same length
    if (internal_prefix_len != external_prefix_len)
        return -EINVAL;
    
    // Check for duplicates first
    list_for_each_entry(tmp, &sn_net->mapping_list, list) {
        if (strncmp(tmp->interface, interface, IFNAMSIZ) == 0 &&
            ipv6_addr_equal(&tmp->internal_prefix, internal_prefix) && 
            tmp->prefix_len == internal_prefix_len) {
            return -EEXIST;
        }
    }
    
    mapping = kmalloc(sizeof(*mapping), GFP_ATOMIC);
    if (!mapping)
        return -ENOMEM;
    
    strncpy(mapping->interface, interface, IFNAMSIZ);
    mapping->interface[IFNAMSIZ-1] = '\0';
    mapping->internal_prefix = *internal_prefix;
    mapping->external_prefix = *external_prefix;
    mapping->prefix_len = internal_prefix_len;
    
    // Generate radix tree keys
    mapping->internal_key = generate_radix_key(internal_prefix, internal_prefix_len);
    mapping->external_key = generate_radix_key(external_prefix, external_prefix_len);
    
    // Add to radix trees with collision handling
    ret = radix_tree_insert(&sn_net->internal_tree, mapping->internal_key, mapping);
    if (ret == -EEXIST) {
        // Handle collision by trying alternative keys
        unsigned long alt_key = mapping->internal_key;
        int attempts = 0;
        do {
            alt_key = alt_key ^ (0x1UL << (attempts % 64));
            ret = radix_tree_insert(&sn_net->internal_tree, alt_key, mapping);
            attempts++;
        } while (ret == -EEXIST && attempts < 64);
        
        if (ret == 0) {
            mapping->internal_key = alt_key;
        }
    }
    
    if (ret == 0) {
        ret = radix_tree_insert(&sn_net->external_tree, mapping->external_key, mapping);
        if (ret == -EEXIST) {
            // Handle collision for external key
            unsigned long alt_key = mapping->external_key;
            int attempts = 0;
            do {
                alt_key = alt_key ^ (0x1UL << (attempts % 64));
                ret = radix_tree_insert(&sn_net->external_tree, alt_key, mapping);
                attempts++;
            } while (ret == -EEXIST && attempts < 64);
            
            if (ret == 0) {
                mapping->external_key = alt_key;
            } else {
                // If external insertion fails, remove from internal tree
                radix_tree_delete(&sn_net->internal_tree, mapping->internal_key);
            }
        }
    }
    
    if (ret == 0) {
        list_add_tail(&mapping->list, &sn_net->mapping_list);
    } else {
        kfree(mapping);
    }
    
    return ret;
}

static int del_mapping_internal_unlocked(struct net *net, const char *interface, 
                                        const struct in6_addr *internal_prefix, int internal_prefix_len) {
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    struct nat_mapping *mapping, *tmp;
    
    list_for_each_entry_safe(mapping, tmp, &sn_net->mapping_list, list) {
        if (strncmp(mapping->interface, interface, IFNAMSIZ) == 0 &&
            ipv6_addr_equal(&mapping->internal_prefix, internal_prefix) && 
            mapping->prefix_len == internal_prefix_len) {
            
            // Remove from radix trees
            radix_tree_delete(&sn_net->internal_tree, mapping->internal_key);
            radix_tree_delete(&sn_net->external_tree, mapping->external_key);
            
            list_del(&mapping->list);
            kfree(mapping);
            return 0;
        }
    }
    return -ENOENT;
}

static int drop_mappings_internal_unlocked(struct net *net, const char *interface) {
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    struct nat_mapping *mapping, *tmp;
    int dropped = 0;
    
    list_for_each_entry_safe(mapping, tmp, &sn_net->mapping_list, list) {
        // If interface is specified, only drop mappings for that interface
        if (interface && strncmp(mapping->interface, interface, IFNAMSIZ) != 0) {
            continue;
        }
        
        // Remove from radix trees
        radix_tree_delete(&sn_net->internal_tree, mapping->internal_key);
        radix_tree_delete(&sn_net->external_tree, mapping->external_key);
        
        list_del(&mapping->list);
        kfree(mapping);
        dropped++;
    }
    
    return dropped;
}

static int add_mapping_internal(struct net *net, const char *interface, 
                               const struct in6_addr *internal_prefix, int internal_prefix_len,
                               const struct in6_addr *external_prefix, int external_prefix_len) {
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    unsigned long flags;
    int ret;
    
    spin_lock_irqsave(&sn_net->mapping_lock, flags);
    ret = add_mapping_internal_unlocked(net, interface, internal_prefix, internal_prefix_len,
                                       external_prefix, external_prefix_len);
    spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
    
    return ret;
}

static int del_mapping_internal(struct net *net, const char *interface, 
                               const struct in6_addr *internal_prefix, int internal_prefix_len) {
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    unsigned long flags;
    int ret;
    
    spin_lock_irqsave(&sn_net->mapping_lock, flags);
    ret = del_mapping_internal_unlocked(net, interface, internal_prefix, internal_prefix_len);
    spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
    
    return ret;
}

static int drop_mappings_internal(struct net *net, const char *interface) {
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    unsigned long flags;
    int ret;
    
    spin_lock_irqsave(&sn_net->mapping_lock, flags);
    ret = drop_mappings_internal_unlocked(net, interface);
    spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
    
    return ret;
}

static int parse_batch_line(const char *line, struct batch_operation *op) {
    char *cmd, *interface, *arg1, *arg2;
    char buf[256];
    int len = strlen(line);
    
    if (len >= sizeof(buf) || len == 0)
        return -EINVAL;
    
    // Copy and remove newline
    strncpy(buf, line, len);
    buf[len] = '\0';
    if (len > 0 && buf[len-1] == '\n')
        buf[len-1] = '\0';
    
    // Skip empty lines and comments
    if (buf[0] == '\0' || buf[0] == '#')
        return -EAGAIN;
    
    cmd = buf;
    interface = strchr(buf, ' ');
    if (!interface)
        return -EINVAL;
    
    *interface = '\0';
    interface++;
    
    // Skip whitespace
    while (*interface == ' ' || *interface == '\t')
        interface++;
    
    // Copy operation and interface
    strncpy(op->operation, cmd, sizeof(op->operation) - 1);
    op->operation[sizeof(op->operation) - 1] = '\0';
    
    if (strcmp(cmd, "add") == 0) {
        arg1 = strchr(interface, ' ');
        if (!arg1)
            return -EINVAL;
        
        *arg1 = '\0';
        arg1++;
        
        // Skip whitespace
        while (*arg1 == ' ' || *arg1 == '\t')
            arg1++;
        
        arg2 = strchr(arg1, ' ');
        if (!arg2)
            return -EINVAL;
        
        *arg2 = '\0';
        arg2++;
        
        // Skip whitespace
        while (*arg2 == ' ' || *arg2 == '\t')
            arg2++;
        
        strncpy(op->interface, interface, IFNAMSIZ - 1);
        op->interface[IFNAMSIZ - 1] = '\0';
        strncpy(op->internal_prefix, arg1, sizeof(op->internal_prefix) - 1);
        op->internal_prefix[sizeof(op->internal_prefix) - 1] = '\0';
        strncpy(op->external_prefix, arg2, sizeof(op->external_prefix) - 1);
        op->external_prefix[sizeof(op->external_prefix) - 1] = '\0';
        
    } else if (strcmp(cmd, "del") == 0) {
        arg1 = strchr(interface, ' ');
        if (!arg1)
            return -EINVAL;
        
        *arg1 = '\0';
        arg1++;
        
        // Skip whitespace
        while (*arg1 == ' ' || *arg1 == '\t')
            arg1++;
        
        strncpy(op->interface, interface, IFNAMSIZ - 1);
        op->interface[IFNAMSIZ - 1] = '\0';
        strncpy(op->internal_prefix, arg1, sizeof(op->internal_prefix) - 1);
        op->internal_prefix[sizeof(op->internal_prefix) - 1] = '\0';
        op->external_prefix[0] = '\0';
        
    } else if (strcmp(cmd, "drop") == 0) {
        // For drop command, interface might be --all or a specific interface
        strncpy(op->interface, interface, IFNAMSIZ - 1);
        op->interface[IFNAMSIZ - 1] = '\0';
        op->internal_prefix[0] = '\0';
        op->external_prefix[0] = '\0';
        
    } else {
        return -EINVAL;
    }
    
    return 0;
}

static ssize_t batch_write(struct file *file, const char __user *buffer, size_t count, loff_t *pos) {
    struct net *net = pde_data(file_inode(file));
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    char *buf, *line, *next_line;
    struct batch_operation op;
    struct in6_addr internal_prefix, external_prefix;
    int internal_prefix_len, external_prefix_len;
    unsigned long flags;
    int ret = 0, processed = 0, errors = 0;
    
    if (count > 1024 * 1024) // 1MB limit
        return -EINVAL;
    
    buf = kmalloc(count + 1, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;
    
    if (copy_from_user(buf, buffer, count)) {
        kfree(buf);
        return -EFAULT;
    }
    
    buf[count] = '\0';
    
    // Process each line separately with proper locking
    line = buf;
    while (line && *line) {
        next_line = strchr(line, '\n');
        if (next_line) {
            *next_line = '\0';
            next_line++;
        }
        
        // Clear the operation structure
        memset(&op, 0, sizeof(op));
        
        ret = parse_batch_line(line, &op);
        if (ret == -EAGAIN) {
            // Skip empty lines/comments
            line = next_line;
            continue;
        } else if (ret < 0) {
            errors++;
            line = next_line;
            continue;
        }
        
        // Process the operation with proper locking
        spin_lock_irqsave(&sn_net->mapping_lock, flags);
        
        if (strcmp(op.operation, "add") == 0) {
            if (parse_ipv6_prefix(op.internal_prefix, &internal_prefix, &internal_prefix_len) < 0 ||
                parse_ipv6_prefix(op.external_prefix, &external_prefix, &external_prefix_len) < 0) {
                errors++;
            } else {
                ret = add_mapping_internal_unlocked(net, op.interface, &internal_prefix, internal_prefix_len,
                                                   &external_prefix, external_prefix_len);
                if (ret == 0) {
                    processed++;
                } else {
                    errors++;
                }
            }
        } else if (strcmp(op.operation, "del") == 0) {
            if (parse_ipv6_prefix(op.internal_prefix, &internal_prefix, &internal_prefix_len) < 0) {
                errors++;
            } else {
                ret = del_mapping_internal_unlocked(net, op.interface, &internal_prefix, internal_prefix_len);
                if (ret == 0) {
                    processed++;
                } else {
                    errors++;
                }
            }
        } else if (strcmp(op.operation, "drop") == 0) {
            if (strncmp(op.interface, "--all", 5) == 0) {
                ret = drop_mappings_internal_unlocked(net, NULL);
            } else {
                ret = drop_mappings_internal_unlocked(net, op.interface);
            }
            if (ret >= 0) {
                processed += ret;
            } else {
                errors++;
            }
        } else {
            errors++;
        }
        
        spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
        
        line = next_line;
    }
    
    kfree(buf);
    
    if (processed > 0 || errors == 0) {
        pr_info("Slick NAT: Batch operation completed - processed: %d, errors: %d\n", processed, errors);
        return count;
    } else {
        return errors > 0 ? -EINVAL : count;
    }
}

static int batch_show(struct seq_file *m, void *v) {
    seq_printf(m, "# Slick NAT Batch Interface\n");
    seq_printf(m, "# Write batch operations to this file\n");
    seq_printf(m, "# Format (one per line):\n");
    seq_printf(m, "#   add <interface> <internal_prefix/len> <external_prefix/len>\n");
    seq_printf(m, "#   del <interface> <internal_prefix/len>\n");
    seq_printf(m, "#   drop <interface>    - Drop all mappings for interface\n");
    seq_printf(m, "#   drop --all         - Drop all mappings\n");
    seq_printf(m, "# Lines starting with # are ignored\n");
    return 0;
}

static int batch_open(struct inode *inode, struct file *file) {
    return single_open(file, batch_show, pde_data(inode));
}

static const struct proc_ops batch_proc_ops = {
    .proc_open = batch_open,
    .proc_read = seq_read,
    .proc_write = batch_write,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static ssize_t mapping_write(struct file *file, const char __user *buffer, size_t count, loff_t *pos) {
    struct net *net = pde_data(file_inode(file));
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    char buf[256];
    char *cmd, *interface, *arg1, *arg2;
    struct in6_addr internal_prefix, external_prefix;
    int internal_prefix_len, external_prefix_len;
    unsigned long flags;
    int ret;
    
    if (count >= sizeof(buf))
        return -EINVAL;
    
    if (copy_from_user(buf, buffer, count))
        return -EFAULT;
    
    buf[count] = '\0';
    
    // Remove trailing newline
    if (count > 0 && buf[count-1] == '\n')
        buf[count-1] = '\0';
    
    cmd = buf;
    interface = strchr(buf, ' ');
    if (!interface)
        return -EINVAL;
    
    *interface = '\0';
    interface++;
    
    spin_lock_irqsave(&sn_net->mapping_lock, flags);
    
    if (strcmp(cmd, "add") == 0) {
        arg1 = strchr(interface, ' ');
        if (!arg1) {
            spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
            return -EINVAL;
        }
        
        *arg1 = '\0';
        arg1++;
        
        arg2 = strchr(arg1, ' ');
        if (!arg2) {
            spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
            return -EINVAL;
        }
        
        *arg2 = '\0';
        arg2++;
        
        if (parse_ipv6_prefix(arg1, &internal_prefix, &internal_prefix_len) < 0 ||
            parse_ipv6_prefix(arg2, &external_prefix, &external_prefix_len) < 0) {
            spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
            return -EINVAL;
        }
        
        ret = add_mapping_internal_unlocked(net, interface, &internal_prefix, internal_prefix_len,
                                           &external_prefix, external_prefix_len);
        
    } else if (strcmp(cmd, "del") == 0) {
        arg1 = strchr(interface, ' ');
        if (!arg1) {
            spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
            return -EINVAL;
        }
        
        *arg1 = '\0';
        arg1++;
        
        if (parse_ipv6_prefix(arg1, &internal_prefix, &internal_prefix_len) < 0) {
            spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
            return -EINVAL;
        }
        
        ret = del_mapping_internal_unlocked(net, interface, &internal_prefix, internal_prefix_len);
        
    } else if (strcmp(cmd, "drop") == 0) {
        // Drop all mappings for interface or all mappings if interface is "--all"
        if (strncmp(interface, "--all", 5) == 0) {
            ret = drop_mappings_internal_unlocked(net, NULL);  // Drop all mappings
        } else {
            ret = drop_mappings_internal_unlocked(net, interface);  // Drop for specific interface
        }
        
        if (ret >= 0) {
            pr_info("Slick NAT: Dropped %d mappings\n", ret);
        }
        
    } else {
        spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
        return -EINVAL;
    }
    
    spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
    
    if (ret < 0)
        return ret;
    
    return count;
}

static const struct proc_ops mapping_proc_ops = {
    .proc_open = mapping_open,
    .proc_read = seq_read,
    .proc_write = mapping_write,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static struct nf_hook_ops nat_nf_hook_ops = {
    .hook     = nat_hook_func,
    .pf       = PF_INET6,
    .hooknum  = NF_INET_PRE_ROUTING,
    .priority = NF_IP6_PRI_NAT_DST,
};

static struct nf_hook_ops nat_post_hook_ops = {
    .hook     = nat_post_hook_func,
    .pf       = PF_INET6,
    .hooknum  = NF_INET_POST_ROUTING,
    .priority = NF_IP6_PRI_NAT_SRC,
};

static int __net_init slick_nat_net_init(struct net *net)
{
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    int ret;
    
    INIT_LIST_HEAD(&sn_net->mapping_list);
    spin_lock_init(&sn_net->mapping_lock);
    
    // Initialize radix trees
    INIT_RADIX_TREE(&sn_net->internal_tree, GFP_ATOMIC);
    INIT_RADIX_TREE(&sn_net->external_tree, GFP_ATOMIC);
    
    sn_net->proc_entry = proc_create_data(PROC_FILENAME, 0666, net->proc_net, 
                                          &mapping_proc_ops, net);
    if (!sn_net->proc_entry) {
        pr_err("Slick NAT: Failed to create proc entry for netns %p\n", net);
        return -ENOMEM;
    }
    
    sn_net->proc_batch_entry = proc_create_data(PROC_BATCH_FILENAME, 0666, net->proc_net,
                                               &batch_proc_ops, net);
    if (!sn_net->proc_batch_entry) {
        pr_err("Slick NAT: Failed to create batch proc entry for netns %p\n", net);
        remove_proc_entry(PROC_FILENAME, net->proc_net);
        return -ENOMEM;
    }
    
    // Register hooks for this namespace
    ret = nf_register_net_hook(net, &nat_nf_hook_ops);
    if (ret < 0) {
        pr_err("Slick NAT: Failed to register PRE_ROUTING hook for netns %p\n", net);
        remove_proc_entry(PROC_BATCH_FILENAME, net->proc_net);
        remove_proc_entry(PROC_FILENAME, net->proc_net);
        return ret;
    }
    
    ret = nf_register_net_hook(net, &nat_post_hook_ops);
    if (ret < 0) {
        pr_err("Slick NAT: Failed to register POST_ROUTING hook for netns %p\n", net);
        nf_unregister_net_hook(net, &nat_nf_hook_ops);
        remove_proc_entry(PROC_BATCH_FILENAME, net->proc_net);
        remove_proc_entry(PROC_FILENAME, net->proc_net);
        return ret;
    }
    
    return 0;
}

static void __net_exit slick_nat_net_exit(struct net *net)
{
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    struct nat_mapping *mapping, *tmp;
    unsigned long flags;
    void **slot;
    struct radix_tree_iter iter;
    
    // Unregister hooks for this namespace
    nf_unregister_net_hook(net, &nat_nf_hook_ops);
    nf_unregister_net_hook(net, &nat_post_hook_ops);
    
    if (sn_net->proc_entry) {
        remove_proc_entry(PROC_FILENAME, net->proc_net);
        sn_net->proc_entry = NULL;
    }
    
    if (sn_net->proc_batch_entry) {
        remove_proc_entry(PROC_BATCH_FILENAME, net->proc_net);
        sn_net->proc_batch_entry = NULL;
    }
    
    // Clean up mappings and radix trees
    spin_lock_irqsave(&sn_net->mapping_lock, flags);
    
    // Clear radix trees
    radix_tree_for_each_slot(slot, &sn_net->internal_tree, &iter, 0) {
        radix_tree_delete(&sn_net->internal_tree, iter.index);
    }
    radix_tree_for_each_slot(slot, &sn_net->external_tree, &iter, 0) {
        radix_tree_delete(&sn_net->external_tree, iter.index);
    }
    
    // Clean up mapping list
    list_for_each_entry_safe(mapping, tmp, &sn_net->mapping_list, list) {
        list_del(&mapping->list);
        kfree(mapping);
    }
    
    spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
}

static struct pernet_operations slick_nat_net_ops = {
    .init = slick_nat_net_init,
    .exit = slick_nat_net_exit,
    .id   = &slick_nat_net_id,
    .size = sizeof(struct slick_nat_net),
};

static int __init slick_nat_init(void) {
    int ret;
    
    ret = register_pernet_subsys(&slick_nat_net_ops);
    if (ret < 0) {
        pr_err("Slick NAT: Failed to register pernet operations\n");
        return ret;
    }
    
    pr_info("Slick NAT: Module loaded with per-netns support\n");
    return 0;
}

static void __exit slick_nat_exit(void) {
    unregister_pernet_subsys(&slick_nat_net_ops);
    
    pr_info("Slick NAT: Module unloaded\n");
}

module_init(slick_nat_init);
module_exit(slick_nat_exit);
