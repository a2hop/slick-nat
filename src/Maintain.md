# Slick NAT - Maintainer Notes

This document contains technical implementation details, workarounds, and maintenance notes for the Slick NAT kernel module.

## Architecture Overview

### Core Components

1. **slick-nat.c**: Main module with netfilter hooks and mapping management
2. **ndp.c**: Neighbor Discovery Protocol proxy implementation
3. **ndp.h**: Header file for NDP functions
4. **slnat**: Bash script for user-space management

### Key Data Structures

```c
struct slick_nat_net {
    struct list_head mapping_list;    // Per-namespace mapping list
    spinlock_t mapping_lock;          // Protection for mapping operations
    struct proc_dir_entry *proc_entry; // Proc filesystem entry
    struct proc_dir_entry *proc_batch_entry; // Batch processing interface
    struct hlist_head internal_hash[SLICK_NAT_HASH_SIZE]; // Internal prefix index
    struct hlist_head external_hash[SLICK_NAT_HASH_SIZE]; // External prefix index
    u16 prefix_len_use[129];          // Mappings per prefix length
    unsigned int mapping_count;       // Total mappings in this namespace
};

struct nat_mapping {
    struct list_head list;            // List linkage
    struct hlist_node internal_node;  // Internal hash bucket linkage
    struct hlist_node external_node;  // External hash bucket linkage
    char interface[IFNAMSIZ];         // Interface name
    struct in6_addr internal_prefix;  // Internal network prefix (masked)
    struct in6_addr external_prefix;  // External network prefix (masked)
    int prefix_len;                   // Prefix length (must match for both)
};

// Snapshot handed to the packet path so it never dereferences a mapping
// after dropping mapping_lock.
struct nat_xlate {
    struct in6_addr from_prefix;
    struct in6_addr to_prefix;
    int prefix_len;
    bool valid;
};
```

## Implementation Details

### Netfilter Hook Strategy

**PRE_ROUTING Hook (NF_IP6_PRI_NAT_DST)** - the only hook registered
- On external interfaces: translates destination (and, where a mapping
  exists, source) from the external prefix into the internal one
- On internal interfaces: translates the source into the external prefix;
  the destination is translated only when it too lives in an internal prefix
- Handles NDP solicitations for external prefixes
- Manages hop limit expiration

There is deliberately **no POST_ROUTING hook**: the module no longer stamps
`skb->mark`, so there is nothing to clean up. Marking translated packets
would clobber any fwmark the administrator relies on for policy routing.

### Address Translation Algorithm

The module uses prefix-based translation with length-aware matching:

```c
// Prefix matching with bit-level precision
static bool compare_prefix_with_len(const struct in6_addr *addr, 
                                   const struct in6_addr *prefix, 
                                   int prefix_len) {
    int bytes = prefix_len / 8;
    int bits = prefix_len % 8;
    
    // Compare full bytes
    for (int i = 0; i < bytes; i++) {
        if (addr->s6_addr[i] != prefix->s6_addr[i])
            return false;
    }
    
    // Compare remaining bits
    if (bits > 0) {
        unsigned char mask = (0xFF << (8 - bits)) & 0xFF;
        if ((addr->s6_addr[bytes] & mask) != (prefix->s6_addr[bytes] & mask))
            return false;
    }
    
    return true;
}
```

### Prefix Index

**Problem**: Linear search through the mapping list is O(n)
**Solution**: Two hash tables keyed on the *masked* prefix plus its length

```c
// The masking is what makes the index work: hashing a packet address at
// length N lands in the same bucket as the stored prefix of length N that
// covers it.  The previous radix-tree key hashed unmasked address bytes, so
// a packet address never matched its own prefix except at exactly /64.
static u32 prefix_hash(const struct in6_addr *addr, int prefix_len) {
    struct in6_addr masked;

    ipv6_addr_prefix(&masked, addr, prefix_len);
    return jhash2((const u32 *)masked.s6_addr32, 4, prefix_len) &
           (SLICK_NAT_HASH_SIZE - 1);
}
```

**Key Design Decisions**:
- Separate hash tables for internal and external prefixes
- `prefix_len_use[]` records which prefix lengths exist, so a lookup probes
  only the lengths actually configured
- Lengths are walked from /128 down, giving true longest-prefix match
- Collisions are handled by ordinary hash chaining plus an exact
  `compare_prefix_with_len()` check, so no key fixups are needed
- Prefixes are masked at parse time, so the index and the `del` path agree
  even when the user leaves host bits set

### Batch Processing Implementation

The module now supports batch operations via the `/proc/net/slick_nat_batch` interface:

