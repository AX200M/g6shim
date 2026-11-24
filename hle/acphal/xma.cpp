// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xma.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

namespace g6shim::acphal::xma {

namespace {

struct Pair {
    std::uint16_t symbol;
    std::uint8_t length;
};

inline constexpr std::uint16_t kCriticalFrequencies[28] = {
    100, 200, 300, 400, 510, 630, 770, 920,
    1080, 1270, 1480, 1720, 2000, 2320, 2700, 3150,
    3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500,
    20675, 28575, 41375, 63875,
};

inline constexpr Pair kScale[121] = {
    {58, 5}, {64, 6}, {66, 7}, {65, 7}, {62, 5}, {63, 6}, {68, 9}, {69, 10},
    {54, 15}, {19, 19}, {20, 19}, {21, 19}, {22, 19}, {23, 19}, {24, 19}, {25, 19},
    {26, 19}, {27, 19}, {28, 19}, {29, 19}, {30, 19}, {31, 19}, {32, 19}, {33, 19},
    {34, 19}, {17, 19}, {36, 19}, {37, 19}, {38, 19}, {39, 19}, {40, 19}, {41, 19},
    {42, 19}, {43, 19}, {44, 19}, {45, 19}, {46, 19}, {47, 19}, {48, 19}, {49, 19},
    {50, 19}, {51, 19}, {52, 19}, {15, 19}, {16, 19}, {14, 19}, {13, 19}, {12, 19},
    {11, 19}, {10, 19}, {0, 19}, {9, 19}, {8, 19}, {7, 19}, {6, 19}, {5, 19},
    {4, 19}, {55, 13}, {70, 13}, {3, 19}, {2, 19}, {1, 19}, {35, 19}, {71, 19},
    {72, 19}, {73, 19}, {74, 19}, {75, 19}, {76, 19}, {77, 19}, {78, 19}, {79, 19},
    {80, 19}, {81, 19}, {82, 19}, {83, 19}, {84, 19}, {85, 19}, {86, 19}, {87, 19},
    {88, 19}, {89, 19}, {90, 19}, {91, 19}, {92, 19}, {93, 19}, {94, 19}, {95, 19},
    {96, 19}, {97, 19}, {98, 19}, {99, 19}, {100, 19}, {101, 19}, {102, 19}, {103, 19},
    {104, 19}, {105, 19}, {106, 19}, {107, 19}, {108, 19}, {109, 19}, {110, 19}, {111, 19},
    {112, 19}, {113, 19}, {114, 19}, {115, 19}, {116, 19}, {117, 19}, {118, 19}, {119, 19},
    {120, 19}, {18, 18}, {53, 16}, {56, 11}, {57, 8}, {67, 7}, {61, 3}, {59, 2},
    {60, 1},
};

inline constexpr Pair kScaleRunLevel[120] = {
    {103, 7}, {80, 11}, {60, 11}, {18, 10}, {56, 10}, {21, 12}, {90, 12}, {58, 11},
    {27, 11}, {69, 12}, {84, 15}, {48, 15}, {86, 14}, {47, 13}, {19, 10}, {32, 9},
    {78, 6}, {5, 5}, {28, 4}, {53, 5}, {9, 7}, {31, 8}, {38, 8}, {10, 7},
    {88, 11}, {25, 12}, {105, 12}, {118, 11}, {23, 12}, {82, 14}, {98, 16}, {110, 16},
    {108, 15}, {93, 13}, {68, 10}, {72, 12}, {97, 12}, {81, 12}, {42, 12}, {64, 8},
    {4, 4}, {1, 2}, {7, 6}, {14, 7}, {0, 9}, {55, 9}, {61, 9}, {117, 10},
    {24, 12}, {44, 12}, {67, 12}, {70, 16}, {99, 18}, {96, 21}, {95, 21}, {2, 21},
    {77, 21}, {52, 21}, {111, 21}, {102, 20}, {101, 17}, {46, 15}, {73, 15}, {109, 15},
    {51, 14}, {92, 14}, {30, 7}, {11, 7}, {66, 7}, {15, 8}, {16, 8}, {116, 9},
    {65, 9}, {57, 10}, {59, 10}, {115, 9}, {12, 7}, {35, 9}, {17, 9}, {41, 9},
    {20, 11}, {91, 11}, {26, 12}, {75, 15}, {45, 15}, {107, 14}, {83, 14}, {100, 15},
    {89, 15}, {43, 11}, {62, 9}, {37, 9}, {104, 8}, {6, 5}, {39, 8}, {40, 9},
    {34, 9}, {79, 7}, {8, 6}, {63, 6}, {87, 12}, {94, 14}, {49, 14}, {50, 13},
    {22, 11}, {119, 10}, {33, 9}, {36, 9}, {113, 11}, {106, 12}, {112, 13}, {71, 15},
    {85, 15}, {74, 14}, {76, 10}, {114, 7}, {29, 5}, {54, 6}, {13, 6}, {3, 2},
};

inline constexpr std::uint8_t kScaleRunLevelRun[120] = {
    0, 0, 0, 0, 1, 2, 3, 4,
    5, 6, 7, 8, 9, 10, 11, 12,
    13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 0, 1, 2, 3,
    4, 5, 6, 7, 8, 9, 10, 11,
    12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 0, 1, 2,
    3, 4, 5, 6, 7, 8, 9, 10,
    11, 12, 13, 14, 15, 16, 17, 18,
    19, 20, 21, 22, 23, 24, 0, 1,
    2, 3, 4, 5, 6, 7, 8, 9,
    10, 11, 12, 13, 14, 15, 16, 17,
    18, 19, 20, 21, 22, 23, 24, 0,
    1, 2, 3, 4, 5, 6, 7, 8,
    9, 10, 0, 1, 0, 1, 0, 1,
};

inline constexpr std::uint8_t kScaleRunLevelLevel[120] = {
    0, 0, 1, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 6,
    6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 7, 7, 8, 8, 9, 9,
};

inline constexpr std::uint8_t kCoef0Lengths[272] = {
    2, 9, 14, 14, 13, 12, 13, 14,
    15, 15, 12, 10, 10, 10, 13, 14,
    15, 15, 12, 11, 13, 14, 14, 13,
    15, 15, 14, 12, 12, 8, 10, 10,
    15, 15, 14, 13, 14, 14, 13, 15,
    20, 20, 19, 21, 21, 20, 19, 17,
    17, 18, 18, 15, 15, 13, 12, 14,
    15, 15, 14, 15, 15, 12, 11, 6,
    7, 8, 9, 13, 13, 13, 14, 14,
    11, 10, 7, 8, 14, 14, 14, 14,
    12, 13, 13, 12, 12, 12, 11, 9,
    13, 14, 14, 12, 11, 11, 11, 9,
    8, 7, 14, 15, 15, 14, 14, 12,
    13, 15, 16, 17, 17, 14, 12, 12,
    12, 15, 15, 14, 14, 14, 13, 13,
    9, 9, 11, 11, 10, 7, 6, 13,
    15, 15, 14, 14, 14, 13, 14, 15,
    15, 13, 14, 14, 14, 14, 10, 9,
    10, 10, 11, 11, 10, 8, 9, 13,
    14, 14, 12, 11, 14, 15, 15, 13,
    12, 14, 14, 14, 14, 13, 14, 14,
    3, 5, 8, 10, 10, 15, 15, 14,
    14, 16, 16, 15, 12, 11, 11, 11,
    7, 8, 8, 9, 12, 13, 13, 12,
    14, 15, 15, 13, 10, 11, 11, 13,
    14, 14, 13, 14, 14, 11, 10, 13,
    15, 15, 14, 12, 11, 4, 6, 6,
    8, 12, 12, 12, 13, 13, 12, 13,
    13, 14, 14, 13, 13, 13, 9, 7,
    9, 11, 14, 14, 13, 14, 14, 13,
    10, 8, 7, 5, 9, 12, 13, 14,
    15, 15, 12, 12, 10, 14, 14, 13,
    12, 13, 14, 14, 12, 13, 13, 12,
    12, 12, 9, 7, 6, 3, 4, 4,
};

inline constexpr std::uint16_t kCoef0Symbols[272] = {
    2, 25, 111, 94, 69, 58, 87, 93,
    136, 135, 59, 37, 34, 36, 82, 182,
    120, 138, 195, 45, 168, 216, 178, 86,
    140, 219, 186, 162, 239, 18, 156, 35,
    127, 236, 109, 85, 180, 253, 88, 147,
    268, 264, 256, 266, 270, 262, 260, 248,
    246, 252, 258, 137, 189, 230, 64, 179,
    146, 208, 101, 118, 238, 163, 46, 9,
    153, 0, 26, 247, 169, 76, 202, 131,
    194, 38, 13, 19, 132, 106, 191, 97,
    65, 198, 77, 62, 66, 164, 48, 27,
    81, 183, 102, 60, 47, 49, 159, 227,
    20, 14, 112, 263, 144, 217, 104, 63,
    79, 209, 269, 250, 254, 203, 241, 196,
    61, 220, 148, 124, 185, 100, 80, 78,
    193, 28, 50, 235, 41, 1, 10, 171,
    226, 150, 103, 114, 115, 170, 105, 211,
    149, 249, 108, 188, 107, 255, 231, 155,
    42, 40, 55, 160, 39, 21, 29, 215,
    234, 184, 228, 51, 116, 142, 145, 172,
    165, 181, 130, 113, 117, 89, 128, 204,
    3, 7, 154, 157, 43, 141, 265, 133,
    225, 271, 244, 221, 74, 54, 56, 52,
    15, 222, 22, 30, 83, 199, 173, 73,
    123, 210, 143, 175, 44, 53, 237, 174,
    139, 134, 110, 218, 129, 161, 213, 177,
    267, 151, 125, 67, 223, 5, 11, 192,
    23, 214, 243, 166, 200, 176, 68, 224,
    187, 257, 261, 232, 96, 251, 31, 16,
    32, 57, 207, 121, 91, 126, 119, 99,
    158, 24, 212, 8, 33, 70, 92, 205,
    240, 242, 75, 197, 233, 259, 190, 98,
    71, 201, 122, 206, 72, 90, 95, 84,
    167, 245, 229, 17, 12, 4, 152, 6,
};

inline constexpr Pair kCoef1[244] = {
    {2, 2}, {3, 3}, {102, 3}, {4, 4}, {148, 6}, {134, 9}, {171, 10}, {18, 10},
    {11, 8}, {159, 8}, {14, 9}, {156, 14}, {235, 15}, {61, 15}, {38, 13}, {153, 13},
    {48, 14}, {49, 14}, {23, 11}, {203, 13}, {208, 19}, {204, 19}, {129, 18}, {94, 17},
    {87, 16}, {62, 15}, {174, 15}, {147, 15}, {29, 12}, {191, 12}, {64, 15}, {65, 15},
    {146, 14}, {164, 13}, {142, 5}, {132, 4}, {103, 5}, {154, 7}, {165, 9}, {181, 11},
    {109, 12}, {30, 12}, {86, 16}, {92, 16}, {239, 15}, {138, 14}, {39, 13}, {50, 14},
    {115, 15}, {238, 21}, {228, 21}, {236, 21}, {222, 21}, {216, 20}, {226, 20}, {196, 18},
    {192, 17}, {120, 16}, {221, 14}, {51, 14}, {24, 11}, {143, 8}, {7, 6}, {9, 7},
    {152, 10}, {136, 12}, {160, 12}, {241, 15}, {66, 15}, {168, 14}, {219, 14}, {113, 14},
    {193, 12}, {19, 10}, {173, 10}, {105, 8}, {149, 9}, {15, 9}, {205, 13}, {207, 13},
    {125, 17}, {190, 17}, {182, 16}, {68, 15}, {70, 15}, {67, 15}, {137, 13}, {31, 12},
    {223, 14}, {116, 15}, {210, 19}, {220, 19}, {198, 18}, {126, 17}, {88, 16}, {41, 13},
    {25, 11}, {40, 13}, {73, 15}, {243, 15}, {53, 14}, {195, 12}, {183, 11}, {225, 14},
    {52, 14}, {71, 15}, {121, 16}, {89, 16}, {170, 14}, {55, 14}, {69, 15}, {83, 15},
    {209, 13}, {108, 11}, {32, 12}, {54, 14}, {122, 16}, {184, 16}, {176, 15}, {42, 13},
    {12, 8}, {161, 8}, {6, 5}, {167, 9}, {106, 9}, {20, 10}, {145, 12}, {111, 13},
    {43, 13}, {26, 11}, {175, 10}, {107, 10}, {34, 12}, {33, 12}, {197, 12}, {74, 15},
    {128, 17}, {232, 20}, {212, 20}, {224, 19}, {202, 18}, {90, 16}, {57, 14}, {227, 14},
    {97, 16}, {93, 16}, {140, 15}, {185, 11}, {27, 11}, {16, 9}, {158, 11}, {211, 13},
    {56, 14}, {117, 15}, {72, 15}, {166, 13}, {91, 16}, {95, 16}, {80, 15}, {101, 16},
    {194, 17}, {127, 17}, {82, 15}, {21, 10}, {144, 10}, {177, 10}, {151, 6}, {10, 7},
    {157, 7}, {8, 6}, {5, 4}, {13, 8}, {0, 9}, {213, 13}, {46, 13}, {199, 12},
    {35, 12}, {162, 12}, {135, 10}, {169, 9}, {45, 13}, {59, 14}, {114, 14}, {44, 13},
    {188, 16}, {186, 16}, {75, 15}, {79, 15}, {118, 15}, {187, 11}, {112, 13}, {139, 14},
    {178, 15}, {81, 15}, {110, 12}, {28, 11}, {163, 8}, {133, 6}, {104, 6}, {17, 9},
    {22, 10}, {229, 14}, {172, 14}, {217, 13}, {201, 12}, {36, 12}, {218, 20}, {242, 22},
    {240, 22}, {234, 21}, {230, 19}, {206, 18}, {200, 18}, {214, 18}, {130, 17}, {131, 17},
    {141, 15}, {84, 15}, {76, 15}, {215, 13}, {58, 14}, {231, 14}, {233, 14}, {180, 15},
    {77, 15}, {37, 12}, {189, 11}, {179, 10}, {155, 10}, {47, 13}, {96, 16}, {99, 16},
    {119, 15}, {63, 14}, {237, 14}, {78, 15}, {85, 15}, {60, 14}, {98, 16}, {100, 16},
    {124, 16}, {123, 16}, {150, 11}, {1, 7},
};

inline constexpr std::uint16_t kCoef0Run[272] = {
    0, 0, 0, 1, 2, 3, 4, 5,
    6, 7, 8, 9, 10, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 26, 27, 28, 29,
    30, 31, 32, 33, 34, 35, 36, 37,
    38, 39, 40, 41, 42, 43, 44, 45,
    46, 47, 48, 49, 50, 51, 52, 53,
    54, 55, 56, 57, 58, 59, 60, 61,
    62, 63, 64, 65, 66, 67, 68, 69,
    70, 71, 72, 73, 74, 75, 76, 77,
    78, 79, 80, 81, 82, 83, 84, 85,
    86, 87, 88, 89, 90, 91, 92, 93,
    94, 95, 96, 97, 98, 99, 100, 101,
    102, 103, 104, 105, 106, 107, 108, 109,
    110, 111, 112, 113, 114, 115, 116, 117,
    118, 119, 120, 121, 122, 123, 124, 125,
    126, 127, 128, 129, 130, 131, 132, 133,
    134, 135, 136, 137, 138, 139, 140, 141,
    142, 143, 144, 145, 146, 147, 148, 149,
    0, 1, 2, 3, 4, 5, 6, 7,
    8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39,
    0, 1, 2, 3, 4, 5, 6, 7,
    8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 0, 1, 2, 3,
    4, 5, 6, 7, 8, 9, 0, 1,
    2, 3, 4, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0,
};

inline constexpr float kCoef0Level[272] = {
    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f,
    2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f,
    2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f,
    2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f,
    2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f,
    3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f,
    3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f,
    3.0f, 3.0f, 3.0f, 3.0f, 4.0f, 4.0f, 4.0f, 4.0f,
    4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 5.0f, 5.0f,
    5.0f, 5.0f, 5.0f, 6.0f, 6.0f, 7.0f, 7.0f, 8.0f,
    8.0f, 9.0f, 9.0f, 10.0f, 10.0f, 11.0f, 11.0f, 12.0f,
    12.0f, 13.0f, 13.0f, 14.0f, 14.0f, 15.0f, 15.0f, 16.0f,
    16.0f, 17.0f, 17.0f, 18.0f, 18.0f, 19.0f, 19.0f, 20.0f,
    20.0f, 21.0f, 21.0f, 22.0f, 22.0f, 23.0f, 23.0f, 24.0f,
    24.0f, 25.0f, 25.0f, 26.0f, 26.0f, 27.0f, 27.0f, 28.0f,
};

inline constexpr std::uint16_t kCoef1Run[244] = {
    0, 0, 0, 1, 2, 3, 4, 5,
    6, 7, 8, 9, 10, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 26, 27, 28, 29,
    30, 31, 32, 33, 34, 35, 36, 37,
    38, 39, 40, 41, 42, 43, 44, 45,
    46, 47, 48, 49, 50, 51, 52, 53,
    54, 55, 56, 57, 58, 59, 60, 61,
    62, 63, 64, 65, 66, 67, 68, 69,
    70, 71, 72, 73, 74, 75, 76, 77,
    78, 79, 80, 81, 82, 83, 84, 85,
    86, 87, 88, 89, 90, 91, 92, 93,
    94, 95, 96, 97, 98, 99, 0, 1,
    2, 3, 4, 5, 6, 7, 8, 9,
    10, 11, 12, 13, 14, 15, 16, 17,
    18, 19, 20, 21, 22, 23, 24, 25,
    26, 27, 28, 29, 0, 1, 2, 3,
    4, 5, 6, 7, 8, 9, 0, 1,
    2, 3, 4, 5, 0, 1, 2, 0,
    1, 2, 0, 1, 2, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 0, 0,
};

inline constexpr float kCoef1Level[244] = {
    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 2.0f, 2.0f,
    2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f,
    2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f,
    2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f,
    2.0f, 2.0f, 2.0f, 2.0f, 3.0f, 3.0f, 3.0f, 3.0f,
    3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 4.0f, 4.0f,
    4.0f, 4.0f, 4.0f, 4.0f, 5.0f, 5.0f, 5.0f, 6.0f,
    6.0f, 6.0f, 7.0f, 7.0f, 7.0f, 8.0f, 8.0f, 9.0f,
    9.0f, 10.0f, 10.0f, 11.0f, 11.0f, 12.0f, 12.0f, 13.0f,
    13.0f, 14.0f, 14.0f, 15.0f, 15.0f, 16.0f, 16.0f, 17.0f,
    17.0f, 18.0f, 18.0f, 19.0f, 19.0f, 20.0f, 20.0f, 21.0f,
    21.0f, 22.0f, 22.0f, 23.0f, 23.0f, 24.0f, 24.0f, 25.0f,
    25.0f, 26.0f, 26.0f, 27.0f, 27.0f, 28.0f, 28.0f, 29.0f,
    29.0f, 30.0f, 30.0f, 31.0f, 31.0f, 32.0f, 32.0f, 33.0f,
    33.0f, 34.0f, 34.0f, 35.0f, 35.0f, 36.0f, 36.0f, 37.0f,
    37.0f, 38.0f, 38.0f, 39.0f, 39.0f, 40.0f, 40.0f, 41.0f,
    41.0f, 42.0f, 42.0f, 43.0f, 43.0f, 44.0f, 44.0f, 45.0f,
    45.0f, 46.0f, 46.0f, 47.0f, 47.0f, 48.0f, 48.0f, 49.0f,
    49.0f, 50.0f, 51.0f, 52.0f,
};

inline constexpr std::uint8_t kVec4Lengths[127] = {
    1, 6, 8, 10, 10, 10, 10, 8,
    8, 10, 10, 9, 8, 8, 9, 12,
    12, 11, 12, 12, 11, 9, 9, 8,
    8, 9, 9, 8, 8, 9, 9, 12,
    12, 12, 14, 14, 13, 11, 11, 9,
    8, 9, 9, 11, 11, 10, 9, 8,
    6, 6, 6, 6, 6, 6, 11, 11,
    10, 11, 11, 10, 10, 11, 11, 9,
    7, 6, 7, 7, 6, 6, 6, 5,
    7, 11, 11, 10, 9, 8, 6, 9,
    9, 10, 10, 9, 8, 8, 6, 6,
    6, 8, 8, 9, 12, 12, 11, 10,
    8, 8, 8, 10, 10, 10, 10, 9,
    9, 8, 10, 11, 11, 9, 8, 8,
    8, 9, 9, 10, 11, 12, 12, 9,
    9, 9, 8, 8, 7, 7, 7,
};

inline constexpr std::uint16_t kVec4Symbols[127] = {
    0, 4370, 275, 8195, 4146, 12545, 8225, 290,
    4625, 515, 20, 8706, 8210, 4355, 4131, 16385,
    5121, 8961, 321, 1041, 51, 4641, 546, 4610,
    530, 513, 8451, 4385, 4130, 33, 8211, 5,
    66, 4161, 1281, 81, 6, 801, 8196, 8481,
    8449, 4611, 531, 561, 769, 12290, 8226, 19,
    4097, 2, 4369, 274, 4354, 4114, 12291, 16641,
    12305, 49, 12321, 260, 4100, 516, 21, 12546,
    8466, 4353, 4371, 4626, 257, 18, 17, 1,
    4386, 8241, 771, 4865, 8705, 8194, 4098, 12561,
    276, 50, 785, 4116, 8209, 4099, 273, 4113,
    258, 259, 4609, 35, 1026, 1025, 16401, 305,
    34, 529, 289, 770, 12289, 4, 4145, 4356,
    12306, 8193, 12801, 261, 16386, 4881, 3, 514,
    4129, 545, 306, 36, 4101, 65, 20481, 786,
    4401, 4866, 8721, 291, 8450, 8465, 4115,
};

inline constexpr Pair kVec2[137] = {
    {19, 5}, {165, 10}, {211, 11}, {46, 11}, {75, 10}, {177, 11}, {12, 11}, {86, 8},
    {83, 7}, {38, 7}, {133, 9}, {178, 10}, {28, 10}, {104, 9}, {73, 9}, {35, 5},
    {52, 6}, {113, 9}, {8, 9}, {101, 8}, {69, 7}, {0, 3}, {71, 8}, {119, 9},
    {91, 10}, {179, 10}, {114, 8}, {166, 10}, {10, 10}, {44, 10}, {145, 10}, {66, 6},
    {21, 6}, {24, 8}, {146, 9}, {26, 9}, {65, 7}, {5, 7}, {226, 11}, {225, 12},
    {15, 12}, {180, 10}, {147, 9}, {115, 8}, {40, 8}, {89, 9}, {134, 9}, {84, 7},
    {54, 7}, {42, 9}, {60, 10}, {31, 11}, {193, 11}, {181, 10}, {76, 10}, {148, 9},
    {37, 6}, {67, 6}, {33, 6}, {3, 6}, {17, 6}, {2, 6}, {102, 8}, {87, 8},
    {116, 8}, {56, 8}, {50, 5}, {20, 5}, {120, 9}, {58, 9}, {29, 10}, {194, 10},
    {135, 9}, {97, 8}, {7, 8}, {105, 9}, {13, 11}, {241, 12}, {16, 12}, {45, 10},
    {149, 9}, {74, 9}, {98, 7}, {23, 7}, {85, 7}, {70, 7}, {195, 10}, {161, 10},
    {129, 9}, {72, 8}, {51, 5}, {36, 5}, {117, 8}, {61, 10}, {11, 10}, {162, 9},
    {1, 7}, {4, 6}, {49, 6}, {68, 6}, {9, 9}, {27, 9}, {130, 8}, {39, 7},
    {53, 6}, {99, 7}, {25, 8}, {150, 9}, {90, 9}, {103, 8}, {163, 9}, {196, 10},
    {210, 10}, {136, 9}, {121, 9}, {41, 8}, {131, 8}, {43, 9}, {164, 9}, {118, 8},
    {88, 8}, {81, 7}, {6, 7}, {55, 7}, {59, 9}, {30, 10}, {209, 11}, {14, 11},
    {151, 9}, {106, 9}, {82, 6}, {22, 6}, {100, 7}, {132, 8}, {57, 8}, {18, 4},
    {34, 4},
};

inline constexpr Pair kVec1[101] = {
    {7, 5}, {32, 8}, {59, 10}, {60, 10}, {83, 11}, {82, 11}, {62, 10}, {33, 8},
    {45, 9}, {61, 10}, {84, 11}, {85, 11}, {1, 6}, {13, 5}, {19, 6}, {25, 7},
    {34, 8}, {46, 9}, {47, 9}, {14, 5}, {6, 5}, {64, 10}, {87, 11}, {86, 11},
    {63, 10}, {88, 11}, {90, 11}, {35, 8}, {26, 7}, {0, 7}, {48, 9}, {65, 10},
    {66, 10}, {36, 8}, {15, 5}, {20, 6}, {91, 11}, {89, 11}, {67, 10}, {49, 9},
    {50, 9}, {69, 10}, {92, 11}, {93, 11}, {27, 7}, {5, 5}, {37, 8}, {68, 10},
    {71, 10}, {51, 9}, {52, 9}, {70, 10}, {94, 11}, {96, 11}, {38, 8}, {21, 6},
    {16, 5}, {4, 5}, {28, 7}, {53, 9}, {95, 11}, {97, 11}, {73, 10}, {39, 8},
    {29, 7}, {72, 10}, {98, 11}, {99, 11}, {54, 9}, {40, 8}, {22, 6}, {30, 7},
    {55, 9}, {74, 10}, {76, 10}, {56, 9}, {75, 10}, {77, 10}, {17, 5}, {3, 5},
    {23, 6}, {41, 8}, {57, 9}, {78, 10}, {79, 10}, {31, 7}, {10, 4}, {9, 4},
    {100, 5}, {2, 5}, {11, 4}, {8, 4}, {18, 5}, {42, 8}, {58, 9}, {80, 10},
    {81, 10}, {43, 8}, {44, 8}, {24, 6}, {12, 4},
};

constexpr std::size_t kPacketBits = kPacketBytes * 8;
constexpr std::size_t kHeaderBits = 32;
constexpr std::uint32_t kLengthBits = 15;
constexpr std::size_t kMinSubframe = kSubframeSamples;
constexpr std::size_t kBlockSizes = 3;
constexpr std::size_t kMaxBands = 29;
constexpr std::size_t kMaxSubframes = 32;
constexpr std::uint32_t kScaleBase = 45;
constexpr std::size_t kScaleTableOffset = 60;
constexpr std::size_t kVectorTableOffset = 1;

std::uint32_t log2_size(std::size_t value) {
    return static_cast<std::uint32_t>(std::bit_width(static_cast<std::uint64_t>(value ? value : 1)) - 1);
}

struct Cursor {
    std::span<const std::uint8_t> data;
    std::size_t pos = 0;

