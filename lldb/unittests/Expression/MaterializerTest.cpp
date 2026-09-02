//===-- MaterializerTest.cpp ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Expression/Materializer.h"
#include "Plugins/Platform/Linux/PlatformLinux.h"
#include "Plugins/ScriptInterpreter/None/ScriptInterpreterNone.h"
#include "Plugins/TypeSystem/Clang/TypeSystemClang.h"
#include "TestingSupport/SubsystemRAII.h"
#include "TestingSupport/Symbol/ClangTestUtils.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/Value.h"
#include "lldb/Target/Process.h"
#include "lldb/ValueObject/ValueObjectConstResult.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <array>
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
    SetCanJIT(false);
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

  size_t GetDefaultWriteCount() const { return m_default_writes.size(); }

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

  size_t DoWriteMemory(lldb::addr_t address, const void *buf, size_t size,
                       Status &error) override {
    const uint8_t *bytes = static_cast<const uint8_t *>(buf);
    m_default_writes.emplace_back(address,
                                  std::vector<uint8_t>(bytes, bytes + size));
    error.Clear();
    return size;
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
  std::vector<std::pair<lldb::addr_t, std::vector<uint8_t>>> m_default_writes;
};

class MaterializerAddressSpaceTest : public ::testing::Test {
protected:
  static constexpr lldb::addr_t StorageAddress = 0x10000;
  static constexpr uint64_t AddressSpace = 5;

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

    m_listener_sp = Listener::MakeListener("materializer test");
    m_process_sp =
        std::make_shared<RecordingProcess>(m_target_sp, m_listener_sp);
    ASSERT_TRUE(m_process_sp);
    m_target_sp->SetProcessSP(m_process_sp);

    m_type_system_holder =
        std::make_unique<clang_utils::TypeSystemClangHolder>("test");
    m_int_type = m_type_system_holder->GetAST()->GetBasicType(
        lldb::BasicType::eBasicTypeInt);
    ASSERT_TRUE(m_int_type.IsValid());

    m_memory_map = std::make_unique<IRMemoryMap>(m_target_sp);
  }

  lldb::ValueObjectSP CreateAddressSpaceValue() {
    Value value;
    value.SetCompilerType(m_int_type);
    value.SetValueType(Value::ValueType::LoadAddress);
    value.GetScalar() = StorageAddress;
    value.SetAddressSpaceId(AddressSpace);
    return ValueObjectConstResult::Create(m_process_sp.get(), value,
                                          ConstString("value"));
  }

  lldb::ValueObjectSP CreateScalarValue(uint32_t value) {
    Scalar scalar(value);
    return ValueObjectConstResult::Create(m_process_sp.get(), m_int_type,
                                          scalar, ConstString("value"));
  }

  lldb::addr_t AllocateMaterializationStruct(Materializer &materializer) {
    auto address_or_error = m_memory_map->Malloc(
        materializer.GetStructByteSize(), materializer.GetStructAlignment(),
        lldb::ePermissionsReadable | lldb::ePermissionsWritable,
        IRMemoryMap::eAllocationPolicyHostOnly, /*zero_memory=*/false);
    if (!address_or_error) {
      ADD_FAILURE() << llvm::toString(address_or_error.takeError());
      return LLDB_INVALID_ADDRESS;
    }
    return *address_or_error;
  }

  lldb::addr_t ReadMaterializedPointer(lldb::addr_t struct_address,
                                       uint32_t offset) {
    lldb::addr_t result = LLDB_INVALID_ADDRESS;
    Status error;
    m_memory_map->ReadPointerFromMemory(&result, struct_address + offset,
                                        error);
    EXPECT_TRUE(error.Success()) << error.AsCString();
    return result;
  }

  void FreeMaterializationStruct(lldb::addr_t address) {
    Status error;
    m_memory_map->Free(address, error);
    EXPECT_TRUE(error.Success()) << error.AsCString();
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
  CompilerType m_int_type;
  std::unique_ptr<IRMemoryMap> m_memory_map;
};

TEST_F(MaterializerAddressSpaceTest,
       ExplicitAddressSpaceValueIsStagedWithoutWriteback) {
  const std::array<uint8_t, 4> original = {0x11, 0x22, 0x33, 0x44};
  m_process_sp->AddMemory(StorageAddress, AddressSpace,
                          {original[0], original[1], original[2], original[3]});
  lldb::ValueObjectSP value_sp = CreateAddressSpaceValue();
  ASSERT_TRUE(value_sp);
  ASSERT_TRUE(value_sp->GetError().Success())
      << value_sp->GetError().AsCString();

  Materializer materializer;
  Status error;
  uint32_t offset = materializer.AddValueObject(
      ConstString("value"), [&](ConstString, StackFrame *) { return value_sp; },
      error);
  ASSERT_TRUE(error.Success()) << error.AsCString();
  lldb::addr_t struct_address = AllocateMaterializationStruct(materializer);
  ASSERT_NE(struct_address, LLDB_INVALID_ADDRESS);

  lldb::StackFrameSP frame_sp;
  Materializer::DematerializerSP dematerializer =
      materializer.Materialize(frame_sp, *m_memory_map, struct_address, error);
  ASSERT_TRUE(error.Success()) << error.AsCString();
  ASSERT_TRUE(dematerializer);

  lldb::addr_t staged_address = ReadMaterializedPointer(struct_address, offset);
  ASSERT_NE(staged_address, LLDB_INVALID_ADDRESS);
  EXPECT_NE(staged_address, StorageAddress);

  std::array<uint8_t, 4> staged = {};
  m_memory_map->ReadMemory(staged.data(), staged_address, staged.size(), error);
  ASSERT_TRUE(error.Success()) << error.AsCString();
  EXPECT_EQ(staged, original);
  EXPECT_GE(
      m_process_sp->CountReads(StorageAddress, original.size(), AddressSpace),
      1u);
  EXPECT_EQ(
      m_process_sp->CountReads(StorageAddress, original.size(), std::nullopt),
      0u);

  dematerializer->Dematerialize(error, LLDB_INVALID_ADDRESS,
                                LLDB_INVALID_ADDRESS);
  EXPECT_TRUE(error.Success()) << error.AsCString();
  EXPECT_EQ(m_process_sp->GetDefaultWriteCount(), 0u);

  size_t allocation_size = 0;
  EXPECT_FALSE(m_memory_map->GetAllocSize(staged_address, allocation_size));
  FreeMaterializationStruct(struct_address);
}

