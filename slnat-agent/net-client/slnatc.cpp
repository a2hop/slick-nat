#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <sys/socket.h>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fstream>

using json = nlohmann::json;

class SlickNatClient {
private:
    std::string server_address;
    int server_port;
    
public:
    SlickNatClient(const std::string& addr, int port = 7001)
        : server_address(addr), server_port(port) {}
    
    json send_request(const json& request) {
        int client_socket = socket(AF_INET6, SOCK_STREAM, 0);
        if (client_socket == -1) {
            return {{"error", "Failed to create socket"}};
        }
        
        struct sockaddr_in6 server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin6_family = AF_INET6;
        server_addr.sin6_port = htons(server_port);
        
        if (inet_pton(AF_INET6, server_address.c_str(), &server_addr.sin6_addr) != 1) {
            close(client_socket);
            return {{"error", "Invalid server IPv6 address"}};
        }
        
        if (connect(client_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
            close(client_socket);
            return {{"error", "Cannot connect to daemon at [" + server_address + "]:" + std::to_string(server_port)}};
        }
        
        // Send request
        std::string request_str = request.dump();
        if (send(client_socket, request_str.c_str(), request_str.length(), 0) == -1) {
            close(client_socket);
            return {{"error", "Failed to send request"}};
        }
        
        // Receive response
        char buffer[2048];
        ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        close(client_socket);
        
        if (bytes_received <= 0) {
            return {{"error", "Failed to receive response"}};
        }
        
        buffer[bytes_received] = '\0';
        
        try {
            return json::parse(buffer);
        } catch (const std::exception& e) {
            return {{"error", std::string("Failed to parse response: ") + e.what()}};
        }
    }
    
    json resolve_ip(const std::string& ip) {
        json request = {
            {"command", "resolve_ip"},
            {"ip", ip}
        };
        return send_request(request);
    }
    
    json get_global_ip(const std::string& ip) {
        json request = {
            {"command", "get2kip"},
            {"ip", ip}
        };
        return send_request(request);
    }
    
    json ping() {
        json request = {{"command", "ping"}};
        return send_request(request);
    }
};

// Helper function to expand IPv6 prefix
std::string expand_ipv6_prefix(const std::string& prefix) {
    // Test if it's already a valid IPv6 address
    struct in6_addr addr;
    if (inet_pton(AF_INET6, prefix.c_str(), &addr) == 1) {
        return prefix;  // Already valid, return as-is
    }
    
    // If it's already a full IPv6 address format, return as is
    if (prefix.find("::") != std::string::npos || std::count(prefix.begin(), prefix.end(), ':') >= 2) {
        return prefix;
    }
    
    // For simple numeric prefixes like "7000", expand to ::1
    if (prefix.find_first_not_of("0123456789") == std::string::npos) {
        return prefix + "::1";
    }
    
    return prefix;
}

// Helper function to get local IPv6 address in a prefix - now reads from /proc/net/if_inet6
std::string get_local_address_in_prefix(const std::string& prefix) {
    std::ifstream if_inet6("/proc/net/if_inet6");
    if (!if_inet6.is_open()) {
        return "";
    }
    
    std::string line;
    while (std::getline(if_inet6, line)) {
        if (line.length() >= 32) {
            std::string addr_hex = line.substr(0, 32);
            // Convert hex to IPv6 format
            std::string addr;
            for (size_t i = 0; i < addr_hex.length(); i += 4) {
                if (i > 0) addr += ":";
                addr += addr_hex.substr(i, 4);
            }
            
            // Simple check if this address might be in our prefix
            if (addr.find(prefix.substr(0, 4)) == 0) {
                return addr;
            }
        }
    }
    
    return "";
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " <daemon_address> <command> [options]\n";
    std::cout << "Commands:\n";
    std::cout << "  get2kip [ip]                Get global unicast IP for local/specified IP\n";
    std::cout << "  resolve <ip>                Resolve IP address mapping\n";
    std::cout << "  ping                        Ping the daemon\n";
    std::cout << "\nExamples:\n";
    std::cout << "  " << program_name << " ::1 get2kip 7607:af56:abb1:c7::100\n";
    std::cout << "  " << program_name << " 7000::1 get2kip\n";
    std::cout << "  " << program_name << " ::1 resolve 2a0a:8dc0:509b:21::1\n";
    std::cout << "  " << program_name << " ::1 ping\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }
    
    std::string daemon_input = argv[1];
    std::string command = argv[2];
    
    // Handle daemon address - expand if needed
    std::string daemon_address = expand_ipv6_prefix(daemon_input);
    
    // Validate the final address
    struct in6_addr test_addr;
    if (inet_pton(AF_INET6, daemon_address.c_str(), &test_addr) != 1) {
        std::cerr << "Error: Invalid IPv6 address format: " << daemon_address << std::endl;
        std::cerr << "Original input: " << daemon_input << std::endl;
        return 1;
    }
    
    int daemon_port = 7001;
    
    SlickNatClient client(daemon_address, daemon_port);
    
    if (command == "get2kip") {
        std::string target_ip;
        
        if (argc > 3) {
            target_ip = argv[3];
        } else {
            // Try to get a local IP address
            target_ip = get_local_address_in_prefix(daemon_input);
            if (target_ip.empty()) {
                std::cerr << "Error: Could not determine local IP address. Please specify an IP address." << std::endl;
                std::cerr << "Usage: " << argv[0] << " " << daemon_input << " get2kip <ip_address>" << std::endl;
                return 1;
            }
        }
        
        std::cout << "Connecting to daemon at [" << daemon_address << "]:" << daemon_port << std::endl;
        std::cout << "Querying global IP for: " << target_ip << std::endl;
        
        json response = client.get_global_ip(target_ip);
        
        if (response.contains("error")) {
            std::cerr << "Error: " << response["error"] << std::endl;
            std::cerr << "Daemon connection: [" << daemon_address << "]:" << daemon_port << std::endl;
            return 1;
        } else if (response.value("status", "") == "success") {
            std::cout << "Internal IP: " << response["internal_ip"] << std::endl;
            std::cout << "Global IP: " << response["global_ip"] << std::endl;
            if (response.contains("interface")) {
                std::cout << "Interface: " << response["interface"] << std::endl;
            }
        } else {
            std::cout << "IP " << target_ip << " not found in global mappings" << std::endl;
            std::cout << "Daemon connection: [" << daemon_address << "]:" << daemon_port << std::endl;
            return 1;
        }
        
    } else if (command == "resolve") {
        if (argc < 4) {
            std::cerr << "Error: IP address required for resolve command" << std::endl;
            return 1;
        }
        
        std::string target_ip = argv[3];
        json response = client.resolve_ip(target_ip);
        
        if (response.contains("error")) {
            std::cerr << "Error: " << response["error"] << std::endl;
            return 1;
        } else if (response.value("status", "") == "success") {
            if (response.contains("internal_ip") && response.contains("public_ip")) {
                std::cout << "Internal IP: " << response["internal_ip"] << std::endl;
                std::cout << "Public IP: " << response["public_ip"] << std::endl;
            } else if (response.contains("external_ip") && response.contains("internal_ip")) {
                std::cout << "External IP: " << response["external_ip"] << std::endl;
                std::cout << "Internal IP: " << response["internal_ip"] << std::endl;
            }
            if (response.contains("interface")) {
                std::cout << "Interface: " << response["interface"] << std::endl;
            }
        } else {
            std::cout << "IP " << target_ip << " not found in mappings" << std::endl;
            return 1;
        }
        
    } else if (command == "ping") {
        std::cout << "Pinging daemon at [" << daemon_address << "]:" << daemon_port << std::endl;
        
        json response = client.ping();
        
        if (response.contains("error")) {
            std::cerr << "Error: " << response["error"] << std::endl;
            std::cerr << "Tried to connect to: [" << daemon_address << "]:" << daemon_port << std::endl;
            return 1;
        } else {
            std::cout << "Daemon at [" << daemon_address << "]:" << daemon_port << " is running" << std::endl;
            if (response.contains("status")) {
                std::cout << "Response: " << response["status"] << std::endl;
            }
        }
        
    } else {
        std::cerr << "Unknown command: " << command << std::endl;
        print_usage(argv[0]);
        return 1;
    }
    
    return 0;
}