    std::optional<std::uint32_t> bit() {
        if (pos % kPacketBits == 0) {
            const std::size_t previous = pos / kPacketBits;
            if (previous > 0) {
                const auto header = packet(data, previous - 1);
                if (!header) return std::nullopt;
                const std::uint64_t next = static_cast<std::uint64_t>(previous) + header->skip;
                if (next > SIZE_MAX / kPacketBits) return std::nullopt;
                pos = static_cast<std::size_t>(next * kPacketBits);
            }
            pos += kHeaderBits;
        }
        if (pos / 8 >= data.size()) return std::nullopt;
        const std::uint8_t byte = data[pos / 8];
        const std::uint32_t value = (byte >> (7 - pos % 8)) & 1u;
        ++pos;
        return value;
    }

    std::optional<std::uint32_t> bits(std::uint32_t count) {
        std::uint32_t value = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto next = bit();
            if (!next) return std::nullopt;
            value = (value << 1) | *next;
        }
        return value;
    }
};

struct Bits {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;
    std::size_t end = 0;

    std::uint32_t peek32() const {
        std::uint64_t value = 0;
        const std::size_t byte = pos / 8;
        for (std::size_t i = 0; i < 5; ++i)
            value = (value << 8) | (byte + i < size ? data[byte + i] : 0);
        return static_cast<std::uint32_t>((value << (24 + pos % 8)) >> 32);
    }

