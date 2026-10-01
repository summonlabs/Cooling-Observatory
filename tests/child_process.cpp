// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "child_process.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <chrono>
#include <thread>

#include "dccp/cooling_observatory/clock.hpp"
#include "dccp/cooling_observatory/engine.hpp"
#include "support.hpp"

namespace cotest {

std::string self_executable() {
#if defined(CO_TEST_EXECUTABLE)
  return std::string(CO_TEST_EXECUTABLE);
#else
  return {};
#endif
}

std::string ccoctl_executable() {
#if defined(CO_CCAOCTL_EXECUTABLE)
  return std::string(CO_CCAOCTL_EXECUTABLE);
#else
  return {};
#endif
}

namespace {

std::string quote(const std::string& value) { return value; }

ChildResult spawn(const std::string& executable, const std::vector<std::string>& arguments) {
  ChildResult result;
  if (executable.empty()) {
    result.failure = "no executable path was provided by the build";
    return result;
  }
#if defined(_WIN32)
  std::string command_line = "\"" + executable + "\"";
  for (const std::string& argument : arguments) {
    command_line += " \"" + quote(argument) + "\"";
  }
  std::vector<char> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back('\0');

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE out_read = nullptr;
  HANDLE out_write = nullptr;
  HANDLE err_read = nullptr;
  HANDLE err_write = nullptr;
  if (CreatePipe(&out_read, &out_write, &attributes, 0) == 0) {
    result.failure = "CreatePipe for stdout failed";
    return result;
  }
  SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
  if (CreatePipe(&err_read, &err_write, &attributes, 0) == 0) {
    CloseHandle(out_read);
    CloseHandle(out_write);
    result.failure = "CreatePipe for stderr failed";
    return result;
  }
  SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = out_write;
  startup.hStdError = err_write;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION process{};
  const BOOL created = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  CloseHandle(out_write);
  CloseHandle(err_write);
  if (created == 0) {
    CloseHandle(out_read);
    CloseHandle(err_read);
    result.failure = "CreateProcess failed with error " + std::to_string(GetLastError());
    return result;
  }

  auto drain = [](HANDLE handle) {
    std::string text;
    char buffer[4096];
    DWORD read = 0;
    for (;;) {
      if (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) == 0 || read == 0) {
        break;
      }
      text.append(buffer, read);
    }
    return text;
  };
  result.output = drain(out_read);
  result.error = drain(err_read);
  CloseHandle(out_read);
  CloseHandle(err_read);

  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  result.exit_code = static_cast<int>(exit_code);
  result.started = true;
  return result;
#else
  int out_pipe[2];
  int err_pipe[2];
  if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
    result.failure = "pipe failed";
    return result;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    result.failure = "fork failed";
    return result;
  }
  if (pid == 0) {
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    execv(executable.c_str(), argv.data());
    _exit(127);
  }
  close(out_pipe[1]);
  close(err_pipe[1]);
  auto drain = [](int fd) {
    std::string text;
    char buffer[4096];
    for (;;) {
      const ssize_t read = ::read(fd, buffer, sizeof(buffer));
      if (read <= 0) {
        break;
      }
      text.append(buffer, static_cast<std::size_t>(read));
    }
    return text;
  };
  result.output = drain(out_pipe[0]);
  result.error = drain(err_pipe[0]);
  close(out_pipe[0]);
  close(err_pipe[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  result.started = true;
  return result;
#endif
}

}  // namespace

ChildResult run_self(const std::vector<std::string>& arguments) { return spawn(self_executable(), arguments); }

ChildResult run_program(const std::string& executable, const std::vector<std::string>& arguments) {
  return spawn(executable, arguments);
}

ChildResult run_lock_holder(const std::string& state_directory) {
  std::vector<std::string> arguments{"--child-lock-holder", state_directory};
  return spawn(self_executable(), arguments);
}

std::string argument_value(int argc, char** argv, const char* name) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], name) == 0) {
      return std::string(argv[i + 1]);
    }
  }
  return {};
}

