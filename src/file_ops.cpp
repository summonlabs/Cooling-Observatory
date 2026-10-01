// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "file_ops.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
// The build defines these for every first-party target; guard them so that a
// translation unit compiled with the definitions does not warn on a redefinition.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dccp::cooling_observatory {
namespace {

#if defined(_WIN32)

/// Convert a UTF-8 path into the wide form Win32 expects, keeping the extended
/// length prefix so that a path beyond MAX_PATH works rather than failing with
/// a misleading "file not found".
std::wstring to_wide(const std::string& path) {
  if (path.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), wide.data(), size);
  // A path that is already absolute and long enough benefits from the extended
  // prefix, which is what lifts the MAX_PATH limit.
  //
  // Two details matter. The prefix disables Win32 path normalisation, so every
  // separator must already be native: a forward slash would be read as an
  // ordinary name character and the call would fail with an invalid-name error.
  // And the UNC form must strip the leading double separator before adding its
  // own, because \\?\UNC\ names a share, not a path that happens to start
  // with a separator.
  if (wide.size() < 240 || wide.compare(0, 4, L"\\\\?\\") == 0) {
    return wide;
  }
  const bool unc = wide.size() >= 2 && wide[0] == L'\\' && wide[1] == L'\\';
  const bool drive_absolute = wide.size() >= 3 && wide[1] == L':' &&
                              (wide[2] == L'\\' || wide[2] == L'/');
  if (!unc && !drive_absolute) {
    // A relative path is left alone: prefixing it would create a path Win32 reads
    // as relative to the extended namespace, which is not this process's
    // directory.
    return wide;
  }
  std::wstring body = wide.substr(unc ? 2 : 0);
  std::wstring collapsed;
  collapsed.reserve(body.size());
  for (const wchar_t c : body) {
    const wchar_t native = c == L'/' ? L'\\' : c;
    // A repeated separator produces an empty component, which the extended-length
    // form treats as an invalid name because it performs no normalisation.
    if (native == L'\\' && !collapsed.empty() && collapsed.back() == L'\\') {
      continue;
    }
    collapsed.push_back(native);
  }
  body = std::move(collapsed);
  return (unc ? std::wstring(L"\\\\?\\UNC\\") : std::wstring(L"\\\\?\\")) + body;
}

std::string last_error_text() {
  const DWORD code = GetLastError();
  return "windows error " + std::to_string(code);
}

#if defined(_WIN32)
/// Create one directory and every missing parent, working on the wide form so
/// that an extended-length path is not converted back and forth.
Status ensure_directory_wide(const std::wstring& wide, const std::string& display);
#endif

#else

std::string last_error_text() { return std::strerror(errno); }

#endif

#if defined(_WIN32)
Status ensure_directory_wide(const std::wstring& wide, const std::string& display) {
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      return Status::success();
    }
    return Status::failure(Code::IoFailure, "directory_path_is_file",
                           "a file already exists at the state directory path");
  }
  const std::size_t separator = wide.find_last_of(L"\\/");
  if (separator != std::wstring::npos && separator > 0) {
    const std::wstring parent = wide.substr(0, separator);
    const bool drive_root = parent.size() == 2 && parent[1] == L':';
    if (!drive_root) {
      const Status parent_status = ensure_directory_wide(parent, display);
      if (!parent_status.ok()) {
        return parent_status;
      }
    }
  }
  if (CreateDirectoryW(wide.c_str(), nullptr) == 0) {
    const DWORD error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS) {
      return Status::failure(Code::IoFailure, "directory_create_failed",
                             last_error_text() + " creating a directory component of " + display);
    }
  }
  return Status::success();
}
#endif

}  // namespace

std::string join_path(std::string_view directory, std::string_view name) {
  std::string out(directory);
  if (!out.empty() && out.back() != '/' && out.back() != '\\') {
#if defined(_WIN32)
    out.push_back('\\');
#else
    out.push_back('/');
#endif
  }
  out.append(name);
  return out;
}

bool file_exists(const std::string& path) {
#if defined(_WIN32)
  const std::wstring wide = to_wide(path);
  if (wide.empty()) {
    return false;
  }
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
  struct stat status {};
  return ::stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode);
#endif
}

