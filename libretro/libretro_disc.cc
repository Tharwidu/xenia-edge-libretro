/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Multi-disc support. See libretro_disc.h.
 */

#include "libretro_disc.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "xenia/base/filesystem.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/vfs/devices/xcontent_container_device.h"

namespace xe {
namespace libretro_disc {

namespace {

LogFn log_fn = nullptr;

#define DISC_LOG(level, ...) \
  do {                       \
    if (log_fn) log_fn(level, __VA_ARGS__); \
  } while (0)

std::mutex mutex;
std::vector<std::filesystem::path> images;
unsigned current_index = 0;
bool from_m3u = false;
bool automatic = true;
bool ejected = false;
// A request is parked waiting for the frontend to close the tray.
uint32_t pending_disc = 0;
bool delivered = false;
// The frontend swapped discs while no request was pending.
bool inserted_unconsumed = false;
bool shutting_down = false;
xe::threading::Fence fence;

// Set by the frontend before retro_load_game.
unsigned initial_index = 0;
std::string initial_path;

std::string utf8(const std::filesystem::path& p) { return xe::path_to_utf8(p); }

bool IsExtension(const std::filesystem::path& p, const char* ext) {
  std::string e = utf8(p.extension());
  if (e.size() != std::strlen(ext)) return false;
  for (size_t i = 0; i < e.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(e[i])) != ext[i]) return false;
  }
  return true;
}

// Which disc of which title a candidate is. known is false when it could
// not be read, and the candidate is then judged by name or playlist order.
struct Probe {
  bool known = false;
  uint32_t disc_number = 0;
  uint32_t title_id = 0;
};

uint32_t ReadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
         p[3];
}
uint32_t ReadLE32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}

// A disc image: find default.xex in the XDVDFS game partition and read the
// disc number and title id from its execution info header, which XEX keeps
// unencrypted.
bool ProbeIso(const std::filesystem::path& path, Probe* probe) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  auto read_at = [&](uint64_t offset, void* dst, size_t size) {
    file.seekg(std::streamoff(offset));
    return bool(file.read(static_cast<char*>(dst), std::streamsize(size)));
  };
  constexpr uint64_t kSector = 2048;
  static const char kMagic[] = "MICROSOFT*XBOX*MEDIA";
  // Game partition offsets: plain XDVDFS, XGD2, XGD3, XGD1.
  uint64_t partition = UINT64_MAX;
  uint8_t volume[28];
  for (uint64_t offset : {0x0ull, 0xFD90000ull, 0x2080000ull, 0x18300000ull}) {
    if (read_at(offset + 32 * kSector, volume, sizeof(volume)) &&
        std::memcmp(volume, kMagic, 20) == 0) {
      partition = offset;
      break;
    }
  }
  if (partition == UINT64_MAX) return false;
  const uint32_t root_sector = ReadLE32(volume + 20);
  const uint32_t root_size = ReadLE32(volume + 24);
  if (!root_size || root_size > 16 * 1024 * 1024) return false;
  std::vector<uint8_t> dir(root_size);
  if (!read_at(partition + uint64_t(root_sector) * kSector, dir.data(),
               dir.size())) {
    return false;
  }
  // Directory entries form a binary tree; offsets are in 4-byte units.
  uint32_t xex_sector = 0, xex_size = 0;
  std::vector<uint32_t> stack = {0};
  while (!stack.empty() && !xex_size) {
    size_t at = size_t(stack.back()) * 4;
    stack.pop_back();
    if (at + 14 > dir.size()) continue;
    const uint8_t* e = dir.data() + at;
    uint16_t left = uint16_t(e[0] | e[1] << 8);
    uint16_t right = uint16_t(e[2] | e[3] << 8);
    if (left == 0xFFFF) continue;
    size_t name_len = e[13];
    if (at + 14 + name_len > dir.size()) continue;
    std::string name(reinterpret_cast<const char*>(e + 14), name_len);
    for (char& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (name == "default.xex") {
      xex_sector = ReadLE32(e + 4);
      xex_size = ReadLE32(e + 8);
    }
    if (left) stack.push_back(left);
    if (right) stack.push_back(right);
  }
  if (xex_size < 0x18) return false;
  const uint64_t xex = partition + uint64_t(xex_sector) * kSector;
  uint8_t head[0x18];
  if (!read_at(xex, head, sizeof(head)) || std::memcmp(head, "XEX2", 4) != 0) {
    return false;
  }
  const uint32_t header_size = std::min(ReadBE32(head + 8), xex_size);
  const uint32_t count = ReadBE32(head + 0x14);
  if (header_size > 1024 * 1024 || 0x18 + uint64_t(count) * 8 > header_size) {
    return false;
  }
  std::vector<uint8_t> hdr(header_size);
  if (!read_at(xex, hdr.data(), hdr.size())) return false;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* h = hdr.data() + 0x18 + i * 8;
    if (ReadBE32(h) != 0x00040006) continue;  // XEX_HEADER_EXECUTION_INFO
    const uint32_t offset = ReadBE32(h + 4);
    if (offset + 0x14 > hdr.size()) return false;
    const uint8_t* info = hdr.data() + offset;
    probe->title_id = ReadBE32(info + 0x0C);
    probe->disc_number = info[0x12];
    probe->known = true;
    return true;
  }
  return false;
}

