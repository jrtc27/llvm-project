//===- ErrorHandler.cpp ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lld/Common/ErrorHandler.h"

#include "llvm/Support/Parallel.h"

#include "llvm/ADT/Twine.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/raw_ostream.h"
#include <mutex>
#include <regex>

#if !defined(_MSC_VER) && !defined(__MINGW32__)
#include <unistd.h>
#endif

using namespace llvm;
using namespace lld;

// The functions defined in this file can be called from multiple threads,
// but lld::outs() or lld::errs() are not thread-safe. We protect them using a
// mutex.
static std::mutex mu;

// We want to separate multi-line messages with a newline. `sep` is "\n"
// if the last messages was multi-line. Otherwise "".
static StringRef sep;

static StringRef getSeparator(const Twine &msg) {
  if (StringRef(msg.str()).contains('\n'))
    return "\n";
  return "";
}

raw_ostream *lld::stdoutOS;
raw_ostream *lld::stderrOS;

raw_ostream &lld::outs() { return stdoutOS ? *stdoutOS : llvm::outs(); }
raw_ostream &lld::errs() { return stderrOS ? *stderrOS : llvm::errs(); }

ErrorHandler &lld::errorHandler() {
  static ErrorHandler handler;
  return handler;
}

void lld::exitLld(int val) {
  // Delete any temporary file, while keeping the memory mapping open.
  if (errorHandler().outputBuffer)
    errorHandler().outputBuffer->discard();

  // Dealloc/destroy ManagedStatic variables before calling _exit().
  // In an LTO build, allows us to get the output of -time-passes.
  // Ensures that the thread pool for the parallel algorithms is stopped to
  // avoid intermittent crashes on Windows when exiting.
  llvm_shutdown();

  {
    std::lock_guard<std::mutex> lock(mu);
    lld::outs().flush();
    lld::errs().flush();
  }
  _exit(val);
}

void lld::diagnosticHandler(const DiagnosticInfo &di) {
  SmallString<128> s;
  raw_svector_ostream os(s);
  DiagnosticPrinterRawOStream dp(os);
  di.print(dp);
  switch (di.getSeverity()) {
  case DS_Error:
    error(s);
    break;
  case DS_Warning:
    warn(s);
    break;
  case DS_Remark:
  case DS_Note:
    message(s);
    break;
  }
}

void lld::checkError(Error e) {
  handleAllErrors(std::move(e),
                  [&](ErrorInfoBase &eib) { error(eib.message()); });
}

// This is for --vs-diagnostics.
//
// Normally, lld's error message starts with argv[0]. Therefore, it usually
// looks like this:
//
//   ld.lld: error: ...
//
// This error message style is unfortunately unfriendly to Visual Studio
// IDE. VS interprets the first word of the first line as an error location
// and make it clickable, thus "ld.lld" in the above message would become a
// clickable text. When you click it, VS opens "ld.lld" executable file with
// a binary editor.
//
// As a workaround, we print out an error location instead of "ld.lld" if
// lld is running in VS diagnostics mode. As a result, error message will
// look like this:
//
//   src/foo.c(35): error: ...
//
// This function returns an error location string. An error location is
// extracted from an error message using regexps.
std::string ErrorHandler::getLocation(const Twine &msg) {
  if (!vsDiagnostics)
    return std::string(logName);

  static std::regex regexes[] = {
      std::regex(
          R"(^undefined (?:\S+ )?symbol:.*\n)"
          R"(>>> referenced by .+\((\S+):(\d+)\))"),
      std::regex(
          R"(^undefined (?:\S+ )?symbol:.*\n>>> referenced by (\S+):(\d+))"),
      std::regex(R"(^undefined symbol:.*\n>>> referenced by (.*):)"),
      std::regex(
          R"(^duplicate symbol: .*\n>>> defined in (\S+)\n>>> defined in.*)"),
      std::regex(
          R"(^duplicate symbol: .*\n>>> defined at .+\((\S+):(\d+)\))"),
      std::regex(R"(^duplicate symbol: .*\n>>> defined at (\S+):(\d+))"),
      std::regex(
          R"(.*\n>>> defined in .*\n>>> referenced by .+\((\S+):(\d+)\))"),
      std::regex(R"(.*\n>>> defined in .*\n>>> referenced by (\S+):(\d+))"),
      std::regex(R"((\S+):(\d+): unclosed quote)"),
  };

  std::string str = msg.str();
  for (std::regex &re : regexes) {
    std::smatch m;
    if (!std::regex_search(str, m, re))
      continue;

    assert(m.size() == 2 || m.size() == 3);
    if (m.size() == 2)
      return m.str(1);
    return m.str(1) + "(" + m.str(2) + ")";
  }

  return std::string(logName);
}

void ErrorHandler::log(const Twine &msg) {
  if (!verbose)
    return;
  std::lock_guard<std::mutex> lock(mu);
  lld::errs() << logName << ": " << msg << "\n";
}

void ErrorHandler::message(const Twine &msg) {
  std::lock_guard<std::mutex> lock(mu);
  lld::outs() << msg << "\n";
  lld::outs().flush();
}

void ErrorHandler::warn(const Twine &msg) {
  if (fatalWarnings) {
    error(msg);
    return;
  }
  static uint64_t warningCount = 0;
  std::lock_guard<std::mutex> lock(mu);
  if (warningLimit == 0 || warningCount < warningLimit) {
    lld::errs() << sep << getLocation(msg) << ": " << Colors::MAGENTA
                << "warning: " << Colors::RESET << msg << "\n";
    sep = getSeparator(msg);
  } else if (warningCount == warningLimit) {
    lld::errs() << sep << getLocation(msg) << ": " << Colors::MAGENTA
                << "warning: " << Colors::RESET << warningLimitExceededMsg
                << "\n";
    // Set separator based on limit exceeded flag and not the message that
    // wasn't printed:
    sep = getSeparator(warningLimitExceededMsg);
  }
  ++warningCount;
}

void ErrorHandler::error(const Twine &msg) {
  // If Visual Studio-style error message mode is enabled,
  // this particular error is printed out as two errors.
  if (vsDiagnostics) {
    static std::regex re(R"(^(duplicate symbol: .*))"
                         R"((\n>>> defined at \S+:\d+.*\n>>>.*))"
                         R"((\n>>> defined at \S+:\d+.*\n>>>.*))");
    std::string str = msg.str();
    std::smatch m;

    if (std::regex_match(str, m, re)) {
      error(m.str(1) + m.str(2));
      error(m.str(1) + m.str(3));
      return;
    }
  }

  bool exit = false;
  {
    std::lock_guard<std::mutex> lock(mu);

    if (errorLimit == 0 || errorCount < errorLimit) {
      lld::errs() << sep << getLocation(msg) << ": " << Colors::RED
                  << "error: " << Colors::RESET << msg << "\n";
      sep = getSeparator(msg);
    } else if (errorCount == errorLimit) {
      lld::errs() << sep << getLocation(msg) << ": " << Colors::RED
                  << "error: " << Colors::RESET << errorLimitExceededMsg
                  << "\n";
      exit = exitEarly;
      // Set separator based on limit exceeded flag and not the message that
      // wasn't printed:
      sep = getSeparator(errorLimitExceededMsg);
    }

    ++errorCount;
  }

  if (exit)
    exitLld(1);
}

void ErrorHandler::fatal(const Twine &msg) {
  error(msg);
  exitLld(1);
}
