#ifndef CHAKRA_FEEDER_V3_CACHE_H
#define CHAKRA_FEEDER_V3_CACHE_H

#include <list>
#include <memory>
#include <unordered_map>

namespace Chakra {
namespace FeederV3 {

// NOTE: intentionally not thread-safe. The analytical simulator drives the
// feeder (and the shared static node cache) from a single thread, so the
// previous std::shared_mutex on every access was pure overhead (~1-2% of
// self-time) and has been removed. Reintroduce locking here if a
// multi-threaded frontend ever shares a Cache instance across threads.
//
// NOTE: eviction is FIFO by design, not LRU. No get* accessor ever promotes
// an entry in `lru` (only put moves keys), so the list holds insertion
// order and eviction takes the oldest insert. Do not "fix" this to true
// LRU: eviction order determines when outstanding weak_ptrs expire.
template <typename K, typename V>
class Cache {
 public:
  Cache(size_t capacity) : capacity(capacity) {}
  // One hash probe per call (the find iterator is reused for every access);
  // behavior is identical to the previous multi-probe version.
  void put(const K& key, const V& value) {
    auto it = this->cache.find(key);
    if (it != this->cache.end()) {
      // hit and update
      this->lru.erase(it->second.second);
      this->lru.push_back(key);
      it->second.second = --this->lru.end();
      it->second.first = std::make_shared<V>(value);
    } else {
      // miss
      while (this->cache.size() >= this->capacity) {
        // evict
        auto victim = this->lru.front();
        this->cache.erase(victim);
        this->lru.pop_front();
      }
      // and put new
      this->lru.push_back(key);
      this->cache.emplace(
          key, std::make_pair(std::make_shared<V>(value), --this->lru.end()));
    }
  }

  // put() + get_locked() fused into a single probe pair: stores the value
  // and returns the stored pointer. Used on the raw-node miss path.
  std::shared_ptr<const V> put_locked(const K& key, const V& value) {
    this->put(key, value);
    return this->cache.find(key)->second.first;
  }

  // Rvalue overload for the raw-node miss path: moves the parsed message
  // into the cache (no protobuf deep copy) and returns the stored pointer
  // without a second probe. Same eviction order as put().
  std::shared_ptr<const V> put_locked(const K& key, V&& value) {
    auto it = this->cache.find(key);
    if (it != this->cache.end()) {
      // hit and update
      this->lru.erase(it->second.second);
      this->lru.push_back(key);
      it->second.second = --this->lru.end();
      it->second.first = std::make_shared<V>(std::move(value));
      return it->second.first;
    }
    // miss
    while (this->cache.size() >= this->capacity) {
      // evict
      auto victim = this->lru.front();
      this->cache.erase(victim);
      this->lru.pop_front();
    }
    this->lru.push_back(key);
    auto ptr = std::make_shared<V>(std::move(value));
    this->cache.emplace(key, std::make_pair(ptr, --this->lru.end()));
    return ptr;
  }
  bool has(const K& key) {
    return this->cache.find(key) != this->cache.end();
  }
  std::weak_ptr<const V> get(const K& key) {
    const auto it = this->cache.find(key);
    if (it == this->cache.end()) {
      throw std::runtime_error("Key not found in cache");
    }
    return std::weak_ptr<const V>(it->second.first);
  }
  std::shared_ptr<const V> get_locked(const K& key) {
    const auto it = this->cache.find(key);
    if (it == this->cache.end()) {
      throw std::runtime_error("Key not found in cache");
    }
    return it->second.first;
  }
  std::weak_ptr<const V> get_or_null(const K& key) {
    const auto it = this->cache.find(key);
    if (it == this->cache.end()) {
      return std::weak_ptr<V>();
    }
    return std::weak_ptr<const V>(it->second.first);
  }
  // Hot path: one hash probe per raw-node access (was find + at).
  std::shared_ptr<const V> get_or_null_locked(const K& key) {
    const auto it = this->cache.find(key);
    if (it == this->cache.end()) {
      return std::shared_ptr<const V>();
    }
    return it->second.first;
  }

  void remove(const K& key) {
    const auto it = this->cache.find(key);
    if (it == this->cache.end()) {
      throw std::runtime_error("Key not found in cache");
    }
    this->lru.erase(it->second.second);
    this->cache.erase(it);
  }

  ~Cache() {
    this->cache.clear();
    this->lru.clear();
  }

 private:
  size_t capacity;
  std::unordered_map<
      K,
      std::pair<std::shared_ptr<V>, typename std::list<K>::iterator>>
      cache;
  std::list<K> lru;
};

} // namespace FeederV3
} // namespace Chakra

#endif
