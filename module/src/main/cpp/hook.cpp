#include <android/log.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>

#include "zygisk_next_api.h"
#include "socket_utils.h"

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "hostsredirect", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "hostsredirect", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "hostsredirect", __VA_ARGS__)

static ZygiskNextAPI api_table;
void* handle;

// backup of old __openat function
static int (*old_openat)(int fd, const char* pathname, int flag, int mode) = nullptr;
// our replacement for __openat function
static int my_openat(int fd, const char* pathname, int flag, int mode) {
    // https://android.googlesource.com/platform/system/netd/+/55864199479074e8fb3d285220280ccda270fe7d
    // https://github.com/LineageOS/android_system_netd/commit/f92bf2804098512142cc8d7934ed9d5031b0532c
    if (strcmp(pathname, "/system/etc/hosts") != 0) {
        return old_openat(fd, pathname, flag, mode);
    }

    auto cp_fd = api_table.connectCompanion(handle);
    if (cp_fd < 0) {
        return old_openat(fd, pathname, flag, mode);
    }

    auto file_fd = socket_utils::recv_fd(cp_fd);
    close(cp_fd);

    if (file_fd < 0) {
        return old_openat(fd, pathname, flag, mode);
    }

    return file_fd;
}

// this function will be called after all of the main executable's needed libraries are loaded
// and before the entry of the main executable called
void onModuleLoaded(void* self_handle, const struct ZygiskNextAPI* api) {
    // You need to copy the api table if you want to use it after this callback finished
    memcpy(&api_table, api, sizeof(struct ZygiskNextAPI));
    handle = self_handle;

    auto resolver = api_table.newSymbolResolver("libc.so", nullptr);
    if (!resolver) {
        LOGE("create resolver failed");
        return;
    }

    size_t sz;
    auto addr = api_table.symbolLookup(resolver, "__openat", false, &sz);

    api_table.freeSymbolResolver(resolver);

    if (addr == nullptr) {
        LOGE("failed to find __openat");
        return;
    }

    // inline hook netd's openat function
    if (api_table.inlineHook(addr, (void *) my_openat, (void**) &old_openat) == ZN_SUCCESS) {
        LOGI("inline hook success %p", old_openat);
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

static void onCompanionLoaded() {
    LOGI("companion loaded");
}

static void onModuleConnected(int fd) {
    auto hosts = "/data/adb/hostsredirect/hosts";
    struct stat st{};
    if (stat(hosts, &st) < 0) {
        LOGD("no hosts file found");
        close(fd);
        return;
    }

    // netd needs to access hosts file socket
    auto system_file = "u:object_r:system_file:s0";
    syscall(__NR_setxattr, hosts, XATTR_NAME_SELINUX, system_file, strlen(system_file) + 1, 0);

    auto hosts_fd = open(hosts, O_RDONLY | O_CLOEXEC);
    if (hosts_fd < 0) {
        LOGD("failed to open hosts file");
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