```c
// Batch interface operations
static ssize_t batch_write(struct file *file, const char __user *buffer, size_t count, loff_t *pos) {
    // Parse each line as a separate operation
    // Apply all operations atomically under a single lock
    // Format: add|del|drop [interface] [internal_prefix] [external_prefix]
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
```

**Benefits of Batch Processing:**
1. Single lock acquisition for multiple operations
2. Improved performance for mass configuration
3. Atomic application of related rules
4. Reduced syscall overhead

**Implementation Notes:**
- Parses input line-by-line with a simple state machine
- Validates all operations before applying
- Uses a consistent data structure for operation representation
- Maintains namespace isolation for multi-tenant environments
- Comments and empty lines are ignored for better readability

## Critical Implementation Decisions

#### 1. No Packet Marks
- **Problem**: Earlier versions stamped `skb->mark = 0xDEADBEEF` on every
  translated packet to prevent re-processing
- **Why it was wrong**: it destroyed the administrator's fwmark, breaking
  policy routing, tc filters and ip6tables mark matches
- **Solution**: dropped entirely. A packet crosses PRE_ROUTING once, and
  self-generated packets (NA replies via `dev_queue_xmit()`, ICMP errors via
  `icmpv6_send()`) never re-enter PRE_ROUTING, so no marker is needed

#### 2. Checksum Handling
- **Problem**: IPv6 pseudo-header checksum updates
- **Solution**: `inet_proto_csum_replace4()` per 32-bit word for outer address
  changes (TCP, UDP, UDP-Lite, ICMPv6)
- **Extension headers**: the transport offset comes from
  `ipv6_skip_exthdr()`, not from assuming `nexthdr` is the transport protocol
- **Fragments**: only the first fragment carries the checksum field, so
  trailing fragments are address-translated without a checksum fixup
- **ICMPv6 errors**: after the embedded header is rewritten the whole ICMPv6
  checksum is recomputed with `csum_ipv6_magic()`, mirroring
  `nf_nat_icmpv6_reply_translation()`, rather than patched incrementally

#### 3. Memory Management
- **Problem**: Kernel memory allocation in interrupt context
- **Solution**: Use `GFP_ATOMIC` for skb allocation
- **Workaround**: Pre-allocate commonly used structures (future enhancement)

#### 4. Hash Index Implementation
- **Problem**: O(n) linear search performance bottleneck
- **Solution**: Dual hash tables for internal/external prefix lookups
- **Tradeoff**: Fixed per-namespace table cost vs. lookup performance
- **Optimization**: `prefix_len_use[]` skips prefix lengths that are unused

#### 5. Batch Processing Interface
- **Problem**: Individual rule application has high syscall and lock overhead
- **Solution**: Batch processing interface with single-lock application
- **Optimization**: Validate all operations before applying any changes
- **User Experience**: Template generation and validation capabilities

## Workarounds and Hacks

### 1. NDP Proxy Implementation

**Problem**: Linux kernel doesn't provide direct NDP proxy API
**Solution**: Manual packet construction and injection

```c
// Direct dev_queue_xmit() instead of netif_receive_skb():
// egress bypasses PRE_ROUTING, so the reply cannot loop back into us.
if (dev_queue_xmit(reply_skb) < 0) {
    pr_err("Slick NAT: Failed to send NA\n");
}
```

The reply is a hand-built Ethernet frame, so `send_neighbor_advertisement()`
bails out unless the device is `ARPHRD_ETHER` with a 6-byte address and is up.
A solicitation whose source is `::` is a DAD probe: the advertisement then goes
unsolicited to `ff02::1` (MAC `33:33:00:00:00:01`) instead of being unicast
back to the unspecified address.

### 2. Interface Detection Logic

**Problem**: Different hook points provide different interface information
**Solution**: Conditional interface selection based on hook state

Only PRE_ROUTING is registered, so `state->in` is always set and
`state->out` is always NULL:

```c
if (!skb || !state->in)
    return NF_ACCEPT;
ifname = state->in->name;
```

### 3. ICMP Error Message Handling

**Problem**: Embedded packets in ICMP errors need translation
**Solution**: Recursive packet parsing with embedded header modification

**Tricky Part**: Checksum updates affect both outer and inner packets

### 4. Hop Limit Management

**Problem**: Need to generate Time Exceeded messages for traceroute
**Solution**: Check hop limit before translation and generate ICMP errors

**Implementation**: `icmpv6_send()` is called *before* translation, so the
embedded packet shows the external address the sender used. It also gives us
the kernel's source-address selection, routing and ICMP rate limiting for
free - the earlier hand-built frame had none of those, and returned a pointer
to an `inet6_ifaddr` address after `rcu_read_unlock()`.

