#ifndef DATACODEC_TEST_COMMON_DATACODECTESTRESULT_H
#define DATACODEC_TEST_COMMON_DATACODECTESTRESULT_H

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace datacodec::test {

// 仅由显式验收入口在测试开始前安装，测试收束后移除
using TestCheckObserver = void (*)(bool, std::string_view) noexcept;
inline TestCheckObserver testCheckObserver{nullptr};

inline bool RunNamedCheck(const char* name, bool (*check)()) {
    const bool passed = check();
    if (testCheckObserver) { testCheckObserver(passed, name); }
    return passed;
}

struct TestFailure {
    std::string check;
    std::string message;
};

struct TestResult {
    bool passed{true};
    std::vector<TestFailure> failures;
    std::vector<std::string> diagnostics;

    void AddFailure(std::string check, std::string message) {
        passed = false;
        failures.push_back(TestFailure{
            .check = std::move(check),
            .message = std::move(message),
        });
    }

    void AddDiagnostic(std::string diagnostic) {
        diagnostics.push_back(std::move(diagnostic));
    }

    void AppendDiagnostics(const std::vector<std::string>& values) {
        diagnostics.insert(diagnostics.end(), values.begin(), values.end());
    }
};

inline bool Require(TestResult& result, const bool condition, std::string check, std::string message) {
    if (testCheckObserver) { testCheckObserver(condition, check); }
    if (condition) {
        return true;
    }
    result.AddFailure(std::move(check), std::move(message));
    return false;
}

} // namespace datacodec::test

#endif
