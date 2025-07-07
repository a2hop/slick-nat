#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <vector>
#include <thread>
#include <mutex>
#include <regex>
#include <chrono>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#include <nlohmann/json.hpp>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

using json = nlohmann::json;

struct ListenConfig {
    std::string address;
    int port;
    int socket_fd;
};

class SlickNatDaemon {
private:
    std::vector<ListenConfig> listen_configs;
    bool running;
    std::mutex mappings_mutex;
    std::map<std::string, std::string> internal_to_external;
    std::map<std::string, std::string> external_to_internal;
    std::string proc_mappings_path;
    std::string config_file_path;
    
    struct NatMapping {
        std::string interface;
        std::string internal_prefix;
        std::string external_prefix;
        int prefix_len;
    };
    
    std::vector<NatMapping> mappings;
    
public:
    SlickNatDaemon(const std::string& config_path = "/etc/slnatd/config",
                   const std::string& proc_path = "/proc/net/slick_nat_mappings")
        : running(false), proc_mappings_path(proc_path), config_file_path(config_path) {}
    
    ~SlickNatDaemon() {
        stop();
    }
    
    bool load_config() {
        std::ifstream config_file(config_file_path);
        if (!config_file.is_open()) {
            std::cerr << "Warning: Cannot open config file " << config_file_path << std::endl;
            std::cerr << "Using default configuration" << std::endl;
            
            // Default configuration
            ListenConfig default_config;
            default_config.address = "::1";
            default_config.port = 7001;
            default_config.socket_fd = -1;
            listen_configs.push_back(default_config);
            return true;
        }
        
        listen_configs.clear();
        std::string line;
        int line_number = 0;
        
        while (std::getline(config_file, line)) {
            line_number++;
            
            // Skip empty lines and comments
            if (line.empty() || line[0] == '#') {
                continue;
            }
            
            // Parse listen directive: listen <address> <port>
            std::istringstream iss(line);
            std::string directive;
            iss >> directive;
            
            if (directive == "listen") {
                std::string address;
                int port;
                
                if (iss >> address >> port) {
                    ListenConfig config;
                    config.address = address;
                    config.port = port;
                    config.socket_fd = -1;
                    listen_configs.push_back(config);
                    std::cout << "Config: Will listen on [" << address << "]:" << port << std::endl;
                } else {
                    std::cerr << "Error parsing config line " << line_number << ": " << line << std::endl;
                }
            } else if (directive == "proc_path") {
                std::string path;
                if (iss >> path) {
                    proc_mappings_path = path;
                    std::cout << "Config: Using proc path: " << path << std::endl;
                }
            } else {
                std::cerr << "Unknown config directive on line " << line_number << ": " << directive << std::endl;
            }
        }
        
        if (listen_configs.empty()) {
            std::cerr << "No valid listen configurations found, using default" << std::endl;
            ListenConfig default_config;
            default_config.address = "::1";
            default_config.port = 7001;
            default_config.socket_fd = -1;
            listen_configs.push_back(default_config);
        }
        
        return true;
    }
    
    bool start() {
        if (!load_config()) {
            return false;
        }
        
        // Create sockets for all listen configurations
        for (auto& config : listen_configs) {
            if (!create_listen_socket(config)) {
                std::cerr << "Failed to create socket for [" << config.address << "]:" << config.port << std::endl;
                stop();
                return false;
            }
        }
        
        running = true;
        std::cout << "SlickNat daemon started, listening on " << listen_configs.size() << " addresses" << std::endl;
        
        // Load initial mappings
        reload_mappings();
        
        // Start mapping reload thread
        std::thread reload_thread(&SlickNatDaemon::mapping_reload_loop, this);
        reload_thread.detach();
        
        // Start accept threads for each listening socket
        std::vector<std::thread> accept_threads;
        for (auto& config : listen_configs) {
            accept_threads.emplace_back(&SlickNatDaemon::accept_loop, this, std::ref(config));
        }
        
        // Wait for all accept threads to complete
        for (auto& thread : accept_threads) {
            thread.join();
        }
        
        return true;
    }
    
