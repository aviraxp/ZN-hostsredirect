#pragma once
#include <sys/socket.h>
#include <sys/un.h>

#include <string>
#include <string_view>
#include <vector>

namespace socket_utils {
    struct sock_cred : ucred {
        std::string context;
    };

    bool get_client_cred(int fd, sock_cred &cred);

    uint8_t read_u8(int fd);
    uint32_t read_u32(int fd);
    int32_t read_i32(int fd);
    std::string read_string(int fd);

    bool write_u8(int fd, uint8_t val);
    bool write_u32(int fd, uint32_t val);
    bool write_u64(int fd, uint64_t val);
    bool write_i32(int fd, int32_t val);
    bool write_string(int fd, std::string_view str);

    bool set_sockcreate_con(const char* con);

    int send_fds(int sockfd, const int *fds, int cnt);
    int send_fd(int sockfd, int fd);

    std::vector<int> recv_fds(int sockfd);
    int recv_fd(int sockfd);

    bool check_unix_socket(int fd, bool block);

    bool clear_cloexec(int fd);
}
