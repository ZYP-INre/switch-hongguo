// SPDX-License-Identifier: GPL-3.0-only
#include "harness.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace hgtest {

std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

void fail(const std::string& message) { throw Failure{message}; }

std::string describe(const std::vector<uint8_t>& value) {
  static const char* kHex = "0123456789abcdef";
  std::string out = "hex(";
  for (const uint8_t byte : value) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0f]);
  }
  out += ")";
  return out;
}

std::string vectorFile(const std::string& name) {
  const std::string path = std::string(HG_VECTOR_DIR) + "/" + name;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw Failure{"缺少向量文件: " + path +
                  "\n    生成方式: cd hongguo-core && node tools/oracle/gen-vectors.mjs"};
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::vector<std::vector<std::string>> tsv(const std::string& name) {
  std::istringstream in(vectorFile(name));
  std::vector<std::vector<std::string>> rows;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> cells;
    size_t start = 0;
    while (true) {
      const size_t tab = line.find('\t', start);
      if (tab == std::string::npos) {
        cells.push_back(line.substr(start));
        break;
      }
      cells.push_back(line.substr(start, tab - start));
      start = tab + 1;
    }
    rows.push_back(std::move(cells));
  }
  return rows;
}

int run(int argc, char** argv) {
  const std::string filter = argc > 1 ? argv[1] : "";
  int passed = 0;
  int failed = 0;

  for (const Case& item : registry()) {
    if (!filter.empty() && item.name.find(filter) == std::string::npos) continue;
    try {
      item.fn();
      ++passed;
      std::printf("  ✓ %s\n", item.name.c_str());
    } catch (const Failure& failure) {
      ++failed;
      std::printf("  ✗ %s\n    %s\n", item.name.c_str(), failure.message.c_str());
    } catch (const std::exception& error) {
      ++failed;
      std::printf("  ✗ %s\n    抛出未预期异常: %s\n", item.name.c_str(), error.what());
    }
  }

  std::printf("\n结果：%d 通过 / %d 失败 / 共 %d\n", passed, failed, passed + failed);
  return failed == 0 ? 0 : 1;
}

}  // namespace hgtest

int main(int argc, char** argv) { return hgtest::run(argc, argv); }
