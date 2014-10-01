/*
 A mathematical optimization solver.

 Copyright (C) 2012 AMPL Optimization Inc

 Permission to use, copy, modify, and distribute this software and its
 documentation for any purpose and without fee is hereby granted,
 provided that the above copyright notice appear in all copies and that
 both that the copyright notice and this permission notice and warranty
 disclaimer appear in supporting documentation.

 The author and AMPL Optimization Inc disclaim all warranties with
 regard to this software, including all implied warranties of
 merchantability and fitness.  In no event shall the author be liable
 for any special, indirect or consequential damages or any damages
 whatsoever resulting from loss of use, data or profits, whether in an
 action of contract, negligence or other tortious action, arising out
 of or in connection with the use or performance of this software.

 Author: Victor Zverovich
 */

#include "mp/solver.h"
#include "mp/problem-base.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <algorithm>
#include <stack>

#ifndef _WIN32
# include <strings.h>
# include <unistd.h>
# define AMPL_WRITE write
#else
# include <io.h>
# define AMPL_WRITE _write
# define strcasecmp _stricmp
# undef max
#endif

#include "mp/clock.h"
#include "mp/rstparser.h"

namespace {

const char *SkipSpaces(const char *s) {
  while (*s && isspace(*s))
    ++s;
  return s;
}

const char *SkipNonSpaces(const char *s) {
  while (*s && !isspace(*s))
    ++s;
  return s;
}

struct Deleter {
  void operator()(mp::SolverOption* p) const { delete p; }
};

// A reStructuredText (RST) formatter.
class RSTFormatter : public rst::ContentHandler {
 private:
  fmt::Writer &writer_;
  mp::ValueArrayRef values_;
  std::stack<int> indents_;
  int indent_;
  int pos_in_line_;
  bool end_block_;  // true if there was no text after the last end of block

  enum {
    LITERAL_BLOCK_INDENT = 3,
    LIST_ITEM_INDENT     = 2
  };

  void Indent() {
    if (end_block_) {
      end_block_ = false;
      EndLine();
    }
    for (; pos_in_line_ < indent_; ++pos_in_line_)
      writer_ << ' ';
  }

  // Ends the current line.
  void EndLine() {
    writer_ << '\n';
    pos_in_line_ = 0;
  }

  // Writes a string doing a text reflow.
  void Write(fmt::StringRef s);

 public:
  RSTFormatter(fmt::Writer &w, mp::ValueArrayRef values, int indent)
  : writer_(w), values_(values), indent_(indent),
    pos_in_line_(0), end_block_(false) {}

  void StartBlock(rst::BlockType type);
  void EndBlock();

  void HandleText(const char *text, std::size_t size);
  void HandleDirective(const char *type);
};

void RSTFormatter::Write(fmt::StringRef s) {
  enum { MAX_LINE_LENGTH = 78 };
  const char *p = s.c_str();
  for (;;) {
    // Skip leading spaces.
    while (*p == ' ')
      ++p;
    const char *word_start = p;
    while (*p != ' ' && *p != '\n' && *p)
      ++p;
    const char *word_end = p;
    if (pos_in_line_ + (word_end - word_start) +
        (pos_in_line_ == 0 ? 0 : 1) > MAX_LINE_LENGTH) {
      EndLine();  // The word doesn't fit in the current line, start a new one.
    }
    if (pos_in_line_ == 0) {
      Indent();
    } else {
      // Separate words.
      writer_ << ' ';
      ++pos_in_line_;
    }
    std::size_t length = word_end - word_start;
    writer_ << fmt::StringRef(word_start, length);
    pos_in_line_ += static_cast<int>(length);
    if (*p == '\n') {
      EndLine();
      ++p;
    }
    if (!*p) break;
  }
}

void RSTFormatter::StartBlock(rst::BlockType type) {
  indents_.push(indent_);
  if (type == rst::LITERAL_BLOCK) {
    indent_ += LITERAL_BLOCK_INDENT;
  } else if (type == rst::LIST_ITEM) {
    Write("*");
    indent_ += LIST_ITEM_INDENT;
  }
}

void RSTFormatter::EndBlock() {
  indent_ = indents_.top();
  indents_.pop();
  end_block_ = true;
}

void RSTFormatter::HandleText(const char *text, std::size_t size) {
  Write(std::string(text, size));
  size = writer_.size();
  if (size != 0 && writer_.data()[size - 1] != '\n')
    EndLine();
}

void RSTFormatter::HandleDirective(const char *type) {
  if (std::strcmp(type, "value-table") != 0)
    throw mp::Error("unknown directive {}", type);
  if (values_.size() == 0)
    throw mp::Error("no values to format");
  std::size_t max_len = 0;
  for (mp::ValueArrayRef::iterator
      i = values_.begin(), e = values_.end(); i != e; ++i) {
    max_len = std::max(max_len, std::strlen(i->value));
  }

  // If the values don't have descriptions indent them as list items.
  if (!values_.begin()->description)
    indent_ += LIST_ITEM_INDENT;
  for (mp::ValueArrayRef::iterator
      i = values_.begin(), e = values_.end(); i != e; ++i) {
    Indent();
    writer_ << fmt::pad(i->value, static_cast<unsigned>(max_len));
    if (i->description) {
      static const char SEP[] = " -";
      writer_ << SEP;
      int saved_indent = indent_;
      indent_ += static_cast<int>(max_len + sizeof(SEP));
      pos_in_line_ = indent_;
      Write(i->description);
      indent_ = saved_indent;
    }
    EndLine();
  }

  // Unindent values if necessary and end the block.
  if (!values_.begin()->description)
    indent_ -= LIST_ITEM_INDENT;
  end_block_ = true;
}
}

