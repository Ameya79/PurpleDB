#include "core/storage/cache.h"

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "core/util/coding.h"

namespace purpledb {

namespace {

struct LRUHandle {
  void* value;
  void (*deleter)(const Slice&, void*);
  LRUHandle* next_hash;
  LRUHandle* next;
  LRUHandle* prev;
  size_t charge;
  size_t key_length;
  bool in_cache;      // Whether entry is in the cache table
  uint32_t refs;      // Refcount (1 for cache, +1 for each open handle)
  uint32_t hash;
  char key_data[1];   // Beginning of key

  Slice key() const {
    // For free-standing node (dummy head), key_data should not be read
    assert(next != this);
    return Slice(key_data, key_length);
  }
};

// Fast Hash function
static uint32_t HashSlice(const Slice& s) {
  const uint32_t m = 0x5bd1e995;
  const int r = 24;
  uint32_t h = 0xbc9f1d34 ^ static_cast<uint32_t>(s.size());
  const char* data = s.data();
  size_t len = s.size();

  while (len >= 4) {
    uint32_t k = DecodeFixed32(data);
    k *= m;
    k ^= k >> r;
    k *= m;
    h *= m;
    h ^= k;
    data += 4;
    len -= 4;
  }

  switch (len) {
    case 3:
      h ^= static_cast<uint8_t>(data[2]) << 16;
      [[fallthrough]];
    case 2:
      h ^= static_cast<uint8_t>(data[1]) << 8;
      [[fallthrough]];
    case 1:
      h ^= static_cast<uint8_t>(data[0]);
      h *= m;
  };

  h ^= h >> 13;
  h *= m;
  h ^= h >> 15;
  return h;
}

// Hash table for LRU handles
class HandleTable {
 public:
  HandleTable() : length_(0), elems_(0), list_(nullptr) { Resize(); }
  ~HandleTable() { delete[] list_; }

  LRUHandle* Lookup(const Slice& key, uint32_t hash) {
    return *FindPointer(key, hash);
  }

  LRUHandle* Insert(LRUHandle* h) {
    LRUHandle** ptr = FindPointer(h->key(), h->hash);
    LRUHandle* old = *ptr;
    h->next_hash = (old == nullptr ? nullptr : old->next_hash);
    *ptr = h;
    if (old == nullptr) {
      elems_++;
      if (elems_ > length_) {
        Resize();
      }
    }
    return old;
  }

  LRUHandle* Remove(const Slice& key, uint32_t hash) {
    LRUHandle** ptr = FindPointer(key, hash);
    LRUHandle* result = *ptr;
    if (result != nullptr) {
      *ptr = result->next_hash;
      elems_--;
    }
    return result;
  }

 private:
  uint32_t length_;
  uint32_t elems_;
  LRUHandle** list_;

  LRUHandle** FindPointer(const Slice& key, uint32_t hash) {
    LRUHandle** ptr = &list_[hash & (length_ - 1)];
    while (*ptr != nullptr && ((*ptr)->hash != hash || key != (*ptr)->key())) {
      ptr = &(*ptr)->next_hash;
    }
    return ptr;
  }

  void Resize() {
    uint32_t new_length = 4;
    while (new_length < elems_ * 2) {
      new_length *= 2;
    }
    LRUHandle** new_list = new LRUHandle*[new_length];
    std::memset(new_list, 0, sizeof(LRUHandle*) * new_length);
    uint32_t count = 0;
    for (uint32_t i = 0; i < length_; i++) {
      LRUHandle* h = list_[i];
      while (h != nullptr) {
        LRUHandle* next = h->next_hash;
        uint32_t hash = h->hash;
        LRUHandle** ptr = &new_list[hash & (new_length - 1)];
        h->next_hash = *ptr;
        *ptr = h;
        h = next;
        count++;
      }
    }
    assert(elems_ == count);
    delete[] list_;
    list_ = new_list;
    length_ = new_length;
  }
};

class LRUCacheShard {
 public:
  LRUCacheShard();
  ~LRUCacheShard();

