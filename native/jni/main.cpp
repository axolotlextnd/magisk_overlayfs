#include <stdio.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <iostream>
#include <sys/mman.h>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <string_view>
#include <string>
#include <selinux/selinux.h>
#include <limits.h>
#include <stdlib.h>

int mkdirs(const char *path, int mode) {
    char *s = strdup(path);
    char *ss = s;
    while ((ss = strchr(ss, '/')) != nullptr) {
        ss[0] = '\0';
        mkdir(s, mode);
        ss[0] = '/';
        ss++;
    }
    free(s);
    return mkdir(path, mode);
}
 

std::string tmp_dir;

#define FAKE_CODE 0

#if !FAKE_CODE
#include <sys/mount.h>

int verbose_mount(const char *a, const char *b, const char *c, int d, const char *e) {
    printf("mount: %s -> %s %s%s%s\n", a, b, e? "[" : "", e? e : "", e? "]" : "");
    return mount(a,b,c,d,e);
}

int verbose_umount(const char *a, int b) {
    printf("umount: %s\n", a);
    return umount2(a,b);
}

#define mount(a,b,c,d,e) verbose_mount(a,b,c,d,e)
#define umount2(a,b) verbose_umount(a,b)

#endif

struct mount_info {
    unsigned int id;
    unsigned int parent;
    dev_t device;
    std::string root;
    std::string target;
    std::string vfs_option;
    struct {
        unsigned int shared;
        unsigned int master;
        unsigned int propagate_from;
    } optional;
    std::string type;
    std::string source;
    std::string fs_option;
};

#define ssprintf snprintf
#define parse_int(s) atoi(s.data())

// based on mountinfo code from https://github.com/yujincheng08

using namespace std;

#define CLEANUP \
    printf("clean up\n"); \
    umount2(tmp_dir.data(), MNT_DETACH); \
    rmdir(tmp_dir.data());

std::vector<mount_info> parse_mount_info(const char *pid) {
    char buf[4098] = {};
    ssprintf(buf, sizeof(buf), "/proc/%s/mountinfo", pid);
    std::vector<mount_info> result;
    FILE *fp = fopen(buf, "re");
    if (fp == nullptr) return result;

    while (fgets(buf, sizeof(buf), fp)) {
        string_view line = buf;
        int root_start = 0, root_end = 0;
        int target_start = 0, target_end = 0;
        int vfs_option_start = 0, vfs_option_end = 0;
        int type_start = 0, type_end = 0;
        int source_start = 0, source_end = 0;
        int fs_option_start = 0, fs_option_end = 0;
        int optional_start = 0, optional_end = 0;
        unsigned int id, parent, maj, min;
        sscanf(line.data(),
               "%u "           // (1) id
               "%u "           // (2) parent
               "%u:%u "        // (3) maj:min
               "%n%*s%n "      // (4) mountroot
               "%n%*s%n "      // (5) target
               "%n%*s%n"       // (6) vfs options (fs-independent)
               "%n%*[^-]%n - " // (7) optional fields
               "%n%*s%n "      // (8) FS type
               "%n%*s%n "      // (9) source
               "%n%*s%n",      // (10) fs options (fs specific)
               &id, &parent, &maj, &min, &root_start, &root_end, &target_start,
               &target_end, &vfs_option_start, &vfs_option_end,
               &optional_start, &optional_end, &type_start, &type_end,
               &source_start, &source_end, &fs_option_start, &fs_option_end);

        auto root = line.substr(root_start, root_end - root_start);
        auto target = line.substr(target_start, target_end - target_start);
        auto vfs_option =
                line.substr(vfs_option_start, vfs_option_end - vfs_option_start);
        ++optional_start;
        --optional_end;
        auto optional = line.substr(
                optional_start,
                optional_end - optional_start > 0 ? optional_end - optional_start : 0);

        auto type = line.substr(type_start, type_end - type_start);
        auto source = line.substr(source_start, source_end - source_start);
        auto fs_option =
                line.substr(fs_option_start, fs_option_end - fs_option_start);

        unsigned int shared = 0;
        unsigned int master = 0;
        unsigned int propagate_from = 0;
        if (auto pos = optional.find("shared:"); pos != std::string_view::npos) {
            shared = parse_int(optional.substr(pos + 7));
        }
        if (auto pos = optional.find("master:"); pos != std::string_view::npos) {
            master = parse_int(optional.substr(pos + 7));
        }
        if (auto pos = optional.find("propagate_from:");
                pos != std::string_view::npos) {
            propagate_from = parse_int(optional.substr(pos + 15));
        }
        mount_info mnt_entry;
        mnt_entry.id = id;
        mnt_entry.parent = parent;
        mnt_entry.device = static_cast<dev_t>(makedev(maj, min));
        mnt_entry.root = root;
        mnt_entry.vfs_option = vfs_option;
        mnt_entry.optional = {
            .shared = shared,
            .master = master,
            .propagate_from = propagate_from,
            };
        mnt_entry.root = root;
        mnt_entry.target = target;
        mnt_entry.type = type;
        mnt_entry.source = source;
        mnt_entry.fs_option = fs_option;

        result.emplace_back(mnt_entry);
    }
    return result;
}

