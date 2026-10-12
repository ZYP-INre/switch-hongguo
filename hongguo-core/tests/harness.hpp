// SPDX-License-Identifier: GPL-3.0-only
//
// 极简测试装置。刻意不引入 gtest —— 测试要能在 devkitPro 上一起编，
// 依赖越少越好。功能就三样：注册用例、断言、失败即停并报出位置。

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace hgtest {

struct Case {
  std::string name;
  std::function<void()> fn;
};

std::vector<Case>& registry();

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) {
    registry().push_back({name, std::move(fn)});
  }
};

// 抛异常终止当前用例，由 run() 捕获后记为失败。
struct Failure {
  std::string message;
};

[[noreturn]] void fail(const std::string& message);

// 读取 tests/vectors/<name>。<name> 不存在时抛 Failure 并提示生成方法。
std::string vectorFile(const std::string& name);

// 解析 tests/vectors/<name> 的 TSV：跳过空行与 '#' 开头的注释行，按 '\t' 切列。
// 刻意不引 JSON —— 测试要能在 devkitPro 上一起编。
std::vector<std::vector<std::string>> tsv(const std::string& name);

// ---- 值的可读呈现 ----

inline std::string describe(const std::string& value) { return '"' + value + '"'; }
inline std::string describe(const char* value) {
  return std::string("\"") + value + '"';
}

// 以十六进制呈现字节序列，便于比对密码学输出。
std::string describe(const std::vector<uint8_t>& value);

template <typename T>
std::string describe(const T& value) {
  if constexpr (std::is_arithmetic_v<T>) {
    return std::to_string(value);
  } else {
    return "<不可打印>";
  }
}

// 执行全部注册用例。返回进程退出码（0 = 全通过）。
int run(int argc, char** argv);

}  // namespace hgtest

#define HG_TEST(name)                                    \
  static void name();                                    \
  static ::hgtest::Registrar hg_reg_##name(#name, name); \
  static void name()

#define HG_CHECK(cond)                                                  \
  do {                                                                  \
    if (!(cond)) {                                                      \
      ::hgtest::fail(std::string(__FILE__) + ":" +                      \
                     std::to_string(__LINE__) + " 断言不成立: " #cond); \
    }                                                                   \
  } while (0)

#define HG_CHECK_EQ(actual, expected)                                         \
  do {                                                                        \
    const auto& hg_a = (actual);                                              \
    const auto& hg_e = (expected);                                            \
    if (!(hg_a == hg_e)) {                                                    \
      ::hgtest::fail(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                     "\n    实得: " + ::hgtest::describe(hg_a) +              \
                     "\n    期望: " + ::hgtest::describe(hg_e));              \
    }                                                                         \
  } while (0)
