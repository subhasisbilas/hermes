/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#include "hermes/BCGen/HBC/DebugInfo.h"

#include "hermes/BCGen/HBC/ConsecutiveStringStorage.h"
#include "hermes/SourceMap/SourceMapGenerator.h"

using namespace hermes;
using namespace hbc;

namespace {

/// A type used to iteratively deserialize function debug info.
struct FunctionDebugInfoDeserializer {
  /// Construct a deserializer that begins deserializing at \p offset in \p
  /// data. It will deserialize until the function's debug info is finished
  /// (address delta = -1) at which point next() will return None. The offset of
  /// the next section can be obtained via getOffset().
  FunctionDebugInfoDeserializer(llvm::ArrayRef<uint8_t> data, uint32_t offset)
      : data_(data), offset_(offset) {
    functionIndex_ = decode1Int();
    current_.line = decode1Int();
    current_.column = decode1Int();
  }

  /// \return the next debug location, or None if we reach the end.
  /// Sample usage: while (auto loc = fdid.next()) {...}
  OptValue<DebugSourceLocation> next() {
    auto addressDelta = decode1Int();
    if (addressDelta == -1)
      return llvm::None;
    // Presence of the statement delta is LSB of line delta.
    int64_t lineDelta = decode1Int();
    int64_t columnDelta = decode1Int();
    int64_t statementDelta = 0;
    if (lineDelta & 1)
      statementDelta = decode1Int();
    lineDelta >>= 1;

    current_.address += addressDelta;
    current_.line += lineDelta;
    current_.column += columnDelta;
    current_.statement += statementDelta;
    return current_;
  }

  /// \return the offset of this deserializer in the data.
  uint32_t getOffset() const {
    return offset_;
  }

  /// \return the index of the function being deserialized.
  uint32_t getFunctionIndex() const {
    return functionIndex_;
  }

  /// \return the current source location.
  const DebugSourceLocation &getCurrent() const {
    return current_;
  }

 private:
  /// LEB-decode the next int.
  int64_t decode1Int() {
    int64_t result;
    offset_ += readSignedLEB128(data_, offset_, &result);
    return result;
  }

  llvm::ArrayRef<uint8_t> data_;
  uint32_t offset_;

  uint32_t functionIndex_;
  DebugSourceLocation current_;
};
} // namespace

/// Decodes a string at offset \p offset in \p data, updating offset in-place.
/// \return the decoded string.
static StringRef decodeString(
    uint32_t *inoutOffset,
    llvm::ArrayRef<uint8_t> data) {
  // The string is represented as its LEB-encoded length, followed by
  // the bytes. This format matches DebugInfoGenerator::appendString().
  uint32_t offset = *inoutOffset;
  int64_t strSize;
  offset += readSignedLEB128(data, offset, &strSize);
  assert(
      strSize >= 0 && // can't be negative
      strSize <= UINT_MAX && // can't overflow uint32
      uint32_t(strSize) + offset >= offset && // sum can't overflow
      uint32_t(strSize) + offset <= data.size() && // validate range
      "Invalid string size");
  const unsigned char *ptr = data.data() + offset;
  offset += strSize;
  *inoutOffset = offset;
  return StringRef(reinterpret_cast<const char *>(ptr), size_t(strSize));
}

OptValue<uint32_t> DebugInfo::getFilenameForAddress(
    uint32_t debugOffset) const {
  OptValue<uint32_t> value = llvm::None;
  // This is sorted list of (address, filename) pairs so we could use
  // binary search. However, we expect the number of entries to be
  // between zero and one.
  for (int i = 0, e = files_.size(); i < e; i++) {
    if (files_[i].fromAddress <= debugOffset) {
      value = files_[i].filenameId;
    } else
      break;
  }
  return value;
}

