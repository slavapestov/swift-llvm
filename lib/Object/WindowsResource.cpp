//===-- WindowsResource.cpp -------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the .res file class.
//
//===----------------------------------------------------------------------===//

#include "llvm/Object/WindowsResource.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/MathExtras.h"
#include <ctime>
#include <queue>
#include <sstream>
#include <system_error>

namespace llvm {
namespace object {

#define RETURN_IF_ERROR(X)                                                     \
  if (auto EC = X)                                                             \
    return EC;

const uint32_t MIN_HEADER_SIZE = 7 * sizeof(uint32_t) + 2 * sizeof(uint16_t);

static const size_t ResourceMagicSize = 16;

static const size_t NullEntrySize = 16;

uint32_t WindowsResourceParser::TreeNode::StringCount = 0;
uint32_t WindowsResourceParser::TreeNode::DataCount = 0;

WindowsResource::WindowsResource(MemoryBufferRef Source)
    : Binary(Binary::ID_WinRes, Source) {
  size_t LeadingSize = ResourceMagicSize + NullEntrySize;
  BBS = BinaryByteStream(Data.getBuffer().drop_front(LeadingSize),
                         support::little);
}

Expected<std::unique_ptr<WindowsResource>>
WindowsResource::createWindowsResource(MemoryBufferRef Source) {
  if (Source.getBufferSize() < ResourceMagicSize + NullEntrySize)
    return make_error<GenericBinaryError>(
        "File too small to be a resource file",
        object_error::invalid_file_type);
  std::unique_ptr<WindowsResource> Ret(new WindowsResource(Source));
  return std::move(Ret);
}

Expected<ResourceEntryRef> WindowsResource::getHeadEntry() {
  Error Err = Error::success();
  auto Ref = ResourceEntryRef(BinaryStreamRef(BBS), this, Err);
  if (Err)
    return std::move(Err);
  return Ref;
}

ResourceEntryRef::ResourceEntryRef(BinaryStreamRef Ref,
                                   const WindowsResource *Owner, Error &Err)
    : Reader(Ref), OwningRes(Owner) {
  if (loadNext())
    Err = make_error<GenericBinaryError>("Could not read first entry.",
                                         object_error::unexpected_eof);
}

Error ResourceEntryRef::moveNext(bool &End) {
  // Reached end of all the entries.
  if (Reader.bytesRemaining() == 0) {
    End = true;
    return Error::success();
  }
  RETURN_IF_ERROR(loadNext());

  return Error::success();
}

static Error readStringOrId(BinaryStreamReader &Reader, uint16_t &ID,
                            ArrayRef<UTF16> &Str, bool &IsString) {
  uint16_t IDFlag;
  RETURN_IF_ERROR(Reader.readInteger(IDFlag));
  IsString = IDFlag != 0xffff;

  if (IsString) {
    Reader.setOffset(
        Reader.getOffset() -
        sizeof(uint16_t)); // Re-read the bytes which we used to check the flag.
    RETURN_IF_ERROR(Reader.readWideString(Str));
  } else
    RETURN_IF_ERROR(Reader.readInteger(ID));

  return Error::success();
}

Error ResourceEntryRef::loadNext() {
  uint32_t DataSize;
  RETURN_IF_ERROR(Reader.readInteger(DataSize));
  uint32_t HeaderSize;
  RETURN_IF_ERROR(Reader.readInteger(HeaderSize));

  if (HeaderSize < MIN_HEADER_SIZE)
    return make_error<GenericBinaryError>("Header size is too small.",
                                          object_error::parse_failed);

  RETURN_IF_ERROR(readStringOrId(Reader, TypeID, Type, IsStringType));

  RETURN_IF_ERROR(readStringOrId(Reader, NameID, Name, IsStringName));

  RETURN_IF_ERROR(Reader.padToAlignment(sizeof(uint32_t)));

  RETURN_IF_ERROR(Reader.readObject(Suffix));

  RETURN_IF_ERROR(Reader.readArray(Data, DataSize));

  RETURN_IF_ERROR(Reader.padToAlignment(sizeof(uint32_t)));

  return Error::success();
}

WindowsResourceParser::WindowsResourceParser() : Root(false) {}

Error WindowsResourceParser::parse(WindowsResource *WR) {
  auto EntryOrErr = WR->getHeadEntry();
  if (!EntryOrErr)
    return EntryOrErr.takeError();

  ResourceEntryRef Entry = EntryOrErr.get();
  bool End = false;
  while (!End) {

    Data.push_back(Entry.getData());

    if (Entry.checkTypeString())
      StringTable.push_back(Entry.getTypeString());

    if (Entry.checkNameString())
      StringTable.push_back(Entry.getNameString());

    Root.addEntry(Entry);

    RETURN_IF_ERROR(Entry.moveNext(End));
  }

  return Error::success();
}

void WindowsResourceParser::printTree() const {
  ScopedPrinter Writer(outs());
  Root.print(Writer, "Resource Tree");
}

void WindowsResourceParser::TreeNode::addEntry(const ResourceEntryRef &Entry) {
  TreeNode &TypeNode = addTypeNode(Entry);
  TreeNode &NameNode = TypeNode.addNameNode(Entry);
  NameNode.addLanguageNode(Entry);
}

WindowsResourceParser::TreeNode::TreeNode(bool IsStringNode) {
  if (IsStringNode)
    StringIndex = StringCount++;
}

WindowsResourceParser::TreeNode::TreeNode(uint16_t MajorVersion,
                                          uint16_t MinorVersion,
                                          uint32_t Characteristics)
    : IsDataNode(true), MajorVersion(MajorVersion), MinorVersion(MinorVersion),
      Characteristics(Characteristics) {
  if (IsDataNode)
    DataIndex = DataCount++;
}

std::unique_ptr<WindowsResourceParser::TreeNode>
WindowsResourceParser::TreeNode::createStringNode() {
  return std::unique_ptr<TreeNode>(new TreeNode(true));
}

std::unique_ptr<WindowsResourceParser::TreeNode>
WindowsResourceParser::TreeNode::createIDNode() {
  return std::unique_ptr<TreeNode>(new TreeNode(false));
}

std::unique_ptr<WindowsResourceParser::TreeNode>
WindowsResourceParser::TreeNode::createDataNode(uint16_t MajorVersion,
                                                uint16_t MinorVersion,
                                                uint32_t Characteristics) {
  return std::unique_ptr<TreeNode>(
      new TreeNode(MajorVersion, MinorVersion, Characteristics));
}

WindowsResourceParser::TreeNode &
WindowsResourceParser::TreeNode::addTypeNode(const ResourceEntryRef &Entry) {
  if (Entry.checkTypeString())
    return addChild(Entry.getTypeString());
  else
    return addChild(Entry.getTypeID());
}

WindowsResourceParser::TreeNode &
WindowsResourceParser::TreeNode::addNameNode(const ResourceEntryRef &Entry) {
  if (Entry.checkNameString())
    return addChild(Entry.getNameString());
  else
    return addChild(Entry.getNameID());
}

WindowsResourceParser::TreeNode &
WindowsResourceParser::TreeNode::addLanguageNode(
    const ResourceEntryRef &Entry) {
  return addChild(Entry.getLanguage(), true, Entry.getMajorVersion(),
                  Entry.getMinorVersion(), Entry.getCharacteristics());
}

WindowsResourceParser::TreeNode &WindowsResourceParser::TreeNode::addChild(
    uint32_t ID, bool IsDataNode, uint16_t MajorVersion, uint16_t MinorVersion,
    uint32_t Characteristics) {
  auto Child = IDChildren.find(ID);
  if (Child == IDChildren.end()) {
    auto NewChild =
        IsDataNode ? createDataNode(MajorVersion, MinorVersion, Characteristics)
                   : createIDNode();
    WindowsResourceParser::TreeNode &Node = *NewChild;
    IDChildren.emplace(ID, std::move(NewChild));
    return Node;
  } else
    return *(Child->second);
}

WindowsResourceParser::TreeNode &
WindowsResourceParser::TreeNode::addChild(ArrayRef<UTF16> NameRef) {
  std::string NameString;
  ArrayRef<UTF16> CorrectedName;
  std::vector<UTF16> EndianCorrectedName;
  if (llvm::sys::IsBigEndianHost) {
    EndianCorrectedName.resize(NameRef.size() + 1);
    std::copy(NameRef.begin(), NameRef.end(), EndianCorrectedName.begin() + 1);
    EndianCorrectedName[0] = UNI_UTF16_BYTE_ORDER_MARK_SWAPPED;
    CorrectedName = makeArrayRef(EndianCorrectedName);
  } else
    CorrectedName = NameRef;
  llvm::convertUTF16ToUTF8String(CorrectedName, NameString);

  auto Child = StringChildren.find(NameString);
  if (Child == StringChildren.end()) {
    auto NewChild = createStringNode();
    WindowsResourceParser::TreeNode &Node = *NewChild;
    StringChildren.emplace(NameString, std::move(NewChild));
    return Node;
  } else
    return *(Child->second);
}

void WindowsResourceParser::TreeNode::print(ScopedPrinter &Writer,
                                            StringRef Name) const {
  ListScope NodeScope(Writer, Name);
  for (auto const &Child : StringChildren) {
    Child.second->print(Writer, Child.first);
  }
  for (auto const &Child : IDChildren) {
    Child.second->print(Writer, to_string(Child.first));
  }
}

// This function returns the size of the entire resource tree, including
// directory tables, directory entries, and data entries.  It does not include
// the directory strings or the relocations of the .rsrc section.
uint32_t WindowsResourceParser::TreeNode::getTreeSize() const {
  uint32_t Size = (IDChildren.size() + StringChildren.size()) *
                  sizeof(llvm::object::coff_resource_dir_entry);

  // Reached a node pointing to a data entry.
  if (IsDataNode) {
    Size += sizeof(llvm::object::coff_resource_data_entry);
    return Size;
  }

  // If the node does not point to data, it must have a directory table pointing
  // to other nodes.
  Size += sizeof(llvm::object::coff_resource_dir_table);

  for (auto const &Child : StringChildren) {
    Size += Child.second->getTreeSize();
  }
  for (auto const &Child : IDChildren) {
    Size += Child.second->getTreeSize();
  }
  return Size;
}

class WindowsResourceCOFFWriter {
public:
  WindowsResourceCOFFWriter(StringRef OutputFile, Machine MachineType,
                            const WindowsResourceParser &Parser, Error &E);