    std::uint32_t read(std::uint32_t count) {
        if (count == 0) return 0;
        const std::uint32_t value = peek32() >> (32 - count);
        pos += count;
        return value;
    }

    bool bit() { return read(1) != 0; }

    std::int32_t signed_value(std::uint32_t count) {
        const std::uint32_t value = read(count) << (32 - count);
        return std::bit_cast<std::int32_t>(value) >> (32 - count);
    }

    std::size_t consumed() const { return pos; }
    bool overread() const { return pos > end; }
};

struct Vlc {
    struct Code {
        std::uint32_t code = 0;
        std::uint8_t length = 0;
        std::int32_t symbol = 0;
    };
    std::vector<Code> codes;

    std::int32_t read(Bits& bits) const {
        const std::uint32_t value = bits.peek32();
        const auto at = std::upper_bound(codes.begin(), codes.end(), value,
                                         [](std::uint32_t v, const Code& c) { return v < c.code; });
        if (at == codes.begin()) return 0;
        const Code& code = *(at - 1);
        bits.pos += code.length;
        return code.symbol;
    }
};

Vlc make_vlc(const Pair* table, std::size_t count, std::int32_t offset) {
    Vlc vlc;
    vlc.codes.reserve(count);
    std::uint64_t code = 0;
    for (std::size_t i = 0; i < count; ++i) {
        vlc.codes.push_back(
            {static_cast<std::uint32_t>(code), table[i].length, static_cast<std::int32_t>(table[i].symbol) + offset});
        code += std::uint64_t{1} << (32 - table[i].length);
    }
    return vlc;
}

Vlc make_vlc(const std::uint16_t* symbols, const std::uint8_t* lengths, std::size_t count, std::int32_t offset) {
    Vlc vlc;
    vlc.codes.reserve(count);
    std::uint64_t code = 0;
    for (std::size_t i = 0; i < count; ++i) {
        vlc.codes.push_back(
            {static_cast<std::uint32_t>(code), lengths[i], static_cast<std::int32_t>(symbols[i]) + offset});
        code += std::uint64_t{1} << (32 - lengths[i]);
    }
    return vlc;
}

struct Codes {
    Vlc scale;
    Vlc scale_rl;
    std::array<Vlc, 2> coef;
    Vlc vec4;
    Vlc vec2;
    Vlc vec1;
};

const Codes& codes() {
    static const Codes table = [] {
        Codes c;
        c.scale = make_vlc(kScale, std::size(kScale), -static_cast<std::int32_t>(kScaleTableOffset));
        c.scale_rl = make_vlc(kScaleRunLevel, std::size(kScaleRunLevel), 0);
        c.coef[0] = make_vlc(kCoef0Symbols, kCoef0Lengths, std::size(kCoef0Symbols), 0);
        c.coef[1] = make_vlc(kCoef1, std::size(kCoef1), 0);
        c.vec4 = make_vlc(kVec4Symbols, kVec4Lengths, std::size(kVec4Symbols), -1);
        c.vec2 = make_vlc(kVec2, std::size(kVec2), -1);
        c.vec1 = make_vlc(kVec1, std::size(kVec1), 0);
        return c;
    }();
    return table;
}

std::uint32_t large_value(Bits& bits) {
    std::uint32_t count = 8;
    if (bits.bit()) {
        count += 8;
        if (bits.bit()) {
            count += 8;
            if (bits.bit()) count += 7;
        }
    }
    return bits.read(count);
}

void fft(std::vector<double>& re, std::vector<double>& im) {
    const std::size_t n = re.size();
    std::size_t j = 0;
    for (std::size_t i = 1; i < n; ++i) {
        std::size_t bit = n >> 1;
        while ((j & bit) != 0) {
            j ^= bit;
            bit >>= 1;
        }
        j |= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * std::numbers::pi / static_cast<double>(len);
        const double ws = std::sin(angle);
        const double wc = std::cos(angle);
        for (std::size_t start = 0; start < n; start += len) {
            double cr = 1.0;
            double ci = 0.0;
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::size_t a = start + k;
                const std::size_t b = a + len / 2;
                const double tr = re[b] * cr - im[b] * ci;
                const double ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] = re[a] + tr;
                im[a] = im[a] + ti;
                const double next_r = cr * wc - ci * ws;
                const double next_i = cr * ws + ci * wc;
                cr = next_r;
                ci = next_i;
            }
        }
    }
}

