#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/netfilter.h>
#include <linux/ipv6.h>
#include <linux/icmpv6.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <net/ipv6.h>
#include <net/ip6_route.h>
#include <net/ndisc.h>
#include <net/addrconf.h>
#include <net/dst.h>
#include "ndp.h"

void send_neighbor_advertisement(struct sk_buff *orig_skb, const struct nf_hook_state *state,
                                const struct in6_addr *target_addr, const struct in6_addr *solicitor_addr) {
    struct sk_buff *reply_skb;
    struct ipv6hdr *reply_iph;
    struct nd_msg *reply_na;
    struct net_device *dev;
    struct in6_addr dst_addr;
    unsigned char *opt;
    int total_len;
    bool is_dad;
    struct ethhdr *eth;

    dev = state->in;
    if (!dev)
        return;

    /* The reply is built as a raw Ethernet frame, so anything that is not
     * Ethernet-like has to be left to the stack. */
    if (dev->type != ARPHRD_ETHER || dev->addr_len != ETH_ALEN)
        return;

    if (!(dev->flags & IFF_UP))
        return;

    if (!skb_mac_header_was_set(orig_skb))
        return;

    /* A solicitation with an unspecified source is a DAD probe: it must be
     * answered unsolicited to the all-nodes multicast group, never unicast
     * back to ::. */
    is_dad = ipv6_addr_any(solicitor_addr);
    dst_addr = is_dad ? in6addr_linklocal_allnodes : *solicitor_addr;

    // Calculate total length: IPv6 + ICMPv6 ND + target link-layer address option
    total_len = sizeof(struct ipv6hdr) + sizeof(struct nd_msg) + 8; // 8 bytes for target LL addr option

    reply_skb = alloc_skb(total_len + LL_MAX_HEADER, GFP_ATOMIC);
    if (!reply_skb) {
        pr_err("Slick NAT: Failed to allocate skb for NA\n");
        return;
    }

    skb_reserve(reply_skb, LL_MAX_HEADER);
    skb_put(reply_skb, total_len);
    skb_reset_network_header(reply_skb);

    reply_iph = ipv6_hdr(reply_skb);
    reply_iph->version = 6;
    reply_iph->priority = 0;
    memset(reply_iph->flow_lbl, 0, sizeof(reply_iph->flow_lbl));
    reply_iph->payload_len = htons(sizeof(struct nd_msg) + 8);
    reply_iph->nexthdr = IPPROTO_ICMPV6;
    reply_iph->hop_limit = 255;
    reply_iph->saddr = *target_addr;
    reply_iph->daddr = dst_addr;

    skb_set_transport_header(reply_skb, sizeof(struct ipv6hdr));
    reply_na = (struct nd_msg *)skb_transport_header(reply_skb);

    reply_na->icmph.icmp6_type = NDISC_NEIGHBOUR_ADVERTISEMENT;
    reply_na->icmph.icmp6_code = 0;
    reply_na->icmph.icmp6_cksum = 0;
    reply_na->icmph.icmp6_dataun.u_nd_advt.solicited = is_dad ? 0 : 1;
    reply_na->icmph.icmp6_dataun.u_nd_advt.override = 1;
    reply_na->icmph.icmp6_dataun.u_nd_advt.router = 0;
    reply_na->icmph.icmp6_dataun.u_nd_advt.reserved = 0;
    reply_na->target = *target_addr;

    // Add target link-layer address option
    opt = (unsigned char *)(reply_na + 1);
    opt[0] = ND_OPT_TARGET_LL_ADDR;
    opt[1] = 1; // length in 8-byte units
    memcpy(opt + 2, dev->dev_addr, ETH_ALEN);

    // Calculate checksum
    reply_na->icmph.icmp6_cksum = csum_ipv6_magic(&reply_iph->saddr, &reply_iph->daddr,
                                                  ntohs(reply_iph->payload_len), IPPROTO_ICMPV6,
                                                  csum_partial(reply_na, ntohs(reply_iph->payload_len), 0));

    // Add Ethernet header
    skb_push(reply_skb, ETH_HLEN);
    skb_reset_mac_header(reply_skb);
    eth = eth_hdr(reply_skb);

    if (is_dad) {
        /* 33:33:00:00:00:01 for ff02::1 */
        ipv6_eth_mc_map(&dst_addr, eth->h_dest);
    } else {
        memcpy(eth->h_dest, eth_hdr(orig_skb)->h_source, ETH_ALEN);
    }
    memcpy(eth->h_source, dev->dev_addr, ETH_ALEN);
    eth->h_proto = htons(ETH_P_IPV6);

    reply_skb->dev = dev;
    reply_skb->protocol = htons(ETH_P_IPV6);
    reply_skb->ip_summed = CHECKSUM_NONE;
    reply_skb->pkt_type = PACKET_OUTGOING;

    // Send the packet
    if (dev_queue_xmit(reply_skb) < 0) {
        pr_err("Slick NAT: Failed to send NA\n");
    }
}
