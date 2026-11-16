#pragma once
#include <atomic>
#include <mutex>
namespace printsphere {
// Serialize task creation and publication: another caller may only observe a
// successful reservation after the create call has actually succeeded.
template<class Create> bool start_task_once(std::atomic<bool>& reserved, std::mutex& mutex, Create create) {
  std::lock_guard<std::mutex> lock(mutex);
  if(reserved.load()) return true;
  if(!create()) return false;
  reserved.store(true);
  return true;
}
}