std::optional<std::size_t> next_frame(std::span<const std::uint8_t> data, std::size_t start, std::size_t end) {
    const std::size_t index = start / kPacketBits;
    const auto header = packet(data, index);
    if (!header) return std::nullopt;
    std::size_t at = index * kPacketBits + kHeaderBits + header->first_frame_bit;
    std::uint32_t number = 0;
    while (at < start) {
        Cursor cursor{data, at};
        const auto bits = cursor.bits(kLengthBits);
        if (!bits) return std::nullopt;
        if (*bits <= kLengthBits) return std::nullopt;
        at += *bits;
        ++number;
    }
    const bool last = number + 1 >= header->frames;
    std::size_t next = end;
    if (end % kPacketBits == 0 || (last && end / kPacketBits == index)) {
        const std::size_t previous = (end - 1) / kPacketBits;
        const auto previous_header = packet(data, previous);
        if (!previous_header) return std::nullopt;
        const std::size_t following = previous + 1 + previous_header->skip;
        const auto following_header = packet(data, following);
        if (!following_header) return std::nullopt;
        next = following * kPacketBits + kHeaderBits + following_header->first_frame_bit;
    }
    return (next / 8 < data.size()) ? std::optional<std::size_t>(next) : std::nullopt;
}

const std::array<std::vector<float>, 3>& windows() {
    static const std::array<std::vector<float>, 3> table = {sine_window(128), sine_window(256), sine_window(512)};
    return table;
}

