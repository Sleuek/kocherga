// This software is distributed under the terms of the MIT License.
// Copyright (c) 2020 Zubax Robotics.
// Author: Pavel Kirienko <pavel.kirienko@zubax.com>

#include "util.hpp"
#include "catch.hpp"

TEST_CASE("util::makeHexDump")
{
    REQUIRE("00000000  31 32 33                                          123             " ==
            util::makeHexDump(std::string("123")));

    REQUIRE("00000000  30 31 32 33 34 35 36 37  38 39 61 62 63 64 65 66  0123456789abcdef\n"
            "00000010  67 68 69 6a 6b 6c 6d 6e  6f 70 71 72 73 74 75 76  ghijklmnopqrstuv\n"
            "00000020  77 78 79 7a 41 42 43 44  45 46 47 48 49 4a 4b 4c  wxyzABCDEFGHIJKL\n"
            "00000030  4d 4e 4f 50 51 52 53 54  55 56 57 58 59 5a        MNOPQRSTUVWXYZ  " ==
            util::makeHexDump(std::string("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")));
}
