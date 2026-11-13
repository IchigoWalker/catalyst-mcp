#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace catalyst {
// A compiled IDA-style byte pattern. '?' and nibble wildcards are supported.
class BytePattern {
public:
  explicit BytePattern(std::string_view text);
  std::vector<size_t> find(std::span<const uint8_t> data, size_t limit = 2, bool simd = true) const;
  size_t size() const {
    return bytes_.size();
  }

private:
  bool matches(const uint8_t *data) const;
  std::vector<uint8_t> bytes_, masks_;
  size_t first_{}, last_{};
};

enum class ImageTarget { Code, ReadOnlyData, WritableData };
struct ImageSection {
  std::string name;
  uint32_t rva{}, size{}, flags{};
};
struct ImageFunction {
  uint32_t begin{}, end{};
};

// Parses a mapped AMD64 PE image, not an on-disk file. Every read is bounded.
class PatternImage {
public:
  explicit PatternImage(std::span<const uint8_t> image, uintptr_t virtual_base = 0);
  std::span<const uint8_t> bytes() const {
    return bytes_;
  }
  const std::vector<ImageSection> &sections() const {
    return sections_;
  }
  const std::vector<ImageFunction> &functions() const {
    return functions_;
  }
  uintptr_t base() const {
    return base_;
  }
  bool contains(uint32_t rva, size_t size, ImageTarget target) const;
  uint32_t relative32(uint32_t operand_rva, int32_t addend = 0) const;
  ImageFunction containing_function(uint32_t rva) const;
  int64_t signed_operand(uint32_t rva, unsigned width) const;
  uint64_t pointer(uint32_t rva) const;
  std::vector<uint32_t> find_code(const BytePattern &pattern, size_t limit = 2) const;

private:
  std::span<const uint8_t> bytes_;
  uintptr_t base_{};
  std::vector<ImageSection> sections_;
  std::vector<ImageFunction> functions_;
};
} // namespace catalyst