Status ensure_directory(const std::string& path) {
  if (path.empty()) {
    return Status::failure(Code::InvalidArgument, "empty_directory",
                           "a state directory must be named");
  }
#if defined(_WIN32)
  const std::wstring wide = to_wide(path);
  if (wide.empty()) {
    return Status::failure(Code::IoFailure, "directory_path_invalid",
                           "the directory path is not representable as UTF-16");
  }
  return ensure_directory_wide(wide, path);
#else
  if (::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) {
    return Status::success();
  }
  if (errno != ENOENT) {
    return Status::failure(Code::IoFailure, "directory_create_failed", last_error_text());
  }
  const std::size_t separator = path.find_last_of('/');
  if (separator == std::string::npos || separator == 0) {
    return Status::failure(Code::IoFailure, "directory_create_failed", last_error_text());
  }
  const Status parent_status = ensure_directory(path.substr(0, separator));
  if (!parent_status.ok()) {
    return parent_status;
  }
  if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
    return Status::failure(Code::IoFailure, "directory_create_failed", last_error_text());
  }
  return Status::success();
#endif
}

Result<std::string> read_file(const std::string& path, std::size_t max_bytes) {
#if defined(_WIN32)
  const std::wstring wide = to_wide(path);
  if (wide.empty()) {
    return fail(Code::IoFailure, "path_invalid", "the path is not representable as UTF-16");
  }
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return fail(Code::NotFound, "file_not_found", path);
    }
    return fail(Code::IoFailure, "file_open_failed", last_error_text());
  }
  LARGE_INTEGER size {};
  if (GetFileSizeEx(handle, &size) == 0) {
    const std::string detail = last_error_text();
    CloseHandle(handle);
    return fail(Code::IoFailure, "file_size_failed", detail);
  }
  if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
    CloseHandle(handle);
    return fail(Code::LimitExceeded, "file_too_large",
                "the file is larger than the configured read limit");
  }
  std::string out;
  out.resize(static_cast<std::size_t>(size.QuadPart));
  std::size_t offset = 0;
  while (offset < out.size()) {
    DWORD read = 0;
    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(out.size() - offset, 1u << 20));
    if (ReadFile(handle, out.data() + offset, request, &read, nullptr) == 0) {
      const std::string detail = last_error_text();
      CloseHandle(handle);
      return fail(Code::IoFailure, "file_read_failed", detail);
    }
    if (read == 0) {
      break;
    }
    offset += read;
  }
  CloseHandle(handle);
  out.resize(offset);
  return out;
#else
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    if (errno == ENOENT) {
      return fail(Code::NotFound, "file_not_found", path);
    }
    return fail(Code::IoFailure, "file_open_failed", last_error_text());
  }
  std::string out;
  // Labelled so a large read is reported against the file it came from, and on
  // the heap so the frame stays small however large the file is.
  std::vector<char> buffer(65536);
  for (;;) {
    const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
    if (read > 0) {
      out.append(buffer.data(), read);
      if (out.size() > max_bytes) {
        std::fclose(file);
        return fail(Code::LimitExceeded, "file_too_large",
                    path + " is larger than the configured read limit");
      }
    }
    if (read < buffer.size()) {
      if (std::ferror(file) != 0) {
        std::fclose(file);
        return fail(Code::IoFailure, "file_read_failed", last_error_text());
      }
      break;
    }
  }
  std::fclose(file);
  return out;
#endif
}

