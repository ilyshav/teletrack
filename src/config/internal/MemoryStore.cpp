#include "config/internal/MemoryStore.h"

bool MemoryStore::load(Settings& out) {
  if (!available_ || !hasStored_) {
    return false;
  }
  out = stored_;
  return true;
}

bool MemoryStore::save(const Settings& s) {
  if (!available_) {
    return false;
  }
  if (failNextSave_) {
    failNextSave_ = false;
    return false;
  }
  stored_ = s;
  hasStored_ = true;
  return true;
}

bool MemoryStore::available() const { return available_; }

void MemoryStore::setAvailable(bool value) { available_ = value; }

void MemoryStore::failNextSave() { failNextSave_ = true; }

bool MemoryStore::hasStored() const { return hasStored_; }

const Settings& MemoryStore::stored() const { return stored_; }
