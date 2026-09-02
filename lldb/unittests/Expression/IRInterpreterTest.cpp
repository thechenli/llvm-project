//===-- IRInterpreterTest.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Expression/IRInterpreter.h"
#include "Plugins/Platform/Linux/PlatformLinux.h"
#include "TestingSupport/SubsystemRAII.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Expression/IRExecutionUnit.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Platform.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/TargetList.h"
#include "lldb/Utility/ArchSpec.h"
#include "lldb/Utility/Status.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SourceMgr.h"
#include "gtest/gtest.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace llvm;

namespace {
class IRInterpreterTest : public testing::Test {
protected:
  std::unique_ptr<Module> ParseModule(StringRef assembly) {
    SMDiagnostic error;
    std::unique_ptr<Module> module =
        parseAssemblyString(assembly, error, m_context);
    if (!module)
      ADD_FAILURE() << error.getMessage().str();
    return module;
  }

  LLVMContext m_context;
};

class IRInterpreterExecutionTest : public IRInterpreterTest {
protected:
  lldb_private::SubsystemRAII<lldb_private::FileSystem, lldb_private::HostInfo,
                              lldb_private::platform_linux::PlatformLinux>
      m_subsystems;
};
} // namespace

TEST_F(IRInterpreterTest, AcceptsAllocaAddressSpaceCastToGeneric) {
  std::unique_ptr<Module> module = ParseModule(R"(
    target datalayout = "e-p:64:64-p1:64:64-p5:32:32-A5"

    define void @expr() {
    entry:
      %slot = alloca i32, align 4, addrspace(5)
      %flat = addrspacecast ptr addrspace(5) %slot to ptr
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module));

  Function *function = module->getFunction("expr");
  ASSERT_NE(function, nullptr);
  auto *alloca = dyn_cast<AllocaInst>(&function->getEntryBlock().front());
  ASSERT_NE(alloca, nullptr);
  auto *addrspace_cast = dyn_cast<AddrSpaceCastInst>(alloca->getNextNode());
  ASSERT_NE(addrspace_cast, nullptr);
  EXPECT_EQ(module->getDataLayout().getAllocaAddrSpace(), 5u);
  EXPECT_EQ(alloca->getAddressSpace(), 5u);
  EXPECT_EQ(addrspace_cast->getOperand(0), alloca);
  EXPECT_EQ(addrspace_cast->getDestAddressSpace(), 0u);

  lldb_private::Status error;
  EXPECT_TRUE(IRInterpreter::CanInterpret(*module, *function, error,
                                          /*support_function_calls=*/false))
      << error.AsCString();
  EXPECT_TRUE(error.Success());
}

TEST_F(IRInterpreterTest, RejectsLoadedAddressSpaceCastToGeneric) {
  std::unique_ptr<Module> module = ParseModule(R"(
    target datalayout = "e-p:64:64-p1:64:64-p5:32:32-A5"

    define void @expr(ptr %slot) {
    entry:
      %device = load ptr addrspace(1), ptr %slot
      %flat = addrspacecast ptr addrspace(1) %device to ptr
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module));

  Function *function = module->getFunction("expr");
  ASSERT_NE(function, nullptr);
  auto *load = dyn_cast<LoadInst>(&function->getEntryBlock().front());
  ASSERT_NE(load, nullptr);
  auto *addrspace_cast = dyn_cast<AddrSpaceCastInst>(load->getNextNode());
  ASSERT_NE(addrspace_cast, nullptr);
  EXPECT_EQ(addrspace_cast->getOperand(0), load);
  EXPECT_EQ(addrspace_cast->getSrcAddressSpace(), 1u);
  EXPECT_EQ(addrspace_cast->getDestAddressSpace(), 0u);

  lldb_private::Status error;
  EXPECT_FALSE(IRInterpreter::CanInterpret(*module, *function, error,
                                           /*support_function_calls=*/false));
  EXPECT_TRUE(error.Fail());
}