std::int16_t to_i16(float value) {
    const float scaled = std::round(value * 32768.0f);
    return static_cast<std::int16_t>(static_cast<std::int32_t>(std::clamp(scaled, -32768.0f, 32767.0f)));
}

}  // namespace

std::optional<Packet> packet(std::span<const std::uint8_t> data, std::size_t index) {
    if (index > SIZE_MAX / kPacketBytes) return std::nullopt;
    const std::size_t start = index * kPacketBytes;
    if (start > data.size() || data.size() - start < 4) return std::nullopt;
    const std::uint32_t header = (static_cast<std::uint32_t>(data[start]) << 24) |
                                 (static_cast<std::uint32_t>(data[start + 1]) << 16) |
                                 (static_cast<std::uint32_t>(data[start + 2]) << 8) |
                                 static_cast<std::uint32_t>(data[start + 3]);
    return Packet{header >> 26, (header >> 11) & 0x7fff, header & 0xff};
}

std::optional<Frame> read_frame(std::span<const std::uint8_t> data, std::size_t start) {
    Cursor first{data, start};
    const auto length = first.bits(kLengthBits);
    if (!length) return std::nullopt;
    const std::size_t bits = *length;
    if (bits <= kLengthBits || bits == 0x7fff) return std::nullopt;
    std::vector<std::uint8_t> out((bits + 7) / 8, 0);
    Cursor cursor{data, start};
    for (std::size_t i = 0; i < bits; ++i) {
        const auto bit = cursor.bit();
        if (!bit) return std::nullopt;
        out[i / 8] = static_cast<std::uint8_t>(out[i / 8] | (static_cast<std::uint8_t>(*bit) << (7 - i % 8)));
    }
    return Frame{std::move(out), bits, start, next_frame(data, start, cursor.pos)};
}