OptValue<DebugSourceLocation> DebugInfo::getLocationForAddress(
    uint32_t debugOffset,
    uint32_t offsetInFunction) const {
  assert(debugOffset < data_.size() && "Debug offset out of range");
  FunctionDebugInfoDeserializer fdid(data_.getData(), debugOffset);
  DebugSourceLocation lastLocation = fdid.getCurrent();
  uint32_t lastLocationOffset = debugOffset;
  uint32_t nextLocationOffset = fdid.getOffset();
  while (auto loc = fdid.next()) {
    if (loc->address > offsetInFunction)
      break;
    lastLocation = *loc;
    lastLocationOffset = nextLocationOffset;
    nextLocationOffset = fdid.getOffset();
  }
  if (auto file = getFilenameForAddress(lastLocationOffset)) {
    lastLocation.address = offsetInFunction;
    lastLocation.filenameId = *file;
    return lastLocation;
  }
  return llvm::None;
}

OptValue<DebugSearchResult> DebugInfo::getAddressForLocation(
    uint32_t filenameId,
    uint32_t targetLine,
    OptValue<uint32_t> targetColumn) const {
  // First, get the start/end debug offsets for the given file.
  uint32_t start = 0;
  uint32_t end = 0;
  bool foundFile{false};
  for (const auto &cur : files_) {
    if (foundFile) {
      // We must have found the file on the last iteration,
      // so set the beginning of this file to be the end index,
      // and break out of the loop, ensuring we know where the target
      // file starts and ends.
      end = cur.fromAddress;
      break;
    }
    if (cur.filenameId == filenameId) {
      foundFile = true;
      start = cur.fromAddress;
      end = lexicalDataOffset_;
    }
  }
  if (!foundFile) {
    return llvm::None;
  }

  unsigned offset = start;

  while (offset < end) {
    FunctionDebugInfoDeserializer fdid(data_.getData(), offset);
    while (auto loc = fdid.next()) {
      uint32_t line = loc->line;
      uint32_t column = loc->column;
      if (line == targetLine &&
          (!targetColumn.hasValue() || column == *targetColumn)) {
        // Short-circuit on a precise match.
        return DebugSearchResult(
            fdid.getFunctionIndex(), loc->address, line, column);
      }
    }
    offset = fdid.getOffset();
  }

  return llvm::None;
}

/// Read \p count variable names from \p offset into the variable name section
/// of the debug info. \return the list of variable names.
llvm::SmallVector<StringRef, 4> DebugInfo::getVariableNames(
    uint32_t offset) const {
  // Incoming offset is given relative to our lexical region.
  llvm::ArrayRef<uint8_t> data = lexicalData();
  int64_t parentId;
  int64_t signedCount;
  offset += readSignedLEB128(data, offset, &parentId);
  offset += readSignedLEB128(data, offset, &signedCount);
  (void)parentId;
  assert(signedCount >= 0 && "Invalid variable name count");
  size_t count = size_t(signedCount);

  llvm::SmallVector<StringRef, 4> result;
  result.reserve(count);
  for (size_t i = 0; i < count; i++)
    result.push_back(decodeString(&offset, data));
  return result;
}

OptValue<uint32_t> DebugInfo::getParentFunctionId(uint32_t offset) const {
  // Incoming offset is given relative to our lexical region.
  llvm::ArrayRef<uint8_t> data = lexicalData();
  int64_t parentId;
  readSignedLEB128(data, offset, &parentId);
  if (parentId < 0)
    return llvm::None;
  assert(parentId <= UINT32_MAX && "Parent ID out of bounds");
  return uint32_t(parentId);
}

void DebugInfo::disassembleFilenames(llvm::raw_ostream &os) const {
  os << "Debug filename table:\n";
  for (uint32_t i = 0, e = filenameTable_.size(); i < e; ++i) {
    os << "  " << i << ": " << getFilenameByID(i) << '\n';
  }
  os << '\n';
}

