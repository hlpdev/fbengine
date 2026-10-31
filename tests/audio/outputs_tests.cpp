// The names outputs are listed and chosen by: two of one name, as two of one model of headphones
// have, are each their own.

#include "audio/outputs.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using names_t = std::vector<std::string>;

TEST_CASE("OutputsOfOneNameAreNumberedFromTheSecond") {
    CHECK(fbe::audio::output_names({ "Speakers", "Headphones", "Speakers", "Speakers" }) ==
          names_t{ "Speakers", "Headphones", "Speakers (2)", "Speakers (3)" });
    CHECK(fbe::audio::output_names({ "Speakers" }) == names_t{ "Speakers" });
    CHECK(fbe::audio::output_names({}).empty());
}

// A number an output's own name has already, before it or after, is skipped.
TEST_CASE("ANumberedNameIsNeverAnotherOutputs") {
    CHECK(fbe::audio::output_names({ "Speakers (2)", "Speakers", "Speakers" }) ==
          names_t{ "Speakers (2)", "Speakers", "Speakers (3)" });
    CHECK(fbe::audio::output_names({ "Speakers", "Speakers", "Speakers (2)" }) ==
          names_t{ "Speakers", "Speakers (3)", "Speakers (2)" });
}
