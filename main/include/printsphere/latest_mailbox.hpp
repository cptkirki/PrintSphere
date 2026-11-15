#pragma once
#include <memory>
#include <mutex>
#include <utility>

namespace printsphere {
// One immutable pending value. Construct/copy outside the mutex; obsolete
// values are destroyed after unlocking. A slow renderer never queues history.
template <class T> class LatestMailbox {
 public:
  void publish(std::shared_ptr<const T> value) {
    { std::lock_guard<std::mutex> lock(mutex_); value.swap(value_); }
  }
  std::shared_ptr<const T> take() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(value_, {});
  }
  std::shared_ptr<const T> peek() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return value_;
  }
 private:
  mutable std::mutex mutex_;
  std::shared_ptr<const T> value_;
};
}  // namespace printsphere
