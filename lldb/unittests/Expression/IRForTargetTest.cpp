//===-- IRForTargetTest.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Plugins/ExpressionParser/Clang/IRForTargetInternal.h"
#include "lldb/Utility/StreamString.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

using namespace llvm;

class IRForTargetTest : public testing::Test {
protected:
  std::unique_ptr<Module> ParseModule(StringRef assembly) {
    SMDiagnostic error;
    std::unique_ptr<Module> module =
        parseAssemblyString(assembly, error, m_context);
    if (!module)
      ADD_FAILURE() << error.getMessage().str();
    return module;
  }

  static bool UnfoldConstant(Constant *old_constant, Function *function,
                             Value *replacement, Instruction *entry_instruction,
                             lldb_private::Stream &error_stream) {
    lldb_private::ir_for_target_detail::FunctionValueCache value_maker(
        [replacement](Function *) { return replacement; });
    lldb_private::ir_for_target_detail::FunctionValueCache
        entry_instruction_finder(
            [entry_instruction](Function *) { return entry_instruction; });
    return lldb_private::ir_for_target_detail::UnfoldConstant(
        old_constant, function, value_maker, entry_instruction_finder,
        error_stream);
  }

  static void ExpectValidModule(Module &module) {
    std::string verification_error;
    raw_string_ostream error_stream(verification_error);
    EXPECT_FALSE(verifyModule(module, &error_stream)) << error_stream.str();
  }

  LLVMContext m_context;
};

TEST_F(IRForTargetTest, UnfoldAddrSpaceCastAfterMaterialization) {
  std::unique_ptr<Module> module = ParseModule(R"(
    target datalayout = "e-p:64:64-p1:64:64"

    @gpu_ptr = external addrspace(1) global ptr addrspace(1)

    define void @expr(ptr %materialized_slot) {
    entry:
      %p = load ptr addrspace(1), ptr addrspacecast (ptr addrspace(1) @gpu_ptr to ptr)
      %v = load i32, ptr addrspace(1) %p
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *function = module->getFunction("expr");
  GlobalVariable *gpu_ptr = module->getGlobalVariable("gpu_ptr");
  ASSERT_NE(function, nullptr);
  ASSERT_NE(gpu_ptr, nullptr);

  auto *pointer_load = dyn_cast<LoadInst>(&function->getEntryBlock().front());
  ASSERT_NE(pointer_load, nullptr);
  Value *materialized_slot = function->getArg(0);
  ASSERT_TRUE(isa<ConstantExpr>(pointer_load->getPointerOperand()));

  lldb_private::StreamString errors;
  ASSERT_TRUE(UnfoldConstant(gpu_ptr, function, materialized_slot, pointer_load,
                             errors));
  EXPECT_TRUE(errors.GetString().empty()) << errors.GetString().str();

  EXPECT_EQ(pointer_load->getPointerOperand(), materialized_slot);
  EXPECT_FALSE(isa<ConstantExpr>(pointer_load->getPointerOperand()));
  EXPECT_TRUE(gpu_ptr->use_empty());
  EXPECT_FALSE(any_of(instructions(*function), [](Instruction &instruction) {
    return isa<AddrSpaceCastInst>(instruction);
  }));
  EXPECT_EQ(pointer_load->getType()->getPointerAddressSpace(), 1u);

  auto *value_load = dyn_cast<LoadInst>(pointer_load->getNextNode());
  ASSERT_NE(value_load, nullptr);
  EXPECT_EQ(value_load->getPointerAddressSpace(), 1u);
  ExpectValidModule(*module);
}

TEST_F(IRForTargetTest, PreserveRequiredAddrSpaceCast) {
  std::unique_ptr<Module> module = ParseModule(R"(
    target datalayout = "e-p:64:64-p1:64:64"

    @gpu_ptr = external addrspace(1) global ptr addrspace(1)

    define void @expr(ptr addrspace(1) %materialized_slot) {
    entry:
      %p = load ptr addrspace(1), ptr addrspacecast (ptr addrspace(1) @gpu_ptr to ptr)
      %v = load i32, ptr addrspace(1) %p
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *function = module->getFunction("expr");
  GlobalVariable *gpu_ptr = module->getGlobalVariable("gpu_ptr");
  ASSERT_NE(function, nullptr);
  ASSERT_NE(gpu_ptr, nullptr);

  auto *pointer_load = dyn_cast<LoadInst>(&function->getEntryBlock().front());
  ASSERT_NE(pointer_load, nullptr);
  Value *materialized_slot = function->getArg(0);
  ASSERT_TRUE(isa<ConstantExpr>(pointer_load->getPointerOperand()));

  lldb_private::StreamString errors;
  ASSERT_TRUE(UnfoldConstant(gpu_ptr, function, materialized_slot, pointer_load,
                             errors));
  EXPECT_TRUE(errors.GetString().empty()) << errors.GetString().str();

  auto *addrspace_cast =
      dyn_cast<AddrSpaceCastInst>(pointer_load->getPointerOperand());
  ASSERT_NE(addrspace_cast, nullptr);
  EXPECT_EQ(addrspace_cast->getOperand(0), materialized_slot);
  EXPECT_EQ(addrspace_cast->getSrcAddressSpace(), 1u);
  EXPECT_EQ(addrspace_cast->getDestAddressSpace(), 0u);
  EXPECT_EQ(addrspace_cast->getNextNode(), pointer_load);
  EXPECT_TRUE(gpu_ptr->use_empty());
  EXPECT_EQ(pointer_load->getType()->getPointerAddressSpace(), 1u);

  auto *value_load = dyn_cast<LoadInst>(pointer_load->getNextNode());
  ASSERT_NE(value_load, nullptr);
  EXPECT_EQ(value_load->getPointerAddressSpace(), 1u);
  ExpectValidModule(*module);
}
