/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Installing content that ships on a title's other discs. See
 * libretro_disc_install.h.
 */

#include "libretro_disc_install.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "libretro_disc.h"
#include "xenia/base/string.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/virtual_file_system.h"

namespace xe {
namespace libretro_disc_install {

namespace {

constexpr char kMountPath[] = "\\Device\\LibretroInstallSource";
// Copies land here first and are renamed into place, so a copy cut short
// never looks installed.
constexpr char kStagingDir[] = ".installing";

struct Job {
  std::filesystem::path disc;
  std::string entry_path;
  std::filesystem::path dest_dir;
  std::string name;
  uint64_t size = 0;
};

std::thread worker;
std::atomic<bool> running{false};
std::atomic<bool> cancel{false};
std::atomic<uint64_t> copied{0};
uint64_t total = 0;

// An ISO, or a GOD package's header file.
std::unique_ptr<vfs::Device> OpenDisc(const std::filesystem::path& path) {
  std::unique_ptr<vfs::Device> device;
  auto header = vfs::XContentContainerDevice::ReadContainerHeader(path);
  if (header && header->content_header.is_magic_valid()) {
    device = vfs::XContentContainerDevice::CreateContentDevice(kMountPath, path);
  } else {
    device = std::make_unique<vfs::DiscImageDevice>(kMountPath, path);
  }
  if (!device || !device->Initialize()) return nullptr;
  return device;
}

// Every package under \content\0000000000000000\<title id>\<type>\ on the
// given discs that content_root does not already hold at the same size.
std::vector<Job> Plan(const std::filesystem::path& content_root,
                      uint32_t title_id,
                      const std::vector<std::filesystem::path>& discs) {
  char id[9];
  std::snprintf(id, sizeof(id), "%08X", title_id);
  const std::string title_path = std::string("\\content\\0000000000000000\\") + id;
  std::vector<Job> jobs;
  for (const auto& disc : discs) {
    auto device = OpenDisc(disc);
    if (!device) continue;
    vfs::Entry* title = device->ResolvePath(title_path);
    if (!title) continue;
    for (const auto& type : title->children()) {
      if (!(type->attributes() & vfs::kFileAttributeDirectory)) continue;
      const std::filesystem::path dest_dir = content_root /
                                             "0000000000000000" / id /
                                             xe::to_path(type->name());
      for (const auto& package : type->children()) {
        if (package->attributes() & vfs::kFileAttributeDirectory) continue;
        std::error_code ec;
        const auto have = std::filesystem::file_size(
            dest_dir / xe::to_path(package->name()), ec);
        if (!ec && have == package->size()) continue;
        jobs.push_back({disc,
                        title_path + "\\" + type->name() + "\\" +
                            package->name(),
                        dest_dir, package->name(), package->size()});
      }
    }
  }
  return jobs;
}

void Run(std::vector<Job> jobs, libretro_disc::LogFn log) {
  std::filesystem::path open_disc;
  std::unique_ptr<vfs::Device> device;
  size_t installed = 0;
  for (const auto& job : jobs) {
    if (cancel) break;
    if (job.disc != open_disc) {
      device = OpenDisc(job.disc);
      open_disc = job.disc;
    }
    vfs::Entry* entry = device ? device->ResolvePath(job.entry_path) : nullptr;
    const std::filesystem::path staging = job.dest_dir / kStagingDir;
    const std::filesystem::path staged = staging / xe::to_path(job.name);
    std::error_code ec;
    std::filesystem::create_directories(staging, ec);
    if (!entry ||
        vfs::VirtualFileSystem::ExtractContentFile(entry, staging, copied,
                                                   true) != 0 ||
        std::filesystem::file_size(staged, ec) != job.size) {
      if (log) {
        log(RETRO_LOG_ERROR, "[Xenia] Could not install %s from %s\n",
            job.name.c_str(), xe::path_to_utf8(job.disc).c_str());
      }
      std::filesystem::remove(staged, ec);
      continue;
    }
    const std::filesystem::path dest = job.dest_dir / xe::to_path(job.name);
    std::filesystem::remove(dest, ec);
    std::filesystem::rename(staged, dest, ec);
    if (ec) {
      if (log) {
        log(RETRO_LOG_ERROR, "[Xenia] Could not move %s into place: %s\n",
            job.name.c_str(), ec.message().c_str());
      }
      continue;
    }
    std::filesystem::remove(staging, ec);
    ++installed;
  }
  if (log) {
    log(installed == jobs.size() ? RETRO_LOG_INFO : RETRO_LOG_WARN,
        "[Xenia] Installed %zu of %zu content package(s) from other discs\n",
        installed, jobs.size());
  }
  running = false;
}

}  // namespace

bool Start(const std::filesystem::path& content_root,
           libretro_disc::LogFn log) {
  Cancel();
  cancel = false;
  copied = 0;
  total = 0;

  uint32_t title_id = 0;
  const auto discs = libretro_disc::OtherDiscs(&title_id);
  if (!title_id || discs.empty()) return false;
  std::vector<Job> jobs = Plan(content_root, title_id, discs);
  if (jobs.empty()) return false;
  for (const auto& job : jobs) total += job.size;

  std::error_code ec;
  const auto space = std::filesystem::space(content_root, ec);
  if (!ec && space.available < total) {
    if (log) {
      log(RETRO_LOG_ERROR,
          "[Xenia] %zu content package(s) on the title's other discs need "
          "%.1f GB in %s, and only %.1f GB is free; not installing\n",
          jobs.size(), total / 1e9, xe::path_to_utf8(content_root).c_str(),
          space.available / 1e9);
    }
    return false;
  }
  if (log) {
    log(RETRO_LOG_INFO,
        "[Xenia] Installing %zu content package(s), %.1f GB, from the "
        "title's other discs into %s\n",
        jobs.size(), total / 1e9, xe::path_to_utf8(content_root).c_str());
  }
  running = true;
  worker = std::thread(Run, std::move(jobs), log);
  return true;
}

bool Running() { return running; }

std::string Progress() {
  const uint64_t done = copied;
  char line[96];
  std::snprintf(line, sizeof(line),
                "Installing content from the other disc: %d%% of %.1f GB",
                total ? int(done * 100 / total) : 100, total / 1e9);
  return line;
}

void Cancel() {
  cancel = true;
  if (worker.joinable()) worker.join();
  running = false;
}

}  // namespace libretro_disc_install
}  // namespace xe