Status write_file_atomic(const std::string& temporary_path, const std::string& live_path,
                         std::string_view bytes, bool sync) {
#if defined(_WIN32)
  const std::wstring wide_temporary = to_wide(temporary_path);
  const std::wstring wide_live = to_wide(live_path);
  if (wide_temporary.empty() || wide_live.empty()) {
    return Status::failure(Code::IoFailure, "path_invalid", "a path is not representable as UTF-16");
  }
  HANDLE handle = CreateFileW(wide_temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Status::failure(Code::IoFailure, "temporary_create_failed", last_error_text());
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    DWORD written = 0;
    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
    if (WriteFile(handle, bytes.data() + offset, request, &written, nullptr) == 0) {
      const std::string detail = last_error_text();
      CloseHandle(handle);
      (void)remove_file(temporary_path);
      return Status::failure(Code::IoFailure, "temporary_write_failed", detail);
    }
    offset += written;
  }
  if (sync && FlushFileBuffers(handle) == 0) {
    const std::string detail = last_error_text();
    CloseHandle(handle);
    (void)remove_file(temporary_path);
    return Status::failure(Code::IoFailure, "temporary_flush_failed", detail);
  }
  CloseHandle(handle);

  // MOVEFILE_REPLACE_EXISTING is the atomic replace: the live name moves from
  // the old file to the new one in a single directory operation.
  if (MoveFileExW(wide_temporary.c_str(), wide_live.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    const std::string detail = last_error_text();
    (void)remove_file(temporary_path);
    return Status::failure(Code::IoFailure, "atomic_replace_failed", detail);
  }
  return Status::success();
#else
  const int fd = ::open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return Status::failure(Code::IoFailure, "temporary_create_failed", last_error_text());
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written <= 0) {
      const std::string detail = last_error_text();
      ::close(fd);
      (void)remove_file(temporary_path);
      return Status::failure(Code::IoFailure, "temporary_write_failed", detail);
    }
    offset += static_cast<std::size_t>(written);
  }
  if (sync && ::fsync(fd) != 0) {
    const std::string detail = last_error_text();
    ::close(fd);
    (void)remove_file(temporary_path);
    return Status::failure(Code::IoFailure, "temporary_flush_failed", detail);
  }
  if (::close(fd) != 0) {
    const std::string detail = last_error_text();
    (void)remove_file(temporary_path);
    return Status::failure(Code::IoFailure, "temporary_close_failed", detail);
  }
  if (::rename(temporary_path.c_str(), live_path.c_str()) != 0) {
    const std::string detail = last_error_text();
    (void)remove_file(temporary_path);
    return Status::failure(Code::IoFailure, "atomic_replace_failed", detail);
  }
  if (sync) {
    // Persist the directory entry so that the replace survives a power loss.
    const std::string directory = temporary_path.substr(0, temporary_path.find_last_of('/'));
    const int directory_fd = ::open(directory.empty() ? "." : directory.c_str(), O_RDONLY);
    if (directory_fd >= 0) {
      (void)::fsync(directory_fd);
      ::close(directory_fd);
    }
  }
  return Status::success();
#endif
}

Status remove_file(const std::string& path) {
#if defined(_WIN32)
  const std::wstring wide = to_wide(path);
  if (wide.empty()) {
    return Status::failure(Code::IoFailure, "path_invalid", "the path is not representable as UTF-16");
  }
  if (DeleteFileW(wide.c_str()) != 0) {
    return Status::success();
  }
  const DWORD error = GetLastError();
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
    return Status::success();
  }
  return Status::failure(Code::IoFailure, "file_delete_failed", last_error_text());
#else
  if (::unlink(path.c_str()) == 0) {
    return Status::success();
  }
  if (errno == ENOENT) {
    return Status::success();
  }
  return Status::failure(Code::IoFailure, "file_delete_failed", last_error_text());
#endif
}

Result<LockHandle> acquire_writer_lock(const std::string& path) {
#if defined(_WIN32)
  const std::wstring wide = to_wide(path);
  if (wide.empty()) {
    return fail(Code::IoFailure, "path_invalid", "the lock path is not representable as UTF-16");
  }
  // A share mode of zero is the kernel-enforced exclusive open: a second
  // process attempting the same open fails instead of proceeding.
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
      return fail(Code::StoreBusy, "store_locked",
                  "another process holds the writer lock on this state directory");
    }
    return fail(Code::LockFailure, "lock_create_failed", last_error_text());
  }
  LockHandle out;
  out.handle = handle;
  return out;
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    return fail(Code::LockFailure, "lock_create_failed", last_error_text());
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const bool busy = errno == EWOULDBLOCK || errno == EAGAIN;
    ::close(fd);
    if (busy) {
      return fail(Code::StoreBusy, "store_locked",
                  "another process holds the writer lock on this state directory");
    }
    return fail(Code::LockFailure, "lock_acquire_failed", last_error_text());
  }
  LockHandle out;
  out.handle = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
  return out;
#endif
}

void release_writer_lock(LockHandle& handle) noexcept {
  if (!handle.valid()) {
    return;
  }
#if defined(_WIN32)
  CloseHandle(static_cast<HANDLE>(handle.handle));
#else
  const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle.handle));
  (void)::flock(fd, LOCK_UN);
  ::close(fd);
#endif
  handle.handle = nullptr;
}

}  // namespace dccp::cooling_observatory