void imdct_half(const float* input, std::size_t n, float* out, float scale) {
    const std::size_t m = n / 2;
    std::vector<double> re(m, 0.0);
    std::vector<double> im(m, 0.0);
    for (std::size_t k = 0; k < m; ++k) {
        const double a = input[2 * k];
        const double b = input[n - 1 - 2 * k];
        const double angle = -std::numbers::pi * static_cast<double>(k) / static_cast<double>(n);
        const double s = std::sin(angle);
        const double c = std::cos(angle);
        re[k] = a * c - b * s;
        im[k] = a * s + b * c;
    }
    fft(re, im);
    for (std::size_t k = 0; k < m; ++k) {
        const double angle = -std::numbers::pi * static_cast<double>(4 * k + 1) / static_cast<double>(4 * n);
        const double s = std::sin(angle);
        const double c = std::cos(angle);
        const double wr = re[k] * c - im[k] * s;
        const double wi = re[k] * s + im[k] * c;
        out[n - 1 - 2 * k] = static_cast<float>(wr) * scale;
        out[2 * k] = -static_cast<float>(wi) * scale;
    }
}

std::vector<float> sine_window(std::size_t n) {
    std::vector<float> window(n);
    for (std::size_t i = 0; i < n; ++i)
        window[i] = static_cast<float>(
            std::sin((static_cast<double>(i) + 0.5) * std::numbers::pi / (2.0 * static_cast<double>(n))));
    return window;
}

void window_overlap(float* block, std::size_t block_len, const std::vector<float>& window) {
    const std::size_t len = block_len / 2;
    for (std::size_t k = 0; k < len; ++k) {
        const std::size_t i = k;
        const std::size_t j = 2 * len - 1 - k;
        const float s0 = block[i];
        const float s1 = block[j];
        const float wi = window[i];
        const float wj = window[j];
        block[i] = s0 * wj - s1 * wi;
        block[j] = s0 * wi + s1 * wj;
    }
}

struct Decoder::Impl {
    struct Channel {
        std::size_t prev_block_len = kFrameSamples;
        bool transmit_coefs = false;
        std::size_t num_subframes = 0;
        std::array<std::size_t, kMaxSubframes> subframe_len{};
        std::size_t cur_subframe = 0;
        std::size_t decoded_samples = 0;
        std::int32_t quant_step = 0;
        bool reuse_sf = false;
        std::int32_t scale_factor_step = 1;
        std::int32_t max_scale_factor = 0;
        std::array<std::array<std::int32_t, kMaxBands>, 2> saved_scale_factors{};
        std::size_t scale_factor_idx = 0;
        std::size_t scale_factors = 0;
        std::size_t table_idx = 0;
        std::size_t num_vec_coeffs = 0;
        std::array<float, kFrameSamples> coeffs{};
        std::array<float, kFrameSamples + kFrameSamples / 2> out{};
    };

    std::size_t channels = 1;
    std::array<std::size_t, kBlockSizes> num_sfb{};
    std::array<std::array<std::size_t, kMaxBands + 1>, kBlockSizes> sfb_offsets{};
    std::array<std::array<std::array<std::size_t, kMaxBands>, kBlockSizes>, kBlockSizes> sf_offsets{};
    std::array<Channel, 2> ch{};
    std::size_t subframe_len = kFrameSamples;
    std::size_t channels_for_cur_subframe = 0;
    std::array<std::size_t, 2> channel_indexes{0, 1};
    std::size_t num_bands = 0;
    std::size_t table_idx = 0;
    std::uint32_t esc_len = 0;
    bool transmit_num_vec_coeffs = false;
    bool transform = false;
    std::array<bool, kMaxBands> transform_band{};
    bool parsed_all_subframes = false;

    Impl(std::size_t channel_count, std::uint32_t sample_rate) {
        channels = std::clamp<std::size_t>(channel_count, 1, 2);
        const std::uint32_t rate = sample_rate > 44100 ? 48000
                                   : sample_rate > 32000 ? 44100
                                   : sample_rate > 24000 ? 32000
                                                         : 24000;
        for (std::size_t i = 0; i < kBlockSizes; ++i) {
            const std::size_t block = kFrameSamples >> i;
            std::size_t band = 1;
            for (std::size_t f = 0; f < kMaxBands - 1; ++f) {
                if (sfb_offsets[i][band - 1] >= block) break;
                const std::size_t offset = ((block * 2 * kCriticalFrequencies[f]) / rate + 2) & ~std::size_t{3};
                if (offset > sfb_offsets[i][band - 1]) {
                    sfb_offsets[i][band] = offset;
                    ++band;
                }
                if (offset >= block) break;
            }
            sfb_offsets[i][band - 1] = block;
            num_sfb[i] = band - 1;
        }
        for (std::size_t i = 0; i < kBlockSizes; ++i) {
            for (std::size_t b = 0; b < num_sfb[i]; ++b) {
                const std::size_t offset = ((sfb_offsets[i][b] + sfb_offsets[i][b + 1] - 1) << i) >> 1;
                for (std::size_t x = 0; x < kBlockSizes; ++x) {
                    std::size_t v = 0;
                    while (v + 1 < kMaxBands && (sfb_offsets[x][v + 1] << x) < offset) ++v;
                    sf_offsets[i][x][b] = v;
                }
            }
        }
    }

    void reset_channels() {
        ch[0] = Channel{};
        ch[1] = Channel{};
    }

    bool decode(std::span<const std::uint8_t> data, std::size_t bits, std::vector<std::int16_t>& out) {
        Bits reader{data.data(), data.size(), 0, bits};
        const std::size_t len = reader.read(kLengthBits);
        const bool result = frame(reader, len);
        if (!result)
            for (std::size_t c = 0; c < channels; ++c) ch[c].out.fill(0.0f);
        for (std::size_t i = 0; i < kFrameSamples; ++i)
            for (std::size_t c = 0; c < channels; ++c) out.push_back(to_i16(ch[c].out[i]));
        for (std::size_t c = 0; c < channels; ++c)
            std::copy_n(ch[c].out.begin() + kFrameSamples, kFrameSamples / 2, ch[c].out.begin());
        return result;
    }

    void flush(std::vector<std::int16_t>& out) {
        for (std::size_t i = 0; i < kFrameSamples / 2; ++i)
            for (std::size_t c = 0; c < channels; ++c) out.push_back(to_i16(ch[c].out[i]));
        reset_channels();
    }

    bool frame(Bits& bits, std::size_t len) {
        if (!tile_header(bits)) return false;
        if (channels > 1 && bits.bit() && bits.bit()) bits.read(static_cast<std::uint32_t>(channels * channels * 4));
        bits.read(8);
        if (bits.bit()) {
            if (bits.bit()) bits.read(log2_size(kFrameSamples * 2));
            if (bits.bit()) bits.read(log2_size(kFrameSamples * 2));
        }
        parsed_all_subframes = false;
        for (std::size_t c = 0; c < channels; ++c) {
            ch[c].decoded_samples = 0;
            ch[c].cur_subframe = 0;
            ch[c].reuse_sf = false;
        }
        while (!parsed_all_subframes)
            if (!subframe(bits)) return false;
        if (bits.overread() || bits.consumed() > len) return false;
        return true;
    }

    std::optional<std::size_t> subframe_length(Bits& bits, std::size_t offset) {
        if (offset == kFrameSamples - kMinSubframe) return kMinSubframe;
        const std::uint32_t shift = bits.bit() ? 1 + bits.read(1) : 0;
        const std::size_t len = kFrameSamples >> shift;
        if (len < kMinSubframe) return std::nullopt;
        return len;
    }

