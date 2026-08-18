#include <gtest/gtest.h>

#include <support/FileSha256.h>
#include <support/FlatJson.h>

#include <filesystem>
#include <fstream>

using namespace EchoRadar;

TEST(SupportUtilities, ComputesStandardSha256Vector) {
    const auto path = std::filesystem::temp_directory_path() /
        "echoradar-sha256-vector.txt";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "abc";
    }
    bool ok = false;
    EXPECT_EQ(
        ComputeFileSha256(path, &ok),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_TRUE(ok);
    std::filesystem::remove(path);
}

TEST(SupportUtilities, ParsesEscapedFlatJsonValues) {
    const auto values = detail::ParseFlatJson(
        R"({"name":"Echo\nRadar","enabled":true,"count":4})");
    EXPECT_EQ(detail::GetStr(values, "name"), "Echo\nRadar");
    EXPECT_TRUE(detail::GetBoolVal(values, "enabled"));
    EXPECT_EQ(detail::GetU64(values, "count"), 4u);
}
