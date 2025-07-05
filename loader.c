#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <sys/socket.h>
#include <linux/if.h>

static int attach_tc_prog(int prog_fd, const char *dev, const char *direction) {
    char cmd[256];
    int ret;
    
    // Delete existing qdisc (ignore errors)
    snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s clsact 2>/dev/null", dev);
    system(cmd);
    
    // Add clsact qdisc
    snprintf(cmd, sizeof(cmd), "tc qdisc add dev %s clsact", dev);
    ret = system(cmd);
    if (ret != 0) {
        printf("Failed to add clsact qdisc to %s\n", dev);
        return -1;
    }
    
    // Attach BPF program
    snprintf(cmd, sizeof(cmd), 
             "tc filter add dev %s %s bpf direct-action obj ipv6_nat.o sec tc",
             dev, direction);
    ret = system(cmd);
    if (ret != 0) {
        printf("Failed to attach BPF program to %s %s\n", dev, direction);
        return -1;
    }
    
    printf("Successfully attached BPF program to %s %s\n", dev, direction);
    return 0;
}

int main(int argc, char **argv) {
    struct bpf_object *obj;
    struct bpf_program *prog_egress, *prog_ingress;
    int prog_fd_egress, prog_fd_ingress;
    int err;
    
    // Load BPF object
    obj = bpf_object__open_file("ipv6_nat.o", NULL);
    if (libbpf_get_error(obj)) {
        printf("Failed to open BPF object file\n");
        return 1;
    }
    
    err = bpf_object__load(obj);
    if (err) {
        printf("Failed to load BPF object: %s\n", strerror(-err));
        return 1;
    }
    
    // Find programs
    prog_egress = bpf_object__find_program_by_name(obj, "ipv6_nat_egress");
    prog_ingress = bpf_object__find_program_by_name(obj, "ipv6_nat_ingress");
    
    if (!prog_egress || !prog_ingress) {
        printf("Failed to find BPF programs\n");
        return 1;
    }
    
    prog_fd_egress = bpf_program__fd(prog_egress);
    prog_fd_ingress = bpf_program__fd(prog_ingress);
    
    // Attach to interfaces
    if (attach_tc_prog(prog_fd_egress, "gtwlo2", "egress") < 0) {
        return 1;
    }
    
    if (attach_tc_prog(prog_fd_ingress, "outline", "ingress") < 0) {
        return 1;
    }
    
    printf("IPv6 NAT eBPF module loaded successfully!\n");
    printf("Press Ctrl+C to unload...\n");
    
    // Keep running
    while (1) {
        sleep(1);
    }
    
    return 0;
}