    bool tile_header(Bits& bits) {
        std::array<std::size_t, 2> num_samples{};
        std::array<bool, 2> contains{};
        std::size_t local_channels = channels;
        std::size_t min_channel_len = 0;
        for (std::size_t c = 0; c < channels; ++c) ch[c].num_subframes = 0;
        const bool fixed = bits.bit();
        for (;;) {
            for (std::size_t c = 0; c < channels; ++c)
                contains[c] = num_samples[c] == min_channel_len &&
                              (fixed || local_channels == 1 || min_channel_len == kFrameSamples - kMinSubframe ||
                               bits.bit());
            const auto length = subframe_length(bits, min_channel_len);
            if (!length) return false;
            const std::size_t subframe_size = *length;
            min_channel_len += subframe_size;
            for (std::size_t c = 0; c < channels; ++c) {
                Channel& channel = ch[c];
                if (contains[c]) {
                    if (channel.num_subframes >= kMaxSubframes) return false;
                    channel.subframe_len[channel.num_subframes] = subframe_size;
                    num_samples[c] += subframe_size;
                    ++channel.num_subframes;
                    if (num_samples[c] > kFrameSamples) return false;
                } else if (num_samples[c] <= min_channel_len) {
                    if (num_samples[c] < min_channel_len) {
                        local_channels = 0;
                        min_channel_len = num_samples[c];
                    }
                    ++local_channels;
                }
            }
            if (min_channel_len >= kFrameSamples || bits.overread()) break;
        }
        return !bits.overread();
    }

    bool channel_transform(Bits& bits) {
        transform = false;
        if (channels == 1) return true;
        if (bits.bit()) return false;
        if (channels_for_cur_subframe == 2) {
            if (bits.bit()) {
                if (bits.bit()) return false;
            } else {
                transform = true;
            }
        }
        if (transform) {
            if (!bits.bit()) {
                for (std::size_t i = 0; i < num_bands; ++i) transform_band[i] = bits.bit();
            } else {
                transform_band.fill(true);
            }
        }
        return true;
    }

    bool coefficients(Bits& bits, std::size_t c) {
        const Codes& table = codes();
        const std::size_t table_index = bits.bit() ? 1 : 0;
        const std::uint16_t* run = table_index == 1 ? kCoef1Run : kCoef0Run;
        const float* level = table_index == 1 ? kCoef1Level : kCoef0Level;
        const std::size_t run_count = table_index == 1 ? std::size(kCoef1Run) : std::size(kCoef0Run);
        const std::size_t coefficients_len = subframe_len;
        Channel& channel = ch[c];
        bool rl_mode = false;
        std::size_t cur = 0;
        std::size_t num_zeros = 0;
        while ((transmit_num_vec_coeffs || !rl_mode) && cur + 3 < channel.num_vec_coeffs) {
            std::array<std::uint32_t, 4> vals{};
            const std::int32_t index = table.vec4.read(bits);
            if (index < 0) {
                for (std::size_t i = 0; i < 4; i += 2) {
                    const std::int32_t pair = table.vec2.read(bits);
                    if (pair < 0) {
                        for (std::size_t v = i; v < i + 2; ++v) {
                            std::uint32_t x = static_cast<std::uint32_t>(table.vec1.read(bits));
                            if (x == static_cast<std::uint32_t>(std::size(kVec1) - 1)) x += large_value(bits);
                            vals[v] = x;
                        }
                    } else {
                        vals[i] = static_cast<std::uint32_t>(pair) >> 4;
                        vals[i + 1] = static_cast<std::uint32_t>(pair) & 0xf;
                    }
                }
            } else {
                const auto value = static_cast<std::uint32_t>(index);
                vals = {value >> 12, (value >> 8) & 0xf, (value >> 4) & 0xf, value & 0xf};
            }
            for (std::uint32_t value : vals) {
                if (value != 0) {
                    const bool negative = !bits.bit();
                    channel.coeffs[cur] = negative ? -static_cast<float>(value) : static_cast<float>(value);
                    num_zeros = 0;
                } else {
                    channel.coeffs[cur] = 0.0f;
                    ++num_zeros;
                    rl_mode = rl_mode || num_zeros > (coefficients_len >> 8);
                }
                ++cur;
            }
            if (bits.overread()) return false;
        }
        if (cur < coefficients_len) {
            std::fill(channel.coeffs.begin() + cur, channel.coeffs.begin() + coefficients_len, 0.0f);
            const std::size_t mask = coefficients_len - 1;
            std::size_t offset = cur;
            while (offset < coefficients_len) {
                const std::int32_t code = table.coef[table_index].read(bits);
                if (code > 1) {
                    if (static_cast<std::size_t>(code) >= run_count) return false;
                    offset += run[code];
                    const bool negative = !bits.bit();
                    const float value = level[code];
                    channel.coeffs[offset & mask] = negative ? -value : value;
                } else if (code == 1) {
                    break;
                } else {
                    const std::int32_t value = static_cast<std::int32_t>(large_value(bits));
                    if (bits.bit()) {
                        if (bits.bit()) {
                            if (bits.bit()) break;
                            offset += bits.read(esc_len) + 4;
                        } else {
                            offset += bits.read(2) + 1;
                        }
                    }
                    const bool negative = !bits.bit();
                    channel.coeffs[offset & mask] =
                        negative ? -static_cast<float>(value) : static_cast<float>(value);
                }
                if (bits.overread()) return false;
                ++offset;
            }
        }
        return true;
    }

