#include "catalyst/pattern.hpp"
#include <Windows.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <emmintrin.h>
#include <stdexcept>

namespace catalyst {
namespace {
[[noreturn]] void invalid(const char *message) {
  throw std::runtime_error(message);
}
template <typename T> T read(std::span<const uint8_t> data, size_t offset) {
  if (offset > data.size() || sizeof(T) > data.size() - offset)
    invalid("Truncated PE image or operand");
  T value;
  std::memcpy(&value, data.data() + offset, sizeof value);
  return value;
}
int nibble(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
bool space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
} // namespace

BytePattern::BytePattern(std::string_view text) {
  size_t cursor = 0;
  while (cursor < text.size()) {
    while (cursor < text.size() && space(text[cursor]))
      ++cursor;
    if (cursor == text.size())
      break;
    auto start = cursor;
    while (cursor < text.size() && !space(text[cursor]))
      ++cursor;
    auto token = text.substr(start, cursor - start);
    if (token == "?")
      token = "??";
    if (token.size() != 2)
      invalid("Pattern tokens must be hex bytes or wildcards");
    uint8_t byte = 0, mask = 0;
    for (char c : token) {
      byte = static_cast<uint8_t>(byte << 4);
      mask = static_cast<uint8_t>(mask << 4);
      if (c != '?') {
        auto value = nibble(c);
        if (value < 0)
          invalid("Invalid hexadecimal pattern token");
        byte |= static_cast<uint8_t>(value);
        mask |= 15;
      }
    }
    if (bytes_.size() == 512)
      invalid("Pattern exceeds 512 bytes");
    bytes_.push_back(byte);
    masks_.push_back(mask);
  }
  if (bytes_.empty())
    invalid("Empty byte pattern");
  auto first = std::find_if(masks_.begin(), masks_.end(), [](uint8_t mask) { return mask != 0; });
  if (first == masks_.end())
    invalid("An all-wildcard pattern is not selective");
  first_ = static_cast<size_t>(first - masks_.begin());
  last_ = masks_.size() - 1;
  while (!masks_[last_])
    --last_;
}

bool BytePattern::matches(const uint8_t *data) const {
  size_t i = 0;
  const auto zero = _mm_setzero_si128();
  for (; i + 16 <= bytes_.size(); i += 16) {
    auto actual = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data + i));
    auto expected = _mm_loadu_si128(reinterpret_cast<const __m128i *>(bytes_.data() + i));
    auto mask = _mm_loadu_si128(reinterpret_cast<const __m128i *>(masks_.data() + i));
    auto difference = _mm_and_si128(_mm_xor_si128(actual, expected), mask);
    if (_mm_movemask_epi8(_mm_cmpeq_epi8(difference, zero)) != 0xffff)
      return false;
  }
  for (; i < bytes_.size(); ++i)
    if (((data[i] ^ bytes_[i]) & masks_[i]) != 0)
      return false;
  return true;
}

std::vector<size_t> BytePattern::find(std::span<const uint8_t> data, size_t limit, bool simd) const {
  if (!limit)
    invalid("Pattern match limit must be positive");
  std::vector<size_t> results;
  if (data.size() < bytes_.size())
    return results;
  const auto count = data.size() - bytes_.size() + 1;
  const auto first_byte = _mm_set1_epi8(static_cast<char>(bytes_[first_]));
  const auto last_byte = _mm_set1_epi8(static_cast<char>(bytes_[last_]));
  const auto first_mask = _mm_set1_epi8(static_cast<char>(masks_[first_]));
  const auto last_mask = _mm_set1_epi8(static_cast<char>(masks_[last_]));
  const auto zero = _mm_setzero_si128();
  size_t cursor = 0;
  if (simd && count >= 16) {
    for (; cursor <= count - 16; cursor += 16) {
      // Both loads stay inside the supplied span, including at page boundaries.
      auto first = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data.data() + cursor + first_));
      auto last = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data.data() + cursor + last_));
      auto a = _mm_cmpeq_epi8(_mm_and_si128(_mm_xor_si128(first, first_byte), first_mask), zero);
      auto b = _mm_cmpeq_epi8(_mm_and_si128(_mm_xor_si128(last, last_byte), last_mask), zero);
      unsigned candidates = static_cast<unsigned>(_mm_movemask_epi8(_mm_and_si128(a, b)));
      while (candidates) {
        auto offset = cursor + std::countr_zero(candidates);
        if (matches(data.data() + offset)) {
          results.push_back(offset);
          if (results.size() == limit)
            return results;
        }
        candidates &= candidates - 1;
      }
    }
  }
  for (; cursor < count; ++cursor)
    if (matches(data.data() + cursor)) {
      results.push_back(cursor);
      if (results.size() == limit)
        break;
    }
  return results;
}