Probe ProbeDisc(const std::filesystem::path& path) {
  Probe probe;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return probe;
  auto header = vfs::XContentContainerDevice::ReadContainerHeader(path);
  if (header && header->content_header.is_magic_valid()) {
    const auto& info = header->content_metadata.execution_info;
    probe.known = true;
    probe.disc_number = info.disc_number;
    probe.title_id = info.title_id;
    return probe;
  }
  ProbeIso(path, &probe);
  return probe;
}

bool Matches(const Probe& probe, uint32_t disc, uint32_t title_id) {
  return probe.known && probe.disc_number == disc &&
         (!title_id || probe.title_id == title_id);
}

// "(Disc 2)", "(Disk 2)", "(Disc 2 of 3)".
const std::regex& DiscPattern() {
  static const std::regex pattern(R"(\((?:Disc|Disk)\s*(\d+)(\s*of\s*\d+)?\))",
                                  std::regex::icase);
  return pattern;
}

// Candidates for disc N next to `base`: swap the number in the nearest
// "(Disc k)" path component, then either take that file or, when the match
// was a folder above base, list the folder standing where base's own folder
// does (GOD packages name their header by hash, which differs per disc).
std::vector<std::filesystem::path> SiblingCandidates(
    const std::filesystem::path& base, uint32_t disc) {
  std::vector<std::filesystem::path> out;
  std::filesystem::path p = base;
  std::filesystem::path rel;
  std::error_code ec;
  for (int depth = 0; depth < 6 && p.has_parent_path() &&
                      p != p.parent_path();
       ++depth) {
    std::string name = utf8(p.filename());
    std::smatch m;
    if (std::regex_search(name, m, DiscPattern())) {
      std::string swapped = m.prefix().str() +
                            name.substr(m.position(0), m.position(1) -
                                                           m.position(0)) +
                            std::to_string(disc) +
                            name.substr(m.position(1) + m.length(1),
                                        m.position(0) + m.length(0) -
                                            m.position(1) - m.length(1)) +
                            m.suffix().str();
      std::filesystem::path sibling = p.parent_path() / xe::to_path(swapped);
      if (rel.empty()) {
        if (std::filesystem::is_regular_file(sibling, ec)) {
          out.push_back(sibling);
        }
      } else {
        std::filesystem::path exact = sibling / rel;
        if (std::filesystem::is_regular_file(exact, ec)) out.push_back(exact);
        std::filesystem::path dir = sibling / rel.parent_path();
        for (const auto& entry :
             std::filesystem::directory_iterator(dir, ec)) {
          if (entry.is_regular_file(ec) && entry.path() != exact) {
            out.push_back(entry.path());
          }
        }
      }
      break;
    }
    rel = rel.empty() ? p.filename() : p.filename() / rel;
    p = p.parent_path();
  }
  return out;
}