    void stop() {
        running = false;
        for (auto& config : listen_configs) {
            if (config.socket_fd != -1) {
                close(config.socket_fd);
                config.socket_fd = -1;
            }
        }
        std::cout << "SlickNat daemon stopped" << std::endl;
    }
    
private:
    bool create_listen_socket(ListenConfig& config) {
        // Create IPv6 TCP socket
        config.socket_fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (config.socket_fd == -1) {
            std::cerr << "Failed to create socket for " << config.address << std::endl;
            return false;
        }
        
        // Enable IPv6 only (disable IPv4-mapped IPv6 addresses)
        int ipv6only = 1;
        if (setsockopt(config.socket_fd, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6only, sizeof(ipv6only)) == -1) {
            std::cerr << "Failed to set IPv6 only for " << config.address << std::endl;
            close(config.socket_fd);
            return false;
        }
        
        // Enable address reuse
        int reuse = 1;
        if (setsockopt(config.socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
            std::cerr << "Failed to set socket reuse for " << config.address << std::endl;
            close(config.socket_fd);
            return false;
        }
        
        struct sockaddr_in6 addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(config.port);
        
        if (inet_pton(AF_INET6, config.address.c_str(), &addr.sin6_addr) != 1) {
            std::cerr << "Invalid IPv6 address: " << config.address << std::endl;
            close(config.socket_fd);
            return false;
        }
        
        if (bind(config.socket_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
            std::cerr << "Failed to bind to " << config.address << ":" << config.port << std::endl;
            close(config.socket_fd);
            return false;
        }
        
        if (listen(config.socket_fd, 5) == -1) {
            std::cerr << "Failed to listen on " << config.address << ":" << config.port << std::endl;
            close(config.socket_fd);
            return false;
        }
        
        std::cout << "Listening on [" << config.address << "]:" << config.port << std::endl;
        return true;
    }
    
    void accept_loop(ListenConfig& config) {
        while (running) {
            struct sockaddr_in6 client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_socket = accept(config.socket_fd, (struct sockaddr*)&client_addr, &client_len);
            
            if (client_socket == -1) {
                if (running) {
                    std::cerr << "Accept failed on " << config.address << ":" << config.port << std::endl;
                }
                continue;
            }
            
            // Log client connection
            char client_ip[INET6_ADDRSTRLEN];
            if (inet_ntop(AF_INET6, &client_addr.sin6_addr, client_ip, sizeof(client_ip))) {
                std::cout << "Client connected from [" << client_ip << "]:" << ntohs(client_addr.sin6_port) 
                          << " to [" << config.address << "]:" << config.port << std::endl;
            }
            
            std::thread client_thread(&SlickNatDaemon::handle_client, this, client_socket);
            client_thread.detach();
        }
    }
    
    void mapping_reload_loop() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (running) {
                reload_mappings();
            }
        }
    }
    
    bool reload_mappings() {
        std::ifstream proc_file(proc_mappings_path);
        if (!proc_file.is_open()) {
            std::cerr << "Warning: Cannot open " << proc_mappings_path << std::endl;
            return false;
        }
        
        std::lock_guard<std::mutex> lock(mappings_mutex);
        mappings.clear();
        internal_to_external.clear();
        external_to_internal.clear();
        
        std::string line;
        std::regex mapping_regex(R"(^(\S+)\s+([a-fA-F0-9:]+)/(\d+)\s+->\s+([a-fA-F0-9:]+)/(\d+)$)");
        
        while (std::getline(proc_file, line)) {
            // Skip comments and empty lines
            if (line.empty() || line[0] == '#') {
                continue;
            }
            
            std::smatch match;
            if (std::regex_match(line, match, mapping_regex)) {
                NatMapping mapping;
                mapping.interface = match[1];
                mapping.internal_prefix = match[2];
                mapping.external_prefix = match[4];
                mapping.prefix_len = std::stoi(match[3]);
                
                mappings.push_back(mapping);
                
                // Build lookup maps for prefix matching
                build_lookup_maps(mapping);
            }
        }
        
        std::cout << "Loaded " << mappings.size() << " NAT mappings" << std::endl;
        return true;
    }
    
    void build_lookup_maps(const NatMapping& mapping) {
        // For simplicity, we'll store the prefix mappings directly
        // In a real implementation, you'd want more sophisticated IP prefix matching
        std::string internal_key = mapping.internal_prefix + "/" + std::to_string(mapping.prefix_len);
        std::string external_key = mapping.external_prefix + "/" + std::to_string(mapping.prefix_len);
        
        internal_to_external[internal_key] = external_key;
        external_to_internal[external_key] = internal_key;
    }
    