PatternImage::PatternImage(std::span<const uint8_t> image, uintptr_t virtual_base) : bytes_(image) {
  auto dos = read<IMAGE_DOS_HEADER>(image, 0);
  if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof dos)
    invalid("Invalid DOS image header");
  auto nt = read<IMAGE_NT_HEADERS64>(image, static_cast<size_t>(dos.e_lfanew));
  if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
      !nt.OptionalHeader.SizeOfImage || nt.OptionalHeader.SizeOfImage > image.size() ||
      !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96 ||
      nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION)
    invalid("Unsupported or truncated AMD64 PE image");
  bytes_ = image.first(nt.OptionalHeader.SizeOfImage);
  base_ = virtual_base ? virtual_base : reinterpret_cast<uintptr_t>(bytes_.data());
  auto table = static_cast<size_t>(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS64);
  for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
    auto section = read<IMAGE_SECTION_HEADER>(bytes_, table + i * sizeof(IMAGE_SECTION_HEADER));
    auto size = section.Misc.VirtualSize ? section.Misc.VirtualSize : section.SizeOfRawData;
    if (!size || section.VirtualAddress > bytes_.size() || size > bytes_.size() - section.VirtualAddress)
      invalid("Invalid PE section bounds");
    size_t length = 0;
    while (length < IMAGE_SIZEOF_SHORT_NAME && section.Name[length])
      ++length;
    sections_.push_back({std::string(reinterpret_cast<const char *>(section.Name), length),
                         section.VirtualAddress, size, section.Characteristics});
  }
  std::sort(sections_.begin(), sections_.end(), [](const auto &a, const auto &b) { return a.rva < b.rva; });
  for (size_t i = 1; i < sections_.size(); ++i)
    if (uint64_t(sections_[i - 1].rva) + sections_[i - 1].size > sections_[i].rva)
      invalid("Overlapping PE sections");
  const auto directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  if (!directory.Size || directory.Size % sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY) ||
      directory.VirtualAddress > bytes_.size() || directory.Size > bytes_.size() - directory.VirtualAddress)
    invalid("Invalid AMD64 unwind directory");
  for (size_t offset = 0; offset < directory.Size; offset += sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY)) {
    auto fn = read<IMAGE_RUNTIME_FUNCTION_ENTRY>(bytes_, directory.VirtualAddress + offset);
    if (fn.BeginAddress >= fn.EndAddress ||
        !contains(fn.BeginAddress, fn.EndAddress - fn.BeginAddress, ImageTarget::Code) ||
        (!functions_.empty() && fn.BeginAddress < functions_.back().begin))
      invalid("Invalid or unsorted AMD64 unwind function");
    functions_.push_back({fn.BeginAddress, fn.EndAddress});
  }
}

bool PatternImage::contains(uint32_t rva, size_t size, ImageTarget target) const {
  for (const auto &section : sections_) {
    if (rva < section.rva || rva - section.rva >= section.size || size > section.size - (rva - section.rva))
      continue;
    bool readable = (section.flags & IMAGE_SCN_MEM_READ) != 0;
    bool executable = (section.flags & IMAGE_SCN_MEM_EXECUTE) != 0;
    bool writable = (section.flags & IMAGE_SCN_MEM_WRITE) != 0;
    if (target == ImageTarget::Code)
      return readable && executable && !writable;
    if (target == ImageTarget::ReadOnlyData)
      return readable && !executable && !writable;
    return readable && !executable && writable;
  }
  return false;
}

uint32_t PatternImage::relative32(uint32_t operand_rva, int32_t addend) const {
  auto displacement = read<int32_t>(bytes_, operand_rva);
  auto target = int64_t(operand_rva) + 4 + displacement + addend;
  if (target < 0 || static_cast<uint64_t>(target) >= bytes_.size())
    invalid("Relative target is outside the image");
  return static_cast<uint32_t>(target);
}
ImageFunction PatternImage::containing_function(uint32_t rva) const {
  auto it = std::upper_bound(functions_.begin(), functions_.end(), rva,
                             [](uint32_t value, const auto &fn) { return value < fn.begin; });
  if (it == functions_.begin())
    invalid("Signature has no AMD64 unwind function");
  --it;
  if (rva >= it->end)
    invalid("Signature is outside its AMD64 unwind function");
  return *it;
}
int64_t PatternImage::signed_operand(uint32_t rva, unsigned width) const {
  if (width == 1)
    return read<int8_t>(bytes_, rva);
  if (width == 4)
    return read<int32_t>(bytes_, rva);
  invalid("Unsupported signature operand width");
}
uint64_t PatternImage::pointer(uint32_t rva) const {
  return read<uint64_t>(bytes_, rva);
}
std::vector<uint32_t> PatternImage::find_code(const BytePattern &pattern, size_t limit) const {
  std::vector<uint32_t> result;
  for (const auto &section : sections_) {
    if (!contains(section.rva, section.size, ImageTarget::Code))
      continue;
    for (auto offset : pattern.find(bytes_.subspan(section.rva, section.size), limit - result.size())) {
      result.push_back(section.rva + static_cast<uint32_t>(offset));
      if (result.size() == limit)
        return result;
    }
  }
  return result;
}
} // namespace catalyst
