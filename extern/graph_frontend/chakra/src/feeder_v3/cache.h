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
template <typename K, typename V>
class Cache {
 public:
  Cache(size_t capacity) : capacity(capacity) {}
  void put(const K& key, const V& value) {
    if (this->cache.find(key) != this->cache.end()) {
      // hit and update
      this->lru.erase(this->cache[key].second);
      this->lru.push_back(key);
      this->cache[key].second = --this->lru.end();
      this->cache[key].first = std::make_shared<V>(value);
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
      this->cache[key] =
          std::make_pair(std::make_shared<V>(value), --this->lru.end());
    }
  }
  bool has(const K& key) {
    return this->cache.find(key) != this->cache.end();
  }
  std::weak_ptr<const V> get(const K& key) {
    if (this->cache.find(key) == this->cache.end()) {
      throw std::runtime_error("Key not found in cache");
    }
    std::weak_ptr<const V> value(this->cache.at(key).first);
    return value;
  }
  std::shared_ptr<const V> get_locked(const K& key) {
    if (this->cache.find(key) == this->cache.end()) {
      throw std::runtime_error("Key not found in cache");
    }
    return this->cache.at(key).first;
  }
  std::weak_ptr<const V> get_or_null(const K& key) {
    if (this->cache.find(key) == this->cache.end()) {
      return std::weak_ptr<V>();
    }
    std::weak_ptr<const V> value(this->cache.at(key).first);
    return value;
  }
  std::shared_ptr<const V> get_or_null_locked(const K& key) {
    if (this->cache.find(key) == this->cache.end()) {
      return std::shared_ptr<const V>();
    }
    return this->cache.at(key).first;
  }

  void remove(const K& key) {
    if (this->cache.find(key) == this->cache.end()) {
      throw std::runtime_error("Key not found in cache");
    }
    this->lru.erase(this->cache[key].second);
    this->cache.erase(key);
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
