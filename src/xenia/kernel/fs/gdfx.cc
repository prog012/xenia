/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 * Major contributions to this file from:
 * - abgx360
 */

#include "xenia/kernel/fs/gdfx.h"

#include "xenia/base/math.h"

namespace xe {
namespace kernel {
namespace fs {

const size_t kXESectorSize = 2048;

GDFXEntry::GDFXEntry()
    : attributes(X_FILE_ATTRIBUTE_NONE), offset(0), size(0) {}

GDFXEntry::~GDFXEntry() {
  for (std::vector<GDFXEntry*>::iterator it = children.begin();
       it != children.end(); ++it) {
    delete *it;
  }
}

GDFXEntry* GDFXEntry::GetChild(const xe::fs::WildcardEngine& engine,
                               child_it_t& ref_it) {
  GDFXEntry* child_entry(nullptr);
  while (ref_it != children.end()) {
    if (engine.Match((*ref_it)->name)) {
      child_entry = (*ref_it);
      ++ref_it;
      break;
    }
    ++ref_it;
  }
  return child_entry;
}

GDFXEntry* GDFXEntry::GetChild(const char* name) {
  // TODO(benvanik): a faster search
  for (std::vector<GDFXEntry*>::iterator it = children.begin();
       it != children.end(); ++it) {
    GDFXEntry* entry = *it;
    if (strcasecmp(entry->name.c_str(), name) == 0) {
      return entry;
    }
  }
  return NULL;
}

void GDFXEntry::Dump(int indent) {
  printf("%s%s\n", std::string(indent, ' ').c_str(), name.c_str());
  for (std::vector<GDFXEntry*>::iterator it = children.begin();
       it != children.end(); ++it) {
    GDFXEntry* entry = *it;
    entry->Dump(indent + 2);
  }
}

GDFX::GDFX(MappedMemory* mmap) : mmap_(mmap) { root_entry_ = nullptr; }

GDFX::~GDFX() { delete root_entry_; }

GDFXEntry* GDFX::root_entry() { return root_entry_; }

GDFX::Error GDFX::Load() {
  ParseState state = {0};

  state.ptr = mmap_->data();
  state.size = mmap_->size();

  auto result = Verify(state);
  if (result != kSuccess) {
    return result;
  }

  result = ReadAllEntries(state, state.ptr + state.root_offset);
  if (result != kSuccess) {
    return result;
  }

  return kSuccess;
}

void GDFX::Dump() {
  if (root_entry_) {
    root_entry_->Dump(0);
  }
}

GDFX::Error GDFX::Verify(ParseState& state) {
  // Find sector 32 of the game partition - try at a few points.
  const static size_t likely_offsets[] = {
      0x00000000, 0x0000FB20, 0x00020600, 0x0FD90000,
  };
  bool magic_found = false;
  for (size_t n = 0; n < xe::countof(likely_offsets); n++) {
    state.game_offset = likely_offsets[n];
    if (VerifyMagic(state, state.game_offset + (32 * kXESectorSize))) {
      magic_found = true;
      break;
    }
  }
  if (!magic_found) {
    // File doesn't have the magic values - likely not a real GDFX source.
    return kErrorFileMismatch;
  }

  // Read sector 32 to get FS state.
  if (state.size < state.game_offset + (32 * kXESectorSize)) {
    return kErrorReadError;
  }
  uint8_t* fs_ptr = state.ptr + state.game_offset + (32 * kXESectorSize);
  state.root_sector = xe::load<uint32_t>(fs_ptr + 20);
  state.root_size = xe::load<uint32_t>(fs_ptr + 24);
  state.root_offset = state.game_offset + (state.root_sector * kXESectorSize);
  if (state.root_size < 13 || state.root_size > 32 * 1024 * 1024) {
    return kErrorDamagedFile;
  }

  return kSuccess;
}

bool GDFX::VerifyMagic(ParseState& state, size_t offset) {
  // Simple check to see if the given offset contains the magic value.
  return memcmp(state.ptr + offset, "MICROSOFT*XBOX*MEDIA", 20) == 0;
}

GDFX::Error GDFX::ReadAllEntries(ParseState& state,
                                 const uint8_t* root_buffer) {
  root_entry_ = new GDFXEntry();
  root_entry_->offset = 0;
  root_entry_->size = 0;
  root_entry_->name = "";
  root_entry_->attributes = X_FILE_ATTRIBUTE_DIRECTORY;

  if (!ReadEntry(state, root_buffer, 0, root_entry_)) {
    return kErrorOutOfMemory;
  }

  return kSuccess;
}

bool GDFX::ReadEntry(ParseState& state, const uint8_t* buffer,
                     uint16_t entry_ordinal, GDFXEntry* parent) {
  const uint8_t* p = buffer + (entry_ordinal * 4);

  uint16_t node_l = xe::load<uint16_t>(p + 0);
  uint16_t node_r = xe::load<uint16_t>(p + 2);
  size_t sector = xe::load<uint32_t>(p + 4);
  size_t length = xe::load<uint32_t>(p + 8);
  uint8_t attributes = xe::load<uint8_t>(p + 12);
  uint8_t name_length = xe::load<uint8_t>(p + 13);
  char* name = (char*)(p + 14);

  if (node_l && !ReadEntry(state, buffer, node_l, parent)) {
    return false;
  }

  GDFXEntry* entry = new GDFXEntry();
  entry->name = std::string(name, name_length);
  entry->attributes = (X_FILE_ATTRIBUTES)attributes;

  // Add to parent.
  parent->children.push_back(entry);

  if (attributes & X_FILE_ATTRIBUTE_DIRECTORY) {
    // Folder.
    entry->offset = 0;
    entry->size = 0;
    if (length) {
      // Not a leaf - read in children.
      if (state.size < state.game_offset + (sector * kXESectorSize)) {
        // Out of bounds read.
        return false;
      }
      // Read child list.
      uint8_t* folder_ptr =
          state.ptr + state.game_offset + (sector * kXESectorSize);
      if (!ReadEntry(state, folder_ptr, 0, entry)) {
        return false;
      }
    }
  } else {
    // File.
    entry->offset = state.game_offset + (sector * kXESectorSize);
    entry->size = length;
  }

  // Read next file in the list.
  if (node_r && !ReadEntry(state, buffer, node_r, parent)) {
    return false;
  }

  return true;
}

}  // namespace fs
}  // namespace kernel
}  // namespace xe
