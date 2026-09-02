//===-- IRForTargetInternal.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CLANG_IRFORTARGETINTERNAL_H
#define LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CLANG_IRFORTARGETINTERNAL_H

#include <functional>
#include <map>

namespace llvm {
class Constant;
class Function;
class Value;
} // namespace llvm

namespace lldb_private {
class Stream;

namespace ir_for_target_detail {

class FunctionValueCache {
public:
  using Maker = std::function<llvm::Value *(llvm::Function *)>;

  FunctionValueCache(const Maker &maker);
  ~FunctionValueCache();

  llvm::Value *GetValue(llvm::Function *function);

private:
  const Maker m_maker;
  using FunctionValueMap = std::map<llvm::Function *, llvm::Value *>;
  FunctionValueMap m_values;
};

/// Operates on a constant which has just been replaced with a non-constant
/// value placed early in the function.
///
/// Replaces uses of \p old_constant with values produced by \p value_maker.
/// When a use is another constant expression, creates an equivalent
/// instruction before the entry insertion point and processes its uses
/// recursively.
///
/// \return
///     True on success; false otherwise.
bool UnfoldConstant(llvm::Constant *old_constant, llvm::Function *llvm_function,
                    FunctionValueCache &value_maker,
                    FunctionValueCache &entry_instruction_finder,
                    lldb_private::Stream &error_stream);

} // namespace ir_for_target_detail
} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_EXPRESSIONPARSER_CLANG_IRFORTARGETINTERNAL_H
