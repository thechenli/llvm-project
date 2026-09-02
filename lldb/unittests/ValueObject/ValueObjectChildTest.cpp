//===-- ValueObjectChildTest.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Plugins/Platform/Linux/PlatformLinux.h"
#include "Plugins/ScriptInterpreter/None/ScriptInterpreterNone.h"
#include "Plugins/TypeSystem/Clang/TypeSystemClang.h"
#include "TestingSupport/SubsystemRAII.h"
#include "TestingSupport/Symbol/ClangTestUtils.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/Value.h"
#include "lldb/Target/Process.h"
#include "lldb/ValueObject/ValueObject.h"
#include "lldb/ValueObject/ValueObjectConstResult.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

using namespace lldb;
using namespace lldb_private;

namespace {

class RecordingProcess : public Process {
public:
  struct Read {
    lldb::addr_t address;
    size_t size;
    std::optional<uint64_t> address_space;
  };

  RecordingProcess(lldb::TargetSP target_sp, lldb::ListenerSP listener_sp)
      : Process(target_sp, listener_sp) {
    m_public_state.SetValue(lldb::eStateRunning);
    m_private_state.SetValue(lldb::eStateRunning);
    m_address_spaces.push_back({"AS-5", 5, true});
  }

  void AddMemory(lldb::addr_t address, std::optional<uint64_t> address_space,
                 std::initializer_list<uint8_t> bytes) {
    std::vector<uint8_t> data(bytes);
    m_memory.emplace(Key{address, data.size(), address_space}, std::move(data));
  }

  size_t CountReads(lldb::addr_t address, size_t size,
                    std::optional<uint64_t> address_space) const {
    return std::count_if(m_reads.begin(), m_reads.end(), [&](const Read &read) {
      return read.address == address && read.size == size &&
             read.address_space == address_space;
    });
  }

  llvm::StringRef GetPluginName() override { return "recording process"; }

  bool CanDebug(lldb::TargetSP, bool) override { return false; }

  Status DoDestroy() override { return {}; }

  void RefreshStateAfterStop() override {}

  bool DoUpdateThreadList(ThreadList &, ThreadList &) override { return false; }

  size_t DoReadMemory(lldb::addr_t address, void *buf, size_t size,
                      Status &error) override {
    return ReadMemoryImpl(address, std::nullopt, buf, size, error);
  }

  size_t DoReadMemory(const AddressSpec &address, const AddressSpaceInfo &,
                      void *buf, size_t size, Status &error) override {
    return ReadMemoryImpl(address.GetValue(), address.GetSpaceId(), buf, size,
                          error);
  }

  size_t ReadMemory(lldb::addr_t address, void *buf, size_t size,
                    Status &error) override {
    return ReadMemoryImpl(address, std::nullopt, buf, size, error);
  }

private:
  using Key = std::tuple<lldb::addr_t, size_t, std::optional<uint64_t>>;

  size_t ReadMemoryImpl(lldb::addr_t address,
                        std::optional<uint64_t> address_space, void *buf,
                        size_t size, Status &error) {
    m_reads.push_back({address, size, address_space});
    auto iter = m_memory.find(Key{address, size, address_space});
    if (iter == m_memory.end()) {
      error = Status::FromErrorString("unexpected memory read");
      return 0;
    }

    std::memcpy(buf, iter->second.data(), size);
    error.Clear();
    return size;
  }

  std::map<Key, std::vector<uint8_t>> m_memory;
  std::vector<Read> m_reads;
};

class ValueObjectChildAddressSpaceTest : public ::testing::Test {
protected:
  void SetUp() override {
    ArchSpec arch("x86_64-pc-linux");
    Platform::SetHostPlatform(
        platform_linux::PlatformLinux::CreateInstance(true, &arch));
    m_debugger_sp = Debugger::CreateInstance();
    ASSERT_TRUE(m_debugger_sp);
    m_debugger_sp->GetTargetList().CreateTarget(*m_debugger_sp, "", arch,
                                                eLoadDependentsNo,
                                                m_platform_sp, m_target_sp);
    ASSERT_TRUE(m_target_sp);
    ASSERT_TRUE(m_target_sp->GetArchitecture().IsValid());
    ASSERT_TRUE(m_platform_sp);

    m_listener_sp = Listener::MakeListener("dummy");
    m_process_sp =
        std::make_shared<RecordingProcess>(m_target_sp, m_listener_sp);
    ASSERT_TRUE(m_process_sp);

    m_type_system_holder =
        std::make_unique<clang_utils::TypeSystemClangHolder>("test");
    m_type_system = m_type_system_holder->GetAST();
  }