  void SetCapacity(size_t capacity) { capacity_ = capacity; }

  Cache::Handle* Insert(const Slice& key, uint32_t hash, void* value,
                        size_t charge, void (*deleter)(const Slice& key, void* value));
  Cache::Handle* Lookup(const Slice& key, uint32_t hash);
  void Release(Cache::Handle* handle);
  void Erase(const Slice& key, uint32_t hash);
  void Prune();
  size_t TotalCharge() const {
    std::lock_guard<std::mutex> l(mutex_);
    return usage_;
  }

 private:
  void LRU_Remove(LRUHandle* e);
  void LRU_Append(LRUHandle* list, LRUHandle* e);
  void Ref(LRUHandle* e);
  void Unref(LRUHandle* e);
  bool FinishErase(LRUHandle* e);

  size_t capacity_;
  mutable std::mutex mutex_;
  size_t usage_;

  // Dummy heads for doubly-linked lists
  // lru.prev is newest, lru.next is oldest
  LRUHandle lru_;
  LRUHandle in_use_;

  HandleTable table_;
};

LRUCacheShard::LRUCacheShard() : capacity_(0), usage_(0) {
  // Empty circular doubly linked lists
  lru_.next = &lru_;
  lru_.prev = &lru_;
  in_use_.next = &in_use_;
  in_use_.prev = &in_use_;
}

LRUCacheShard::~LRUCacheShard() {
  assert(in_use_.next == &in_use_);  // Error if caller hasn't released all handles
  for (LRUHandle* e = lru_.next; e != &lru_;) {
    LRUHandle* next = e->next;
    assert(e->in_cache);
    e->in_cache = false;
    assert(e->refs == 1);  // Invariants: only cache references it
    Unref(e);
    e = next;
  }
}

void LRUCacheShard::Ref(LRUHandle* e) {
  if (e->refs == 1 && e->in_cache) {
    // Moving from lru_ to in_use_
    LRU_Remove(e);
    LRU_Append(&in_use_, e);
  }
  e->refs++;
}

void LRUCacheShard::Unref(LRUHandle* e) {
  assert(e->refs > 0);
  e->refs--;
  if (e->refs == 0) {
    assert(!e->in_cache);
    (*e->deleter)(e->key(), e->value);
    std::free(e);
  } else if (e->in_cache && e->refs == 1) {
    // Only referenced by cache: move to lru_ for possible eviction
    LRU_Remove(e);
    LRU_Append(&lru_, e);
  }
}

void LRUCacheShard::LRU_Remove(LRUHandle* e) {
  e->next->prev = e->prev;
  e->prev->next = e->next;
}

void LRUCacheShard::LRU_Append(LRUHandle* list, LRUHandle* e) {
  // Insert at back of circular list (most recent position)
  e->next = list;
  e->prev = list->prev;
  e->prev->next = e;
  e->next->prev = e;
}

Cache::Handle* LRUCacheShard::Lookup(const Slice& key, uint32_t hash) {
  std::lock_guard<std::mutex> l(mutex_);
  LRUHandle* e = table_.Lookup(key, hash);
  if (e != nullptr) {
    Ref(e);
  }
  return reinterpret_cast<Cache::Handle*>(e);
}

void LRUCacheShard::Release(Cache::Handle* handle) {
  std::lock_guard<std::mutex> l(mutex_);
  Unref(reinterpret_cast<LRUHandle*>(handle));
}

Cache::Handle* LRUCacheShard::Insert(const Slice& key, uint32_t hash, void* value,
                                     size_t charge,
                                     void (*deleter)(const Slice& key, void* value)) {
  std::lock_guard<std::mutex> l(mutex_);

  LRUHandle* e = reinterpret_cast<LRUHandle*>(
      std::malloc(sizeof(LRUHandle) - 1 + key.size()));
  e->value = value;
  e->deleter = deleter;
  e->charge = charge;
  e->key_length = key.size();
  e->hash = hash;
  e->in_cache = false;
  e->refs = 1;  // For the caller handle
  std::memcpy(e->key_data, key.data(), key.size());

  if (capacity_ > 0) {
    e->refs++;  // For cache reference
    e->in_cache = true;
    LRU_Append(&in_use_, e);
    usage_ += charge;
    FinishErase(table_.Insert(e));
  } else {
    // Cache is disabled (capacity 0)
    e->next = nullptr;
    e->prev = nullptr;
  }

  while (usage_ > capacity_ && lru_.next != &lru_) {
    LRUHandle* old = lru_.next;
    assert(old->refs == 1);
    bool erased = FinishErase(table_.Remove(old->key(), old->hash));
    (void)erased;
    assert(erased);
  }

  return reinterpret_cast<Cache::Handle*>(e);
}

bool LRUCacheShard::FinishErase(LRUHandle* e) {
  if (e != nullptr) {
    assert(e->in_cache);
    LRU_Remove(e);
    e->in_cache = false;
    usage_ -= e->charge;
    Unref(e);
    return true;
  }
  return false;
}

void LRUCacheShard::Erase(const Slice& key, uint32_t hash) {
  std::lock_guard<std::mutex> l(mutex_);
  FinishErase(table_.Remove(key, hash));
}

void LRUCacheShard::Prune() {
  std::lock_guard<std::mutex> l(mutex_);
  while (lru_.next != &lru_) {
    LRUHandle* e = lru_.next;
    assert(e->refs == 1);
    bool erased = FinishErase(table_.Remove(e->key(), e->hash));
    (void)erased;
    assert(erased);
  }
}

static const int kNumShardBits = 4;
static const int kNumShards = 1 << kNumShardBits;  // 16 shards

class ShardedLRUCache : public Cache {
 public:
  explicit ShardedLRUCache(size_t capacity) : last_id_(0) {
    const size_t per_shard = (capacity + (kNumShards - 1)) / kNumShards;
    for (int s = 0; s < kNumShards; s++) {
      shards_[s].SetCapacity(per_shard);
    }
  }

