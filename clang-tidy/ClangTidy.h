//===--- ClangTidy.h - clang-tidy -------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CLANGTIDY_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CLANGTIDY_H

#include "ClangTidyDiagnosticConsumer.h"
#include "ClangTidyOptions.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Tooling/Refactoring.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>
#include <type_traits>
#include <vector>

namespace clang {

class CompilerInstance;
namespace tooling {
class CompilationDatabase;
}

namespace tidy {

/// \brief Provides access to the \c ClangTidyCheck options via check-local
/// names.
///
/// Methods of this class prepend <tt>CheckName + "."</tt> to translate
/// check-local option names to global option names.
class OptionsView {
public:
  /// \brief Initializes the instance using \p CheckName + "." as a prefix.
  OptionsView(StringRef CheckName,
              const ClangTidyOptions::OptionMap &CheckOptions);

  /// \brief Read a named option from the \c Context.
  ///
  /// Reads the option with the check-local name \p LocalName from the
  /// \c CheckOptions. If the corresponding key is not present, returns
  /// \p Default.
  std::string get(StringRef LocalName, std::string Default) const;

  /// \brief Read a named option from the \c Context and parse it as an integral
  /// type \c T.
  ///
  /// Reads the option with the check-local name \p LocalName from the
  /// \c CheckOptions. If the corresponding key is not present, returns
  /// \p Default.
  template <typename T>
  typename std::enable_if<std::is_integral<T>::value, T>::type
  get(StringRef LocalName, T Default) const {
    std::string Value = get(LocalName, "");
    T Result = Default;
    if (!Value.empty())
      StringRef(Value).getAsInteger(10, Result);
    return Result;
  }

  /// \brief Stores an option with the check-local name \p LocalName with string
  /// value \p Value to \p Options.
  void store(ClangTidyOptions::OptionMap &Options, StringRef LocalName,
             StringRef Value) const;

  /// \brief Stores an option with the check-local name \p LocalName with
  /// \c int64_t value \p Value to \p Options.
  void store(ClangTidyOptions::OptionMap &Options, StringRef LocalName,
             int64_t Value) const;

private:
  std::string NamePrefix;
  const ClangTidyOptions::OptionMap &CheckOptions;
};

/// \brief Base class for all clang-tidy checks.
///
/// To implement a \c ClangTidyCheck, write a subclass and override some of the
/// base class's methods. E.g. to implement a check that validates namespace
/// declarations, override \c registerMatchers:
///
/// \code
/// registerMatchers(ast_matchers::MatchFinder *Finder) {
///   Finder->addMatcher(namespaceDecl().bind("namespace"), this);
/// }
/// \endcode
///
/// and then override \c check(const MatchResult &Result) to do the actual
/// check for each match.
///
/// A new \c ClangTidyCheck instance is created per translation unit.
///
/// FIXME: Figure out whether carrying information from one TU to another is
/// useful/necessary.
class ClangTidyCheck : public ast_matchers::MatchFinder::MatchCallback {
public:
  /// \brief Initializes the check with \p CheckName and \p Context.
  ///
  /// Derived classes must implement the constructor with this signature or
  /// delegate it. If a check needs to read options, it can do this in the
  /// constructor using the Options.get() methods below.
  ClangTidyCheck(StringRef CheckName, ClangTidyContext *Context)
      : CheckName(CheckName), Context(Context),
        Options(CheckName, Context->getOptions().CheckOptions) {
    assert(Context != nullptr);
    assert(!CheckName.empty());
  }

  /// \brief Override this to register \c PPCallbacks with \c Compiler.
  ///
  /// This should be used for clang-tidy checks that analyze preprocessor-
  /// dependent properties, e.g. the order of include directives.
  virtual void registerPPCallbacks(CompilerInstance &Compiler) {}

  /// \brief Override this to register ASTMatchers with \p Finder.
  ///
  /// This should be used by clang-tidy checks that analyze code properties that
  /// dependent on AST knowledge.
  ///
  /// You can register as many matchers as necessary with \p Finder. Usually,
  /// "this" will be used as callback, but you can also specify other callback
  /// classes. Thereby, different matchers can trigger different callbacks.
  ///
  /// If you need to merge information between the different matchers, you can
  /// store these as members of the derived class. However, note that all
  /// matches occur in the order of the AST traversal.
  virtual void registerMatchers(ast_matchers::MatchFinder *Finder) {}

  /// \brief \c ClangTidyChecks that register ASTMatchers should do the actual
  /// work in here.
  virtual void check(const ast_matchers::MatchFinder::MatchResult &Result) {}

  /// \brief Add a diagnostic with the check's name.
  DiagnosticBuilder diag(SourceLocation Loc, StringRef Description,
                         DiagnosticIDs::Level Level = DiagnosticIDs::Warning);

  /// \brief Should store all options supported by this check with their
  /// current values or default values for options that haven't been overridden.
  ///
  /// The check should use \c Options.store() to store each option it supports
  /// whether it has the default value or it has been overridden.
  virtual void storeOptions(ClangTidyOptions::OptionMap &Options) {}

private:
  void run(const ast_matchers::MatchFinder::MatchResult &Result) override;
  StringRef getID() const override { return CheckName; }
  std::string CheckName;
  ClangTidyContext *Context;

protected:
  OptionsView Options;
};

class ClangTidyCheckFactories;

class ClangTidyASTConsumerFactory {
public:
  ClangTidyASTConsumerFactory(ClangTidyContext &Context);

  /// \brief Returns an ASTConsumer that runs the specified clang-tidy checks.
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &Compiler, StringRef File);

  /// \brief Get the list of enabled checks.
  std::vector<std::string> getCheckNames();

  /// \brief Get the union of options from all checks.
  ClangTidyOptions::OptionMap getCheckOptions();

private:
  typedef std::vector<std::pair<std::string, bool>> CheckersList;
  CheckersList getCheckersControlList(GlobList &Filter);

  ClangTidyContext &Context;
  std::unique_ptr<ClangTidyCheckFactories> CheckFactories;
};

/// \brief Fills the list of check names that are enabled when the provided
/// filters are applied.
std::vector<std::string> getCheckNames(const ClangTidyOptions &Options);

/// \brief Returns the effective check-specific options.
///
/// The method configures ClangTidy with the specified \p Options and collects
/// effective options from all created checks. The returned set of options
/// includes default check-specific options for all keys not overridden by \p
/// Options.
ClangTidyOptions::OptionMap getCheckOptions(const ClangTidyOptions &Options);

/// \brief Run a set of clang-tidy checks on a set of files.
///
/// \param Profile if provided, it enables check profile collection in
/// MatchFinder, and will contain the result of the profile.
ClangTidyStats
runClangTidy(std::unique_ptr<ClangTidyOptionsProvider> OptionsProvider,
             const tooling::CompilationDatabase &Compilations,
             ArrayRef<std::string> InputFiles,
             std::vector<ClangTidyError> *Errors,
             ProfileData *Profile = nullptr);

// FIXME: This interface will need to be significantly extended to be useful.
// FIXME: Implement confidence levels for displaying/fixing errors.
//
/// \brief Displays the found \p Errors to the users. If \p Fix is true, \p
/// Errors containing fixes are automatically applied.
void handleErrors(const std::vector<ClangTidyError> &Errors, bool Fix);

/// \brief Serializes replacements into YAML and writes them to the specified
/// output stream.
void exportReplacements(const std::vector<ClangTidyError> &Errors,
                        raw_ostream &OS);

} // end namespace tidy
} // end namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CLANGTIDY_H