### 5. Hash Collisions

**Problem**: Different IPv6 prefixes may hash to the same bucket
**Solution**: Ordinary chaining. Every candidate is confirmed with an exact
prefix comparison, so a collision costs one extra comparison and never a
wrong answer.

```c
hlist_for_each_entry(mapping, &sn_net->internal_hash[prefix_hash(addr, prefix_len)],
                     internal_node) {
    if (mapping->prefix_len == prefix_len &&
        compare_prefix_with_len(addr, &mapping->internal_prefix, prefix_len))
        return mapping;
}
```

## Known Issues and Limitations

### 1. Race Conditions
- **Issue**: Mapping list modifications vs. packet processing
- **Mitigation**: `spin_lock_irqsave()` for all mapping access. The packet
  path copies what it needs into a `struct nat_xlate` while holding the lock;
  it must never keep a `struct nat_mapping *` past the unlock, or a concurrent
  `del`/`drop` will free it underneath us
- **Future**: Consider RCU for better performance

### 2. Memory Leaks
- **Watch**: skb allocation in NDP proxy
- **Mitigation**: Careful error handling and kfree_skb()
- **Test**: Run with KASAN enabled

### 3. Performance Bottlenecks
- **Issue**: `is_external_interface()` still walks the whole mapping list on
  every packet to decide which direction to translate
- **Mitigation**: the hook returns early when the namespace has no mappings
- **Future**: index interfaces by ifindex instead of scanning by name

### 4. Hash Table Memory Usage
- **Issue**: Two 256-entry tables per network namespace
- **Mitigation**: ~4 KB per namespace, independent of mapping count
- **Monitor**: Memory usage with very large namespace counts

## Performance Improvements

### Before the Prefix Index
- **Lookup Time**: O(n) linear search
- **Memory Usage**: Lower (list only)
- **Scalability**: Poor with >100 mappings

### With the Hash Index
- **Lookup Time**: one bucket probe per prefix length in use (typically 1-3)
- **Memory Usage**: two fixed 256-entry tables per namespace
- **Scalability**: Good up to the 10,000-mapping cap
- **Correctness**: overlapping prefixes now resolve longest-first; the
  previous radix key only ever matched at exactly /64, so every other prefix
  length silently fell back to a full list walk that returned the *first*
  match rather than the most specific one

## Testing Strategies

### 1. Unit Testing
```bash
# Test mapping addition/deletion
echo "add eth0 2001:db8:1::/64 2001:db8:2::/64" > /proc/net/slick_nat_mappings
echo "del eth0 2001:db8:1::/64" > /proc/net/slick_nat_mappings

# Test duplicate prevention
echo "add eth0 2001:db8:1::/64 2001:db8:2::/64" > /proc/net/slick_nat_mappings
echo "add eth0 2001:db8:1::/64 2001:db8:3::/64" > /proc/net/slick_nat_mappings  # Should fail

# Test batch processing
cat <<EOF > /tmp/batch.txt
add eth0 2001:db8:1::/64 2001:db8:2::/64
add eth0 2001:db8:3::/64 2001:db8:4::/64
del eth0 2001:db8:1::/64
EOF
cat /tmp/batch.txt > /proc/net/slick_nat_batch
```

### 2. Integration Testing
```bash
# Test bidirectional translation
ping6 -c 3 2001:db8:external::1
tcpdump -i eth0 -n icmp6

# Test NDP proxy
ndisc6 2001:db8:external::1 eth0
```

### 3. Stress Testing
```bash
# High packet rate
hping3 -6 -I eth0 -i u1000 2001:db8:external::1

# Multiple mappings
for i in {1..50}; do
    echo "add eth0 2001:db8:$i::/64 2001:db8:ext$i::/64" > /proc/net/slick_nat_mappings
done
```

### 4. Performance Testing
```bash
# Test with high mapping count
for i in {1..1000}; do
    echo "add eth0 2001:db8:$i::/64 2001:db8:ext$i::/64" > /proc/net/slick_nat_mappings
done

# Compare with batch processing
(
echo "drop --all"
for i in {1..1000}; do
    echo "add eth0 2001:db8:$i::/64 2001:db8:ext$i::/64"
done
) > /tmp/batch.txt
time cat /tmp/batch.txt > /proc/net/slick_nat_batch

# Measure lookup performance
time ping6 -c 1000 2001:db8:external::1
```

## Debugging Techniques

### 1. Kernel Debugging
```bash
# Enable debug prints
echo 8 > /proc/sys/kernel/printk

# Watch specific events
dmesg -w | grep -E "(Slick NAT|slick_nat)"
```

