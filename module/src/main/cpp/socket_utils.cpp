#include <cstddef>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include "utils.h"
#include "socket_utils.h"

namespace socket_utils {
    constexpr auto kMaxStringSize = 4096;

    bool get_client_cred(int fd, sock_cred &cred) {
        socklen_t len = sizeof(ucred);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
            return false;
        }
        char buf[4096];
        len = sizeof(buf);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERSEC, buf, &len) != 0) {
            len = 0;
        }
        buf[len] = '\0';
        cred.context = buf;
        return true;
    }

    ssize_t xread(int fd, void* buf, size_t count) {
        size_t read_sz = 0;
        ssize_t ret;
        do {
            ret = read(fd, (std::byte*) buf + read_sz, count - read_sz);
            if (ret < 0) {
                if (errno == EINTR) continue;
                return ret;
            }
            read_sz += ret;
        } while (read_sz != count && ret != 0);
        if (read_sz != count) {
            errno = EIO;
        }
        return read_sz;
    }

    size_t xwrite(int fd, const void* buf, size_t count) {
        size_t write_sz = 0;
        ssize_t ret;
        do {
            ret = write(fd, (std::byte*) buf + write_sz, count - write_sz);
            if (ret < 0) {
                if (errno == EINTR) continue;
                return write_sz;
            }
            write_sz += ret;
        } while (write_sz != count && ret != 0);
        if (write_sz != count) {
            errno = EIO;
        }
        return write_sz;
    }

    ssize_t xrecvmsg(int sockfd, struct msghdr* msg, int flags) {
        int rec = recvmsg(sockfd, msg, flags);
        return rec;
    }

    ssize_t xsendmsg(int sockfd, struct msghdr* msg, int flags) {
        int rec = sendmsg(sockfd, msg, flags);
        return rec;
    }

    template<typename T>
    inline T read_exact_or(int fd, T fail) {
        T res;
        return sizeof(T) == xread(fd, &res, sizeof(T)) ? res : fail;
    }

    template<typename T>
    inline bool write_exact(int fd, T val) {
        return sizeof(T) == xwrite(fd, &val, sizeof(T));
    }

    uint8_t read_u8(int fd) {
        return read_exact_or<uint8_t>(fd, -1);
    }

    uint32_t read_u32(int fd) {
        return read_exact_or<uint32_t>(fd, -1);
    }

    int32_t read_i32(int fd) {
        return read_exact_or<int32_t>(fd, -1);
    }

    std::string read_string(int fd) {
        auto len = read_i32(fd);
        if (len > kMaxStringSize) {
            errno = E2BIG;
            return "";
        } else if (len <= 0) return "";
        char buf[len + 1];
        buf[len] = '\0';
        xread(fd, buf, len);
        return buf;
    }

    bool write_u8(int fd, uint8_t val) {
        return write_exact<uint8_t>(fd, val);
    }

    bool write_u32(int fd, uint32_t val) {
        return write_exact<uint32_t>(fd, val);
    }

    bool write_u64(int fd, uint64_t val) {
        return write_exact<uint64_t>(fd, val);
    }

    bool write_i32(int fd, int32_t val) {
        return write_exact<int32_t>(fd, val);
    }

    bool write_string(int fd, std::string_view str) {
        if (str.size() > kMaxStringSize) {
            errno = E2BIG;
            return write_i32(fd, 0);
        }
        return write_i32(fd, str.size()) && (str.empty() || str.size() == xwrite(fd, str.data(), str.size()));
    }

    bool set_sockcreate_con(const char* con) {
        auto sz = static_cast<ssize_t>(strlen(con) + 1);
        UniqueFd fd = open("/proc/thread-self/attr/sockcreate", O_WRONLY | O_CLOEXEC);
        if (fd == -1 || write(fd, con, sz) != sz) {
            char buf[128];
            snprintf(buf, sizeof(buf), "/proc/%d/attr/sockcreate", gettid());
            fd = open(buf, O_WRONLY | O_CLOEXEC);
            if (fd == -1 || write(fd, con, sz) != sz) {
                return false;
            }
        }
        return true;
    }

    static int send_fds(int sockfd, void *cmsgbuf, size_t bufsz, const int *fds, int cnt) {
        iovec iov = {
                .iov_base = &cnt,
                .iov_len  = sizeof(cnt),
        };
        msghdr msg = {
                .msg_iov        = &iov,
                .msg_iovlen     = 1,
        };

        if (cnt) {
            msg.msg_control    = cmsgbuf;
            msg.msg_controllen = bufsz;
            cmsghdr *cmsg    = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_len   = CMSG_LEN(sizeof(int) * cnt);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type  = SCM_RIGHTS;

            memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * cnt);
        }

        return xsendmsg(sockfd, &msg, 0);
    }

    int send_fds(int sockfd, const int *fds, int cnt) {
        if (cnt == 0) {
            return send_fds(sockfd, nullptr, 0, nullptr, 0);
        }
        std::vector<char> cmsgbuf;
        cmsgbuf.resize(CMSG_SPACE(sizeof(int) * cnt));
        return send_fds(sockfd, cmsgbuf.data(), cmsgbuf.size(), fds, cnt);
    }

    int send_fd(int sockfd, int fd) {
        if (fd < 0) {
            return send_fds(sockfd, nullptr, 0, nullptr, 0);
        }
        char cmsgbuf[CMSG_SPACE(sizeof(int))];
        return send_fds(sockfd, cmsgbuf, sizeof(cmsgbuf), &fd, 1);
    }

    static void *recv_fds(int sockfd, char *cmsgbuf, size_t bufsz, int cnt) {
        iovec iov = {
                .iov_base = &cnt,
                .iov_len  = sizeof(cnt),
        };
        msghdr msg = {
                .msg_iov        = &iov,
                .msg_iovlen     = 1,
                .msg_control    = cmsgbuf,
                .msg_controllen = bufsz
        };

        xrecvmsg(sockfd, &msg, MSG_WAITALL);
        if (msg.msg_controllen != bufsz) {
            return nullptr;
        }

        cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg == nullptr) {
            return nullptr;
        }
        if (cmsg->cmsg_len != CMSG_LEN(sizeof(int) * cnt)) {
            return nullptr;
        }
        if (cmsg->cmsg_level != SOL_SOCKET) {
            return nullptr;
        }
        if (cmsg->cmsg_type != SCM_RIGHTS) {
            return nullptr;
        }

        return CMSG_DATA(cmsg);
    }

    std::vector<int> recv_fds(int sockfd) {
        std::vector<int> results;

        // Peek fd count to allocate proper buffer
        int cnt;
        recv(sockfd, &cnt, sizeof(cnt), MSG_PEEK);
        if (cnt == 0) {
            // Consume data
            recv(sockfd, &cnt, sizeof(cnt), MSG_WAITALL);
            return results;
        }

        std::vector<char> cmsgbuf;
        cmsgbuf.resize(CMSG_SPACE(sizeof(int) * cnt));

        void *data = recv_fds(sockfd, cmsgbuf.data(), cmsgbuf.size(), cnt);
        if (data == nullptr)
            return results;

        results.resize(cnt);
        memcpy(results.data(), data, sizeof(int) * cnt);

        return results;
    }

    int recv_fd(int sockfd) {
        // Peek fd count
        int cnt;
        recv(sockfd, &cnt, sizeof(cnt), MSG_PEEK);
        if (cnt == 0) {
            // Consume data
            recv(sockfd, &cnt, sizeof(cnt), MSG_WAITALL);
            return -1;
        }

        char cmsgbuf[CMSG_SPACE(sizeof(int))];

        void *data = recv_fds(sockfd, cmsgbuf, sizeof(cmsgbuf), 1);
        if (data == nullptr)
            return -1;

        int result;
        memcpy(&result, data, sizeof(int));
        return result;
    }

    bool check_unix_socket(int fd, bool block) {
        // Make sure the socket is still valid
        pollfd pfd = { fd, POLLIN, 0 };
        TEMP_FAILURE_RETRY(poll(&pfd, 1, block ? -1 : 0));
        if ((pfd.revents & ~POLLIN) != 0) {
            // Any revent means error
            close(fd);
            return false;
        }
        return true;
    }

    bool clear_cloexec(int fd) {
        auto flags = fcntl(fd, F_GETFD);
        if (flags == -1) {
            return false;
        }
        if (fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) == -1) {
            return false;
        }
        return true;
    }
}