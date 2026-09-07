#include "config/internal/MemoryStore.h"

bool MemoryStore::load(Settings& out) {
  if (!hasStored_) {
    return false;
  }
  out = stored_;
  return true;
}

bool MemoryStore::save(const Settings& s) {
  if (failNextSave_) {
    failNextSave_ = false;
    return false;
  }
  stored_ = s;
  hasStored_ = true;
  return true;
}

void MemoryStore::failNextSave() { failNextSave_ = true; }

bool MemoryStore::hasStored() const { return hasStored_; }

const Settings& MemoryStore::stored() const { return stored_; }