  ~ShardedLRUCache() override = default;

  Handle* Insert(const Slice& key, void* value, size_t charge,
                 void (*deleter)(const Slice& key, void* value)) override {
    const uint32_t hash = HashSlice(key);
    return shards_[Shard(hash)].Insert(key, hash, value, charge, deleter);
  }

  Handle* Lookup(const Slice& key) override {
    const uint32_t hash = HashSlice(key);
    return shards_[Shard(hash)].Lookup(key, hash);
  }

  void Release(Handle* handle) override {
    LRUHandle* h = reinterpret_cast<LRUHandle*>(handle);
    shards_[Shard(h->hash)].Release(handle);
  }

  void Erase(const Slice& key) override {
    const uint32_t hash = HashSlice(key);
    shards_[Shard(hash)].Erase(key, hash);
  }

  void* Value(Handle* handle) override {
    return reinterpret_cast<LRUHandle*>(handle)->value;
  }

  uint64_t NewId() override {
    return last_id_.fetch_add(1, std::memory_order_relaxed);
  }

  void Prune() override {
    for (int s = 0; s < kNumShards; s++) {
      shards_[s].Prune();
    }
  }

  size_t TotalCharge() const override {
    size_t total = 0;
    for (int s = 0; s < kNumShards; s++) {
      total += shards_[s].TotalCharge();
    }
    return total;
  }

 private:
  static inline uint32_t Shard(uint32_t hash) {
    return hash >> (32 - kNumShardBits);
  }

  LRUCacheShard shards_[kNumShards];
  std::atomic<uint64_t> last_id_;
};

}  // namespace

std::shared_ptr<Cache> NewLRUCache(size_t capacity) {
  return std::make_shared<ShardedLRUCache>(capacity);
}

}  // namespace purpledb