// Index of disc N in `images`, adding a discovered sibling when allowed.
// -1 when nothing suitable is known. Caller holds the mutex.
int FindDiscLocked(uint32_t disc, uint32_t title_id) {
  for (size_t i = 0; i < images.size(); ++i) {
    if (Matches(ProbeDisc(images[i]), disc, title_id)) return int(i);
  }
  if (from_m3u) {
    // An entry that could not be identified is trusted by its position -
    // unless it identifies as some other disc.
    size_t i = disc - 1;
    if (disc >= 1 && i < images.size() && !ProbeDisc(images[i]).known) {
      return int(i);
    }
    return -1;
  }
  if (!automatic || images.empty()) return -1;
  for (const auto& candidate : SiblingCandidates(images[0], disc)) {
    Probe probe = ProbeDisc(candidate);
    // A disc that identifies itself must be the right one; one that cannot
    // be read is taken on its name, if it is an ISO.
    if (probe.known ? !Matches(probe, disc, title_id)
                    : !IsExtension(candidate, ".iso")) {
      continue;
    }
    images.push_back(candidate);
    return int(images.size() - 1);
  }
  return -1;
}

std::string LabelFor(const std::filesystem::path& path) {
  // A GOD header is named by hash; the "(Disc N)" folder above it is the
  // readable name.
  std::filesystem::path p = path;
  for (int depth = 0; depth < 6 && p.has_parent_path() &&
                      p != p.parent_path();
       ++depth) {
    std::string name = utf8(p.filename());
    if (std::regex_search(name, DiscPattern())) return name;
    p = p.parent_path();
  }
  return utf8(path.filename());
}

// States the "(Disc N)" siblings of single loaded content, so the set is
// known at load time rather than at the first swap. Nothing is added to
// images: the search runs again, with the title's own id, when a disc is
// requested. Caller holds the mutex.
void LogDiscSetLocked() {
  const Probe base = ProbeDisc(images[0]);
  std::string found;
  for (uint32_t disc = 1; disc <= 9; ++disc) {
    if (base.known && base.disc_number == disc) continue;
    for (const auto& candidate : SiblingCandidates(images[0], disc)) {
      if (candidate == images[0]) continue;
      Probe probe = ProbeDisc(candidate);
      if (probe.known ? !Matches(probe, disc, base.known ? base.title_id : 0)
                      : !IsExtension(candidate, ".iso")) {
        continue;
      }
      found += (found.empty() ? " disc " : "; disc ") +
               std::to_string(disc) + " = " + utf8(candidate);
      break;
    }
  }
  if (found.empty()) return;
  DISC_LOG(RETRO_LOG_INFO, "Sibling discs of %s:%s\n",
           LabelFor(images[0]).c_str(), found.c_str());
}

bool CopyOut(const std::string& s, char* dst, size_t len) {
  if (!dst || !len) return false;
  std::snprintf(dst, len, "%s", s.c_str());
  return true;
}

// ---- libretro disk control -------------------------------------------------

bool RETRO_CALLCONV SetEjectState(bool eject) {
  std::lock_guard<std::mutex> lock(mutex);
  if (ejected && !eject) {
    if (pending_disc && current_index < images.size()) {
      delivered = true;
      fence.Signal();
    } else {
      inserted_unconsumed = true;
    }
  }
  ejected = eject;
  return true;
}

bool RETRO_CALLCONV GetEjectState() {
  std::lock_guard<std::mutex> lock(mutex);
  return ejected;
}

unsigned RETRO_CALLCONV GetImageIndex() {
  std::lock_guard<std::mutex> lock(mutex);
  return current_index;
}

bool RETRO_CALLCONV SetImageIndex(unsigned index) {
  std::lock_guard<std::mutex> lock(mutex);
  // index == size means an empty tray, which libretro allows.
  if (!ejected || index > images.size()) return false;
  current_index = index;
  return true;
}