  Error write();

private:
  void performFileLayout();
  void performSectionOneLayout();
  void performSectionTwoLayout();
  void writeCOFFHeader();
  void writeFirstSectionHeader();
  void writeSecondSectionHeader();
  void writeFirstSection();
  void writeSecondSection();
  void writeSymbolTable();
  void writeStringTable();
  void writeDirectoryTree();
  void writeDirectoryStringTable();
  void writeFirstSectionRelocations();
  std::unique_ptr<FileOutputBuffer> Buffer;
  uint8_t *Current;
  Machine MachineType;
  const WindowsResourceParser::TreeNode &Resources;
  const ArrayRef<std::vector<uint8_t>> Data;
  uint64_t FileSize;
  uint32_t SymbolTableOffset;
  uint32_t SectionOneSize;
  uint32_t SectionOneOffset;
  uint32_t SectionOneRelocations;
  uint32_t SectionTwoSize;
  uint32_t SectionTwoOffset;
  const ArrayRef<std::vector<UTF16>> StringTable;
  std::vector<uint32_t> StringTableOffsets;
  std::vector<uint32_t> DataOffsets;
  std::vector<uint32_t> RelocationAddresses;
};

WindowsResourceCOFFWriter::WindowsResourceCOFFWriter(
    StringRef OutputFile, Machine MachineType,
    const WindowsResourceParser &Parser, Error &E)
    : MachineType(MachineType), Resources(Parser.getTree()),
      Data(Parser.getData()), StringTable(Parser.getStringTable()) {
  performFileLayout();

  ErrorOr<std::unique_ptr<FileOutputBuffer>> BufferOrErr =
      FileOutputBuffer::create(OutputFile, FileSize);
  if (!BufferOrErr) {
    E = errorCodeToError(BufferOrErr.getError());
    return;
  }

  Buffer = std::move(*BufferOrErr);
}

void WindowsResourceCOFFWriter::performFileLayout() {
  // Add size of COFF header.
  FileSize = llvm::COFF::Header16Size;

  // one .rsrc section header for directory tree, another for resource data.
  FileSize += 2 * llvm::COFF::SectionSize;

  performSectionOneLayout();
  performSectionTwoLayout();

  // We have reached the address of the symbol table.
  SymbolTableOffset = FileSize;

  FileSize += llvm::COFF::Symbol16Size;     // size of the @feat.00 symbol.
  FileSize += 4 * llvm::COFF::Symbol16Size; // symbol + aux for each section.
  FileSize += Data.size() * llvm::COFF::Symbol16Size; // 1 symbol per resource.
  FileSize += 4; // four null bytes for the string table.
}

void WindowsResourceCOFFWriter::performSectionOneLayout() {
  SectionOneOffset = FileSize;

  SectionOneSize = Resources.getTreeSize();
  uint32_t CurrentStringOffset = SectionOneSize;
  uint32_t TotalStringTableSize = 0;
  for (auto const &String : StringTable) {
    StringTableOffsets.push_back(CurrentStringOffset);
    uint32_t StringSize = String.size() * sizeof(UTF16) + sizeof(uint16_t);
    CurrentStringOffset += StringSize;
    TotalStringTableSize += StringSize;
  }
  SectionOneSize += alignTo(TotalStringTableSize, sizeof(uint32_t));

  // account for the relocations of section one.
  SectionOneRelocations = FileSize + SectionOneSize;
  FileSize += SectionOneSize;
  FileSize += Data.size() *
              llvm::COFF::RelocationSize; // one relocation for each resource.
}

void WindowsResourceCOFFWriter::performSectionTwoLayout() {
  // add size of .rsrc$2 section, which contains all resource data on 8-byte
  // alignment.
  SectionTwoOffset = FileSize;
  SectionTwoSize = 0;
  for (auto const &Entry : Data) {
    DataOffsets.push_back(SectionTwoSize);
    SectionTwoSize += llvm::alignTo(Entry.size(), sizeof(uint64_t));
  }
  FileSize += SectionTwoSize;
}

static std::time_t getTime() {
  std::time_t Now = time(nullptr);
  if (Now < 0 || !isUInt<32>(Now))
    return UINT32_MAX;
  return Now;
}

Error WindowsResourceCOFFWriter::write() {
  Current = Buffer->getBufferStart();

  writeCOFFHeader();
  writeFirstSectionHeader();
  writeSecondSectionHeader();
  writeFirstSection();
  writeSecondSection();
  writeSymbolTable();
  writeStringTable();

  if (auto EC = Buffer->commit()) {
    return errorCodeToError(EC);
  }

  return Error::success();
}

void WindowsResourceCOFFWriter::writeCOFFHeader() {
  // Write the COFF header.
  auto *Header = reinterpret_cast<llvm::object::coff_file_header *>(Current);
  switch (MachineType) {
  case Machine::ARM:
    Header->Machine = llvm::COFF::IMAGE_FILE_MACHINE_ARMNT;
    break;
  case Machine::X64:
    Header->Machine = llvm::COFF::IMAGE_FILE_MACHINE_AMD64;
    break;
  case Machine::X86:
    Header->Machine = llvm::COFF::IMAGE_FILE_MACHINE_I386;
    break;
  default:
    Header->Machine = llvm::COFF::IMAGE_FILE_MACHINE_UNKNOWN;
  }
  Header->NumberOfSections = 2;
  Header->TimeDateStamp = getTime();
  Header->PointerToSymbolTable = SymbolTableOffset;
  // One symbol for every resource plus 2 for each section and @feat.00
  Header->NumberOfSymbols = Data.size() + 5;
  Header->SizeOfOptionalHeader = 0;
  Header->Characteristics = llvm::COFF::IMAGE_FILE_32BIT_MACHINE;
}

void WindowsResourceCOFFWriter::writeFirstSectionHeader() {
  // Write the first section header.
  Current += sizeof(llvm::object::coff_file_header);
  auto *SectionOneHeader =
      reinterpret_cast<llvm::object::coff_section *>(Current);
  strncpy(SectionOneHeader->Name, ".rsrc$01", (size_t)llvm::COFF::NameSize);
  SectionOneHeader->VirtualSize = 0;
  SectionOneHeader->VirtualAddress = 0;
  SectionOneHeader->SizeOfRawData = SectionOneSize;
  SectionOneHeader->PointerToRawData = SectionOneOffset;
  SectionOneHeader->PointerToRelocations = SectionOneRelocations;
  SectionOneHeader->PointerToLinenumbers = 0;
  SectionOneHeader->NumberOfRelocations = Data.size();
  SectionOneHeader->NumberOfLinenumbers = 0;
  SectionOneHeader->Characteristics = llvm::COFF::IMAGE_SCN_ALIGN_1BYTES;
  SectionOneHeader->Characteristics +=
      llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA;
  SectionOneHeader->Characteristics += llvm::COFF::IMAGE_SCN_MEM_DISCARDABLE;
  SectionOneHeader->Characteristics += llvm::COFF::IMAGE_SCN_MEM_READ;
}

void WindowsResourceCOFFWriter::writeSecondSectionHeader() {
  // Write the second section header.
  Current += sizeof(llvm::object::coff_section);
  auto *SectionTwoHeader =
      reinterpret_cast<llvm::object::coff_section *>(Current);
  strncpy(SectionTwoHeader->Name, ".rsrc$02", (size_t)llvm::COFF::NameSize);
  SectionTwoHeader->VirtualSize = 0;
  SectionTwoHeader->VirtualAddress = 0;
  SectionTwoHeader->SizeOfRawData = SectionTwoSize;
  SectionTwoHeader->PointerToRawData = SectionTwoOffset;
  SectionTwoHeader->PointerToRelocations = 0;
  SectionTwoHeader->PointerToLinenumbers = 0;
  SectionTwoHeader->NumberOfRelocations = 0;
  SectionTwoHeader->NumberOfLinenumbers = 0;
  SectionTwoHeader->Characteristics =
      llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA;
  SectionTwoHeader->Characteristics += llvm::COFF::IMAGE_SCN_MEM_READ;
}

void WindowsResourceCOFFWriter::writeFirstSection() {
  // Write section one.
  Current += sizeof(llvm::object::coff_section);

  writeDirectoryTree();
  writeDirectoryStringTable();
  writeFirstSectionRelocations();
}

void WindowsResourceCOFFWriter::writeSecondSection() {
  // Now write the .rsrc$02 section.
  for (auto const &RawDataEntry : Data) {
    std::copy(RawDataEntry.begin(), RawDataEntry.end(), Current);
    Current += alignTo(RawDataEntry.size(), sizeof(uint64_t));
  }
}

void WindowsResourceCOFFWriter::writeSymbolTable() {
  // Now write the symbol table.
  // First, the feat symbol.
  auto *Symbol = reinterpret_cast<llvm::object::coff_symbol16 *>(Current);
  strncpy(Symbol->Name.ShortName, "@feat.00", (size_t)llvm::COFF::NameSize);
  Symbol->Value = 0x11;
  Symbol->SectionNumber = 0xffff;
  Symbol->Type = llvm::COFF::IMAGE_SYM_DTYPE_NULL;
  Symbol->StorageClass = llvm::COFF::IMAGE_SYM_CLASS_STATIC;
  Symbol->NumberOfAuxSymbols = 0;
  Current += sizeof(llvm::object::coff_symbol16);

  // Now write the .rsrc1 symbol + aux.
  Symbol = reinterpret_cast<llvm::object::coff_symbol16 *>(Current);
  strncpy(Symbol->Name.ShortName, ".rsrc$01", (size_t)llvm::COFF::NameSize);
  Symbol->Value = 0;
  Symbol->SectionNumber = 1;
  Symbol->Type = llvm::COFF::IMAGE_SYM_DTYPE_NULL;
  Symbol->StorageClass = llvm::COFF::IMAGE_SYM_CLASS_STATIC;
  Symbol->NumberOfAuxSymbols = 1;
  Current += sizeof(llvm::object::coff_symbol16);
  auto *Aux =
      reinterpret_cast<llvm::object::coff_aux_section_definition *>(Current);
  Aux->Length = SectionOneSize;
  Aux->NumberOfRelocations = Data.size();
  Aux->NumberOfLinenumbers = 0;
  Aux->CheckSum = 0;
  Aux->NumberLowPart = 0;
  Aux->Selection = 0;
  Current += sizeof(llvm::object::coff_aux_section_definition);

  // Now write the .rsrc2 symbol + aux.
  Symbol = reinterpret_cast<llvm::object::coff_symbol16 *>(Current);
  strncpy(Symbol->Name.ShortName, ".rsrc$02", (size_t)llvm::COFF::NameSize);
  Symbol->Value = 0;
  Symbol->SectionNumber = 2;
  Symbol->Type = llvm::COFF::IMAGE_SYM_DTYPE_NULL;
  Symbol->StorageClass = llvm::COFF::IMAGE_SYM_CLASS_STATIC;
  Symbol->NumberOfAuxSymbols = 1;
  Current += sizeof(llvm::object::coff_symbol16);
  Aux = reinterpret_cast<llvm::object::coff_aux_section_definition *>(Current);
  Aux->Length = SectionTwoSize;
  Aux->NumberOfRelocations = 0;
  Aux->NumberOfLinenumbers = 0;
  Aux->CheckSum = 0;
  Aux->NumberLowPart = 0;
  Aux->Selection = 0;
  Current += sizeof(llvm::object::coff_aux_section_definition);

  // Now write a symbol for each relocation.
  for (unsigned i = 0; i < Data.size(); i++) {
    char RelocationName[9];
    sprintf(RelocationName, "$R%06X", DataOffsets[i]);
    Symbol = reinterpret_cast<llvm::object::coff_symbol16 *>(Current);
    strncpy(Symbol->Name.ShortName, RelocationName,
            (size_t)llvm::COFF::NameSize);
    Symbol->Value = DataOffsets[i];
    Symbol->SectionNumber = 1;
    Symbol->Type = llvm::COFF::IMAGE_SYM_DTYPE_NULL;
    Symbol->StorageClass = llvm::COFF::IMAGE_SYM_CLASS_STATIC;
    Symbol->NumberOfAuxSymbols = 0;
    Current += sizeof(llvm::object::coff_symbol16);
  }
}

void WindowsResourceCOFFWriter::writeStringTable() {
  // Just 4 null bytes for the string table.
  auto COFFStringTable = reinterpret_cast<uint32_t *>(Current);
  *COFFStringTable = 0;
}

void WindowsResourceCOFFWriter::writeDirectoryTree() {
  // Traverse parsed resource tree breadth-first and write the corresponding
  // COFF objects.
  std::queue<const WindowsResourceParser::TreeNode *> Queue;
  Queue.push(&Resources);
  uint32_t NextLevelOffset = sizeof(llvm::object::coff_resource_dir_table) +
                             (Resources.getStringChildren().size() +
                              Resources.getIDChildren().size()) *
                                 sizeof(llvm::object::coff_resource_dir_entry);
  std::vector<const WindowsResourceParser::TreeNode *> DataEntriesTreeOrder;
  uint32_t CurrentRelativeOffset = 0;

  while (!Queue.empty()) {
    auto CurrentNode = Queue.front();
    Queue.pop();
    auto *Table =
        reinterpret_cast<llvm::object::coff_resource_dir_table *>(Current);
    Table->Characteristics = CurrentNode->getCharacteristics();
    Table->TimeDateStamp = 0;
    Table->MajorVersion = CurrentNode->getMajorVersion();
    Table->MinorVersion = CurrentNode->getMinorVersion();
    auto &IDChildren = CurrentNode->getIDChildren();
    auto &StringChildren = CurrentNode->getStringChildren();
    Table->NumberOfNameEntries = StringChildren.size();
    Table->NumberOfIDEntries = IDChildren.size();
    Current += sizeof(llvm::object::coff_resource_dir_table);
    CurrentRelativeOffset += sizeof(llvm::object::coff_resource_dir_table);

    // Write the directory entries immediately following each directory table.
    for (auto const &Child : StringChildren) {
      auto *Entry =
          reinterpret_cast<llvm::object::coff_resource_dir_entry *>(Current);
      Entry->Identifier.NameOffset =
          StringTableOffsets[Child.second->getStringIndex()];
      if (Child.second->checkIsDataNode()) {
        Entry->Offset.DataEntryOffset = NextLevelOffset;
        NextLevelOffset += sizeof(llvm::object::coff_resource_data_entry);
        DataEntriesTreeOrder.push_back(Child.second.get());
      } else {
        Entry->Offset.SubdirOffset = NextLevelOffset + (1 << 31);
        NextLevelOffset += sizeof(llvm::object::coff_resource_dir_table) +
                           (Child.second->getStringChildren().size() +
                            Child.second->getIDChildren().size()) *
                               sizeof(llvm::object::coff_resource_dir_entry);
        Queue.push(Child.second.get());
      }
      Current += sizeof(llvm::object::coff_resource_dir_entry);
      CurrentRelativeOffset += sizeof(llvm::object::coff_resource_dir_entry);
    }
    for (auto const &Child : IDChildren) {
      auto *Entry =
          reinterpret_cast<llvm::object::coff_resource_dir_entry *>(Current);
      Entry->Identifier.ID = Child.first;
      if (Child.second->checkIsDataNode()) {
        Entry->Offset.DataEntryOffset = NextLevelOffset;
        NextLevelOffset += sizeof(llvm::object::coff_resource_data_entry);
        DataEntriesTreeOrder.push_back(Child.second.get());
      } else {
        Entry->Offset.SubdirOffset = NextLevelOffset + (1 << 31);
        NextLevelOffset += sizeof(llvm::object::coff_resource_dir_table) +
                           (Child.second->getStringChildren().size() +
                            Child.second->getIDChildren().size()) *
                               sizeof(llvm::object::coff_resource_dir_entry);
        Queue.push(Child.second.get());
      }
      Current += sizeof(llvm::object::coff_resource_dir_entry);
      CurrentRelativeOffset += sizeof(llvm::object::coff_resource_dir_entry);
    }
  }

  RelocationAddresses.resize(Data.size());
  // Now write all the resource data entries.
  for (auto DataNodes : DataEntriesTreeOrder) {
    auto *Entry =
        reinterpret_cast<llvm::object::coff_resource_data_entry *>(Current);
    RelocationAddresses[DataNodes->getDataIndex()] = CurrentRelativeOffset;
    Entry->DataRVA = 0; // Set to zero because it is a relocation.
    Entry->DataSize = Data[DataNodes->getDataIndex()].size();
    Entry->Codepage = 0;
    Entry->Reserved = 0;
    Current += sizeof(llvm::object::coff_resource_data_entry);
    CurrentRelativeOffset += sizeof(llvm::object::coff_resource_data_entry);
  }
}

void WindowsResourceCOFFWriter::writeDirectoryStringTable() {
  // Now write the directory string table for .rsrc$01
  uint32_t TotalStringTableSize = 0;
  for (auto String : StringTable) {
    auto *LengthField = reinterpret_cast<uint16_t *>(Current);
    uint16_t Length = String.size();
    *LengthField = Length;
    Current += sizeof(uint16_t);
    auto *Start = reinterpret_cast<UTF16 *>(Current);
    std::copy(String.begin(), String.end(), Start);
    Current += Length * sizeof(UTF16);
    TotalStringTableSize += Length * sizeof(UTF16) + sizeof(uint16_t);
  }
  Current +=
      alignTo(TotalStringTableSize, sizeof(uint32_t)) - TotalStringTableSize;
}

void WindowsResourceCOFFWriter::writeFirstSectionRelocations() {

  // Now write the relocations for .rsrc$01
  // Five symbols already in table before we start, @feat.00 and 2 for each
  // .rsrc section.
  uint32_t NextSymbolIndex = 5;
  for (unsigned i = 0; i < Data.size(); i++) {
    auto *Reloc = reinterpret_cast<llvm::object::coff_relocation *>(Current);
    Reloc->VirtualAddress = RelocationAddresses[i];
    Reloc->SymbolTableIndex = NextSymbolIndex++;
    switch (MachineType) {
    case Machine::ARM:
      Reloc->Type = llvm::COFF::IMAGE_REL_ARM_ADDR32NB;
      break;
    case Machine::X64:
      Reloc->Type = llvm::COFF::IMAGE_REL_AMD64_ADDR32NB;
      break;
    case Machine::X86:
      Reloc->Type = llvm::COFF::IMAGE_REL_I386_DIR32NB;
      break;
    default:
      Reloc->Type = 0;
    }
    Current += sizeof(llvm::object::coff_relocation);
  }
}

Error writeWindowsResourceCOFF(StringRef OutputFile, Machine MachineType,
                               const WindowsResourceParser &Parser) {
  Error E = Error::success();
  WindowsResourceCOFFWriter Writer(OutputFile, MachineType, Parser, E);
  if (E)
    return E;
  return Writer.write();
}

} // namespace object
} // namespace llvm
