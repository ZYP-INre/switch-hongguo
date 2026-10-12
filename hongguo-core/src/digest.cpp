// SPDX-License-Identifier: GPL-3.0-only
//
// MD5 / SHA-512 / SM3 实现。三者写法保持一致：
// 定长块变换 + 统一的填充逻辑，便于逐块对照标准文档核验。
#include "hongguo/digest.hpp"

#include <cstring>

namespace hongguo {
namespace {

// 把消息补成整块：附加 0x80，补零至 (blockSize - lengthBytes)，再写比特长度。
// lengthBytes = 8（MD5/SM3）或 16（SHA-512）。
//
// ⚠ 长度字段的字节序随算法而异：MD5 用小端，SHA-512 与 SM3 用大端。
// 写错的话，空消息（长度 0）依然正确，只有非空消息才会错 —— 极易漏测。
Bytes padMessage(const Bytes& data, size_t blockSize, size_t lengthBytes,
                 bool littleEndianLength) {
  const uint64_t bitLength = static_cast<uint64_t>(data.size()) * 8;
  Bytes out = data;
  out.push_back(0x80);
  while (out.size() % blockSize != blockSize - lengthBytes) out.push_back(0x00);
  for (size_t i = 0; i < lengthBytes; ++i) {
    const size_t slot = littleEndianLength ? i : (lengthBytes - 1 - i);
    const size_t shift = slot * 8;
    // 消息长度远超 2^64 不现实；高位一律补零。
    out.push_back(shift >= 64 ? 0x00
                              : static_cast<uint8_t>((bitLength >> shift) & 0xff));
  }
  return out;
}

// ------------------------------------------------------------------ MD5

constexpr uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

constexpr int kMd5S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12,
                           17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                           5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11,
                           16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21,
                           6, 10, 15, 21, 6, 10, 15, 21};

void md5Block(uint32_t state[4], const uint8_t* block) {
  uint32_t m[16];
  for (int i = 0; i < 16; ++i) m[i] = readU32LE(Bytes(block, block + 64), i * 4);

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  for (int i = 0; i < 64; ++i) {
    uint32_t f;
    int g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) % 16;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) % 16;
    }
    const uint32_t temp = d;
    d = c;
    c = b;
    b = b + rol32(a + f + kMd5K[i] + m[g], kMd5S[i]);
    a = temp;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
}

// --------------------------------------------------------------- SHA-512

constexpr uint64_t kSha512K[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full,
    0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull, 0x59f111f1b605d019ull,
    0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull,
    0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
    0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull,
    0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull, 0x2de92c6f592b0275ull,
    0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full,
    0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
    0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull,
    0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull,
    0x92722c851482353bull, 0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull,
    0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
    0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull,
    0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull,
    0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull,
    0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull,
    0xc67178f2e372532bull, 0xca273eceea26619cull, 0xd186b8c721c0c207ull,
    0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull,
    0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
    0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull,
    0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};

void sha512Block(uint64_t state[8], const uint8_t* block) {
  uint64_t w[80];
  for (int i = 0; i < 16; ++i) {
    w[i] = 0;
    for (int j = 0; j < 8; ++j) w[i] = (w[i] << 8) | block[i * 8 + j];
  }
  for (int i = 16; i < 80; ++i) {
    const uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
    const uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint64_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint64_t e = state[4], f = state[5], g = state[6], h = state[7];

  for (int i = 0; i < 80; ++i) {
    const uint64_t s1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
    const uint64_t ch = (e & f) ^ (~e & g);
    const uint64_t temp1 = h + s1 + ch + kSha512K[i] + w[i];
    const uint64_t s0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
    const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint64_t temp2 = s0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

// ------------------------------------------------------------------ SM3

constexpr uint32_t kSm3Iv[8] = {0x7380166f, 0x4914b2b9, 0x172442d7, 0xda8a0600,
                                0xa96f30bc, 0x163138aa, 0xe38dee4d, 0xb0fb0e4e};

inline uint32_t sm3P0(uint32_t x) { return x ^ rol32(x, 9) ^ rol32(x, 17); }
inline uint32_t sm3P1(uint32_t x) { return x ^ rol32(x, 15) ^ rol32(x, 23); }

void sm3Block(uint32_t state[8], const uint8_t* block) {
  uint32_t w[68];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 68; ++i) {
    w[i] = sm3P1(w[i - 16] ^ w[i - 9] ^ rol32(w[i - 3], 15)) ^ rol32(w[i - 13], 7) ^
           w[i - 6];
  }

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

  for (int j = 0; j < 64; ++j) {
    const uint32_t t = j < 16 ? 0x79cc4519u : 0x7a879d8au;
    const uint32_t ff = j < 16 ? (a ^ b ^ c) : ((a & b) | (a & c) | (b & c));
    const uint32_t gg = j < 16 ? (e ^ f ^ g) : ((e & f) | (~e & g));
    const uint32_t wPrime = w[j] ^ w[j + 4];

    const uint32_t ss1 = rol32(rol32(a, 12) + e + rol32(t, j), 7);
    const uint32_t ss2 = ss1 ^ rol32(a, 12);
    const uint32_t tt1 = ff + d + ss2 + wPrime;
    const uint32_t tt2 = gg + h + ss1 + w[j];

    d = c;
    c = rol32(b, 9);
    b = a;
    a = tt1;
    h = g;
    g = rol32(f, 19);
    f = e;
    e = sm3P0(tt2);
  }

  state[0] ^= a;
  state[1] ^= b;
  state[2] ^= c;
  state[3] ^= d;
  state[4] ^= e;
  state[5] ^= f;
  state[6] ^= g;
  state[7] ^= h;
}

}  // namespace

Bytes md5(const Bytes& data) {
  uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  const Bytes padded = padMessage(data, 64, 8, /*littleEndianLength=*/true);
  for (size_t at = 0; at < padded.size(); at += 64) {
    md5Block(state, padded.data() + at);
  }
  Bytes out;
  out.reserve(16);
  for (int i = 0; i < 4; ++i) {
    const Bytes word = le32(state[i]);
    out.insert(out.end(), word.begin(), word.end());
  }
  return out;
}

Bytes sha512(const Bytes& data) {
  uint64_t state[8] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull,
                       0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
                       0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
                       0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};
  const Bytes padded = padMessage(data, 128, 16, /*littleEndianLength=*/false);
  for (size_t at = 0; at < padded.size(); at += 128) {
    sha512Block(state, padded.data() + at);
  }
  Bytes out;
  out.reserve(64);
  for (int i = 0; i < 8; ++i) {
    const Bytes word = be64(state[i]);
    out.insert(out.end(), word.begin(), word.end());
  }
  return out;
}

Bytes sm3(const Bytes& data) {
  uint32_t state[8];
  std::memcpy(state, kSm3Iv, sizeof(state));
  const Bytes padded = padMessage(data, 64, 8, /*littleEndianLength=*/false);
  for (size_t at = 0; at < padded.size(); at += 64) {
    sm3Block(state, padded.data() + at);
  }
  Bytes out;
  out.reserve(32);
  for (int i = 0; i < 8; ++i) {
    const Bytes word = be32(state[i]);
    out.insert(out.end(), word.begin(), word.end());
  }
  return out;
}

std::string md5Hex(const Bytes& data) { return toHex(md5(data)); }
std::string sha512Hex(const Bytes& data) { return toHex(sha512(data)); }
std::string md5HexUpper(const Bytes& data) { return toHexUpper(md5(data)); }

}  // namespace hongguo
