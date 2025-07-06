#ifndef NDP_H
#define NDP_H

#include <linux/skbuff.h>
#include <linux/netfilter.h>
#include <linux/ipv6.h>

void send_neighbor_advertisement(struct sk_buff *skb, const struct nf_hook_state *state, 
                                const struct in6_addr *target, const struct in6_addr *dest);

#endif