TEST_F(MaterializerAddressSpaceTest,
       ChangedExplicitAddressSpaceValueIsNotWrittenToDefaultMemory) {
  m_process_sp->AddMemory(StorageAddress, AddressSpace,
                          {0x11, 0x22, 0x33, 0x44});
  size_t provider_calls = 0;

  Materializer materializer;
  Status error;
  uint32_t offset = materializer.AddValueObject(
      ConstString("value"),
      [&](ConstString, StackFrame *) {
        ++provider_calls;
        return CreateAddressSpaceValue();
      },
      error);
  ASSERT_TRUE(error.Success()) << error.AsCString();
  lldb::addr_t struct_address = AllocateMaterializationStruct(materializer);
  ASSERT_NE(struct_address, LLDB_INVALID_ADDRESS);

  lldb::StackFrameSP frame_sp;
  Materializer::DematerializerSP dematerializer =
      materializer.Materialize(frame_sp, *m_memory_map, struct_address, error);
  ASSERT_TRUE(error.Success()) << error.AsCString();
  ASSERT_TRUE(dematerializer);

  lldb::addr_t staged_address = ReadMaterializedPointer(struct_address, offset);
  const std::array<uint8_t, 4> changed = {0x55, 0x66, 0x77, 0x88};
  m_memory_map->WriteMemory(staged_address, changed.data(), changed.size(),
                            error);
  ASSERT_TRUE(error.Success()) << error.AsCString();

  dematerializer->Dematerialize(error, LLDB_INVALID_ADDRESS,
                                LLDB_INVALID_ADDRESS);
  ASSERT_TRUE(error.Fail());
  EXPECT_TRUE(llvm::StringRef(error.AsCString())
                  .contains("can't write the new contents of value back"))
      << error.AsCString();
  EXPECT_EQ(provider_calls, 2u);
  EXPECT_EQ(m_process_sp->GetDefaultWriteCount(), 0u);

  size_t allocation_size = 0;
  EXPECT_FALSE(m_memory_map->GetAllocSize(staged_address, allocation_size));
  FreeMaterializationStruct(struct_address);
}

TEST_F(MaterializerAddressSpaceTest, ChangedScalarTemporaryWritesBack) {
  lldb::ValueObjectSP value_sp = CreateScalarValue(7);
  ASSERT_TRUE(value_sp);
  ASSERT_TRUE(value_sp->GetError().Success())
      << value_sp->GetError().AsCString();

  Materializer materializer;
  Status error;
  uint32_t offset = materializer.AddValueObject(
      ConstString("value"), [&](ConstString, StackFrame *) { return value_sp; },
      error);
  ASSERT_TRUE(error.Success()) << error.AsCString();
  lldb::addr_t struct_address = AllocateMaterializationStruct(materializer);
  ASSERT_NE(struct_address, LLDB_INVALID_ADDRESS);

  lldb::StackFrameSP frame_sp;
  Materializer::DematerializerSP dematerializer =
      materializer.Materialize(frame_sp, *m_memory_map, struct_address, error);
  ASSERT_TRUE(error.Success()) << error.AsCString();
  ASSERT_TRUE(dematerializer);
  lldb::addr_t staged_address = ReadMaterializedPointer(struct_address, offset);

  const std::array<uint8_t, 4> changed = {0x2a, 0x00, 0x00, 0x00};
  m_memory_map->WriteMemory(staged_address, changed.data(), changed.size(),
                            error);
  ASSERT_TRUE(error.Success()) << error.AsCString();

  dematerializer->Dematerialize(error, LLDB_INVALID_ADDRESS,
                                LLDB_INVALID_ADDRESS);
  EXPECT_TRUE(error.Success()) << error.AsCString();
  EXPECT_EQ(m_process_sp->GetDefaultWriteCount(), 0u);
  size_t allocation_size = 0;
  EXPECT_FALSE(m_memory_map->GetAllocSize(staged_address, allocation_size));

  bool success = false;
  EXPECT_EQ(value_sp->GetValueAsUnsigned(0, &success), 42u);
  EXPECT_TRUE(success);
  FreeMaterializationStruct(struct_address);
}

} // namespace
