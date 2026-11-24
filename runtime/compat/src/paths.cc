#include "andropkg/paths.h"

#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

// True if `path` is `prefix` or below it ("/storage" matches "/storage" and "/storage/x").
static bool Under(const char* path, const char* prefix, size_t n) {
  return strncmp(path, prefix, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

// /proc/self/status as Linux writes it (apps read it for memory use, thread counts, and
// TracerPid: whether a debugger is attached).
extern "C" size_t andropkg_proc_status(char* buf, size_t n) {
  unsigned long long rss = 0, vsize = 0;
  mach_task_basic_info_data_t info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
    rss = info.resident_size >> 10;
    vsize = info.virtual_size >> 10;
  }
  thread_act_array_t threads;
  mach_msg_type_number_t nthreads = 0;
  if (task_threads(mach_task_self(), &threads, &nthreads) == KERN_SUCCESS) {
    for (mach_msg_type_number_t i = 0; i < nthreads; i++) mach_port_deallocate(mach_task_self(), threads[i]);
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), nthreads * sizeof(thread_act_t));
  }
  const char* pkg = getenv("ANDROPKG_APP_DATA_PKG");
  char name[16] = "app_process64";
  if (pkg && *pkg) {  // the kernel's comm: the last 15 characters of the process name
    size_t l = strlen(pkg);
    snprintf(name, sizeof name, "%s", l > 15 ? pkg + l - 15 : pkg);
  }
  int pid = getpid(), uid = static_cast<int>(getuid());
  int r = snprintf(buf, n,
                   "Name:\t%s\nUmask:\t0077\nState:\tS (sleeping)\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t1\n"
                   "TracerPid:\t0\nUid:\t%d\t%d\t%d\t%d\nGid:\t%d\t%d\t%d\t%d\nFDSize:\t256\nGroups:\t\n"
                   "VmPeak:\t%llu kB\nVmSize:\t%llu kB\nVmLck:\t0 kB\nVmPin:\t0 kB\nVmHWM:\t%llu kB\nVmRSS:\t%llu kB\n"
                   "RssAnon:\t%llu kB\nRssFile:\t0 kB\nRssShmem:\t0 kB\nVmData:\t%llu kB\nVmStk:\t8192 kB\nVmExe:\t0 kB\n"
                   "VmLib:\t0 kB\nVmPTE:\t0 kB\nVmSwap:\t0 kB\nThreads:\t%u\nSigQ:\t0/0\nSigPnd:\t0000000000000000\n"
                   "ShdPnd:\t0000000000000000\nSigBlk:\t0000000000000000\nSigIgn:\t0000000000001000\n"
                   "SigCgt:\t00000006400084f8\nCapInh:\t0000000000000000\nCapPrm:\t0000000000000000\n"
                   "CapEff:\t0000000000000000\nCapBnd:\t0000000000000000\nCapAmb:\t0000000000000000\n"
                   "NoNewPrivs:\t0\nSeccomp:\t2\nSeccomp_filters:\t1\nCpus_allowed:\tff\nCpus_allowed_list:\t0-7\n"
                   "voluntary_ctxt_switches:\t0\nnonvoluntary_ctxt_switches:\t0\n",
                   name, pid, pid, uid, uid, uid, uid, uid, uid, uid, uid, vsize, vsize, rss, rss, rss, rss, nthreads);
  return r > 0 ? static_cast<size_t>(r) : 0;
}

// Linux kernel files Java code reads (FileInputStream, Files): there is no /proc on Darwin.
// The content is written to a file under $TMPDIR/andropkg-proc, which is opened instead.
static const char* VirtualProcFile(const char* path, char* buf, size_t len) {
  char self[32];
  snprintf(self, sizeof self, "/proc/%d/", getpid());
  const char* rest = nullptr;
  if (strncmp(path, "/proc/self/", 11) == 0) rest = path + 11;
  else if (strncmp(path, "/proc/thread-self/", 18) == 0) rest = path + 18;
  else if (strncmp(path, self, strlen(self)) == 0) rest = path + strlen(self);
  static char content[8192];
  size_t n = 0;
  const char* file = nullptr;
  if (rest && strcmp(rest, "status") == 0) {
    n = andropkg_proc_status(content, sizeof content);
    file = "status";
  } else if (rest && strcmp(rest, "cmdline") == 0) {
    const char* pkg = getenv("ANDROPKG_APP_DATA_PKG");
    n = static_cast<size_t>(snprintf(content, sizeof content, "%s", pkg && *pkg ? pkg : "app_process64")) + 1;
    file = "cmdline";
  } else if (strcmp(path, "/proc/version") == 0) {
    n = static_cast<size_t>(snprintf(content, sizeof content, "Linux version 6.1.0-andropkg (android-build@andropkg) #1 SMP PREEMPT\n"));
    file = "version";
  } else if (strcmp(path, "/proc/meminfo") == 0) {
    unsigned long long mem = 0;
    size_t sz = sizeof mem;
    sysctlbyname("hw.memsize", &mem, &sz, nullptr, 0);
    n = static_cast<size_t>(snprintf(content, sizeof content,
                                     "MemTotal:       %llu kB\nMemFree:        %llu kB\nMemAvailable:   %llu kB\n"
                                     "Buffers:               0 kB\nCached:                0 kB\nSwapTotal:             0 kB\n"
                                     "SwapFree:              0 kB\n", mem / 1024, mem / 4096, mem / 2048));
    file = "meminfo";
  }
  if (file == nullptr) return nullptr;
  const char* tmp = getenv("TMPDIR");
  char dir[1024];
  snprintf(dir, sizeof dir, "%s/andropkg-proc", tmp && *tmp ? tmp : "/tmp");
  mkdir(dir, 0700);
  int r = snprintf(buf, len, "%s/%s", dir, file);
  if (r <= 0 || static_cast<size_t>(r) >= len) return nullptr;
  if (FILE* f = fopen(buf, "w")) {
    fwrite(content, 1, n, f);
    fclose(f);
  }
  return buf;
}