namespace mp {

namespace internal {

void FormatRST(fmt::Writer &w,
    fmt::StringRef s, int indent, ValueArrayRef values) {
  RSTFormatter formatter(w, values, indent);
  rst::Parser parser(&formatter);
  parser.Parse(s.c_str());
}

const char OptionHelper<int>::TYPE_NAME[] = "int";
const char OptionHelper<double>::TYPE_NAME[] = "double";
const char OptionHelper<std::string>::TYPE_NAME[] = "string";
int OptionHelper<int>::Parse(const char *&s) {
  char *end = 0;
  long value = std::strtol(s, &end, 10);
  s = end;
  return value;
}

double OptionHelper<double>::Parse(const char *&s) {
  char *end = 0;
  double value = std::strtod(s, &end);
  s = end;
  return value;
}

std::string OptionHelper<std::string>::Parse(const char *&s) {
  const char *start = s;
  s = SkipNonSpaces(s);
  return std::string(start, s - start);
}

SolverAppOptionParser::SolverAppOptionParser(Solver &s) : solver_(s) {
  // Add command-line options.
  OptionList::Builder<SolverAppOptionParser> app_options(options_, *this);
  app_options.Add<&SolverAppOptionParser::ShowUsage>(
        '?', "show usage and exit");
  app_options.Add<&SolverAppOptionParser::EndOptions>('-', "end of options");
  app_options.Add<&SolverAppOptionParser::ShowSolverOptions>(
        '=', "show solver options and exit");
  OptionList::Builder<mp::Solver> options(options_, s);
  options.Add<&mp::Solver::ShowVersion>('v', "show version and exit");
  // TODO: add options -e, -s and, if solver supports functions, -ix and -u
  /*
  static const char *options[] = {
    "e",  "suppress echoing of assignments",
    "ix", "import user-defined functions from x; -i? gives details",
    "s",  "write .sol file (without -AMPL)",
    "u",  "just show available user-defined functions",
  for (const char **s = options; *s; s += 2) {
    // Filter out -ix & -u options if FUNC_OPTIONS flags is not set.
    char c = **s;
    if ((flags & FUNC_OPTIONS) == 0 && (c == 'i' || c == 'u'))
      continue;
    fmt::print(f, "\t-{:3}{{{}}}\n", s[0], s[1]);
  }
  */
}

bool SolverAppOptionParser::ShowUsage() {
  solver_.Print("usage: {} [options] stub [-AMPL] [<assignment> ...]\n",
                solver_.name());
  solver_.Print("\nOptions:\n");
  for (OptionList::iterator
       i = options_.begin(), end = options_.end(); i != end; ++i) {
    solver_.Print("\t-{}  {}\n", i->name, i->description);
  }
  return false;
}

bool SolverAppOptionParser::ShowSolverOptions() {
  fmt::Writer writer;
  const char *option_header = solver_.option_header();
  internal::FormatRST(writer, option_header);
  if (!*option_header)
    writer << '\n';
  solver_.Print("{}", writer.c_str());
  solver_.Print("Options:\n");
  const int DESC_INDENT = 6;
  for (Solver::option_iterator
       i = solver_.option_begin(), end = solver_.option_end(); i != end; ++i) {
    writer.clear();
    writer << '\n' << i->name() << '\n';
    internal::FormatRST(writer, i->description(), DESC_INDENT, i->values());
    solver_.Print("{}", fmt::StringRef(writer.data(), writer.size()));
  }
  return false;
}

const char *SolverAppOptionParser::Parse(char **&argv) {
  argv = ParseOptions(argv, options_);
  if (!argv) return 0;
  const char *stub = *argv;
  if (!stub) {
    ShowUsage();
    return 0;
  }
  ++argv;
  if (*argv && std::strcmp(*argv, "-AMPL") == 0) {
    solver_.set_wantsol(1);
    ++argv;
  }
  return stub;
}
}  // namespace internal

std::string SignalHandler::signal_message_;
const char *SignalHandler::signal_message_ptr_;
unsigned SignalHandler::signal_message_size_;
Interruptible *SignalHandler::interruptible_;

// Set stop_ to 1 initially to avoid accessing handler_ which may not be atomic.
volatile std::sig_atomic_t SignalHandler::stop_ = 1;

SignalHandler::SignalHandler(const Solver &s, Interruptible *i) {
  signal_message_ = fmt::format("\n<BREAK> ({})\n", s.name());
  signal_message_ptr_ = signal_message_.c_str();
  signal_message_size_ = static_cast<unsigned>(signal_message_.size());
  interruptible_ = i;
  std::signal(SIGINT, HandleSigInt);
  stop_ = 0;
}

void SignalHandler::HandleSigInt(int sig) {
  unsigned count = 0;
  do {
    // Use asynchronous-safe function write instead of printf!
    int result = AMPL_WRITE(1, signal_message_ptr_ + count,
        signal_message_size_ - count);
    if (result < 0) break;
    count += result;
  } while (count < signal_message_size_);
  if (stop_) {
    // Use asynchronous-safe function _exit instead of exit!
    _exit(1);
  }
  stop_ = 1;
  if (interruptible_)
    interruptible_->Interrupt();
  // Restore the handler since it might have been reset before the handler
  // is called (this is implementation defined).
  std::signal(sig, HandleSigInt);
}

bool Solver::OptionNameLess::operator()(
    const SolverOption *lhs, const SolverOption *rhs) const {
  return strcasecmp(lhs->name(), rhs->name()) < 0;
}

Solver::Solver(
    fmt::StringRef name, fmt::StringRef long_name, long date, int flags)
: name_(name), long_name_(long_name.c_str() ? long_name : name), date_(date),
  wantsol_(0), obj_precision_(-1), flags_(0),
  has_errors_(false), count_solutions_(false),
  read_flags_(0), timing_(false) {
  version_ = long_name_;
  error_handler_ = this;
  output_handler_ = this;

  struct VersionOption : SolverOption {
    Solver &s;
    VersionOption(Solver &s) : SolverOption("version",
        "Single-word phrase: report version details "
        "before solving the problem.", ValueArrayRef(), true), s(s) {}

    void Write(fmt::Writer &w) { w << ((s.flags_ & SHOW_VERSION) != 0); }
    void Parse(const char *&) { s.flags_ |= SHOW_VERSION; }
  };
  AddOption(OptionPtr(new VersionOption(*this)));

  struct WantSolOption : TypedSolverOption<int> {
    Solver &s;
    WantSolOption(Solver &s) : TypedSolverOption<int>("wantsol",
        "In a stand-alone invocation (no ``-AMPL`` on the command line), "
        "what solution information to write.  Sum of\n"
        "\n"
        "| 1 - write ``.sol`` file\n"
        "| 2 - primal variables to stdout\n"
        "| 4 - dual variables to stdout\n"
        "| 8 - suppress solution message\n"), s(s) {}

    int GetValue() const { return s.wantsol(); }
    void SetValue(int value) {
      if ((value & ~0xf) != 0)
        throw InvalidOptionValue("wantsol", value);
      s.wantsol_ = value;
    }
  };
  AddOption(OptionPtr(new WantSolOption(*this)));

  struct BoolOption : TypedSolverOption<int> {
    bool &value_;
    BoolOption(bool &value, const char *name, const char *description)
    : TypedSolverOption<int>(name, description), value_(value) {}

    int GetValue() const { return value_; }
    void SetValue(int value) {
      if (value != 0 && value != 1)
        throw InvalidOptionValue(name(), value);
      value_ = value != 0;
    }
  };
  AddOption(OptionPtr(new BoolOption(timing_, "timing",
      "0 or 1 (default 0): Whether to display timings for the run.\n")));

  if ((flags & MULTIPLE_SOL) != 0) {
    AddSuffix("nsol", 0, suf::PROBLEM | suf::OUTONLY);

    AddOption(OptionPtr(new BoolOption(count_solutions_, "countsolutions",
        "0 or 1 (default 0): Whether to count the number of solutions "
        "and return it in the ``.nsol`` problem suffix.")));

    struct StringOption : TypedSolverOption<std::string> {
      std::string &value_;
      StringOption(std::string &value,
          const char *name, const char *description)
      : TypedSolverOption<std::string>(name, description), value_(value) {}

      std::string GetValue() const { return value_; }
      void SetValue(const char *value) { value_ = value; }
    };
    AddOption(OptionPtr(new StringOption(solution_stub_, "solutionstub",
        "Stub for solution files.  If ``solutionstub`` is specified, "
        "found solutions are written to files (``solutionstub & '1' & "
        "'.sol'``) ... (``solutionstub & Current.nsol & '.sol'``), where "
        "``Current.nsol`` holds the number of returned solutions.  That is, "
        "file names are obtained by appending 1, 2, ... ``Current.nsol`` to "
        "``solutionstub``.")));
  }
}

Solver::~Solver() {
  std::for_each(options_.begin(), options_.end(), Deleter());
}

bool Solver::ShowVersion() {
  Print("{} ({})", version_, MP_SYSINFO);
  if (date_ > 0)
    Print(", driver({})", date_);
  Print(", ASL({})\n", MP_DATE);
  if (!license_info_.empty())
    Print("{}\n", license_info_);
  return false;
}

SolverOption *Solver::FindOption(const char *name) const {
  struct DummyOption : SolverOption {
    DummyOption(const char *name) : SolverOption(name, 0) {}
    void Write(fmt::Writer &) {}
    void Parse(const char *&) {}
  };
  DummyOption option(name);
  OptionSet::const_iterator i = options_.find(&option);
  return i != options_.end() ? *i : 0;
}

void Solver::ParseOptionString(const char *s, unsigned flags) {
  bool skip = false;
  for (;;) {
    if (!*(s = SkipSpaces(s)))
      return;

    // Parse the option name.
    const char *name_start = s;
    while (*s && !std::isspace(*s) && *s != '=')
      ++s;
    fmt::internal::Array<char, 50> name;
    std::size_t name_size = s - name_start;
    name.resize(name_size + 1);
    for (std::size_t i = 0; i < name_size; ++i)
      name[i] = name_start[i];
    name[name_size] = 0;

    // Parse the option value.
    bool equal_sign = false;
    s = SkipSpaces(s);
    if (*s == '=') {
      s = SkipSpaces(s + 1);
      equal_sign = true;
    }

    SolverOption *opt = FindOption(&name[0]);
    if (!opt) {
      if (!skip)
        HandleUnknownOption(&name[0]);
      if (equal_sign) {
        s = SkipNonSpaces(s);
      } else {
        // Skip everything until the next known option if there is no "="
        // because it is impossible to know whether the next token is an
        // option name or a value.
        // For example, if "a" in "a b c" is an unknown option, then "b"
        // can be either a value of option "a" or another option.
        skip = true;
      }
      continue;
    }

    skip = false;
    if (*s == '?') {
      char next = s[1];
      if (!next || std::isspace(next)) {
        ++s;
        if ((flags & NO_OPTION_ECHO) == 0) {
          fmt::Writer w;
          w << &name[0] << '=';
          opt->Write(w);
          w << '\n';
          Print(fmt::StringRef(w.c_str(), w.size()));
        }
        continue;
      }
    }
    if (opt->is_flag() && equal_sign) {
      ReportError("Option \"{}\" doesn't accept argument", &name[0]);
      s = SkipNonSpaces(s);
      continue;
    }
    try {
      opt->Parse(s);
    } catch (const OptionError &e) {
      ReportError("{}", e.what());
    }
    if ((flags & NO_OPTION_ECHO) == 0)
      Print("{}\n", std::string(name_start, s - name_start));
  }
}

bool Solver::ParseOptions(char **argv, unsigned flags, const Problem *) {
  has_errors_ = false;
  flags_ &= ~SHOW_VERSION;
  if (const char *s = std::getenv((name_ + "_options").c_str()))
    ParseOptionString(s, flags);
  while (const char *s = *argv++)
    ParseOptionString(s, flags);
  if ((flags_ & SHOW_VERSION) != 0)
    ShowVersion();
  return !has_errors_;
}

Solver::DoubleFormatter Solver::FormatObjValue(double value) {
  if (obj_precision_ < 0) {
    const char *s =  std::getenv("objective_precision");
    obj_precision_ = s ? std::atoi(s) : 0;
    if (obj_precision_ == 0)
      obj_precision_ = DEFAULT_PRECISION;
  }
  DoubleFormatter formatter = {value, obj_precision_};
  return formatter;
}
}  // namespace mp
