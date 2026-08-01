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
#include <net/ndisc.h>
#include <net/dst.h>
#include <linux/netdevice.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/inet.h>
#include <linux/jhash.h>
#include <net/addrconf.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include "ndp.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lukasz Xu-Kafarski");
MODULE_DESCRIPTION("Slick NAT - Bidirectional IPv6 NAT Kernel Module");
MODULE_VERSION("0.0.4");

#define PROC_FILENAME "slick_nat_mappings"
#define PROC_BATCH_FILENAME "slick_nat_batch"

#define SLICK_NAT_HASH_BITS 8
#define SLICK_NAT_HASH_SIZE (1u << SLICK_NAT_HASH_BITS)
#define SLICK_NAT_MAX_MAPPINGS 10000
#define SLICK_NAT_BATCH_MAX (1024 * 1024)
#define SLICK_NAT_LINE_MAX 256

// Per-namespace data structure
struct slick_nat_net {
    struct list_head mapping_list;
    spinlock_t mapping_lock;
    struct proc_dir_entry *proc_entry;
    struct proc_dir_entry *proc_batch_entry;
    struct hlist_head internal_hash[SLICK_NAT_HASH_SIZE];
    struct hlist_head external_hash[SLICK_NAT_HASH_SIZE];
    /* Number of mappings using each prefix length; drives the
     * longest-prefix-match walk without touching every mapping. */
    u16 prefix_len_use[129];
    unsigned int mapping_count;
};

// Dynamic mapping structure
struct nat_mapping {
    struct list_head list;
    struct hlist_node internal_node;
    struct hlist_node external_node;
    char interface[IFNAMSIZ];
    struct in6_addr internal_prefix;
    struct in6_addr external_prefix;
    int prefix_len;
};

/* Snapshot of a mapping, taken while the lock is held, so that the packet
 * path never dereferences a mapping after dropping the lock. */