bool starts_with(const char *s, const char *ss) {
    const char *str = strstr(s,ss);
    return str != nullptr && str == s;
}

#undef ssprintf

#define UNDER(s) (starts_with(info.target.data(), s "/") || info.target == s)

std::string random_strc(int n){
    std::string result = "";
    FILE *urandom = fopen("/dev/urandom", "re");
    if (urandom == nullptr) return result;
    char *str = new char[n+1];
    if (str == nullptr) {
        fclose(urandom);
        return result;
    }
    for (int i=0;i<n;i++){
        str[i] = 'a' + (fgetc(urandom) % ('z'-'a'+1));
    }
    fclose(urandom);
    result = str;
    delete []str;
    return result;
}

#if FAKE_CODE

#define umount2(a,b) nullptr

int fake_return_mount(const char *a, const char *b, const char *c) {
    printf("%s -> %s (%s)\n", a, b, c? c : "");
	return 0;
}

#define mount(a,b,c,d,e) fake_return_mount(a,b,e)
#define mkdir(a,b) 0

#endif

#define MAKEDIR(s) \
    if (std::find(mountpoint.begin(), mountpoint.end(), "/" s) != mountpoint.end()) \
        mkdir(std::string(tmp_dir + "/" s).data(), 0755);

bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 &&
           S_ISDIR(st.st_mode);
}

bool mkdir_ensure(const char *path, int mode) {
    mkdir(path, mode);
    return is_dir(path);
}