unsigned RETRO_CALLCONV GetNumImages() {
  std::lock_guard<std::mutex> lock(mutex);
  return unsigned(images.size());
}

bool RETRO_CALLCONV ReplaceImageIndex(unsigned index,
                                      const struct retro_game_info* info) {
  std::lock_guard<std::mutex> lock(mutex);
  if (index >= images.size()) return false;
  if (!info || !info->path) {
    images.erase(images.begin() + index);
    if (current_index > index) --current_index;
    return true;
  }
  std::filesystem::path resolved;
  if (!ResolveContentPath(xe::to_path(info->path), &resolved)) return false;
  images[index] = resolved;
  return true;
}

bool RETRO_CALLCONV AddImageIndex() {
  std::lock_guard<std::mutex> lock(mutex);
  images.emplace_back();
  return true;
}

bool RETRO_CALLCONV SetInitialImage(unsigned index, const char* path) {
  std::lock_guard<std::mutex> lock(mutex);
  initial_index = index;
  initial_path = path ? path : "";
  return true;
}

bool RETRO_CALLCONV GetImagePath(unsigned index, char* s, size_t len) {
  std::lock_guard<std::mutex> lock(mutex);
  if (index >= images.size() || images[index].empty()) return false;
  return CopyOut(utf8(images[index]), s, len);
}

bool RETRO_CALLCONV GetImageLabel(unsigned index, char* s, size_t len) {
  std::lock_guard<std::mutex> lock(mutex);
  if (index >= images.size() || images[index].empty()) return false;
  return CopyOut(LabelFor(images[index]), s, len);
}

}  // namespace

void SetLogger(LogFn log) { log_fn = log; }

void RegisterDiskControl(retro_environment_t environ_cb) {
  unsigned version = 0;
  if (environ_cb(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION,
                 &version) &&
      version >= 1) {
    static struct retro_disk_control_ext_callback ext = {
        SetEjectState,    GetEjectState,     GetImageIndex, SetImageIndex,
        GetNumImages,     ReplaceImageIndex, AddImageIndex, SetInitialImage,
        GetImagePath,     GetImageLabel};
    environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &ext);
  } else {
    static struct retro_disk_control_callback legacy = {
        SetEjectState, GetEjectState,     GetImageIndex, SetImageIndex,
        GetNumImages,  ReplaceImageIndex, AddImageIndex};
    environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &legacy);
  }
}

bool ResolveContentPath(const std::filesystem::path& content,
                        std::filesystem::path* out) {
  // Frontends pass forward slashes on Windows too; sibling discs are built
  // with operator/, so without this the paths (and the log) mix separators.
  std::filesystem::path in = content;
  in.make_preferred();
  if (!IsExtension(in, ".x360")) {
    *out = in;
    return true;
  }
  std::ifstream file(in, std::ios::binary);
  std::string line;
  if (!file || !std::getline(file, line)) {
    DISC_LOG(RETRO_LOG_ERROR, "Cannot read pointer file %s\n",
             utf8(in).c_str());
    return false;
  }
  while (!line.empty() && std::strchr("\r\n \t", line.back())) line.pop_back();
  if (line.size() >= 2 && line.front() == '"' && line.back() == '"') {
    line = line.substr(1, line.size() - 2);
  }
  if (line.empty()) {
    DISC_LOG(RETRO_LOG_ERROR, "Pointer file %s is empty\n", utf8(in).c_str());
    return false;
  }
  std::filesystem::path target = xe::to_path(line);
  if (target.is_relative()) target = in.parent_path() / target;
  target.make_preferred();
  // Pointer files routinely hold absolute paths, so a moved library breaks
  // them; without this check the launch fails deep inside LaunchPath as a bare
  // 0xC00000BB that says nothing about the cause.
  std::error_code ec;
  if (!std::filesystem::exists(target, ec)) {
    DISC_LOG(RETRO_LOG_ERROR,
             "Pointer file %s targets a path that does not exist: %s\n",
             utf8(in).c_str(), utf8(target).c_str());
    DISC_LOG(RETRO_LOG_ERROR,
             "Edit the pointer file so it contains the full path to the "
             "content's current location.\n");
    return false;
  }
  DISC_LOG(RETRO_LOG_INFO, "Pointer file resolved to: %s\n",
           utf8(target).c_str());
  *out = target;
  return true;
}

