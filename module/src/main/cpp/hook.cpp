#include <cstring>
#include <android/log.h>
#include <dlfcn.h>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>

#include "zygisk_next_api.h"
#include "socket_utils.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "hostsredirect", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "hostsredirect", __VA_ARGS__)

static ZygiskNextAPI api_table;
void* handle;

long setfilecon(const char* path, const char* con) {
    return syscall(__NR_setxattr, path, XATTR_NAME_SELINUX, con, strlen(con) + 1, 0);
}

static FILE*(*old_fopen)(const char *name, const char *mode) = nullptr;
static FILE* my_fopen(const char *name, const char *mode) {
    // https://cs.android.com/android/platform/superproject/main/+/main:packages/modules/DnsResolver/getaddrinfo.cpp;l=1467;drc=a4ac93b700ed623bdb333ccb2ac567b8a33081a7;bpv=0;bpt=1
    if (strcmp(name, "/system/etc/hosts") == 0) {
        auto fd = api_table.connectCompanion(handle);
        if (fd < 0) {
            LOGE("failed to connect to companion");
            return old_fopen(name, mode);
        }
        auto file_fd = socket_utils::recv_fd(fd);
        if (file_fd < 0) {
            LOGE("failed to get hosts file");
            close(fd);
            return old_fopen(name, mode);
        }
        close(fd);
        return fdopen(file_fd, "r");
    }
    return old_fopen(name, mode);
}

// this function will be called after all of the main executable's needed libraries are loaded
// and before the entry of the main executable called
void onModuleLoaded(void* self_handle, const struct ZygiskNextAPI* api) {
    // You need to copy the api table if you want to use it after this callback finished
    memcpy(&api_table, api, sizeof(struct ZygiskNextAPI));
    handle = self_handle;

    // inline hook netd's fopen function
    auto fun = dlsym(RTLD_DEFAULT, "fopen");
    if (api_table.inlineHook(fun, (void *) my_fopen, (void**) &old_fopen) == ZN_SUCCESS) {
        LOGI("inline hook success %p", old_fopen);
    } else {
        LOGE("inline hook failed");
    }
}

// declaration of the zygisk next module
__attribute__((visibility("default"), unused))
struct ZygiskNextModule zn_module = {
        .target_api_version = ZYGISK_NEXT_API_VERSION_1,
        .onModuleLoaded = onModuleLoaded,
};

void onCompanionLoaded() {
    LOGI("companion loaded");
}

void onModuleConnected(int fd) {
    const char* hosts = "/data/adb/hostsredirect/hosts";
    struct stat st{};
    if (stat(hosts, &st) < 0) {
        LOGE("no hosts file found");
        close(fd);
        return;
    }
    // netd needs to access hosts file socket
    setfilecon(hosts, "u:object_r:system_file:s0");
    auto hosts_fd = open(hosts, O_RDONLY | O_CLOEXEC);
    if (hosts_fd < 0) {
        LOGE("failed to open hosts file");
        close(fd);
        return;
    }
    socket_utils::send_fd(fd, hosts_fd);
    close(hosts_fd);
    // need to be closed unconditionally
    close(fd);
}

__attribute__((visibility("default"), unused))
struct ZygiskNextCompanionModule zn_companion_module = {
        .target_api_version = ZYGISK_NEXT_API_VERSION_1,
        .onCompanionLoaded = onCompanionLoaded,
        .onModuleConnected = onModuleConnected,
};
