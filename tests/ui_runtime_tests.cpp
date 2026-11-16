#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
#include "printsphere/latest_mailbox.hpp"
#include "printsphere/ui_performance.hpp"

int main() {
  using namespace printsphere;
  LatestMailbox<int> mailbox;
  auto old = std::make_shared<const int>(1);
  std::weak_ptr<const int> weak = old;
  mailbox.publish(old);
  old.reset();
  mailbox.publish(std::make_shared<const int>(2));
  assert(weak.expired()); // replaced pending history is released
  auto displayed = mailbox.take();
  assert(displayed && *displayed == 2 && !mailbox.take());
  mailbox.publish(std::make_shared<const int>(3));
  assert(*displayed == 2 && *mailbox.peek() == 3); // immutable consumer lifetime
  mailbox.take();
  std::atomic<bool> finished{false};
  std::thread producer([&] {
    for (int i = 1; i <= 10000; ++i) mailbox.publish(std::make_shared<const int>(i));
    finished = true;
  });
  int last = 0;
  while (!finished.load() || mailbox.peek()) {
    if (auto value = mailbox.take()) { assert(*value >= last); last = *value; }
    std::this_thread::yield();
  }
  producer.join();
  assert(last == 10000);

  UiTimingAccumulator histogram;
  assert(histogram.summary().samples == 0);
  for (int i = 0; i < 95; ++i) histogram.add(1000);
  for (int i = 0; i < 5; ++i) histogram.add(9000);
  auto report = histogram.summary();
  assert(report.samples == 100 && report.mean_us == 1400);
  assert(report.p95_upper_us == 1000 && report.max_us == 9000);
  histogram.add(9000);
  assert(histogram.summary().p95_upper_us == 9000);
  histogram = {};
  histogram.add(UINT64_MAX);
  assert(histogram.summary().max_us == UINT32_MAX);
  std::cout << "UI latest-value ownership/coalescing/concurrency and timing aggregation passed\n";
}