bool LoadContent(const std::filesystem::path& content,
                 std::filesystem::path* boot_path) {
  std::lock_guard<std::mutex> lock(mutex);
  images.clear();
  current_index = 0;
  ejected = false;
  pending_disc = 0;
  delivered = false;
  inserted_unconsumed = false;
  shutting_down = false;
  fence.TryWait();  // drop a signal left by the previous Shutdown
  from_m3u = IsExtension(content, ".m3u");

  if (!from_m3u) {
    std::filesystem::path resolved;
    if (!ResolveContentPath(content, &resolved)) return false;
    images.push_back(resolved);
    *boot_path = resolved;
    LogDiscSetLocked();
    return true;
  }

  std::ifstream file(content, std::ios::binary);
  std::string line;
  while (file && std::getline(file, line)) {
    while (!line.empty() && std::strchr("\r\n \t", line.back())) {
      line.pop_back();
    }
    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos || line[start] == '#') continue;
    std::filesystem::path entry = xe::to_path(line.substr(start));
    if (entry.is_relative()) entry = content.parent_path() / entry;
    std::filesystem::path resolved;
    if (!ResolveContentPath(entry, &resolved)) return false;
    images.push_back(resolved);
  }
  if (images.empty()) {
    DISC_LOG(RETRO_LOG_ERROR, "Playlist lists no discs: %s\n",
             utf8(content).c_str());
    return false;
  }
  if (initial_index < images.size() &&
      xe::to_path(initial_path) == content) {
    current_index = initial_index;
  }
  DISC_LOG(RETRO_LOG_INFO, "Playlist: %u disc(s), booting entry %u: %s\n",
           unsigned(images.size()), current_index + 1,
           LabelFor(images[current_index]).c_str());
  *boot_path = images[current_index];
  return true;
}

void SetAutomatic(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex);
  automatic = enabled;
}

std::filesystem::path Resolve(uint32_t disc_number, bool retry,
                              uint32_t title_id) {
  std::unique_lock<std::mutex> lock(mutex);
  if (shutting_down) {
    // XamSwapDisc asks again at once; do not let that spin during teardown.
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return {};
  }

  if (!retry && (from_m3u || automatic)) {
    int index = FindDiscLocked(disc_number, title_id);
    if (index >= 0) {
      current_index = unsigned(index);
      inserted_unconsumed = false;
      DISC_LOG(RETRO_LOG_INFO, "Disc %u requested: inserted %s\n",
               disc_number, utf8(images[index]).c_str());
      return images[index];
    }
  }
  if (!retry && inserted_unconsumed && current_index < images.size()) {
    // The player swapped discs before the title asked; use that one.
    inserted_unconsumed = false;
    return images[current_index];
  }

  DISC_LOG(retry ? RETRO_LOG_WARN : RETRO_LOG_INFO,
           "Disc %u requested%s: insert it with the frontend's disc control "
           "(eject, choose the disc, close the tray)\n",
           disc_number, retry ? " and the last disc was rejected" : "");
  pending_disc = disc_number;
  delivered = false;
  while (true) {
    lock.unlock();
    kernel::GuestScheduler::WaitOnFence(fence);
    lock.lock();
    if (shutting_down) {
      pending_disc = 0;
      return {};
    }
    if (delivered && current_index < images.size()) {
      delivered = false;
      pending_disc = 0;
      return images[current_index];
    }
  }
}

void Shutdown() {
  std::lock_guard<std::mutex> lock(mutex);
  shutting_down = true;
  if (pending_disc) fence.Signal();
}

}  // namespace libretro_disc
}  // namespace xe