void DebugInfo::disassembleFilesAndOffsets(llvm::raw_ostream &OS) const {
  OS << "Debug file table:\n";
  for (int i = 0, e = files_.size(); i < e; i++) {
    OS << "  Debug offset " << files_[i].fromAddress << ": string id "
       << files_[i].filenameId << "\n";
  }
  if (!files_.size()) {
    OS << "(none)\n";
  }
  OS << "\n";

  OS << "Debug data table:\n";

  uint32_t offset = 0;
  llvm::ArrayRef<uint8_t> locsData = sourceLocationsData();
  while (offset < locsData.size()) {
    FunctionDebugInfoDeserializer fdid(locsData, offset);
    OS << "  DebugOffset " << llvm::format_hex(offset, 2);
    OS << " for function at " << fdid.getFunctionIndex();
    OS << " starts at line=" << fdid.getCurrent().line
       << ", col=" << fdid.getCurrent().column;
    OS << " and emits locations for ";
    uint32_t count = 0;
    while (auto loc = fdid.next()) {
      OS << loc->address << " ";
      count++;
    }
    OS << " (" << count << " in total).\n";
    offset = fdid.getOffset();
  }
  OS << "  Debug table ends at debugOffset " << llvm::format_hex(offset, 2)
     << "\n";
}

void DebugInfo::disassembleLexicalData(llvm::raw_ostream &OS) const {
  uint32_t offset = 0;
  llvm::ArrayRef<uint8_t> lexData = lexicalData();

  OS << "Debug variables table:\n";
  auto next = [&]() {
    int64_t result;
    offset += readSignedLEB128(lexData, offset, &result);
    return (int32_t)result;
  };
  while (offset < lexData.size()) {
    OS << "  Offset: " << llvm::format_hex(offset, 2);
    int64_t parentId = next();
    int64_t varNamesCount = next();
    OS << ", vars count: " << varNamesCount << ", lexical parent: ";
    if (parentId < 0) {
      OS << "none";
    } else {
      OS << parentId;
    }
    OS << '\n';
    for (int64_t i = 0; i < varNamesCount; i++) {
      const auto startOffset = offset;
      StringRef name = decodeString(&offset, lexData);
      OS << "    " << llvm::format_hex(startOffset, 6) << ": ";
      OS << '"';
      OS.write_escaped(name);
      OS << '"' << '\n';
    }
  }
}

void DebugInfo::populateSourceMap(
    SourceMapGenerator *sourceMap,
    llvm::ArrayRef<uint32_t> functionOffsets,
    uint32_t cjsModuleOffset) const {
  // Since our bytecode is not JavaScript, we interpret the source map in a
  // creative way: each bytecode module is represented as a line, and bytecode
  // addresses in the file are represented as column offsets.
  // Our debug information has a function start and then offsets within the
  // function, but the source map will do its own delta encoding, so we provide
  // absolute addresses to the source map.
  auto segmentFor = [&](const DebugSourceLocation &loc,
                        uint32_t offsetInFile,
                        uint32_t debugOffset) {
    SourceMap::Segment segment;
    segment.generatedColumn = loc.address + offsetInFile;
    segment.sourceIndex = sourceMap->getSourceIndex(
        getFilenameByID(*getFilenameForAddress(debugOffset)));
    segment.representedLine = loc.line;
    segment.representedColumn = loc.column;
    return segment;
  };

  std::vector<SourceMap::Segment> segments;
  llvm::ArrayRef<uint8_t> locsData = sourceLocationsData();
  uint32_t offset = 0;
  while (offset < locsData.size()) {
    FunctionDebugInfoDeserializer fdid(locsData, offset);
    uint32_t offsetInFile = functionOffsets[fdid.getFunctionIndex()];
    segments.push_back(segmentFor(fdid.getCurrent(), offsetInFile, offset));
    while (auto loc = fdid.next())
      segments.push_back(segmentFor(*loc, offsetInFile, offset));
    offset = fdid.getOffset();
  }
  sourceMap->addMappingsLine(std::move(segments), cjsModuleOffset);
}