    void handle_client(int client_socket) {
        char buffer[1024];
        ssize_t bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        
        if (bytes_read <= 0) {
            close(client_socket);
            return;
        }
        
        buffer[bytes_read] = '\0';
        
        try {
            json request = json::parse(buffer);
            json response = process_request(request);
            
            std::string response_str = response.dump();
            send(client_socket, response_str.c_str(), response_str.length(), 0);
            
        } catch (const std::exception& e) {
            json error_response = {{"error", e.what()}};
            std::string error_str = error_response.dump();
            send(client_socket, error_str.c_str(), error_str.length(), 0);
        }
        
        close(client_socket);
    }
    
    json process_request(const json& request) {
        std::string command = request.value("command", "");
        
        if (command == "resolve_ip") {
            std::string ip = request.value("ip", "");
            if (ip.empty()) {
                return {{"error", "Missing IP parameter"}};
            }
            return resolve_ip(ip);
        } else if (command == "get_global_ip" || command == "get2kip") {
            std::string ip = request.value("ip", "");
            if (ip.empty()) {
                return {{"error", "Missing IP parameter"}};
            }
            return get_global_ip(ip);
        } else if (command == "ping") {
            return {{"status", "pong"}};
        } else {
            return {{"error", "Unknown command: " + command}};
        }
    }
    
    json resolve_ip(const std::string& ip) {
        if (!is_valid_ipv6(ip)) {
            return {{"error", "Invalid IPv6 address format"}};
        }
        
        std::lock_guard<std::mutex> lock(mappings_mutex);
        
        // Find matching mapping by checking if IP matches any internal prefix
        for (const auto& mapping : mappings) {
            if (ip_matches_prefix(ip, mapping.internal_prefix, mapping.prefix_len)) {
                std::string public_ip = remap_address(ip, mapping.internal_prefix, 
                                                     mapping.external_prefix, mapping.prefix_len);
                return {
                    {"internal_ip", ip},
                    {"public_ip", public_ip},
                    {"interface", mapping.interface},
                    {"status", "success"}
                };
            }
        }
        
        // Check if it's already an external IP
        for (const auto& mapping : mappings) {
            if (ip_matches_prefix(ip, mapping.external_prefix, mapping.prefix_len)) {
                std::string internal_ip = remap_address(ip, mapping.external_prefix, 
                                                       mapping.internal_prefix, mapping.prefix_len);
                return {
                    {"external_ip", ip},
                    {"internal_ip", internal_ip},
                    {"interface", mapping.interface},
                    {"status", "success"}
                };
            }
        }
        
        return {
            {"ip", ip},
            {"error", "IP not found in mappings"},
            {"status", "not_found"}
        };
    }
    
    json get_global_ip(const std::string& ip) {
        if (!is_valid_ipv6(ip)) {
            return {{"error", "Invalid IPv6 address format"}};
        }
        
        std::lock_guard<std::mutex> lock(mappings_mutex);
        
        std::cout << "Looking for global IP mapping for: " << ip << std::endl;
        std::cout << "Available mappings:" << std::endl;
        for (const auto& mapping : mappings) {
            std::cout << "  " << mapping.interface << " " << mapping.internal_prefix 
                      << "/" << mapping.prefix_len << " -> " << mapping.external_prefix 
                      << "/" << mapping.prefix_len << std::endl;
        }
        
        // Find matching mapping and return the external (global) IP
        for (const auto& mapping : mappings) {
            std::cout << "Checking if " << ip << " matches prefix " << mapping.internal_prefix 
                      << "/" << mapping.prefix_len << std::endl;
            
            if (ip_matches_prefix(ip, mapping.internal_prefix, mapping.prefix_len)) {
                std::string global_ip = remap_address(ip, mapping.internal_prefix, 
                                                     mapping.external_prefix, mapping.prefix_len);
                
                std::cout << "Found match! Mapped to: " << global_ip << std::endl;
                
                // Check if the result is in 2000::/3 (global unicast range)
                struct in6_addr addr;
                if (inet_pton(AF_INET6, global_ip.c_str(), &addr) == 1) {
                    if ((addr.s6_addr[0] & 0xE0) == 0x20) {  // 2000::/3 check
                        return {
                            {"internal_ip", ip},
                            {"global_ip", global_ip},
                            {"interface", mapping.interface},
                            {"status", "success"}
                        };
                    } else {
                        std::cout << "Mapped IP " << global_ip << " is not in global unicast range (2000::/3)" << std::endl;
                    }
                } else {
                    std::cout << "Failed to parse mapped IP: " << global_ip << std::endl;
                }
            }
        }
        
        return {
            {"ip", ip},
            {"error", "No global unicast mapping found for " + ip},
            {"status", "not_found"},
            {"available_mappings", mappings.size()}
        };
    }
    
