/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "devices.h"

#include <errno.h>
#include <fnmatch.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <memory>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <private/android_filesystem_config.h>
#include <selinux/android.h>
#include <selinux/selinux.h>

#include "ueventd.h"
#include "util.h"

#ifdef _INIT_INIT_H
#error "Do not include init.h in files used by ueventd or watchdogd; it will expose init's globals"
#endif

using android::base::Basename;
using android::base::Dirname;
using android::base::Readlink;
using android::base::Realpath;
using android::base::StartsWith;
using android::base::StringPrintf;

namespace android {
namespace init {

/* Given a path that may start with a PCI device, populate the supplied buffer
 * with the PCI domain/bus number and the peripheral ID and return 0.
 * If it doesn't start with a PCI device, or there is some error, return -1 */
static bool FindPciDevicePrefix(const std::string& path, std::string* result) {
    result->clear();

    if (!StartsWith(path, "/devices/pci")) return false;

    /* Beginning of the prefix is the initial "pci" after "/devices/" */
    std::string::size_type start = 9;

    /* End of the prefix is two path '/' later, capturing the domain/bus number
     * and the peripheral ID. Example: pci0000:00/0000:00:1f.2 */
    auto end = path.find('/', start);
    if (end == std::string::npos) return false;

    end = path.find('/', end + 1);
    if (end == std::string::npos) return false;

    auto length = end - start;
    if (length <= 4) {
        // The minimum string that will get to this check is 'pci/', which is malformed,
        // so return false
        return false;
    }

    *result = path.substr(start, length);
    return true;
}

/* Given a path that may start with a virtual block device, populate
 * the supplied buffer with the virtual block device ID and return 0.
 * If it doesn't start with a virtual block device, or there is some
 * error, return -1 */
static bool FindVbdDevicePrefix(const std::string& path, std::string* result) {
    result->clear();

    if (!StartsWith(path, "/devices/vbd-")) return false;

    /* Beginning of the prefix is the initial "vbd-" after "/devices/" */
    std::string::size_type start = 13;

    /* End of the prefix is one path '/' later, capturing the
       virtual block device ID. Example: 768 */
    auto end = path.find('/', start);
    if (end == std::string::npos) return false;

    auto length = end - start;
    if (length == 0) return false;

    *result = path.substr(start, length);
    return true;
}

Permissions::Permissions(const std::string& name, mode_t perm, uid_t uid, gid_t gid)
    : name_(name), perm_(perm), uid_(uid), gid_(gid), prefix_(false), wildcard_(false) {
    // Set 'prefix_' or 'wildcard_' based on the below cases:
    //
    // 1) No '*' in 'name' -> Neither are set and Match() checks a given path for strict
    //    equality with 'name'
    //
    // 2) '*' only appears as the last character in 'name' -> 'prefix'_ is set to true and
    //    Match() checks if 'name' is a prefix of a given path.
    //
    // 3) '*' appears elsewhere -> 'wildcard_' is set to true and Match() uses fnmatch()
    //    with FNM_PATHNAME to compare 'name' to a given path.

    auto wildcard_position = name_.find('*');
    if (wildcard_position != std::string::npos) {
        if (wildcard_position == name_.length() - 1) {
            prefix_ = true;
            name_.pop_back();
        } else {
            wildcard_ = true;
        }
    }
}

bool Permissions::Match(const std::string& path) const {
    if (prefix_) return StartsWith(path, name_.c_str());
    if (wildcard_) return fnmatch(name_.c_str(), path.c_str(), FNM_PATHNAME) == 0;
    return path == name_;
}

bool SysfsPermissions::MatchWithSubsystem(const std::string& path,
                                          const std::string& subsystem) const {
    std::string path_basename = Basename(path);
    if (name().find(subsystem) != std::string::npos) {
        if (Match("/sys/class/" + subsystem + "/" + path_basename)) return true;
        if (Match("/sys/bus/" + subsystem + "/devices/" + path_basename)) return true;
    }
    return Match(path);
}

void SysfsPermissions::SetPermissions(const std::string& path) const {
    std::string attribute_file = path + "/" + attribute_;
    LOG(VERBOSE) << "fixup " << attribute_file << " " << uid() << " " << gid() << " " << std::oct
                 << perm();

    if (access(attribute_file.c_str(), F_OK) == 0) {
        if (chown(attribute_file.c_str(), uid(), gid()) != 0) {
            PLOG(ERROR) << "chown(" << attribute_file << ", " << uid() << ", " << gid()
                        << ") failed";
        }
        if (chmod(attribute_file.c_str(), perm()) != 0) {
            PLOG(ERROR) << "chmod(" << attribute_file << ", " << perm() << ") failed";
        }
    }
}

// Given a path that may start with a platform device, find the parent platform device by finding a
// parent directory with a 'subsystem' symlink that points to the platform bus.
// If it doesn't start with a platform device, return false
bool DeviceHandler::FindPlatformDevice(std::string path, std::string* platform_device_path) const {
    platform_device_path->clear();

    // Uevents don't contain the mount point, so we need to add it here.
    path.insert(0, sysfs_mount_point_);

    std::string directory = Dirname(path);

    while (directory != "/" && directory != ".") {
        std::string subsystem_link_path;
        if (Realpath(directory + "/subsystem", &subsystem_link_path) &&
            subsystem_link_path == sysfs_mount_point_ + "/bus/platform") {
            // We need to remove the mount point that we added above before returning.
            directory.erase(0, sysfs_mount_point_.size());
            *platform_device_path = directory;
            return true;
        }

        auto last_slash = path.rfind('/');
        if (last_slash == std::string::npos) return false;

        path.erase(last_slash);
        directory = Dirname(path);
    }

    return false;
}

void DeviceHandler::FixupSysPermissions(const std::string& upath,
                                        const std::string& subsystem) const {
    // upaths omit the "/sys" that paths in this list
    // contain, so we prepend it...
    std::string path = "/sys" + upath;

    for (const auto& s : sysfs_permissions_) {
        if (s.MatchWithSubsystem(path, subsystem)) s.SetPermissions(path);
    }

    if (is_selinux_enabled() <= 0)
        return;

    if (!skip_restorecon_ && access(path.c_str(), F_OK) == 0) {
        LOG(VERBOSE) << "restorecon_recursive: " << path;
        if (selinux_android_restorecon(path.c_str(), SELINUX_ANDROID_RESTORECON_RECURSE) != 0) {
            PLOG(ERROR) << "selinux_android_restorecon(" << path << ") failed";
        }
    }
}

std::tuple<mode_t, uid_t, gid_t> DeviceHandler::GetDevicePermissions(
    const std::string& path, const std::vector<std::string>& links) const {
    // Search the perms list in reverse so that ueventd.$hardware can override ueventd.rc.
    for (auto it = dev_permissions_.crbegin(); it != dev_permissions_.crend(); ++it) {
        if (it->Match(path) || std::any_of(links.cbegin(), links.cend(),
                                           [it](const auto& link) { return it->Match(link); })) {
            return {it->perm(), it->uid(), it->gid()};
        }
    }
    /* Default if nothing found. */
    return {0600, 0, 0};
}

void DeviceHandler::MakeDevice(const std::string& path, bool block, int major, int minor,
                               const std::vector<std::string>& links) const {
    auto[mode, uid, gid] = GetDevicePermissions(path, links);
    mode |= (block ? S_IFBLK : S_IFCHR);

    char* secontext = nullptr;

    if (is_selinux_enabled() > 0) {
        if (sehandle_) {
            std::vector<const char*> c_links;
            for (const auto& link : links) {
                c_links.emplace_back(link.c_str());
            }
            c_links.emplace_back(nullptr);
            if (selabel_lookup_best_match(sehandle_, &secontext, path.c_str(), &c_links[0], mode)) {
                PLOG(ERROR) << "Device '" << path << "' not created; cannot find SELinux label";
                return;
            }
            setfscreatecon(secontext);
        }
    }

    dev_t dev = makedev(major, minor);
    /* Temporarily change egid to avoid race condition setting the gid of the
     * device node. Unforunately changing the euid would prevent creation of
     * some device nodes, so the uid has to be set with chown() and is still
     * racy. Fixing the gid race at least fixed the issue with system_server
     * opening dynamic input devices under the AID_INPUT gid. */
    if (setegid(gid)) {
        PLOG(ERROR) << "setegid(" << gid << ") for " << path << " device failed";
        goto out;
    }
    /* If the node already exists update its SELinux label to handle cases when
     * it was created with the wrong context during coldboot procedure. */

    mknod(path.c_str(), mode, dev);
    if (is_selinux_enabled() > 0) {
        if ((errno == EEXIST) && secontext) {
            char* fcon = nullptr;
            int rc = lgetfilecon(path.c_str(), &fcon);
            if (rc < 0) {
                PLOG(ERROR) << "Cannot get SELinux label on '" << path << "' device";
                goto out;
            }

            bool different = strcmp(fcon, secontext) != 0;
            freecon(fcon);

            if (different && lsetfilecon(path.c_str(), secontext)) {
                PLOG(ERROR) << "Cannot set '" << secontext << "' SELinux label on '" << path
                            << "' device";
            }
        }
    }

out:
    chown(path.c_str(), uid, -1);
    if (setegid(AID_ROOT)) {
        PLOG(FATAL) << "setegid(AID_ROOT) failed";
    }
    if (is_selinux_enabled() > 0) {
        if (secontext) {
            freecon(secontext);
            setfscreatecon(nullptr);
        }
    }
}

// replaces any unacceptable characters with '_', the
// length of the resulting string is equal to the input string
void SanitizePartitionName(std::string* string) {
    const char* accept =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789"
        "_-.";

    if (!string) return;

    std::string::size_type pos = 0;
    while ((pos = string->find_first_not_of(accept, pos)) != std::string::npos) {
        (*string)[pos] = '_';
    }
}

std::vector<std::string> DeviceHandler::GetBlockDeviceSymlinks(const Uevent& uevent) const {
    std::string device;
    std::string type;

    if (FindPlatformDevice(uevent.path, &device)) {
        // Skip /devices/platform or /devices/ if present
        static const std::string devices_platform_prefix = "/devices/platform/";
        static const std::string devices_prefix = "/devices/";

        if (StartsWith(device, devices_platform_prefix.c_str())) {
            device = device.substr(devices_platform_prefix.length());
        } else if (StartsWith(device, devices_prefix.c_str())) {
            device = device.substr(devices_prefix.length());
        }

        type = "platform";
    } else if (FindPciDevicePrefix(uevent.path, &device)) {
        type = "pci";
    } else if (FindVbdDevicePrefix(uevent.path, &device)) {
        type = "vbd";
    } else {
        return {};
    }

    std::vector<std::string> links;

    LOG(VERBOSE) << "found " << type << " device " << device;

    auto link_path = "/dev/block/" + type + "/" + device;

    if (!uevent.partition_name.empty()) {
        std::string partition_name_sanitized(uevent.partition_name);
        SanitizePartitionName(&partition_name_sanitized);
        if (partition_name_sanitized != uevent.partition_name) {
            LOG(VERBOSE) << "Linking partition '" << uevent.partition_name << "' as '"
                         << partition_name_sanitized << "'";
        }
        links.emplace_back(link_path + "/by-name/" + partition_name_sanitized);
    }

    if (uevent.partition_num >= 0) {
        links.emplace_back(link_path + "/by-num/p" + std::to_string(uevent.partition_num));
    }

    auto last_slash = uevent.path.rfind('/');
    links.emplace_back(link_path + "/" + uevent.path.substr(last_slash + 1));

    return links;
}

void DeviceHandler::HandleDevice(const std::string& action, const std::string& devpath, bool block,
                                 int major, int minor, const std::vector<std::string>& links) const {
    if (action == "add") {
        MakeDevice(devpath, block, major, minor, links);
        for (const auto& link : links) {
            if (mkdir_recursive(Dirname(link), 0755, sehandle_)) {
                PLOG(ERROR) << "Failed to create directory " << Dirname(link);
            }

            if (symlink(devpath.c_str(), link.c_str()) && errno != EEXIST) {
                PLOG(ERROR) << "Failed to symlink " << devpath << " to " << link;
            }
        }
    }

    if (action == "remove") {
        for (const auto& link : links) {
            std::string link_path;
            if (Readlink(link, &link_path) && link_path == devpath) {
                unlink(link.c_str());
            }
        }
        unlink(devpath.c_str());
    }
}

void DeviceHandler::HandleDeviceEvent(const Uevent& uevent) {
    if (uevent.action == "add" || uevent.action == "change" || uevent.action == "online") {
        FixupSysPermissions(uevent.path, uevent.subsystem);
    }

    // if it's not a /dev device, nothing to do
    if (uevent.major < 0 || uevent.minor < 0) return;

    std::string devpath;
    std::vector<std::string> links;
    bool block = false;

    if (uevent.subsystem == "block") {
        block = true;
        devpath = "/dev/block/" + Basename(uevent.path);

        if (StartsWith(uevent.path, "/devices")) {
            links = GetBlockDeviceSymlinks(uevent);
        }
    } else if (StartsWith(uevent.subsystem, "usb")) {
        if (uevent.subsystem == "usb") {
            if (!uevent.device_name.empty()) {
                devpath = "/dev/" + uevent.device_name;
            } else {
                // This imitates the file system that would be created
                // if we were using devfs instead.
                // Minors are broken up into groups of 128, starting at "001"
                int bus_id = uevent.minor / 128 + 1;
                int device_id = uevent.minor % 128 + 1;
                devpath = StringPrintf("/dev/bus/usb/%03d/%03d", bus_id, device_id);
            }
        } else {
            // ignore other USB events
            return;
        }
    } else if (const auto subsystem =
                   std::find(subsystems_.cbegin(), subsystems_.cend(), uevent.subsystem);
               subsystem != subsystems_.cend()) {
        devpath = subsystem->ParseDevPath(uevent);
    } else {
        devpath = "/dev/" + Basename(uevent.path);
    }

    mkdir_recursive(Dirname(devpath), 0755, sehandle_);

    HandleDevice(uevent.action, devpath, block, uevent.major, uevent.minor, links);
}

DeviceHandler::DeviceHandler(std::vector<Permissions> dev_permissions,
                             std::vector<SysfsPermissions> sysfs_permissions,
                             std::vector<Subsystem> subsystems, bool skip_restorecon)
    : dev_permissions_(std::move(dev_permissions)),
      sysfs_permissions_(std::move(sysfs_permissions)),
      subsystems_(std::move(subsystems)),
      sehandle_(selinux_android_file_context_handle()),
      skip_restorecon_(skip_restorecon),
      sysfs_mount_point_("/sys") {}

DeviceHandler::DeviceHandler()
    : DeviceHandler(std::vector<Permissions>{}, std::vector<SysfsPermissions>{},
                    std::vector<Subsystem>{}, false) {}

}  // namespace init
}  // namespace android