namespace {

/// The child programs. Each is a complete, small program: it opens the engine
/// the way the test asked, does one thing, prints one line, and exits with a
/// code the parent asserts on.
int child_main(int argc, char** argv) {
  using namespace dccp::cooling_observatory;
  const std::string program = argv[1];
  const std::string directory = argc > 2 ? argv[2] : std::string();

  if (program == "--child-lock-holder") {
    FixedClock clock(kStart);
    Engine engine;
    EngineOptions options;
    options.state_directory = directory;
    const Status opened = engine.open(options, clock);
    if (!opened.ok()) {
      std::cout << "child: open failed " << to_token(opened.code()) << '\n';
      return 2;
    }
    std::cout << "child: holding lock\n" << std::flush;
    // Hold the lock for a bounded interval. A child that waited on its parent
    // would turn a parent defect into a hang, and this suite treats a hang as a
    // defect rather than as a timeout, so the child always terminates on its
    // own and reports how long it held.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    std::cout << "child: releasing lock\n" << std::flush;
    return 0;
  }

  if (program == "--child-writer") {
    // The writer opens the store, commits a fixed batch, prints the revision it
    // reached, and exits. The parent starts several of these at once, so
    // exactly one must win the lock and the others must report that they did
    // not.
    FixedClock clock(kStart);
    Engine engine;
    EngineOptions options;
    options.state_directory = directory;
    const Status opened = engine.open(options, clock);
    if (!opened.ok()) {
      std::cout << "writer: refused " << to_token(opened.code()) << ' ' << opened.reason() << '\n';
      return opened.code() == Code::StoreBusy ? 3 : 2;
    }
    const EpochId epoch = engine.image().value().epoch;
    // The sequence continues from the revision this child found, so each run of
    // this program contributes a record nobody has contributed before. A fixed
    // sequence would be recognised as a replay on the second run, which is the
    // idempotency rule working, and would make this probe measure nothing.
    const std::uint64_t sequence = engine.image().value().revision.value() + 1;
    const std::vector<IngestRecord> records{
        zone_flow_measurement(sequence, epoch, "zone.a", "sensor.a.flow",
                              Quantity::flow(30'000'000), clock.now_ms())};
    const auto report = engine.ingest(records);
    if (!report) {
      std::cout << "writer: ingest failed\n";
      return 2;
    }
    std::cout << "writer: revision " << report.value().patch.revision.value() << '\n';
    const Status closed = engine.close();
    return closed.ok() ? 0 : 2;
  }

  if (program == "--child-structure-writer") {
    FixedClock clock(kStart);
    Engine engine;
    EngineOptions options;
    options.state_directory = directory;
    const Status opened = engine.open(options, clock);
    if (!opened.ok()) {
      std::cout << "structure-writer: refused " << to_token(opened.code()) << ' ' << opened.reason()
                << '\n';
      return opened.code() == Code::StoreBusy ? 3 : 2;
    }
    if (engine.image().value().structure.components().empty()) {
      const auto report = engine.ingest({make_structure(GenerationId(1), synthetic_plant(), "child")});
      if (!report) {
        return 2;
      }
    }
    std::cout << "structure-writer: ready\n" << std::flush;
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    return 0;
  }

  if (program == "--child-reopen") {
    // Reopen the state directory a parent committed and print what came back.
    // This is the restart proof from a separate process, which is what makes it
    // a real reopen rather than a second read in the same process.
    FixedClock clock(kStart + 3'600'000);
    Engine engine;
    EngineOptions options;
    options.state_directory = directory;
    const Status opened = engine.open(options, clock);
    if (!opened.ok()) {
      std::cout << "reopen: failed " << to_token(opened.code()) << '\n';
      return 2;
    }
    const auto image = engine.image();
    if (!image) {
      return 2;
    }
    std::cout << "reopen: revision " << image.value().revision.value() << " generation "
              << image.value().generation.value() << " evidence " << image.value().evidence.size()
              << " recovered " << (image.value().recovered ? "true" : "false") << '\n';
    return 0;
  }

  std::cout << "child: unknown program " << program << '\n';
  return 2;
}

}  // namespace

bool dispatch_child(int argc, char** argv, int& exit_code) {
  if (argc < 2) {
    return false;
  }
  const std::string first = argv[1];
  if (first.rfind("--child-", 0) != 0) {
    return false;
  }
  exit_code = child_main(argc, argv);
  return true;
}

}  // namespace cotest