uint32_t DebugInfoGenerator::appendSourceLocations(
    const DebugSourceLocation &start,
    uint32_t functionIndex,
    llvm::ArrayRef<DebugSourceLocation> offsets) {
  assert(validData && "DebugInfoGenerator not valid");

  // The start of the function isn't part of a statement,
  // so require that statement = 0 for the start debug value.
  assert(start.statement == 0 && "function must start at statement 0");

  const uint32_t startOffset = sourcesData_.size();
  if (offsets.empty())
    return startOffset;

  if (!files_.size() || files_.back().filenameId != start.filenameId) {
    files_.push_back(DebugFileRegion{
        startOffset, start.filenameId, start.sourceMappingUrlId});
  }

  appendSignedLEB128(sourcesData_, functionIndex);
  appendSignedLEB128(sourcesData_, start.line);
  appendSignedLEB128(sourcesData_, start.column);
  const DebugSourceLocation *previous = &start;

  for (auto &next : offsets) {
    if (next.filenameId != previous->filenameId) {
      files_.push_back(DebugFileRegion{(unsigned)sourcesData_.size(),
                                       next.filenameId,
                                       start.sourceMappingUrlId});
    }

    int32_t adelta = delta(next.address, previous->address);
    // ldelta needs 64 bits because we will use it to encode an extra bit.
    int64_t ldelta = delta(next.line, previous->line);
    int32_t cdelta = delta(next.column, previous->column);
    int32_t sdelta = delta(next.statement, previous->statement);

    // Encode the presence of statementNo as a bit in the line delta, which is
    // usually very small.
    // ldelta encoding: bits 1..32 contain the line delta. Bit 0 indicates the
    // presence of statementNo.
    ldelta = (ldelta * 2) + (sdelta != 0);

    appendSignedLEB128(sourcesData_, adelta);
    appendSignedLEB128(sourcesData_, ldelta);
    appendSignedLEB128(sourcesData_, cdelta);
    if (sdelta)
      appendSignedLEB128(sourcesData_, sdelta);
    previous = &next;
  }
  appendSignedLEB128(sourcesData_, -1);

  return startOffset;
}

DebugInfoGenerator::DebugInfoGenerator(UniquingStringTable &&filenameTable)
    : filenameStrings_(filenameTable.generateStorage()) {
  // Initialize the empty lexical data.
  assert(
      lexicalData_.size() == kEmptyLexicalDataOffset &&
      "Lexical data should initially be kEmptyLexicalDataOffset");
  appendSignedLEB128(lexicalData_, -1); // parent function
  appendSignedLEB128(lexicalData_, 0); // name count
}

uint32_t DebugInfoGenerator::appendLexicalData(
    OptValue<uint32_t> parentFunc,
    llvm::ArrayRef<Identifier> names) {
  assert(validData && "DebugInfoGenerator not valid");
  if (!parentFunc.hasValue() && names.empty()) {
    return kEmptyLexicalDataOffset;
  }
  const uint32_t startOffset = lexicalData_.size();
  appendSignedLEB128(lexicalData_, parentFunc ? *parentFunc : int64_t(-1));
  appendSignedLEB128(lexicalData_, names.size());
  for (Identifier name : names)
    appendString(lexicalData_, name.str());
  return startOffset;
}

DebugInfo DebugInfoGenerator::serializeWithMove() {
  assert(validData);
  validData = false;

  // Append the lexical data after the sources data.
  uint32_t lexicalStart = sourcesData_.size();
  auto combinedData = std::move(sourcesData_);
  combinedData.insert(
      combinedData.end(), lexicalData_.begin(), lexicalData_.end());
  return DebugInfo(
      std::move(filenameStrings_),
      std::move(files_),
      lexicalStart,
      StreamVector<uint8_t>(std::move(combinedData)));
}