    bool is_valid_ipv6(const std::string& ip) {
        struct in6_addr addr;
        return inet_pton(AF_INET6, ip.c_str(), &addr) == 1;
    }
    
    bool ip_matches_prefix(const std::string& ip, const std::string& prefix, int prefix_len) {
        struct in6_addr ip_addr, prefix_addr;
        
        if (inet_pton(AF_INET6, ip.c_str(), &ip_addr) != 1 ||
            inet_pton(AF_INET6, prefix.c_str(), &prefix_addr) != 1) {
            return false;
        }
        
        // Compare prefix bits
        int bytes = prefix_len / 8;
        int bits = prefix_len % 8;
        
        // Compare full bytes
        for (int i = 0; i < bytes; i++) {
            if (ip_addr.s6_addr[i] != prefix_addr.s6_addr[i]) {
                return false;
            }
        }
        
        // Compare remaining bits
        if (bits > 0 && bytes < 16) {
            uint8_t mask = (0xFF << (8 - bits)) & 0xFF;
            if ((ip_addr.s6_addr[bytes] & mask) != (prefix_addr.s6_addr[bytes] & mask)) {
                return false;
            }
        }
        
        return true;
    }
    
    std::string remap_address(const std::string& ip, const std::string& old_prefix, 
                             const std::string& new_prefix, int prefix_len) {
        struct in6_addr ip_addr, old_prefix_addr, new_prefix_addr;
        
        if (inet_pton(AF_INET6, ip.c_str(), &ip_addr) != 1 ||
            inet_pton(AF_INET6, old_prefix.c_str(), &old_prefix_addr) != 1 ||
            inet_pton(AF_INET6, new_prefix.c_str(), &new_prefix_addr) != 1) {
            return ip; // Return original if parsing fails
        }
        
        // Replace prefix bits
        int bytes = prefix_len / 8;
        int bits = prefix_len % 8;
        
        // Replace full bytes
        for (int i = 0; i < bytes && i < 16; i++) {
            ip_addr.s6_addr[i] = new_prefix_addr.s6_addr[i];
        }
        
        // Replace remaining bits
        if (bits > 0 && bytes < 16) {
            uint8_t mask = (0xFF << (8 - bits)) & 0xFF;
            ip_addr.s6_addr[bytes] = (new_prefix_addr.s6_addr[bytes] & mask) | 
                                    (ip_addr.s6_addr[bytes] & ~mask);
        }
        
        // Convert back to string
        char result[INET6_ADDRSTRLEN];
        if (inet_ntop(AF_INET6, &ip_addr, result, sizeof(result))) {
            return std::string(result);
        }
        
        return ip; // Return original if conversion fails
    }
};

// Global daemon instance for signal handling
SlickNatDaemon* g_daemon = nullptr;

void signal_handler(int signum) {
    std::cout << "\nReceived signal " << signum << std::endl;
    if (g_daemon) {
        g_daemon->stop();
    }
    exit(0);
}

int main(int argc, char* argv[]) {
    std::string config_path = "/etc/slnatd/config";
    std::string proc_path = "/proc/net/slick_nat_mappings";
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--proc" && i + 1 < argc) {
            proc_path = argv[++i];
        } else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [--config PATH] [--proc PATH]\n";
            std::cout << "  --config PATH   Configuration file path (default: /etc/slnatd/config)\n";
            std::cout << "  --proc PATH     Kernel proc file path (default: /proc/net/slick_nat_mappings)\n";
            return 0;
        }
    }
    
    SlickNatDaemon daemon(config_path, proc_path);
    g_daemon = &daemon;
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    if (!daemon.start()) {
        std::cerr << "Failed to start daemon" << std::endl;
        return 1;
    }
    
    return 0;
}
