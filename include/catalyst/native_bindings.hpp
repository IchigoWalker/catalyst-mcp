#pragma once
#include "common.hpp"
#include "pattern.hpp"
#include <array>

namespace catalyst {
enum class NativeSymbol : size_t {
  Tick,
  Present,
  Destroy,
  SerializeProject,
  RegistryInstance,
  JsonDestroy,
  StringDestroy,
  OperatorNew,
  FindView,
  OpenProject,
  RefreshApp,
  NewProject,
  CreateCollection,
  SaveProject,
  ImportTexture,
  ImportDrawable,
  ImportYpt,
  ExportYpt,
  BuildTimeline,
  SetTimelineMode,
  GetWindow,
  InitializeClipMap,
  ParseClipRegions,
  DestroyClipRegions,
  FileDialog,
  ShowError,
  RegistryHead,
  ClipVtable,
  SerializeNode,
  SerializeBiasLink,
  SerializeEvolution,
  PreviewModeHandler,
  DirtyVariable,
  PreviewElement,
  SetPreviewViewMode,
  Count
};
enum class SignatureKind { Match, Function, Relative32 };
struct NativeSignature {
  NativeSymbol symbol;
  std::string_view name, pattern;
  SignatureKind kind;
  uint16_t operand_offset{};
  int32_t addend{};
  ImageTarget target{ImageTarget::Code};
  NativeSymbol scope{NativeSymbol::Count};
};
struct LayoutGuard {
  std::string_view name, pattern;
  uint16_t operand_offset{};
  uint8_t operand_width{};
  int32_t expected{};
};
struct NativeBindings {
  uintptr_t base{};
  std::array<uint32_t, static_cast<size_t>(NativeSymbol::Count)> rvas{};
  bool compatible{};
  Json report;
  uintptr_t address(NativeSymbol symbol) const;
};
std::span<const NativeSignature> native_signatures();
std::span<const LayoutGuard> native_layout_guards();
NativeBindings resolve_native_bindings(const PatternImage &image);
} // namespace catalyst