int overlay_main(int argc, const char **argv) {
#if FAKE_CODE
    if (getuid() != 0) {
        execlp("su", "su", "-c", argv[0], 0);
        _exit(0);
    }
    argc = 2;
    argv[1] = "/mnt/test";
    const char *mirrors = nullptr;
#endif
    bool overlay = false;
    FILE *fp = fopen("/proc/filesystems", "re");
    if (fp) {
        char buf[1024];
        while (fgets(buf, sizeof(buf), fp)) {
            buf[strlen(buf)-1] = '\0';
            if (strcmp(buf + 6, "overlay") == 0) {
                overlay = true;
                break;
            }
        }
        fclose(fp);
    }
#if !FAKE_CODE
    if (!overlay) {
        printf("No overlay supported by kernel!\n");
        return 1;
    }
    if (argc<2) {
        printf("You forgot to tell me the write-able folder :v\n");
        return 1;
    }
    if (strcmp(argv[1], "--test") == 0) {
        return 0;
    } else if (argv[1][0] != '/') {
        printf("Please tell me the full path of folder >:)\n");
        return 1;
    }
    struct stat z;
    if (stat(argv[1], &z)) {
        printf("%s does not exist!\n", argv[1]);
        return 1;
    }
    if (!S_ISDIR(z.st_mode)) {
        printf("This is not folder %s\n", argv[1]);
        return 1;
    }
    const char *mirrors =
    (argc >= 3 && argv[2][0] == '/' && stat(argv[2], &z) == 0 && S_ISDIR(z.st_mode))?
        argv[2] : nullptr;
#endif
    std::vector<string> mountpoint;
    std::vector<mount_info> mountinfo;
    // trim mountinfo
    tmp_dir = std::string("/mnt/") + "overlayfs_" + random_strc(20);
#if !FAKE_CODE
    if (mkdir(tmp_dir.data(), 750) != 0) {
        printf("Cannot create temp folder, please make sure /mnt is clean and write-able!\n");
        return -1;
    }
    mkdir(std::string(std::string(argv[1]) + "/upper").data(), 0750);
    mkdir(std::string(std::string(argv[1]) + "/worker").data(), 0750);
    mount("tmpfs", tmp_dir.data(), "tmpfs", 0, nullptr);
#endif
    do {
        auto current_mount_info = parse_mount_info("self");
        std::reverse(current_mount_info.begin(), current_mount_info.end());
        for (auto &info : current_mount_info) {
            struct stat st;
            // skip mount under another mounr
            if (stat(info.target.data(), &st) || info.device != st.st_dev)
                continue;
            if (UNDER("/system") ||
                 UNDER("/vendor") ||
                 UNDER("/system_ext") ||
                 UNDER("/product")) {
                for (auto &s : mountpoint) {
                    if (s == info.target)
                        goto next_mountpoint;
                 }
                //printf("new mount: %s\n", info.target.data());
                mountpoint.emplace_back(info.target);
                mountinfo.emplace_back(info);
            }
            next_mountpoint:
            continue;
        }
    } while(false);

    struct mount_info system;
    system.target = "/system";
    system.type = "ext4";
    if (std::find(mountpoint.begin(), mountpoint.end(), "/system") == mountpoint.end()) {
        mountinfo.emplace_back(system);
        mountpoint.emplace_back("/system");
    }

    MAKEDIR("system")
    MAKEDIR("vendor")
    MAKEDIR("system_ext")
    MAKEDIR("product")

    mountpoint.clear();

    std::reverse(mountinfo.begin(), mountinfo.end());
    for (auto &mnt : mountinfo) {
        auto info = mnt.target;
        struct stat st;
        if (stat(info.data(), &st))
            continue;
        std::string tmp_mount = tmp_dir + info;
        if (strcmp(mnt.type.data(), "vfat") != 0 && S_ISDIR(st.st_mode)) {
            std::string upperdir = std::string(argv[1]) + "/upper" + info;
            std::string workerdir = std::string(argv[1]) + "/worker" + info;
#if !FAKE_CODE
            char *s = strdup(info.data());
            char *ss = s;
            char *con;
            while ((ss = strchr(ss, '/')) != nullptr) {
                ss[0] = '\0';
                auto sub = std::string(argv[1]) + "/upper" + s;
                if (mkdir(sub.data(), 0755) == 0 && getfilecon(s, &con) >= 0) {
                    printf("clone selinux context [%s] from %s\n", con, s);
                    chmod(sub.data(), 0755);
                    setfilecon(sub.data(), con);
                    freecon(con);
                }
                ss[0] = '/';
                ss++;
            }
            if (mkdir(upperdir.data(), 0755) == 0 && getfilecon(info.data(), &con) >= 0) {
                printf("clone selinux context [%s] from %s\n", con, info.data());
                chmod(upperdir.data(), 0755);
                setfilecon(upperdir.data(), con);
                freecon(con);
            }
            mkdirs(workerdir.data(), 0755);
            free(s);

            if (!is_dir(upperdir.data()) ||
                !is_dir(workerdir.data())) {
                printf("setup upperdir or workdir failed!\n");
                CLEANUP
                return 1;
            }
#endif
            std::string opts;
            opts += "lowerdir=";
            opts += info.data();
            opts += ",upperdir=";
            opts += upperdir;
            opts += ",workdir=";
            opts += workerdir;
            if (mount("overlay", tmp_mount.data(), "overlay", 0, opts.data())) {
                printf("mount failed, try read-only overlayfs...\n");
                opts = "lowerdir=";
                opts += upperdir;
                opts += ":";
                opts += info.data();
                if (mount("overlay", tmp_mount.data(), "overlay", 0, opts.data())) {
                    printf("mount failed, use mount bind to restore...\n");
                    if (mount(info.data(), tmp_mount.data(), nullptr, MS_BIND, nullptr)) {
                        printf("mount failed, abort!\n");
                        CLEANUP
                        return 1;
                    }
                }
            }
        } else {
            if (mount(info.data(), tmp_mount.data(), nullptr, MS_BIND, nullptr)) {
                printf("mount failed, abort!\n");
                CLEANUP
                return 1;
            }
        }
    }
    std::vector<string> mounted;
    for (auto &mnt : mountinfo) {
        auto info = mnt.target;
        std::string tmp_mount = tmp_dir + info;
        if (mount(tmp_mount.data(), info.data(), nullptr, MS_BIND, nullptr)) {
            printf("mount failed, abort!\n");
            // revert all mounts
            std::reverse(mounted.begin(), mounted.end());
            for (auto &dir : mounted) {
                umount2(dir.data(), MNT_DETACH);
                if (mirrors != nullptr) {
                    std::string mirror_dir = string(mirrors) + dir;
                    umount2(mirror_dir.data(), MNT_DETACH);
                }
            }
            CLEANUP
            return 1;
        }
        if (mirrors != nullptr) {
            std::string mirror_dir = string(mirrors) + info;
            mount(tmp_mount.data(), mirror_dir.data(), nullptr, MS_BIND, nullptr);
            mount("", mirror_dir.data(), nullptr, MS_PRIVATE, nullptr);
            mount("", mirror_dir.data(), nullptr, MS_SHARED, nullptr);
        }
        mounted.emplace_back(info);
    }
    printf("mount done!\n");
    CLEANUP
    return 0;
}

#define OVERLAY_PATH "/data/overlayfs"

int main(argc, argv) {
    if (getpid() != 1)
        return overlay_main(argc, argv);
    // In INIT process
    char *path = realpath(argv[0], nullptr);
    const char *new_argv[] = { path, OVERLAY_PATH, nullptr };
    mkdir(OVERLAY_PATH, 0755);
    overlay_main(2, new_argv);
    umount2(path, MNT_DETACH);
    execv(path, argv);
    return -1;
}
