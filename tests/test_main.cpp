#include "harness.hpp"

#include <cstdio>

int main() {
    int passed = 0;
    std::vector<std::string> failures;

    for (const auto& c : tbtest::registry()) {
        try {
            c.fn();
            ++passed;
        } catch (const tbtest::Failure& f) {
            failures.push_back(c.name + "\n    " + f.message);
        } catch (const std::exception& e) {
            failures.push_back(c.name + "\n    unexpected exception: " + e.what());
        } catch (...) {
            failures.push_back(c.name + "\n    unexpected non-standard exception");
        }
    }

    for (const auto& f : failures) std::printf("FAIL %s\n", f.c_str());
    std::printf("\n%d passed, %zu failed (%zu total)\n", passed, failures.size(),
                tbtest::registry().size());
    return failures.empty() ? 0 : 1;
}