extern "C" const char* andropkg_translate_path(const char* path, char* buf, size_t len) {
  // "/data" too: framework code stats the partition itself (SQLiteGlobal, StorageManager).
  static const char* const kPartitions[] = {"/system", "/apex", "/product", "/system_ext",
                                            "/vendor", "/odm", "/data", "/storage"};
  const char* root = getenv("ANDROPKG_ANDROID_ROOT");
  if (path == nullptr || root == nullptr || *root == '\0' || path[0] != '/') return path;

  // A path into an earlier data container of this iOS app (/var/mobile/Containers/Data/
  // Application/<UUID>/...): iOS moves the container when the app is reinstalled, and apps keep
  // absolute paths they once got (settings locations, caches). Redirect to the current one ($HOME).
  for (const char* c : {"/private/var/mobile/Containers/Data/Application/",
                        "/var/mobile/Containers/Data/Application/"}) {
    size_t cn = strlen(c);
    const char* home = getenv("HOME");
    if (home == nullptr || strncmp(path, c, cn) != 0 || strlen(path) < cn + 36) continue;
    const char* rest = path + cn + 36;  // after the UUID
    if (*rest != '/' && *rest != '\0') continue;
    const char* hc = strstr(home, "/Containers/Data/Application/");
    if (hc == nullptr) continue;
    hc += strlen("/Containers/Data/Application/");
    if (strncmp(hc, path + cn, 36) == 0) return path;  // the current container
    int n = snprintf(buf, len, "%s%s", home, rest);
    return (n > 0 && static_cast<size_t>(n) < len) ? buf : path;
  }
  if (strncmp(path, root, strlen(root)) == 0) return path;  // already a host path
  if (strncmp(path, "/proc/", 6) == 0) {
    if (const char* v = VirtualProcFile(path, buf, len)) return v;
  }

  // The app's private data: /data/user/0/<pkg> (alias /data/data/<pkg>) and the device-protected
  // /data/user_de/0/<pkg> are the host directory ANDROPKG_APP_DATA_HOST (+ "_de"). Apps keep these
  // paths (settings, caches), and the iOS container path changes when the app is reinstalled.
  const char* pkg = getenv("ANDROPKG_APP_DATA_PKG");
  const char* host = getenv("ANDROPKG_APP_DATA_HOST");
  if (pkg != nullptr && host != nullptr && *pkg != '\0') {
    static const char* const kData[] = {"/data/user/0/", "/data/data/", "/data/user_de/0/"};
    size_t pn = strlen(pkg);
    for (const char* d : kData) {
      size_t dn = strlen(d);
      if (strncmp(path, d, dn) == 0 && Under(path + dn, pkg, pn)) {
        const char* de = d[6] == '_' ? "_de" : "";
        int n = snprintf(buf, len, "%s%s%s", host, de, path + dn + pn);
        return (n > 0 && static_cast<size_t>(n) < len) ? buf : path;
      }
    }
  }

  // Shared storage: /storage/emulated/0 (and its /sdcard alias) is a host directory of its
  // own (ANDROPKG_STORAGE_DIR, e.g. the iOS app's Documents), else <root>/storage/emulated/0.
  static const char kPrimary[] = "/storage/emulated/0";
  const char* rest = nullptr;
  if (Under(path, kPrimary, sizeof(kPrimary) - 1)) rest = path + sizeof(kPrimary) - 1;
  else if (Under(path, "/sdcard", 7)) rest = path + 7;
  if (rest != nullptr) {
    const char* dir = getenv("ANDROPKG_STORAGE_DIR");
    int n = (dir != nullptr && *dir != '\0') ? snprintf(buf, len, "%s%s", dir, rest)
                                             : snprintf(buf, len, "%s%s%s", root, kPrimary, rest);
    return (n > 0 && static_cast<size_t>(n) < len) ? buf : path;
  }

  for (const char* p : kPartitions) {
    if (Under(path, p, strlen(p))) {
      int n = snprintf(buf, len, "%s%s", root, path);
      return (n > 0 && static_cast<size_t>(n) < len) ? buf : path;
    }
  }
  return path;
}
