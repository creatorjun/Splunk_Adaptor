// tests/RelayLogPolicyTest.cpp
#include "TestSupport.hpp"
#include "domain/RelayLogPolicy.hpp"

#include <string_view>

namespace {

using namespace monitor;

void feed(RelayLogSelection& selection, std::string_view text) {
    for (const char value : text) selection.consume(value);
}

void selection_policy() {
    RelayLogSelection selection;
    CHECK(RelayLogSelection::accepts_level(RelayLogLevel::error));
    CHECK(!RelayLogSelection::accepts_level(RelayLogLevel::other));
    feed(selection, "Connection refused");
    CHECK(selection.matches(RelayLogLevel::error));
    CHECK(!selection.matches(RelayLogLevel::other));
    selection.reset();
    feed(selection, "disk failure");
    CHECK(!selection.matches(RelayLogLevel::error));
    selection.reset();
    feed(selection, "prefix coCONNE");
    CHECK(!selection.matches(RelayLogLevel::error));
    feed(selection, "CTION suffix");
    CHECK(selection.matches(RelayLogLevel::error));
    selection.reset();
    feed(selection, "connec");
    selection.reset();
    feed(selection, "tion");
    CHECK(!selection.matches(RelayLogLevel::error));
    selection.reset();
    for (unsigned int index = 0; index < 32768; ++index) selection.consume('x');
    feed(selection, "cOnNeCtIoN");
    CHECK(selection.matches(RelayLogLevel::error));
    CHECK(evaluate_relay_log(true, true, true) == RelayLogStatus::warning);
    CHECK(evaluate_relay_log(false, true, true) == RelayLogStatus::error);
    CHECK(evaluate_relay_log(false, false, true) == RelayLogStatus::partial);
    CHECK(evaluate_relay_log(false, false, false) == RelayLogStatus::ok);
}

}

int main() {
    return test::run(selection_policy);
}
