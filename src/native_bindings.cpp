#include "catalyst/native_bindings.hpp"
#include <chrono>
#include <set>

namespace catalyst {
namespace {
#include "native_signatures.inc"
} // namespace
std::span<const NativeSignature> native_signatures() {
  return signatures;
}
std::span<const LayoutGuard> native_layout_guards() {
  return layout_guards;
}
uintptr_t NativeBindings::address(NativeSymbol symbol) const {
  auto index = static_cast<size_t>(symbol);
  if (!compatible || index >= rvas.size() || !rvas[index])
    throw ToolError("Native signature bindings are unavailable");
  return base + rvas[index];
}

NativeBindings resolve_native_bindings(const PatternImage &image) {
  const auto started = std::chrono::steady_clock::now();
  NativeBindings result;
  result.base = image.base();
  result.report = {
      {"mode", "patterns"},       {"profile", "catalyst-msvc-x64-v1"}, {"scanner", "SSE2 masked anchors"},
      {"symbols", Json::array()}, {"layout_guards", Json::array()},    {"errors", Json::array()}};
  std::set<uint32_t> code_targets;
  for (const auto &signature : signatures) {
    Json diagnostic = {{"name", signature.name}};
    try {
      const BytePattern pattern(signature.pattern);
      std::vector<uint32_t> matches;
      if (signature.scope != NativeSymbol::Count) {
        auto scope = result.rvas[static_cast<size_t>(signature.scope)];
        if (!scope)
          throw ToolError("Signature scope was not resolved");
        auto function = image.containing_function(scope);
        for (auto offset : pattern.find(image.bytes().subspan(function.begin, function.end - function.begin)))
          matches.push_back(function.begin + static_cast<uint32_t>(offset));
      } else {
        matches = image.find_code(pattern);
      }
      if (matches.size() != 1)
        throw ToolError(matches.empty() ? "Signature was not found" : "Signature is ambiguous");
      auto match = matches[0];
      uint32_t target = match;
      if (signature.kind == SignatureKind::Function) {
        auto owner = image.containing_function(match);
        if (pattern.size() > owner.end - match)
          throw ToolError("Signature crosses a function boundary");
        target = owner.begin;
      } else if (signature.kind == SignatureKind::Relative32) {
        if (signature.operand_offset > pattern.size() || pattern.size() - signature.operand_offset < 4)
          throw ToolError("Signature relative operand is not bounded by its pattern");
        target = image.relative32(match + signature.operand_offset, signature.addend);
      }
      const auto extent = signature.target == ImageTarget::Code ? size_t{1} : size_t{8};
      if (!image.contains(target, extent, signature.target))
        throw ToolError("Resolved target has the wrong section protection");
      if (signature.target == ImageTarget::Code && !code_targets.insert(target).second)
        throw ToolError("Two native symbols resolved to the same code address");
      result.rvas[static_cast<size_t>(signature.symbol)] = target;
      diagnostic["rva"] = target;
      diagnostic["matched"] = true;
    } catch (const std::exception &error) {
      diagnostic["matched"] = false;
      diagnostic["error"] = error.what();
      result.report["errors"].push_back(std::string(signature.name) + ": " + error.what());
    }
    result.report["symbols"].push_back(std::move(diagnostic));
  }
  for (const auto &guard : layout_guards) {
    Json diagnostic = {{"name", guard.name}, {"expected", guard.expected}};
    try {
      const BytePattern pattern(guard.pattern);
      if ((guard.operand_width != 1 && guard.operand_width != 4) || guard.operand_offset > pattern.size() ||
          guard.operand_width > pattern.size() - guard.operand_offset)
        throw ToolError("Layout guard operand is not bounded by its pattern");
      auto matches = image.find_code(pattern);
      if (matches.size() != 1)
        throw ToolError(matches.empty() ? "Layout anchor was not found" : "Layout anchor is ambiguous");
      auto actual = image.signed_operand(matches[0] + guard.operand_offset, guard.operand_width);
      diagnostic["actual"] = actual;
      if (actual != guard.expected)
        throw ToolError("Native object layout changed");
      diagnostic["matched"] = true;
    } catch (const std::exception &error) {
      diagnostic["matched"] = false;
      diagnostic["error"] = error.what();
      result.report["errors"].push_back(std::string(guard.name) + ": " + error.what());
    }
    result.report["layout_guards"].push_back(std::move(diagnostic));
  }
  const auto table = result.rvas[static_cast<size_t>(NativeSymbol::ClipVtable)];
  if (table) {
    for (uint32_t offset : {0u, 8u}) {
      try {
        if (!image.contains(table, offset + 8, ImageTarget::ReadOnlyData))
          throw ToolError("Clip-region vtable is truncated");
        auto pointer = image.pointer(table + offset);
        if (pointer < image.base() || pointer - image.base() >= image.bytes().size() ||
            !image.contains(static_cast<uint32_t>(pointer - image.base()), 1, ImageTarget::Code))
          throw ToolError("Clip-region vtable does not point into executable code");
        if (offset == 0 &&
            pointer != image.base() + result.rvas[static_cast<size_t>(NativeSymbol::DestroyClipRegions)])
          throw ToolError("Clip-region destructor vtable slot changed");
      } catch (const std::exception &error) {
        result.report["errors"].push_back(error.what());
      }
    }
  }
  result.compatible = result.report["errors"].empty();
  result.report["compatible"] = result.compatible;
  result.report["symbols_resolved"] =
      std::count_if(result.rvas.begin(), result.rvas.end(), [](auto rva) { return rva != 0; });
  result.report["symbols_required"] = std::size(signatures);
  result.report["layout_checks_required"] = std::size(layout_guards);
  result.report["resolution_ms"] =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  return result;
}
} // namespace catalyst