### 2. Packet Tracing
```bash
# Use ftrace for detailed tracing
echo 'function_graph' > /sys/kernel/debug/tracing/current_tracer
echo 'nat_hook_func' > /sys/kernel/debug/tracing/set_ftrace_filter
```

### 3. Memory Debugging
```bash
# Enable SLUB debugging
echo 1 > /sys/kernel/slab/kmalloc-*/validate

# Check for leaks
cat /proc/slabinfo | grep -E "(kmalloc|skbuff)"
```

### 4. Prefix Index Debugging
```bash
# Confirm which prefix lengths are configured
awk '/->/ {split($2,a,"/"); print a[2]}' /proc/net/slick_nat_mappings | sort -n | uniq -c

# Trace lookups
echo '__find_mapping_by_internal' > /sys/kernel/debug/tracing/set_ftrace_filter
echo '__find_mapping_by_external' >> /sys/kernel/debug/tracing/set_ftrace_filter
```

## Development Guidelines

### 1. Code Style
- Follow kernel coding style (checkpatch.pl)
- Use appropriate error codes (-EINVAL, -ENOMEM, etc.)
- Add proper error handling for all allocations

### 2. Locking Rules
- Always use spinlock_irqsave() for mapping operations
- Keep critical sections as short as possible
- Document lock ordering to prevent deadlocks

### 3. Error Handling
- Always check return values
- Use proper cleanup paths (goto labels)
- Log errors with appropriate severity levels

### 4. Prefix Index Guidelines
- Always hash the *masked* prefix, never raw address bytes
- Keep `prefix_len_use[]` in step with insertions and removals
- Test with overlapping prefixes of differing lengths
- Never let a `struct nat_mapping *` escape `mapping_lock`

## Future Enhancements

### 1. Performance Optimizations
- ~~Replace linear search with hash table~~ ✓ **DONE: masked-prefix hash index**
- ~~Add batch processing interface~~ ✓ **DONE: Added in v0.0.3**
- Implement per-CPU mapping caches
- Add bulk packet processing
- Consider RCU for lockless reads

### 2. Feature Additions
- Port-based NAT for better granularity
- Connection tracking integration
- IPv4-IPv6 translation support

### 3. Monitoring and Statistics
- Per-mapping packet counters
- Translation success/failure rates
- Performance metrics via proc/sysfs

### 4. Advanced Data Structures
- Implement Patricia trie for true prefix matching
- Add LRU cache for frequently accessed mappings
- Consider lockless data structures for better SMP scaling

## Security Considerations

### 1. Input Validation
- All user input through proc interface is validated
- IPv6 address parsing uses kernel-provided functions
- Bounds checking on all array accesses

### 2. Privilege Separation
- Only root can modify mappings
- Kernel memory is protected from user space
- No direct hardware access

### 3. DoS Prevention
- Mapping count is capped at `SLICK_NAT_MAX_MAPPINGS` (10,000) per namespace
- Generated ICMP errors go through `icmpv6_send()`, which honours
  `net.ipv6.icmp.ratelimit`
- Proc entries are mode 0644, so only root can change forwarding behaviour
- Proper resource cleanup on errors

## Kernel Version Compatibility

### Supported Versions
- 4.14+: Full support with all features
- 4.9-4.13: Basic support (some features may be limited)
- < 4.9: Not supported

### API Changes to Watch
- netfilter hook registration changes
- proc filesystem API modifications
- IPv6 address manipulation functions

## Build System Notes

### Makefile Structure
```makefile
# Standard kernel module build
obj-m := slick_nat.o
slick_nat-objs := slick-nat.o ndp.o

# Kernel build directory detection
KDIR ?= /lib/modules/$(KVERSION)/build
```

### Cross-Compilation
```bash
# For different architectures
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
```

## Maintenance Checklist

### Regular Tasks
- [ ] Test with latest kernel versions
- [ ] Review security advisories
- [ ] Update documentation
- [ ] Performance benchmarking
- [ ] Memory leak testing

### Release Preparation
- [ ] Run checkpatch.pl
- [ ] Test all features
- [ ] Update version numbers
- [ ] Generate changelog
- [ ] Code review

### Performance Testing
- [ ] Benchmark lookup times with various mapping counts
- [ ] Monitor memory usage growth
- [ ] Test hash collision handling and longest-prefix resolution
- [ ] Validate performance under high packet rates

## Contact Information

For technical questions or bug reports:
- Check kernel logs first
- Review this document
- Test with minimal configuration
- Provide reproduction steps