TEST_F(IRInterpreterTest, RejectsAllocaAddressSpaceCastToNonzero) {
  std::unique_ptr<Module> module = ParseModule(R"(
    target datalayout = "e-p:64:64-p1:64:64-p5:32:32-A5"

    define void @expr() {
    entry:
      %slot = alloca i32, align 4, addrspace(5)
      %device = addrspacecast ptr addrspace(5) %slot to ptr addrspace(1)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  ASSERT_FALSE(verifyModule(*module));

  Function *function = module->getFunction("expr");
  ASSERT_NE(function, nullptr);
  auto *alloca = dyn_cast<AllocaInst>(&function->getEntryBlock().front());
  ASSERT_NE(alloca, nullptr);
  auto *addrspace_cast = dyn_cast<AddrSpaceCastInst>(alloca->getNextNode());
  ASSERT_NE(addrspace_cast, nullptr);
  EXPECT_EQ(module->getDataLayout().getAllocaAddrSpace(), 5u);
  EXPECT_EQ(alloca->getAddressSpace(), 5u);
  EXPECT_EQ(addrspace_cast->getOperand(0), alloca);
  EXPECT_EQ(addrspace_cast->getDestAddressSpace(), 1u);

  lldb_private::Status error;
  EXPECT_FALSE(IRInterpreter::CanInterpret(*module, *function, error,
                                           /*support_function_calls=*/false));
  EXPECT_TRUE(error.Fail());
}

TEST_F(IRInterpreterExecutionTest,
       AllocaAddressSpaceCastPreservesFullHostPointer) {
  auto context = std::make_unique<LLVMContext>();
  SMDiagnostic parse_error;
  std::unique_ptr<Module> module = parseAssemblyString(R"(
    target datalayout = "e-p:64:64-p5:32:32-A5"

    define void @expr(ptr %result) {
    entry:
      %slot = alloca i32, align 4, addrspace(5)
      %flat = addrspacecast ptr addrspace(5) %slot to ptr
      %address = ptrtoint ptr %flat to i64
      store i64 %address, ptr %result, align 8
      ret void
    }
  )",
                                                       parse_error, *context);
  ASSERT_NE(module, nullptr) << parse_error.getMessage().str();
  ASSERT_FALSE(verifyModule(*module));

  Function *function = module->getFunction("expr");
  ASSERT_NE(function, nullptr);
  EXPECT_EQ(module->getDataLayout().getPointerSize(0), 8u);
  EXPECT_EQ(module->getDataLayout().getPointerSize(5), 4u);
  EXPECT_EQ(module->getDataLayout().getAllocaAddrSpace(), 5u);

  lldb_private::ArchSpec arch("x86_64-unknown-linux-gnu");
  lldb::PlatformSP platform_sp =
      lldb_private::platform_linux::PlatformLinux::CreateInstance(true, &arch);
  ASSERT_NE(platform_sp, nullptr);
  lldb_private::Platform::SetHostPlatform(platform_sp);

  lldb::DebuggerSP debugger_sp = lldb_private::Debugger::CreateInstance();
  ASSERT_NE(debugger_sp, nullptr);
  lldb::TargetSP target_sp;
  lldb_private::Status target_error = debugger_sp->GetTargetList().CreateTarget(
      *debugger_sp, "", arch, lldb_private::eLoadDependentsNo, platform_sp,
      target_sp);
  ASSERT_TRUE(target_error.Success()) << target_error.AsCString();
  ASSERT_NE(target_sp, nullptr);
  ASSERT_EQ(target_sp->GetArchitecture().GetAddressByteSize(), 8u);

  lldb_private::ConstString function_name("expr");
  lldb_private::SymbolContext symbol_context;
  std::vector<std::string> cpu_features;
  lldb_private::IRExecutionUnit execution_unit(
      context, module, function_name, target_sp, symbol_context, cpu_features);

  constexpr uint32_t permissions =
      lldb::ePermissionsReadable | lldb::ePermissionsWritable;
  auto result_address = execution_unit.Malloc(
      sizeof(lldb::addr_t), alignof(lldb::addr_t), permissions,
      lldb_private::IRMemoryMap::eAllocationPolicyHostOnly,
      /*zero_memory=*/true);
  if (!result_address)
    FAIL() << llvm::toString(result_address.takeError());

  constexpr size_t stack_size = 128;
  auto stack_address = execution_unit.Malloc(
      stack_size, /*alignment=*/16, permissions,
      lldb_private::IRMemoryMap::eAllocationPolicyHostOnly,
      /*zero_memory=*/true);
  if (!stack_address)
    FAIL() << llvm::toString(stack_address.takeError());
  const lldb::addr_t stack_bottom = *stack_address;
  const lldb::addr_t stack_top = stack_bottom + stack_size;

  lldb_private::Status can_interpret_error;
  ASSERT_TRUE(IRInterpreter::CanInterpret(*execution_unit.GetModule(),
                                          *function, can_interpret_error,
                                          /*support_function_calls=*/false))
      << can_interpret_error.AsCString();

  std::array<lldb::addr_t, 1> args = {*result_address};
  lldb_private::ExecutionContext exe_ctx(target_sp, /*get_process=*/false);
  lldb_private::Status interpret_error;
  ASSERT_TRUE(IRInterpreter::Interpret(
      *execution_unit.GetModule(), *function, args, execution_unit,
      interpret_error, stack_bottom, stack_top, exe_ctx,
      lldb_private::Timeout<std::micro>(std::nullopt)))
      << interpret_error.AsCString();

  lldb::addr_t interpreted_address = LLDB_INVALID_ADDRESS;
  lldb_private::Status read_error;
  execution_unit.ReadPointerFromMemory(&interpreted_address, *result_address,
                                       read_error);
  ASSERT_TRUE(read_error.Success()) << read_error.AsCString();
  EXPECT_GE(interpreted_address, stack_bottom);
  EXPECT_LT(interpreted_address, stack_top);
  EXPECT_NE(interpreted_address >> 32, 0u);

  // The alloca object begins at the address produced by the cast. The stack
  // allocation was zeroed, so any nonzero byte here means the alloca's pointer
  // backing was only four bytes wide and its eight-byte write overlapped the
  // object.
  std::array<uint8_t, 4> object_bytes;
  read_error.Clear();
  execution_unit.ReadMemory(object_bytes.data(), interpreted_address,
                            object_bytes.size(), read_error);
  ASSERT_TRUE(read_error.Success()) << read_error.AsCString();
  EXPECT_EQ(object_bytes, (std::array<uint8_t, 4>{}));

  lldb_private::Debugger::Destroy(debugger_sp);
}
