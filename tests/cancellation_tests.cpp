#include <atomic>
#include <iostream>
#include <stdexcept>
#include "environmental_grib/environment.h"
#include "environmental_grib/parallel.h"

int main() {
  namespace eg = environmental_grib;
  try {
    auto flag = std::make_shared<std::atomic<bool>>(true);
    eg::EnvironmentRequest request;
    request.execution.cancelled = flag;
    bool cancelled = false;
    try { eg::GenerateEnvironment(request); }
    catch (const std::runtime_error& error) {
      cancelled = std::string(error.what()) == "Generation cancelled";
    }
    if (!cancelled || eg::current_execution.cancelled)
      throw std::runtime_error("pre-cancelled job or context restoration failed");
    flag->store(false);
    {
      eg::ExecutionScope scope({flag, "test-ca.pem"});
      const auto values = eg::ParallelMapOrdered(std::vector<int>{1,2,3}, 2,
          [flag](int value) {
            if (eg::current_execution.cancelled != flag ||
                eg::current_execution.ca_bundle != "test-ca.pem")
              throw std::runtime_error("worker did not inherit job context");
            return value * value;
          });
      if (values != std::vector<int>({1,4,9})) throw std::runtime_error("parallel results changed");
      flag->store(true);
      std::atomic<int> calls{0};
      cancelled = false;
      try { eg::ParallelMapOrdered(std::vector<int>{1,2,3}, 2,
          [&](int value) { ++calls; return value; }); }
      catch (const std::runtime_error&) { cancelled = true; }
      if (!cancelled || calls.load()) throw std::runtime_error("cancelled worker performed work");
    }
    if (eg::current_execution.cancelled) throw std::runtime_error("context leaked into another job");
    std::cout << "Job cancellation and context isolation passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