    bool scale_factors(Bits& bits) {
        const Codes& table = codes();
        for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
            const std::size_t c = channel_indexes[i];
            const auto& offsets = this->sf_offsets[table_idx][ch[c].table_idx];
            Channel& channel = ch[c];
            const std::size_t target = 1 - channel.scale_factor_idx;
            channel.scale_factors = target;
            if (channel.reuse_sf) {
                const auto from = channel.saved_scale_factors[channel.scale_factor_idx];
                for (std::size_t band = 0; band < num_bands; ++band)
                    channel.saved_scale_factors[target][band] = from[offsets[band]];
            }
            if (channel.cur_subframe == 0 || bits.bit()) {
                if (!channel.reuse_sf) {
                    channel.scale_factor_step = static_cast<std::int32_t>(bits.read(2)) + 1;
                    std::int32_t value = static_cast<std::int32_t>(kScaleBase) / channel.scale_factor_step;
                    for (std::size_t band = 0; band < num_bands; ++band) {
                        value += table.scale.read(bits);
                        channel.saved_scale_factors[target][band] = value;
                    }
                } else {
                    std::size_t band = 0;
                    while (band < num_bands) {
                        const std::int32_t index = table.scale_rl.read(bits);
                        std::size_t skip = 0;
                        std::int32_t value = 0;
                        bool negative = false;
                        if (index == 0) {
                            const std::uint32_t code = bits.read(14);
                            skip = (code & 0x3f) >> 1;
                            value = static_cast<std::int32_t>(code >> 6);
                            negative = (code & 1) == 0;
                        } else if (index == 1) {
                            break;
                        } else {
                            if (static_cast<std::size_t>(index) >= std::size(kScaleRunLevelRun)) return false;
                            skip = kScaleRunLevelRun[index];
                            value = kScaleRunLevelLevel[index];
                            negative = !bits.bit();
                        }
                        band += skip;
                        if (band >= num_bands) return false;
                        channel.saved_scale_factors[target][band] += negative ? -value : value;
                        ++band;
                    }
                }
                channel.scale_factor_idx = target;
                channel.table_idx = table_idx;
                channel.reuse_sf = true;
            }
            const auto& factors = channel.saved_scale_factors[channel.scale_factors];
            channel.max_scale_factor =
                num_bands ? *std::max_element(factors.begin(), factors.begin() + num_bands) : 0;
        }
        return true;
    }

    bool subframe(Bits& bits) {
        std::size_t offset = kFrameSamples;
        std::size_t subframe_size = kFrameSamples;
        std::int64_t total_samples = static_cast<std::int64_t>(kFrameSamples * channels);
        for (std::size_t c = 0; c < channels; ++c) {
            const Channel& channel = ch[c];
            if (offset > channel.decoded_samples) {
                offset = channel.decoded_samples;
                subframe_size = channel.subframe_len[std::min(channel.cur_subframe, kMaxSubframes - 1)];
            }
        }
        channels_for_cur_subframe = 0;
        for (std::size_t c = 0; c < channels; ++c) {
            Channel& channel = ch[c];
            total_samples -= static_cast<std::int64_t>(channel.decoded_samples);
            if (offset == channel.decoded_samples && channel.cur_subframe < channel.num_subframes &&
                subframe_size == channel.subframe_len[channel.cur_subframe]) {
                total_samples -= static_cast<std::int64_t>(subframe_size);
                channel.decoded_samples += subframe_size;
                channel_indexes[channels_for_cur_subframe] = c;
                ++channels_for_cur_subframe;
            }
        }
        if (channels_for_cur_subframe == 0) return false;
        if (total_samples == 0) parsed_all_subframes = true;
        table_idx = log2_size(kFrameSamples / subframe_size);
        num_bands = num_sfb[table_idx];
        subframe_len = subframe_size;
        esc_len = log2_size(subframe_size - 1) + 1;

        if (bits.bit()) {
            std::size_t fill = bits.read(2);
            if (fill == 0) {
                const std::uint32_t length = bits.read(4);
                fill = static_cast<std::size_t>(bits.read(length)) + 1;
            }
            bits.pos += fill;
        }
        if (bits.bit()) return false;
        if (!channel_transform(bits)) return false;

        bool transmit_coeffs = false;
        for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
            const std::size_t c = channel_indexes[i];
            ch[c].transmit_coefs = bits.bit();
            transmit_coeffs = transmit_coeffs || ch[c].transmit_coefs;
        }
        if (transmit_coeffs) {
            transmit_num_vec_coeffs = bits.bit();
            if (transmit_num_vec_coeffs) {
                const std::uint32_t num_bits = log2_size((subframe_size + 3) / 4) + 1;
                for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
                    const std::size_t c = channel_indexes[i];
                    const std::size_t count = static_cast<std::size_t>(bits.read(num_bits) << 2);
                    if (count > subframe_size) return false;
                    ch[c].num_vec_coeffs = count;
                }
            } else {
                for (std::size_t i = 0; i < channels_for_cur_subframe; ++i)
                    ch[channel_indexes[i]].num_vec_coeffs = subframe_size;
            }
            std::int32_t quant_step = 90;
            std::int32_t step = bits.signed_value(6);
            quant_step += step;
            if (step == -32 || step == 31) {
                const bool negative = step == -32;
                std::int32_t quant = 0;
                while (bits.pos + 5 < bits.end) {
                    step = static_cast<std::int32_t>(bits.read(5));
                    if (step != 31) break;
                    quant += 31;
                }
                quant_step += negative ? -(quant + step) : quant + step;
            }
            if (channels_for_cur_subframe == 1) {
                ch[channel_indexes[0]].quant_step = quant_step;
            } else {
                const std::uint32_t modifier_len = bits.read(3);
                for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
                    const std::size_t c = channel_indexes[i];
                    ch[c].quant_step = quant_step;
                    if (bits.bit()) {
                        ch[c].quant_step += modifier_len > 0
                                                ? static_cast<std::int32_t>(bits.read(modifier_len)) + 1
                                                : 1;
                    }
                }
            }
            if (!scale_factors(bits)) return false;
        }

        for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
            const std::size_t c = channel_indexes[i];
            if (ch[c].transmit_coefs && bits.pos < bits.end) {
                if (!coefficients(bits, c)) return false;
            } else {
                std::fill(ch[c].coeffs.begin(), ch[c].coeffs.begin() + subframe_size, 0.0f);
            }
        }

        const std::size_t at = offset + kFrameSamples / 2;
        if (transmit_coeffs) {
            inverse_channel_transform();
            const float scale = 2.0f / static_cast<float>(subframe_size) / 32768.0f;
            const auto& sfb = sfb_offsets[table_idx];
            std::array<float, kFrameSamples> tmp{};
            for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
                Channel& channel = ch[channel_indexes[i]];
                const auto& factors = channel.saved_scale_factors[channel.scale_factors];
                for (std::size_t band = 0; band < num_bands; ++band) {
                    const std::size_t end = std::min(sfb[band + 1], subframe_size);
                    const std::int32_t exponent =
                        channel.quant_step - (channel.max_scale_factor - factors[band]) * channel.scale_factor_step;
                    const float quant = std::pow(10.0f, static_cast<float>(exponent) / 20.0f);
                    for (std::size_t t = sfb[band]; t < end; ++t) tmp[t] = channel.coeffs[t] * quant;
                }
                imdct_half(tmp.data(), subframe_size, channel.out.data() + at, scale);
            }
        } else {
            for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
                Channel& channel = ch[channel_indexes[i]];
                std::fill(channel.out.begin() + at, channel.out.begin() + at + subframe_size, 0.0f);
            }
        }

        for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
            Channel& channel = ch[channel_indexes[i]];
            std::size_t winlen = channel.prev_block_len;
            std::size_t start = at - winlen / 2;
            if (subframe_size < winlen) {
                start += (winlen - subframe_size) / 2;
                winlen = subframe_size;
            }
            window_overlap(channel.out.data() + start, winlen, windows()[log2_size(winlen) - 7]);
            channel.prev_block_len = subframe_size;
        }

        for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
            Channel& channel = ch[channel_indexes[i]];
            if (channel.cur_subframe >= channel.num_subframes) return false;
            ++channel.cur_subframe;
        }
        return !bits.overread();
    }

    void inverse_channel_transform() {
        if (!transform || channels_for_cur_subframe != 2) return;
        const auto& sfb = sfb_offsets[table_idx];
        Channel& a = ch[0];
        Channel& b = ch[1];
        for (std::size_t band = 0; band < num_bands; ++band) {
            const std::size_t end = std::min(sfb[band + 1], subframe_len);
            if (transform_band[band]) {
                for (std::size_t y = sfb[band]; y < end; ++y) {
                    const float left = a.coeffs[y];
                    const float right = b.coeffs[y];
                    a.coeffs[y] = left - right;
                    b.coeffs[y] = left + right;
                }
            } else {
                for (std::size_t y = sfb[band]; y < end; ++y) {
                    a.coeffs[y] *= 181.0f / 128.0f;
                    b.coeffs[y] *= 181.0f / 128.0f;
                }
            }
        }
    }
};

Decoder::Decoder(std::size_t channels, std::uint32_t sample_rate)
    : impl_(std::make_unique<Impl>(channels, sample_rate)) {}

Decoder::~Decoder() = default;
Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;

std::size_t Decoder::channels() const { return impl_->channels; }

void Decoder::reset() { impl_->reset_channels(); }

bool Decoder::decode(std::span<const std::uint8_t> data, std::size_t bits, std::vector<std::int16_t>& out) {
    return impl_->decode(data, bits, out);
}

void Decoder::flush(std::vector<std::int16_t>& out) { impl_->flush(out); }

}  // namespace g6shim::acphal::xma