  lldb::ValueObjectSP CreateValueObject(const CompilerType &type,
                                        lldb::addr_t address) {
    Value value;
    value.SetCompilerType(type);
    value.SetValueType(Value::ValueType::LoadAddress);
    value.GetScalar() = address;
    value.SetAddressSpaceId(5);

    lldb::ValueObjectSP value_sp = ValueObjectConstResult::Create(
        m_process_sp.get(), value, ConstString("value"));
    value_sp->SetAddressTypeOfChildren(eAddressTypeLoad);
    return value_sp;
  }

  SubsystemRAII<FileSystem, HostInfo, platform_linux::PlatformLinux,
                ScriptInterpreterNone>
      m_subsystems;
  lldb::DebuggerSP m_debugger_sp;
  lldb::PlatformSP m_platform_sp;
  lldb::TargetSP m_target_sp;
  lldb::ListenerSP m_listener_sp;
  std::shared_ptr<RecordingProcess> m_process_sp;
  std::unique_ptr<clang_utils::TypeSystemClangHolder> m_type_system_holder;
  TypeSystemClang *m_type_system = nullptr;
};

TEST_F(ValueObjectChildAddressSpaceTest,
       PointerPointeeDoesNotInheritStorageAddressSpace) {
  constexpr lldb::addr_t pointer_storage = 0x100;
  constexpr lldb::addr_t pointee = 0x1000;
  constexpr uint64_t address_space = 5;

  m_process_sp->AddMemory(pointer_storage, address_space,
                          {0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  m_process_sp->AddMemory(pointee, std::nullopt, {0x2a, 0x00, 0x00, 0x00});

  CompilerType int_type =
      m_type_system->GetBasicType(lldb::BasicType::eBasicTypeInt);
  lldb::ValueObjectSP pointer_sp =
      CreateValueObject(int_type.GetPointerType(), pointer_storage);
  ASSERT_TRUE(pointer_sp);
  ASSERT_TRUE(pointer_sp->GetError().Success())
      << pointer_sp->GetError().AsCString();

  lldb::ValueObjectSP pointee_sp = pointer_sp->GetSyntheticArrayMember(0, true);
  ASSERT_TRUE(pointee_sp);
  ASSERT_TRUE(pointee_sp->GetError().Success())
      << pointee_sp->GetError().AsCString();

  EXPECT_FALSE(pointee_sp->GetValue().GetAddressSpaceId().has_value());
  bool success = false;
  EXPECT_EQ(pointee_sp->GetValueAsUnsigned(0, &success), 42u);
  EXPECT_TRUE(success);

  EXPECT_EQ(m_process_sp->CountReads(pointer_storage, 8, address_space), 1u);
  EXPECT_GE(m_process_sp->CountReads(pointee, 4, std::nullopt), 1u);
  EXPECT_EQ(m_process_sp->CountReads(pointee, 4, address_space), 0u);
}

TEST_F(ValueObjectChildAddressSpaceTest,
       ArrayElementInheritsStorageAddressSpace) {
  constexpr lldb::addr_t array_storage = 0x200;
  constexpr uint64_t address_space = 5;

  m_process_sp->AddMemory(array_storage, address_space,
                          {0x0b, 0x00, 0x00, 0x00, 0x16, 0x00, 0x00, 0x00});
  m_process_sp->AddMemory(array_storage + 4, address_space,
                          {0x16, 0x00, 0x00, 0x00});

  CompilerType int_type =
      m_type_system->GetBasicType(lldb::BasicType::eBasicTypeInt);
  lldb::ValueObjectSP array_sp =
      CreateValueObject(int_type.GetArrayType(2), array_storage);
  ASSERT_TRUE(array_sp);
  ASSERT_TRUE(array_sp->GetError().Success())
      << array_sp->GetError().AsCString();

  lldb::ValueObjectSP element_sp = array_sp->GetChildAtIndex(1, true);
  ASSERT_TRUE(element_sp);
  ASSERT_TRUE(element_sp->GetError().Success())
      << element_sp->GetError().AsCString();

  ASSERT_TRUE(element_sp->GetValue().GetAddressSpaceId().has_value());
  EXPECT_EQ(*element_sp->GetValue().GetAddressSpaceId(), address_space);
  bool success = false;
  EXPECT_EQ(element_sp->GetValueAsUnsigned(0, &success), 22u);
  EXPECT_TRUE(success);

  EXPECT_EQ(m_process_sp->CountReads(array_storage, 8, address_space), 1u);
  EXPECT_GE(m_process_sp->CountReads(array_storage + 4, 4, address_space), 1u);
  EXPECT_EQ(m_process_sp->CountReads(array_storage + 4, 4, std::nullopt), 0u);
}

} // namespace
