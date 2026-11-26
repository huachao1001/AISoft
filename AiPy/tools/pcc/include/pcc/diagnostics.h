// 编译器面向用户的全部文案，集中一处。
//
// ── 为什么集中 ──
//
// 1. **国际化**：以后要出多语言版本，只需替换本文件的表，不用在二十个
//    生成点里找字符串。分散的文案做国际化必然漏。
// 2. **一致性**：同一条约束（比如「没有一等函数」）会在多处触发，分散写
//    就会各写一遍、各说一套。
//
// ── 语言选择 ──
//
// 全部英文。编译器是工具链的一环，输出会被编辑器、CI 日志、其他工具解析；
// 英文对非中文使用者可用，也避免了「源码 UTF-8、控制台代码页 GBK」时的
// 乱码问题。中文说明留在代码注释里给维护者看。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pcc {
namespace diag {

// ── 前端（词法 / 语法）──
std::string unexpectedIndent();
std::string indentationMismatch();
std::string missingQuote();
std::string unterminatedString();
std::string newlineInString();
std::string badHexEscape(int digits);
std::string expectedToken(const std::string& wanted, const std::string& got);
std::string expectedNewline();
std::string blockExpected();          // ':' 后面应当是缩进块
std::string nameExpected(const char* what);
std::string unsupportedExpressionToken(const std::string& token);
std::string badAssignTarget();
std::string strayToken(const std::string& token);

// ── 语义 / 代码生成（与 compiler.py 的文案一一对应）──
std::string multipleActivity();
std::string classMustDeriveActivity();
std::string activityBodyOnlyFieldsAndMethods();
std::string duplicateFunction(const std::string& name);
std::string variadicParams();
std::string positionalOnlyParams();
std::string defaultParams();
std::string nestedFunction(const std::string& name);
std::string keywordArguments();
std::string attributeAccess();
std::string builtinAsValue(const std::string& name);
std::string functionAsValue(const std::string& name);
std::string reassignFunction(const std::string& name);
std::string missingGlobal(const std::string& name);
std::string yieldUnsupported();
std::string awaitUnsupported();
std::string whileElse();
std::string forElse();
std::string breakOutsideLoop();
std::string continueOutsideLoop();
std::string nonlocalUnsupported();
std::string assertUnsupported();
std::string raiseUnsupported();
std::string tryUnsupported();
// try 少了 except / finally（CPython 同样拒绝）。
std::string tryNeedsHandler();
// bare raise 出现在 except 之外。
std::string bareRaiseOutsideExcept();
// except 的类型不是异常类（这是运行期判定，这里只给文案）。
std::string exceptTypeNotException(const std::string& name);
// raise X(...) 给了多于一个参数。
std::string raiseTooManyArgs(const std::string& name);
std::string deleteUnsupported();
std::string withAsNameOnly();
std::string tupleUnpackAssign();
std::string unsupportedAugTarget();
std::string loopVariableName();
std::string unsupportedStatement(const std::string& kind);
std::string unsupportedExpression(const std::string& kind);
std::string unsupportedLiteral(const std::string& repr);
std::string decoratorUnsupported();
std::string methodSelfRequired(const std::string& name);std::string unsupportedOperator(const std::string& name);
std::string unsupportedComparison(const std::string& name);
std::string unsupportedUnary(const std::string& name);
std::string fstringUnsupported();
std::string tupleLiteralUnsupported();
std::string dictSpreadUnsupported();
std::string unsupportedCallTarget();
std::string missingInKeyword();  // for 的目标

// ── 后端（字节码 / 打包 / 链接）──
std::string fileNotFound(const std::string& path);
std::string notBytecode(const std::string& path);
std::string emptyBytecode(const std::string& path);
std::string alreadyBundled(const std::string& path);
std::string noEmbeddedScript(const std::string& path);
std::string missingLibrary(const std::string& module, const std::string& dir);
// 库找到了，但它不提供脚本调用的那个函数（版本不匹配）。
std::string missingFunction(const std::string& module,
                            const std::string& function,
                            const std::string& archive,
                            const std::vector<std::string>& provided);
// 脚本调用了一个不带模块前缀的名字（`open`），但没有任何库声明它。
std::string unknownBareName(const std::string& name, const std::string& dir);
std::string missingRuntime(const std::string& name, const std::string& dir);
std::string missingObject(const std::string& path);
std::string missingCMakeCache(const std::string& dir);
std::string badLinker(const std::string& path);
std::string linkFailed(const std::string& stdoutText,
                       const std::string& stderrText);
std::string unsupportedPlatform(const std::string& platform);

}  // namespace diag
}  // namespace pcc
