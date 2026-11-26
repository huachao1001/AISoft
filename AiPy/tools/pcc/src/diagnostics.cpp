// 诊断文案的实现。集中在这里，见头文件的理由。
#include "pcc/diagnostics.h"

namespace pcc {
namespace diag {
namespace {

// 拼接小工具：避免到处写 `std::string("...") + name + "..."` 时漏个空格。
std::string quote(const std::string& text) { return "'" + text + "'"; }

}  // namespace

// ── 前端 ──

std::string unexpectedIndent() { return "unexpected indent"; }

std::string indentationMismatch() {
  return "unindent does not match any outer indentation level";
}

std::string missingQuote() { return "string literal is missing a quote"; }

std::string unterminatedString() {
  return "unterminated string literal (reached end of input)";
}

std::string newlineInString() {
  return "unterminated string literal (newline in single-quoted string)";
}

std::string badHexEscape(int digits) {
  return "hex escape requires exactly " + std::to_string(digits) +
         " hex digits";
}

std::string expectedToken(const std::string& wanted, const std::string& got) {
  return "expected " + quote(wanted) + " but found " + quote(got);
}

std::string expectedNewline() { return "expected a newline after the statement"; }

std::string blockExpected() {
  return "expected an indented block after ':'";
}

std::string nameExpected(const char* what) {
  return std::string("expected a name after ") + what;
}

std::string unsupportedExpressionToken(const std::string& token) {
  return "unsupported expression starting at " + quote(token);
}

std::string badAssignTarget() { return "unsupported assignment target"; }

std::string strayToken(const std::string& token) {
  return "unexpected token " + quote(token);
}

// ── 语义 / 代码生成 ──

std::string multipleActivity() {
  return "only one Activity class is allowed per script";
}

std::string classMustDeriveActivity() {
  return "a class must derive from Activity";
}

std::string activityBodyOnlyFieldsAndMethods() {
  return "an Activity body may only contain field assignments and methods";
}

std::string duplicateFunction(const std::string& name) {
  return "function " + quote(name) + " is defined more than once";
}

std::string variadicParams() {
  return "*args / **kwargs / keyword-only parameters are not supported";
}

std::string positionalOnlyParams() {
  return "positional-only parameters (the '/' separator) are not supported";
}

std::string defaultParams() {
  return "default parameter values are not supported by the bytecode compiler";
}

std::string nestedFunction(const std::string& name) {
  return "nested function " + quote(name) +
         " is not supported; move it to module level";
}

std::string keywordArguments() {
  return "keyword arguments are not supported; pass arguments by position";
}

std::string attributeAccess() {
  return "attribute access is not supported (there is no object model); "
         "call extension functions as 'module.function(...)'";
}

std::string builtinAsValue(const std::string& name) {
  return "builtin " + quote(name) + " cannot be used as a value";
}

std::string functionAsValue(const std::string& name) {
  return "function " + quote(name) +
         " cannot be used as a value (there are no first-class functions); "
         "call it directly";
}

std::string reassignFunction(const std::string& name) {
  return "cannot reassign function " + quote(name) +
         "; there are no first-class functions";
}

std::string missingGlobal(const std::string& name) {
  return "assignment to module-level variable " + quote(name) +
         " inside a function without a 'global' declaration; CPython would "
         "treat it as a new local variable. Write 'global " + name +
         "' first to modify the module-level variable";
}

std::string yieldUnsupported() {
  return "'yield' is not supported: scripts have no generators";
}

std::string awaitUnsupported() {
  return "'await' is not supported: scripts run on a single thread";
}

std::string whileElse() { return "'while ... else' is not supported"; }

std::string forElse() { return "'for ... else' is not supported"; }

std::string breakOutsideLoop() { return "'break' outside a loop"; }

std::string continueOutsideLoop() { return "'continue' outside a loop"; }

std::string nonlocalUnsupported() {
  return "'nonlocal' is not supported; nested functions are not supported "
         "either";
}

std::string assertUnsupported() {
  return "'assert' is not supported; use 'if' and handle the case explicitly";
}

std::string raiseUnsupported() {
  return "'raise' is not supported: there is no exception channel; signal "
         "failure with a return value";
}

std::string tryUnsupported() { return "'try' statements are not supported"; }

std::string raiseTooManyArgs(const std::string& name) {
  return "raise " + name + "(...) takes at most one argument here; "
         "exception argument tuples are not supported yet";
}

std::string tryNeedsHandler() {
  return "'try' must be followed by 'except' or 'finally'";
}

std::string bareRaiseOutsideExcept() {
  return "'raise' with no exception re-raises the current one, "
         "which only exists inside an 'except' block";
}

std::string exceptTypeNotException(const std::string& name) {
  return "\"" + name + "\" is not an exception type";
}

std::string deleteUnsupported() {
  return "'del' is not supported; use .pop() or .clear() on containers";
}

std::string withAsNameOnly() {
  return "'with ... as' may only bind a single name";
}

std::string tupleUnpackAssign() {
  return "tuple unpacking assignment is not supported; assign each name "
         "separately";
}

std::string unsupportedAugTarget() {
  return "unsupported target for an augmented assignment";
}

std::string loopVariableName() {
  return "the for-loop variable must be a single name";
}

std::string unsupportedStatement(const std::string& kind) {
  return "unsupported statement: " + kind;
}

std::string unsupportedExpression(const std::string& kind) {
  return "unsupported expression: " + kind;
}

std::string unsupportedLiteral(const std::string& repr) {
  return "unsupported literal: " + repr;
}

std::string decoratorUnsupported() {
  return "decorators are not supported: scripts have no first-class "
         "functions";
}

std::string methodSelfRequired(const std::string& name) {
  return "the first parameter of method '" + name + "' must be 'self'";
}

std::string unsupportedOperator(const std::string& name) {
  return "unsupported operator: " + name;
}

std::string unsupportedComparison(const std::string& name) {
  return "unsupported comparison: " + name;
}

std::string unsupportedUnary(const std::string& name) {
  return "unsupported unary operator: " + name;
}

std::string fstringUnsupported() {
  return "f-strings are not supported by the bytecode compiler; use "
         "format_text() or string concatenation";
}

std::string tupleLiteralUnsupported() {
  return "tuple literals are not supported (tuples would become lists); the "
         "bytecode compiler only supports list literals";
}

std::string dictSpreadUnsupported() {
  return "'**' dictionary unpacking is not supported";
}

std::string unsupportedCallTarget() {
  return "this call target is not supported";
}

std::string missingInKeyword() { return "'for' statement is missing 'in'"; }

// ── 后端 ──

std::string fileNotFound(const std::string& path) {
  return "file not found: " + path;
}

std::string notBytecode(const std::string& path) {
  return path + " is not AiPy bytecode (magic mismatch)";
}

std::string emptyBytecode(const std::string& path) {
  return path + " is an empty file";
}

std::string alreadyBundled(const std::string& path) {
  return path +
         " is already a bundled application. Use the original aipy_app.exe "
         "produced by CMake as the launcher, otherwise the result is nested "
         "and still runs, but reads the inner (stale) script";
}

std::string noEmbeddedScript(const std::string& path) {
  return path + " has no embedded script";
}

std::string missingLibrary(const std::string& module, const std::string& dir) {
  return "the script uses extension module '" + module + "' but no matching "
         "library was found in " + dir + " (expected " + module + ".pylib)";
}

std::string missingFunction(const std::string& module,
                            const std::string& function,
                            const std::string& archive,
                            const std::vector<std::string>& provided) {
  std::string available;
  for (const std::string& name : provided) {
    if (!available.empty()) available += ", ";
    available += name;
  }
  if (available.empty()) available = "(none)";
  return "the script calls " + module + "." + function +
         " but the library in " + archive + " does not provide it (available: " +
         available + "); the script and the library are out of sync";
}

std::string missingRuntime(const std::string& name, const std::string& dir) {
  return "cannot find " + name + " (looked in " + dir + ")";
}

std::string unknownBareName(const std::string& name, const std::string& dir) {
  return "'" + name + "' is not a builtin, and no library in " + dir +
         " declares it as a bare name; if a library provides it, put its "
         ".pylib in that directory";
}

std::string missingObject(const std::string& path) {
  return "cannot find the launcher object file: " + path;
}

std::string missingCMakeCache(const std::string& dir) {
  return dir + " has no CMakeCache.txt; cannot determine the linker path";
}

std::string badLinker(const std::string& path) {
  return "the configured linker is not usable: " + path;
}

std::string linkFailed(const std::string& stdoutText,
                       const std::string& stderrText) {
  std::string text = "linker failed";
  if (!stdoutText.empty()) text += "\n" + stdoutText;
  if (!stderrText.empty()) text += "\n" + stderrText;
  return text;
}std::string unsupportedPlatform(const std::string& platform) {
  return "pylib linking is not implemented for platform '" + platform +
         "' (only Windows/MSVC is supported). To add a platform, implement "
         "the Toolchain interface and the matching branch in "
         "include/aipy/pylib.h";
}

}  // namespace diag
}  // namespace pcc