struct nat_xlate {
    struct in6_addr from_prefix;
    struct in6_addr to_prefix;
    int prefix_len;
    bool valid;
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

/* Hash an address masked down to prefix_len.  Because the host bits are
 * masked off first, hashing a packet address at length N yields the same
 * bucket as hashing a stored prefix of length N that covers it. */
static u32 prefix_hash(const struct in6_addr *addr, int prefix_len) {
    struct in6_addr masked;

    ipv6_addr_prefix(&masked, addr, prefix_len);
    return jhash2((const u32 *)masked.s6_addr32, 4, prefix_len) & (SLICK_NAT_HASH_SIZE - 1);
}

/* Longest-prefix-match lookups.  Caller must hold mapping_lock. */
static struct nat_mapping *__find_mapping_by_internal(struct slick_nat_net *sn_net,
                                                      const struct in6_addr *addr) {
    struct nat_mapping *mapping;
    int prefix_len;

    for (prefix_len = 128; prefix_len >= 0; prefix_len--) {
        if (!sn_net->prefix_len_use[prefix_len])
            continue;

        hlist_for_each_entry(mapping, &sn_net->internal_hash[prefix_hash(addr, prefix_len)],
                             internal_node) {
            if (mapping->prefix_len == prefix_len &&
                compare_prefix_with_len(addr, &mapping->internal_prefix, prefix_len))
                return mapping;
        }
    }

    return NULL;
}

static struct nat_mapping *__find_mapping_by_external(struct slick_nat_net *sn_net,
                                                      const struct in6_addr *addr,
                                                      const char *ifname) {
    struct nat_mapping *mapping;
    int prefix_len;

    for (prefix_len = 128; prefix_len >= 0; prefix_len--) {
        if (!sn_net->prefix_len_use[prefix_len])
            continue;

        hlist_for_each_entry(mapping, &sn_net->external_hash[prefix_hash(addr, prefix_len)],
                             external_node) {
            if (mapping->prefix_len == prefix_len &&
                strncmp(mapping->interface, ifname, IFNAMSIZ) == 0 &&
                compare_prefix_with_len(addr, &mapping->external_prefix, prefix_len))
                return mapping;
        }
    }

    return NULL;
}

static void nat_xlate_set(struct nat_xlate *x, const struct nat_mapping *mapping,
                          bool external_to_internal) {
    if (!mapping) {
        x->valid = false;
        return;
    }

    x->prefix_len = mapping->prefix_len;
    if (external_to_internal) {
        x->from_prefix = mapping->external_prefix;
        x->to_prefix = mapping->internal_prefix;
    } else {
        x->from_prefix = mapping->internal_prefix;
        x->to_prefix = mapping->external_prefix;
    }
    x->valid = true;
}

/* Look up both addresses of a header in one lock acquisition and copy out
 * everything the packet path needs. */
static void nat_lookup_pair(struct slick_nat_net *sn_net, const struct in6_addr *saddr,
                            const struct in6_addr *daddr, bool is_external_if,
                            const char *ifname, struct nat_xlate *xs, struct nat_xlate *xd) {
    unsigned long flags;

    spin_lock_irqsave(&sn_net->mapping_lock, flags);
    if (is_external_if) {
        nat_xlate_set(xs, __find_mapping_by_external(sn_net, saddr, ifname), true);
        nat_xlate_set(xd, __find_mapping_by_external(sn_net, daddr, ifname), true);
    } else {
        nat_xlate_set(xs, __find_mapping_by_internal(sn_net, saddr), false);
        nat_xlate_set(xd, __find_mapping_by_internal(sn_net, daddr), false);
    }
    spin_unlock_irqrestore(&sn_net->mapping_lock, flags);
}

static bool is_external_interface(struct slick_nat_net *sn_net, const char *ifname) {
    struct nat_mapping *mapping;
    unsigned long flags;
    bool found = false;

    spin_lock_irqsave(&sn_net->mapping_lock, flags);
    list_for_each_entry(mapping, &sn_net->mapping_list, list) {
        if (strncmp(mapping->interface, ifname, IFNAMSIZ) == 0) {
            found = true;
            break;
        }
    }
    spin_unlock_irqrestore(&sn_net->mapping_lock, flags);

    return found;
}

/* Locate the transport header, skipping any extension headers.  Returns a
 * negative value if the chain could not be parsed. */
static int nat_transport_offset(struct sk_buff *skb, u8 *proto, bool *first_frag) {
    __be16 frag_off = 0;
    u8 nexthdr = ipv6_hdr(skb)->nexthdr;
    int off;

    off = ipv6_skip_exthdr(skb, sizeof(struct ipv6hdr), &nexthdr, &frag_off);
    if (off < 0)
        return off;

    *proto = nexthdr;
    /* Only the first fragment carries the transport checksum field. */
    *first_frag = (frag_off & htons(IP6_OFFSET)) == 0;
    return off;
}

/* Bytes that must be linear and writable before we start editing. */
static int nat_writable_len(struct sk_buff *skb, int thoff, u8 proto, bool icmp_error) {
    int need = sizeof(struct ipv6hdr);

    if (thoff >= (int)sizeof(struct ipv6hdr)) {
        switch (proto) {
        case IPPROTO_TCP:
            need = thoff + sizeof(struct tcphdr);
            break;
        case IPPROTO_UDP:
        case IPPROTO_UDPLITE:
            need = thoff + sizeof(struct udphdr);
            break;
        case IPPROTO_ICMPV6:
            need = thoff + sizeof(struct icmp6hdr);
            if (icmp_error)
                need += sizeof(struct ipv6hdr);
            break;
        }
    }

    return min_t(int, need, skb->len);
}

/* Incremental transport checksum fixup for an outer address change.  The
 * addresses are part of the transport pseudo-header, hence pseudohdr=true. */
static void update_csum(struct sk_buff *skb, int thoff, u8 proto, bool first_frag,
                        const struct in6_addr *old_addr, const struct in6_addr *new_addr) {
    __sum16 *check = NULL;
    int i;

    /* Non-initial fragments carry no transport header to fix up; the
     * checksum lives in the first fragment and is corrected there. */
    if (!first_frag || thoff < (int)sizeof(struct ipv6hdr))
        return;

    switch (proto) {
    case IPPROTO_TCP:
        if (skb->len < thoff + sizeof(struct tcphdr))
            return;
        check = &((struct tcphdr *)(skb->data + thoff))->check;
        break;
    case IPPROTO_UDP:
    case IPPROTO_UDPLITE: {
        struct udphdr *udph;

        if (skb->len < thoff + sizeof(struct udphdr))
            return;
        udph = (struct udphdr *)(skb->data + thoff);
        /* A zero UDP checksum is illegal in IPv6, but leave it alone if
         * present rather than turning garbage into a "valid" checksum. */
        if (udph->check == 0)
            return;
        check = &udph->check;
        break;
    }
    case IPPROTO_ICMPV6:
        if (skb->len < thoff + sizeof(struct icmp6hdr))
            return;
        check = &((struct icmp6hdr *)(skb->data + thoff))->icmp6_cksum;
        break;
    default:
        return;
    }

    for (i = 0; i < 4; i++)
        inet_proto_csum_replace4(check, skb, old_addr->s6_addr32[i],
                                 new_addr->s6_addr32[i], true);
}

/* Recompute the ICMPv6 checksum from scratch.  Used after the embedded
 * packet of an ICMPv6 error has been rewritten, mirroring what
 * nf_nat_icmpv6_reply_translation() does. */
static void nat_icmp6_csum_recalc(struct sk_buff *skb, int thoff) {
    struct ipv6hdr *iph = ipv6_hdr(skb);
    struct icmp6hdr *icmp6h;
    unsigned int len;

    if (skb->ip_summed == CHECKSUM_PARTIAL)
        return;
    if (skb->len < thoff + sizeof(struct icmp6hdr))
        return;

    icmp6h = (struct icmp6hdr *)(skb->data + thoff);
    len = skb->len - thoff;

    icmp6h->icmp6_cksum = 0;
    icmp6h->icmp6_cksum = csum_ipv6_magic(&iph->saddr, &iph->daddr, len, IPPROTO_ICMPV6,
                                          skb_checksum(skb, thoff, len, 0));
    if (skb->ip_summed == CHECKSUM_COMPLETE)
        skb->ip_summed = CHECKSUM_NONE;
}

/* Translate the IPv6 header embedded in an ICMPv6 error message.  The outer
 * ICMPv6 checksum is fixed up by the caller. */
static bool handle_icmp_error_embedded_packet(struct sk_buff *skb, int thoff, struct net *net,
                                              bool is_external_if, const char *ifname) {
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    struct ipv6hdr *embedded_iph;
    struct nat_xlate xs, xd;
    bool translated = false;
    int emb_off = thoff + sizeof(struct icmp6hdr);

    if (skb->len < emb_off + sizeof(struct ipv6hdr))
        return false;

    embedded_iph = (struct ipv6hdr *)(skb->data + emb_off);

    /* The embedded packet travelled in the opposite direction, so its source
     * is what our destination would be and vice versa - but the prefix space
     * it lives in is the same one this interface is talking, so the lookup
     * direction matches the outer packet. */
    nat_lookup_pair(sn_net, &embedded_iph->saddr, &embedded_iph->daddr,
                    is_external_if, ifname, &xs, &xd);

    if (xs.valid && compare_prefix_with_len(&embedded_iph->saddr, &xs.from_prefix, xs.prefix_len)) {
        remap_address_with_len(&embedded_iph->saddr, &xs.to_prefix, xs.prefix_len);
        translated = true;
    }

    if (xd.valid && compare_prefix_with_len(&embedded_iph->daddr, &xd.from_prefix, xd.prefix_len)) {
        remap_address_with_len(&embedded_iph->daddr, &xd.to_prefix, xd.prefix_len);
        translated = true;
    }

    return translated;
}

/* Answer a neighbour solicitation for any external prefix we proxy. */
static bool nat_ndp_target_is_proxied(struct slick_nat_net *sn_net, const struct in6_addr *target,
                                      bool is_external_if, const char *ifname) {
    struct nat_mapping *mapping;
    unsigned long flags;
    bool found = false;

    spin_lock_irqsave(&sn_net->mapping_lock, flags);
    list_for_each_entry(mapping, &sn_net->mapping_list, list) {
        /* On an interface that owns mappings, only proxy that interface's
         * external prefixes; on internal interfaces proxy any of them. */
        if (is_external_if && strncmp(mapping->interface, ifname, IFNAMSIZ) != 0)
            continue;
        if (compare_prefix_with_len(target, &mapping->external_prefix, mapping->prefix_len)) {
            found = true;
            break;
        }
    }
    spin_unlock_irqrestore(&sn_net->mapping_lock, flags);

    return found;
}

/* Returns NF_ACCEPT/NF_DROP to short-circuit, or -1 to keep processing. */
static int nat_handle_icmpv6(struct sk_buff *skb, const struct nf_hook_state *state,
                             struct slick_nat_net *sn_net, int thoff, bool is_external_if,
                             const char *ifname, bool *is_icmp_error) {
    struct icmp6hdr *icmp6h;
    struct ipv6hdr *iph;
    struct nd_msg *ns_msg;

    if (!pskb_may_pull(skb, thoff + sizeof(struct icmp6hdr)))
        return NF_ACCEPT;

    icmp6h = (struct icmp6hdr *)(skb->data + thoff);

    switch (icmp6h->icmp6_type) {
    case NDISC_NEIGHBOUR_SOLICITATION:
        iph = ipv6_hdr(skb);
        /* RFC 4861: valid ND messages always arrive with hop limit 255. */
        if (iph->hop_limit != 255)
            return NF_ACCEPT;
        if (!pskb_may_pull(skb, thoff + sizeof(struct nd_msg)))
            return NF_ACCEPT;

        iph = ipv6_hdr(skb);
        ns_msg = (struct nd_msg *)(skb->data + thoff);

        if (ipv6_addr_type(&ns_msg->target) & IPV6_ADDR_MULTICAST)
            return NF_ACCEPT;

        if (nat_ndp_target_is_proxied(sn_net, &ns_msg->target, is_external_if, ifname)) {
            send_neighbor_advertisement(skb, state, &ns_msg->target, &iph->saddr);
            return NF_DROP;
        }
        return NF_ACCEPT;

    case NDISC_NEIGHBOUR_ADVERTISEMENT:
    case NDISC_ROUTER_SOLICITATION:
    case NDISC_ROUTER_ADVERTISEMENT:
    case NDISC_REDIRECT:
        return NF_ACCEPT;

    case ICMPV6_DEST_UNREACH:
    case ICMPV6_PKT_TOOBIG:
    case ICMPV6_TIME_EXCEED:
    case ICMPV6_PARAMPROB:
        *is_icmp_error = true;
        return -1;

    case ICMPV6_ECHO_REQUEST:
    case ICMPV6_ECHO_REPLY:
        return -1;

    default:
        return NF_ACCEPT;
    }
}

static unsigned int nat_hook_func(void *priv, struct sk_buff *skb, const struct nf_hook_state *state) {
    struct ipv6hdr *iph;
    struct in6_addr old_addr;
    struct nat_xlate xs = { }, xd = { };
    struct slick_nat_net *sn_net;
    const char *ifname;
    struct net *net = state->net;
    bool is_external_if;
    bool is_icmp_error = false;
    bool inner_translated = false;
    bool first_frag = true;
    u8 proto = 0;
    int thoff;
    int verdict;
    int need;

    /* The module only registers PRE_ROUTING, so state->in is always set. */
    if (!skb || !state->in)
        return NF_ACCEPT;

    if (!pskb_may_pull(skb, sizeof(struct ipv6hdr)))
        return NF_ACCEPT;

    iph = ipv6_hdr(skb);
    if (iph->version != 6)
        return NF_ACCEPT;

    sn_net = slick_nat_pernet(net);

    /* Nothing configured in this namespace: skip the locking entirely.
     * Racing with a concurrent add at worst lets one packet through
     * untranslated, which the sender will retransmit. */
    if (!READ_ONCE(sn_net->mapping_count))
        return NF_ACCEPT;

    ifname = state->in->name;
    is_external_if = is_external_interface(sn_net, ifname);

    thoff = nat_transport_offset(skb, &proto, &first_frag);
    if (thoff < 0)
        return NF_ACCEPT;

    /* Neighbour discovery has to be inspected before the link-local
     * shortcut below: solicitations normally travel from a link-local
     * source to a solicited-node multicast group. */
    if (proto == IPPROTO_ICMPV6 && first_frag) {
        verdict = nat_handle_icmpv6(skb, state, sn_net, thoff, is_external_if,
                                    ifname, &is_icmp_error);
        if (verdict >= 0)
            return verdict;
        /* pskb_may_pull() may have reallocated the buffer. */
        iph = ipv6_hdr(skb);
    }
    /* Trailing fragments carry no ICMPv6 header to classify, but their outer
     * addresses still have to be translated for the packet to be routed. */

    /* Nothing to translate when both ends are link-local. */
    if ((ipv6_addr_type(&iph->saddr) & IPV6_ADDR_LINKLOCAL) &&
        (ipv6_addr_type(&iph->daddr) & IPV6_ADDR_LINKLOCAL))
        return NF_ACCEPT;

    nat_lookup_pair(sn_net, &iph->saddr, &iph->daddr, is_external_if, ifname, &xs, &xd);

    if (!xs.valid && !xd.valid)
        return NF_ACCEPT;

    if (is_external_if) {
        /* Ingress from an external interface: only traffic addressed to one
         * of our external prefixes is ours to translate. */
        if (!xd.valid)
            return NF_ACCEPT;

        /* Report the expiry against the pre-translation addresses so that
         * traceroute sees the external address of this hop. */
        if (iph->hop_limit <= 1) {
            icmpv6_send(skb, ICMPV6_TIME_EXCEED, ICMPV6_EXC_HOPLIMIT, 0);
            return NF_DROP;
        }

        need = nat_writable_len(skb, thoff, proto, is_icmp_error);
        if (skb_ensure_writable(skb, need))
            return NF_DROP;
        iph = ipv6_hdr(skb);

        if (is_icmp_error)
            inner_translated = handle_icmp_error_embedded_packet(skb, thoff, net,
                                                                 is_external_if, ifname);

        old_addr = iph->daddr;
        remap_address_with_len(&iph->daddr, &xd.to_prefix, xd.prefix_len);
        update_csum(skb, thoff, proto, first_frag, &old_addr, &iph->daddr);

        if (xs.valid) {
            old_addr = iph->saddr;
            remap_address_with_len(&iph->saddr, &xs.to_prefix, xs.prefix_len);
            update_csum(skb, thoff, proto, first_frag, &old_addr, &iph->saddr);
        }
    } else {
        /* Ingress from an internal interface: rewrite our source into the
         * external prefix.  The destination is only rewritten when it also
         * lives in an internal prefix (NAT-to-NAT traffic). */
        if (!xs.valid)
            return NF_ACCEPT;

        need = nat_writable_len(skb, thoff, proto, is_icmp_error);
        if (skb_ensure_writable(skb, need))
            return NF_DROP;
        iph = ipv6_hdr(skb);

        if (is_icmp_error)
            inner_translated = handle_icmp_error_embedded_packet(skb, thoff, net,
                                                                 is_external_if, ifname);

        old_addr = iph->saddr;
        remap_address_with_len(&iph->saddr, &xs.to_prefix, xs.prefix_len);
        update_csum(skb, thoff, proto, first_frag, &old_addr, &iph->saddr);

        if (xd.valid) {
            old_addr = iph->daddr;
            remap_address_with_len(&iph->daddr, &xd.to_prefix, xd.prefix_len);
            update_csum(skb, thoff, proto, first_frag, &old_addr, &iph->daddr);
        }
    }

    if (inner_translated)
        nat_icmp6_csum_recalc(skb, thoff);

    return NF_ACCEPT;
}

static int mapping_show(struct seq_file *m, void *v) {
    struct net *net = m->private;
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    struct nat_mapping *mapping;
    unsigned long flags;

    seq_printf(m, "# IPv6 NAT Mappings\n");
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
    char buf[64];
    char *prefix_str;
    char *len_str;
    struct in6_addr parsed;
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

    ret = in6_pton(prefix_str, -1, (u8 *)parsed.s6_addr, -1, NULL);
    if (ret != 1)
        return -EINVAL;

    ret = kstrtoint(len_str, 10, prefix_len);
    if (ret < 0)
        return -EINVAL;

    if (*prefix_len < 0 || *prefix_len > 128)
        return -EINVAL;

    /* Store the prefix canonically so hashing and comparison agree even when
     * the caller left host bits set. */
    ipv6_addr_prefix(addr, &parsed, *prefix_len);

    return 0;
}

static void nat_mapping_unlink(struct slick_nat_net *sn_net, struct nat_mapping *mapping) {
    hlist_del(&mapping->internal_node);
    hlist_del(&mapping->external_node);
    list_del(&mapping->list);
    sn_net->prefix_len_use[mapping->prefix_len]--;
    WRITE_ONCE(sn_net->mapping_count, sn_net->mapping_count - 1);
    kfree(mapping);
}

static int add_mapping_internal_unlocked(struct net *net, const char *interface,
                                        const struct in6_addr *internal_prefix, int internal_prefix_len,
                                        const struct in6_addr *external_prefix, int external_prefix_len) {
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    struct nat_mapping *mapping, *tmp;

    // Both prefixes must have the same length
    if (internal_prefix_len != external_prefix_len)
        return -EINVAL;

    if (sn_net->mapping_count >= SLICK_NAT_MAX_MAPPINGS)
        return -ENOSPC;

    // Reject duplicates on either side - two mappings claiming the same
    // prefix on the same interface would make lookups ambiguous.
    list_for_each_entry(tmp, &sn_net->mapping_list, list) {
        if (strncmp(tmp->interface, interface, IFNAMSIZ) != 0)
            continue;
        if (tmp->prefix_len != internal_prefix_len)
            continue;
        if (ipv6_addr_equal(&tmp->internal_prefix, internal_prefix) ||
            ipv6_addr_equal(&tmp->external_prefix, external_prefix))
            return -EEXIST;
    }

    mapping = kmalloc(sizeof(*mapping), GFP_ATOMIC);
    if (!mapping)
        return -ENOMEM;

    strscpy(mapping->interface, interface, IFNAMSIZ);
    mapping->internal_prefix = *internal_prefix;
    mapping->external_prefix = *external_prefix;
    mapping->prefix_len = internal_prefix_len;

    hlist_add_head(&mapping->internal_node,
                   &sn_net->internal_hash[prefix_hash(internal_prefix, internal_prefix_len)]);
    hlist_add_head(&mapping->external_node,
                   &sn_net->external_hash[prefix_hash(external_prefix, external_prefix_len)]);
    list_add_tail(&mapping->list, &sn_net->mapping_list);
    sn_net->prefix_len_use[internal_prefix_len]++;
    WRITE_ONCE(sn_net->mapping_count, sn_net->mapping_count + 1);

    return 0;
}

static int del_mapping_internal_unlocked(struct net *net, const char *interface,
                                        const struct in6_addr *internal_prefix, int internal_prefix_len) {
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    struct nat_mapping *mapping, *tmp;

    list_for_each_entry_safe(mapping, tmp, &sn_net->mapping_list, list) {
        if (strncmp(mapping->interface, interface, IFNAMSIZ) == 0 &&
            ipv6_addr_equal(&mapping->internal_prefix, internal_prefix) &&
            mapping->prefix_len == internal_prefix_len) {
            nat_mapping_unlink(sn_net, mapping);
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
        if (interface && strncmp(mapping->interface, interface, IFNAMSIZ) != 0)
            continue;

        nat_mapping_unlink(sn_net, mapping);
        dropped++;
    }

    return dropped;
}

/* Pull the next whitespace-delimited token out of *s, NUL-terminating it. */
static char *nat_next_token(char **s) {
    char *tok;

    if (!*s)
        return NULL;

    while (**s == ' ' || **s == '\t')
        (*s)++;

    if (**s == '\0')
        return NULL;

    tok = *s;
    while (**s && **s != ' ' && **s != '\t')
        (*s)++;

    if (**s) {
        **s = '\0';
        (*s)++;
    }

    return tok;
}

/*
 * Execute a single configuration line.  The line buffer is modified in place.
 * Returns a negative errno on failure, the number of dropped mappings for
 * "drop", 0 otherwise.  -EAGAIN means "blank line or comment, nothing to do".
 * Caller must hold mapping_lock.
 */
static int nat_exec_line_unlocked(struct net *net, char *line) {
    struct in6_addr internal_prefix, external_prefix;
    int internal_prefix_len, external_prefix_len;
    char *cmd, *interface, *arg1, *arg2;

    cmd = nat_next_token(&line);
    if (!cmd || cmd[0] == '#')
        return -EAGAIN;

    interface = nat_next_token(&line);
    if (!interface || interface[0] == '\0')
        return -EINVAL;

    if (strcmp(cmd, "add") == 0) {
        arg1 = nat_next_token(&line);
        arg2 = nat_next_token(&line);
        if (!arg1 || !arg2)
            return -EINVAL;

        if (strlen(interface) >= IFNAMSIZ)
            return -EINVAL;

        if (parse_ipv6_prefix(arg1, &internal_prefix, &internal_prefix_len) < 0 ||
            parse_ipv6_prefix(arg2, &external_prefix, &external_prefix_len) < 0)
            return -EINVAL;

        return add_mapping_internal_unlocked(net, interface, &internal_prefix, internal_prefix_len,
                                             &external_prefix, external_prefix_len);
    }

    if (strcmp(cmd, "del") == 0) {
        arg1 = nat_next_token(&line);
        if (!arg1)
            return -EINVAL;

        if (parse_ipv6_prefix(arg1, &internal_prefix, &internal_prefix_len) < 0)
            return -EINVAL;

        return del_mapping_internal_unlocked(net, interface, &internal_prefix, internal_prefix_len);
    }

    if (strcmp(cmd, "drop") == 0) {
        if (strcmp(interface, "--all") == 0)
            return drop_mappings_internal_unlocked(net, NULL);
        return drop_mappings_internal_unlocked(net, interface);
    }

    return -EINVAL;
}

static int nat_exec_line(struct net *net, char *line) {
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    unsigned long flags;
    int ret;

    spin_lock_irqsave(&sn_net->mapping_lock, flags);
    ret = nat_exec_line_unlocked(net, line);
    spin_unlock_irqrestore(&sn_net->mapping_lock, flags);

    return ret;
}

static ssize_t batch_write(struct file *file, const char __user *buffer, size_t count, loff_t *pos) {
    struct net *net = pde_data(file_inode(file));
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    char *buf, *line, *next_line;
    unsigned long flags;
    int ret, processed = 0, errors = 0;

    if (count == 0 || count > SLICK_NAT_BATCH_MAX)
        return -EINVAL;

    buf = kvmalloc(count + 1, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    if (copy_from_user(buf, buffer, count)) {
        kvfree(buf);
        return -EFAULT;
    }

    buf[count] = '\0';

    line = buf;
    while (line && *line) {
        next_line = strchr(line, '\n');
        if (next_line) {
            *next_line = '\0';
            next_line++;
        }

        spin_lock_irqsave(&sn_net->mapping_lock, flags);
        ret = nat_exec_line_unlocked(net, line);
        spin_unlock_irqrestore(&sn_net->mapping_lock, flags);

        if (ret == -EAGAIN)
            ;                       /* blank line or comment */
        else if (ret < 0)
            errors++;
        else
            processed += (ret > 0) ? ret : 1;

        line = next_line;
    }

    kvfree(buf);

    pr_info("Slick NAT: Batch operation completed - processed: %d, errors: %d\n",
            processed, errors);

    if (processed == 0 && errors > 0)
        return -EINVAL;

    return count;
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
    char buf[SLICK_NAT_LINE_MAX];
    char *newline;
    int ret;

    if (count == 0 || count >= sizeof(buf))
        return -EINVAL;

    if (copy_from_user(buf, buffer, count))
        return -EFAULT;

    buf[count] = '\0';

    newline = strchr(buf, '\n');
    if (newline)
        *newline = '\0';

    ret = nat_exec_line(net, buf);
    if (ret == -EAGAIN)
        return count;
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

static int __net_init slick_nat_net_init(struct net *net)
{
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    unsigned int i;
    int ret;

    INIT_LIST_HEAD(&sn_net->mapping_list);
    spin_lock_init(&sn_net->mapping_lock);

    for (i = 0; i < SLICK_NAT_HASH_SIZE; i++) {
        INIT_HLIST_HEAD(&sn_net->internal_hash[i]);
        INIT_HLIST_HEAD(&sn_net->external_hash[i]);
    }
    memset(sn_net->prefix_len_use, 0, sizeof(sn_net->prefix_len_use));
    sn_net->mapping_count = 0;

    /* Mode 0644: the mapping table controls packet forwarding, so only root
     * may write it. */
    sn_net->proc_entry = proc_create_data(PROC_FILENAME, 0644, net->proc_net,
                                          &mapping_proc_ops, net);
    if (!sn_net->proc_entry) {
        pr_err("Slick NAT: Failed to create proc entry\n");
        return -ENOMEM;
    }

    sn_net->proc_batch_entry = proc_create_data(PROC_BATCH_FILENAME, 0644, net->proc_net,
                                                &batch_proc_ops, net);
    if (!sn_net->proc_batch_entry) {
        pr_err("Slick NAT: Failed to create batch proc entry\n");
        proc_remove(sn_net->proc_entry);
        sn_net->proc_entry = NULL;
        return -ENOMEM;
    }

    ret = nf_register_net_hook(net, &nat_nf_hook_ops);
    if (ret < 0) {
        pr_err("Slick NAT: Failed to register PRE_ROUTING hook\n");
        proc_remove(sn_net->proc_batch_entry);
        proc_remove(sn_net->proc_entry);
        sn_net->proc_batch_entry = NULL;
        sn_net->proc_entry = NULL;
        return ret;
    }

    return 0;
}

static void __net_exit slick_nat_net_exit(struct net *net)
{
    struct slick_nat_net *sn_net = slick_nat_pernet(net);
    unsigned long flags;

    /* Unregister first: this waits for in-flight hook invocations, so no
     * packet can still be looking at a mapping when we free it. */
    nf_unregister_net_hook(net, &nat_nf_hook_ops);

    if (sn_net->proc_entry) {
        proc_remove(sn_net->proc_entry);
        sn_net->proc_entry = NULL;
    }

    if (sn_net->proc_batch_entry) {
        proc_remove(sn_net->proc_batch_entry);
        sn_net->proc_batch_entry = NULL;
    }

    spin_lock_irqsave(&sn_net->mapping_lock, flags);
    drop_mappings_internal_unlocked(net, NULL);
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
