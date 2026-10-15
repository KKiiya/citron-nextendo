"""Compile the real loader selector with isolated IPS/catalog boundaries."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    headers = {
        'common/common_types.h': '''#pragma once
#include <cstdint>
using u8 = std::uint8_t;
using u64 = std::uint64_t;
''',
        'common/hex_util.h': '''#pragma once
#include <array>
#include <string>
#include "common/common_types.h"
namespace Common {
inline std::string HexToString(const std::array<u8, 32>& bytes) {
    const char* digits = "0123456789ABCDEF";
    std::string result;
    for (auto byte : bytes) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}
}
''',
        'common/logging.h': '''#pragma once
template <class... Args> void TestLog(Args&&...) {}
#define LOG_INFO(category, ...) TestLog(__VA_ARGS__)
#define LOG_WARNING(category, ...) TestLog(__VA_ARGS__)
#define LOG_ERROR(category, ...) TestLog(__VA_ARGS__)
''',
        'core/file_sys/vfs/vfs_vector.h': '''#pragma once
#include <memory>
#include <string>
#include <vector>
#include "common/common_types.h"
namespace FileSys {
class VectorVfsFile {
    std::vector<u8> bytes;
public:
    VectorVfsFile(std::vector<u8> data, const std::string&) : bytes(std::move(data)) {}
    std::vector<u8> ReadAllBytes() const { return bytes; }
};
using VirtualFile = std::shared_ptr<VectorVfsFile>;
}
''',
        'core/file_sys/ips_layer.h': '''#pragma once
#include "core/file_sys/vfs/vfs_vector.h"
namespace FileSys { VirtualFile PatchIPS(const VirtualFile&, const VirtualFile&); }
'''
    }
    for name, content in headers.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    for web_service in [False, True]:
        command = ['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(root), '-I' + str(repo / 'src')]
        if web_service:
            command += ['-DENABLE_WEB_SERVICE']
        command += [str(repo / 'src/core/loader/nextendo_s3_patches.cpp'),
                    str(repo / 'test-files/npln_patch_loader_test.cpp'), '-o', str(root / 'test-loader')]
        subprocess.run(command, check=True)
        subprocess.run([str(root / 'test-loader')], check=True)
print('PASS: no NPLN built-in fallback, signed selection/withdrawal, exact CTR title/module fallback, with and without web service')
