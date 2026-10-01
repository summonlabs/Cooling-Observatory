// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// ccoctl - the Cooling Observatory inspection tool.
//
// The tool is a thin, deterministic shell over the library: it parses
// arguments, feeds records in, asks one query, and prints the answer. Every
// decision about what an answer means belongs to the library, so the tool can
// never disagree with an embedded consumer about semantics.
//
// Exit codes:
//   0  the requested operation succeeded and the answer is determined
//   1  the operation failed (I/O, persistence, refused lifecycle)
//   2  the command line was not understood
//   3  the operation succeeded but part of the answer is not determined

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "dccp/cooling_observatory/clock.hpp"
#include "dccp/cooling_observatory/engine.hpp"
#include "dccp/cooling_observatory/textproto.hpp"
#include "dccp/cooling_observatory/version.hpp"

namespace {

using dccp::cooling_observatory::Engine;
using dccp::cooling_observatory::EngineOptions;
using dccp::cooling_observatory::ObserveRequest;
using dccp::cooling_observatory::QueryKind;
using dccp::cooling_observatory::Status;
using dccp::cooling_observatory::SystemClock;

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;
constexpr int kExitIndeterminate = 3;

void usage(std::ostream& out) {
  out << "ccoctl - Cooling Observatory inspection tool\n"
      << "\n"
      << "usage: ccoctl <command> [options]\n"
      << "\n"
      << "commands:\n"
      << "  image         the whole observable state\n"
      << "  delivery      what cooling is evidenced as arriving\n"
      << "  constraints   where delivery is constrained and what is responsible\n"
      << "  failures      declared failures and whether delivery confirms them\n"
      << "  reserve       claimed and evidenced reserve\n"
      << "  divergence    declared expectation against measurement\n"
      << "  coverage      what the observatory cannot speak about\n"
      << "  dependencies  the delivery path from a root\n"
      << "  history       what the engine committed, in order\n"
      << "  ingest        read records from a file or stdin and commit them\n"
      << "  observe       answer a request written in the interchange format\n"
      << "  stats         counters for this engine instance\n"
      << "  version       the library version\n"
      << "\n"
      << "options:\n"
      << "  --state <dir>       state directory (durable state; omit for in-memory)\n"
      << "  --input <file>      read records or a request from a file instead of stdin\n"
      << "  --json              print the answer as JSON instead of text\n"
      << "  --point <id>        restrict to one delivery point\n"
      << "  --loop <id>         restrict to one loop\n"
      << "  --zone <id>         restrict to one zone\n"
      << "  --plant <id>        restrict to one plant\n"
      << "  --root <kind:id>    traversal root for dependencies\n"
      << "  --direction <d>     upstream or downstream (dependencies)\n"
      << "  --include-stale     include stale evidence in the answer\n"
      << "  --require-evidenced reserve only from measurement\n"
      << "  --tolerance <ppm>   relative divergence tolerance in parts per million\n"
      << "  --help              this text\n";
}

struct Options {
  std::string command{};
  std::string state_directory{};
  std::string input_path{};
  bool json = false;
  bool include_stale = false;
  bool require_evidenced = false;
  bool have_tolerance = false;
  std::int64_t tolerance_ppm = 50'000;
  std::string point{};
  std::string loop{};
  std::string zone{};
  std::string plant{};
  std::string root{};
  std::string direction{};
};

bool take_value(const std::vector<std::string>& arguments, std::size_t& index, std::string& out,
                std::string& error) {
  if (index + 1 >= arguments.size()) {
    error = "option '" + arguments[index] + "' requires a value";
    return false;
  }
  out = arguments[++index];
  return true;
}

bool parse_options(const std::vector<std::string>& arguments, Options& options, std::string& error) {
  for (std::size_t i = 1; i < arguments.size(); ++i) {
    const std::string& argument = arguments[i];
    if (argument == "--help" || argument == "-h") {
      options.command = "help";
      return true;
    }
    if (argument == "--json") {
      options.json = true;
      continue;
    }
    if (argument == "--include-stale") {
      options.include_stale = true;
      continue;
    }
    if (argument == "--require-evidenced") {
      options.require_evidenced = true;
      continue;
    }
    if (argument == "--state") {
      if (!take_value(arguments, i, options.state_directory, error)) {
        return false;
      }
      continue;
    }
    if (argument == "--input") {
      if (!take_value(arguments, i, options.input_path, error)) {
        return false;
      }
      continue;
    }
    if (argument == "--point") {
      if (!take_value(arguments, i, options.point, error)) {
        return false;
      }
      continue;
    }
    if (argument == "--loop") {
      if (!take_value(arguments, i, options.loop, error)) {
        return false;
      }
      continue;
    }
    if (argument == "--zone") {
      if (!take_value(arguments, i, options.zone, error)) {
        return false;
      }
      continue;
    }
    if (argument == "--plant") {
      if (!take_value(arguments, i, options.plant, error)) {
        return false;
      }
      continue;
    }
    if (argument == "--root") {
      if (!take_value(arguments, i, options.root, error)) {
        return false;
      }
      continue;
    }
    if (argument == "--direction") {
      if (!take_value(arguments, i, options.direction, error)) {
        return false;
      }
      continue;
    }
    if (argument == "--tolerance") {
      std::string value;
      if (!take_value(arguments, i, value, error)) {
        return false;
      }
      try {
        options.tolerance_ppm = std::stoll(value);
      } catch (const std::exception&) {
        error = "--tolerance needs an integer number of parts per million";
        return false;
      }
      options.have_tolerance = true;
      continue;
    }
    if (!argument.empty() && argument[0] == '-') {
      error = "unknown option '" + argument + "'";
      return false;
    }
    if (!options.command.empty()) {
      error = "more than one command was given: '" + options.command + "' and '" + argument + "'";
      return false;
    }
    options.command = argument;
  }
  if (options.command.empty()) {
    error = "no command was given";
    return false;
  }
  return true;
}

std::string read_stream(std::istream& in) {
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

dccp::cooling_observatory::Result<std::string> read_input(const Options& options) {
  using dccp::cooling_observatory::Code;
  using dccp::cooling_observatory::fail;
  using dccp::cooling_observatory::Result;
  if (options.input_path.empty()) {
    return read_stream(std::cin);
  }
  std::FILE* file = std::fopen(options.input_path.c_str(), "rb");
  if (file == nullptr) {
    return fail(Code::NotFound, "input_not_found", "cannot open " + options.input_path);
  }
  std::string out;
  // The buffer lives on the heap rather than the stack: a 64 KiB frame in a
  // command-line tool is a needless risk, and the static analyser is right to
  // report it.
  std::vector<char> buffer(65536);
  for (;;) {
    const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
    if (read > 0) {
      out.append(buffer.data(), read);
    }
    if (read < buffer.size()) {
      if (std::ferror(file) != 0) {
        std::fclose(file);
        return fail(Code::IoFailure, "input_read_failed", "reading " + options.input_path);
      }
      break;
    }
  }
  std::fclose(file);
  return out;
}

EngineOptions engine_options(const Options& options) {
  EngineOptions engine;
  engine.state_directory = options.state_directory;
  return engine;
}

void apply_filters(const Options& options, ObserveRequest& request) {
  using dccp::cooling_observatory::LoopId;
  using dccp::cooling_observatory::Maybe;
  using dccp::cooling_observatory::PlantId;
  using dccp::cooling_observatory::StrongId;
  using dccp::cooling_observatory::ZoneId;
  if (!options.point.empty()) {
    request.filter.point = Maybe<StrongId>::of(StrongId::from_validated(options.point));
  }
  if (!options.loop.empty()) {
    request.filter.loop = Maybe<LoopId>::of(LoopId(StrongId::from_validated(options.loop)));
  }
  if (!options.zone.empty()) {
    request.filter.zone = Maybe<ZoneId>::of(ZoneId(StrongId::from_validated(options.zone)));
  }
  if (!options.plant.empty()) {
    request.filter.plant = Maybe<PlantId>::of(PlantId(StrongId::from_validated(options.plant)));
  }
  request.filter.include_stale = options.include_stale;
  request.filter.require_evidenced = options.require_evidenced;
  if (options.have_tolerance) {
    request.filter.relative_tolerance_ppm = options.tolerance_ppm;
  }
  if (options.direction == "upstream") {
    request.filter.traversal.direction = dccp::cooling_observatory::TraversalOptions::Direction::Upstream;
  }
  if (!options.root.empty()) {
    const std::size_t colon = options.root.find(':');
    if (colon != std::string::npos) {
      const auto kind = dccp::cooling_observatory::parse_subject_kind(
          std::string_view(options.root).substr(0, colon));
      if (kind.has_value()) {
        request.filter.root = Maybe<dccp::cooling_observatory::SubjectRef>::of(
            dccp::cooling_observatory::SubjectRef(
                kind.value(), dccp::cooling_observatory::StrongId::from_validated(
                                  options.root.substr(colon + 1))));
      }
    }
  }
}

bool command_is_query(const std::string& command, QueryKind& kind) {
  using dccp::cooling_observatory::parse_query_kind;
  const auto parsed = parse_query_kind(command);
  if (!parsed.has_value()) {
    return false;
  }
  kind = parsed.value();
  return true;
}

int report_exit(const dccp::cooling_observatory::ObservationReport& report) {
  using dccp::cooling_observatory::Freshness;
  if (report.indeterminacies.empty() && report.freshness != Freshness::Unknown) {
    return kExitOk;
  }
  return kExitIndeterminate;
}

int print_error(const dccp::cooling_observatory::Error& error) {
  std::cerr << "ccoctl: " << dccp::cooling_observatory::to_token(error.code) << ": " << error.reason;
  if (!error.detail.empty()) {
    std::cerr << ": " << error.detail;
  }
  std::cerr << "\n";
  return kExitFailure;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace dccp::cooling_observatory;

  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    arguments.emplace_back(argv[i]);
  }

  Options options;
  std::string error;
  if (!parse_options(arguments, options, error)) {
    std::cerr << "ccoctl: " << error << "\n\n";
    usage(std::cerr);
    return kExitUsage;
  }
  if (options.command == "help") {
    usage(std::cout);
    return kExitOk;
  }
  if (options.command == "version") {
    std::cout << producer_identity() << " schema " << kSnapshotSchemaVersion << '/'
              << kInterchangeSchemaVersion << "\n";
    return kExitOk;
  }

  SystemClock clock;
  Engine engine;
  const Status opened = engine.open(engine_options(options), clock);
  if (!opened.ok()) {
    return print_error(opened.error());
  }

  int exit_code = kExitOk;
  QueryKind kind = QueryKind::Image;
  if (command_is_query(options.command, kind)) {
    auto input = read_input(options);
    if (!input) {
      return print_error(input.error());
    }
    ObserveRequest request;
    request.kind = kind;
    if (!input.value().empty()) {
      auto parsed = parse_request(input.value());
      if (!parsed) {
        return print_error(parsed.error());
      }
      request = parsed.value();
      request.kind = kind;
    }
    apply_filters(options, request);
    auto report = engine.observe(request);
    if (!report) {
      return print_error(report.error());
    }
    std::cout << (options.json ? render_json(report.value()) : render_text(report.value()));
    exit_code = report_exit(report.value());
  } else if (options.command == "ingest") {
    auto input = read_input(options);
    if (!input) {
      return print_error(input.error());
    }
    auto records = parse_records(input.value());
    if (!records) {
      return print_error(records.error());
    }
    auto report = engine.ingest(records.value());
    if (!report) {
      return print_error(report.error());
    }
    std::cout << "records_applied " << report.value().patch.records_applied << "\n";
    std::cout << "records_rejected " << report.value().rejections.size() << "\n";
    std::cout << "duplicates " << report.value().duplicates.size() << "\n";
    std::cout << "revision " << report.value().patch.revision.value() << "\n";
    std::cout << "generation " << report.value().patch.generation.value() << "\n";
    std::cout << "digest " << report.value().patch.digest << "\n";
    for (const auto& rejection : report.value().rejections) {
      std::cout << "rejected seq" << rejection.record_seq.value() << " " << rejection.reason << " "
                << rejection.detail << "\n";
      exit_code = kExitIndeterminate;
    }
  } else if (options.command == "observe") {
    auto input = read_input(options);
    if (!input) {
      return print_error(input.error());
    }
    auto request = parse_request(input.value());
    if (!request) {
      return print_error(request.error());
    }
    auto report = engine.observe(request.value());
    if (!report) {
      return print_error(report.error());
    }
    std::cout << (options.json ? render_json(report.value()) : render_text(report.value()));
    exit_code = report_exit(report.value());
  } else if (options.command == "stats") {
    std::cout << engine.stats().render();
  } else {
    std::cerr << "ccoctl: unknown command '" << options.command << "'\n\n";
    usage(std::cerr);
    exit_code = kExitUsage;
  }

  const Status closed = engine.close();
  if (!closed.ok() && exit_code == kExitOk) {
    return print_error(closed.error());
  }
  return exit_code;
}
