// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addrinterface.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void* alloc_sys(const ADDR_ALLOCSYSMEM_INPUT* in) { return std::malloc(in->sizeInBytes); }

static ADDR_E_RETURNCODE free_sys(const ADDR_FREESYSMEM_INPUT* in) {
    std::free(in->pVirtAddr);
    return ADDR_OK;
}

static unsigned config_word(unsigned pipes, unsigned fragments, unsigned packers) {
    return pipes | (fragments << 6) | (packers << 8);
}

struct Equation {
    unsigned swizzle;
    unsigned element;
    unsigned samples;
    unsigned thick;
    unsigned bits;
    std::uint16_t mask[4][16];
};

static unsigned xor_bits(unsigned value, unsigned mask) {
    unsigned mixed = 0;
    while (mask != 0) {
        if ((mask & 1u) != 0) mixed ^= value & 1u;
        value >>= 1;
        mask >>= 1;
    }
    return mixed;
}

static unsigned evaluate(const Equation& equation, unsigned x, unsigned y, unsigned z, unsigned sample) {
    unsigned offset = 0;
    for (unsigned bit = 0; bit < equation.bits; ++bit) {
        const unsigned mixed = xor_bits(x, equation.mask[0][bit]) ^ xor_bits(y, equation.mask[1][bit]) ^
                               xor_bits(z, equation.mask[2][bit]) ^ xor_bits(sample, equation.mask[3][bit]);
        offset |= mixed << bit;
    }
    return offset;
}

struct Library {
    ADDR_HANDLE handle;
    const ADDR_EQUATION* equations;
    unsigned count;
};

static Library open_library(unsigned pipes, unsigned fragments, unsigned packers) {
    ADDR_CREATE_INPUT in{};
    ADDR_CREATE_OUTPUT out{};
    in.size = sizeof(in);
    out.size = sizeof(out);
    in.chipEngine = 0xD;
    in.chipFamily = 0x8F;
    in.chipRevision = 0x28;
    in.callbacks.allocSysMem = alloc_sys;
    in.callbacks.freeSysMem = free_sys;
    in.regValue.gbAddrConfig = config_word(pipes, fragments, packers);
    if (AddrCreate(&in, &out) != ADDR_OK) return {};
    return {out.hLib, out.pEquationTable, out.numEquations};
}

static AddrResourceType resource_type(unsigned dimension) {
    if (dimension == 3) return ADDR_RSRC_TEX_3D;
    if (dimension == 1) return ADDR_RSRC_TEX_1D;
    return ADDR_RSRC_TEX_2D;
}

struct Surface {
    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT out;
    ADDR2_MIP_INFO mips[16];
    bool ok;
};

static Surface query_surface(ADDR_HANDLE library, unsigned swizzle, unsigned dimension, unsigned bpp, unsigned width,
                             unsigned height, unsigned depth, unsigned mips, unsigned samples) {
    Surface surface{};
    ADDR2_COMPUTE_SURFACE_INFO_INPUT in{};
    in.size = sizeof(in);
    surface.out.size = sizeof(surface.out);
    surface.out.pMipInfo = surface.mips;
    in.swizzleMode = static_cast<AddrSwizzleMode>(swizzle);
    in.resourceType = resource_type(dimension);
    in.bpp = bpp;
    in.width = width;
    in.height = height;
    in.numSlices = depth;
    in.numMipLevels = mips;
    in.numSamples = samples;
    in.numFrags = samples;
    surface.ok = Addr2ComputeSurfaceInfo(library, &in, &surface.out) == ADDR_OK;
    return surface;
}

static bool query_address(ADDR_HANDLE library, unsigned swizzle, unsigned dimension, unsigned bpp, unsigned width,
                          unsigned height, unsigned depth, unsigned mips, unsigned samples, unsigned mip, unsigned x,
                          unsigned y, unsigned z, unsigned sample, unsigned long long* out) {
    ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT in{};
    ADDR2_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT at{};
    in.size = sizeof(in);
    at.size = sizeof(at);
    in.swizzleMode = static_cast<AddrSwizzleMode>(swizzle);
    in.resourceType = resource_type(dimension);
    in.bpp = bpp;
    in.unalignedWidth = width;
    in.unalignedHeight = height;
    in.numSlices = depth;
    in.numMipLevels = mips;
    in.numSamples = samples;
    in.numFrags = samples;
    in.mipId = mip;
    in.x = x;
    in.y = y;
    in.slice = z;
    in.sample = sample;
    if (Addr2ComputeSurfaceAddrFromCoord(library, &in, &at) != ADDR_OK) return false;
    *out = at.addr;
    return true;
}

static bool masks_from_equation(const ADDR_EQUATION& source, unsigned element, Equation* equation) {
    if (source.numBits == 0 || source.numBits > 16 || source.numBitComponents > 5) return false;
    equation->bits = source.numBits;
    std::memset(equation->mask, 0, sizeof(equation->mask));
    for (unsigned bit = 0; bit < source.numBits; ++bit) {
        for (unsigned component = 0; component < source.numBitComponents; ++component) {
            const ADDR_CHANNEL_SETTING setting = source.comps[component][bit];
            if (setting.valid == 0) continue;
            unsigned index = setting.index;
            unsigned channel = setting.channel;
            if (channel == 0) {
                if (index < element) continue;
                index -= element;
            }
            if (channel > 3 || index >= 16) return false;
            equation->mask[channel][bit] = static_cast<std::uint16_t>(equation->mask[channel][bit] | (1u << index));
        }
    }
    return true;
}

static unsigned long long composed(const Equation& equation, const Surface& surface, unsigned x, unsigned y,
                                   unsigned z, unsigned sample) {
    const unsigned block_w = surface.out.blockWidth;
    const unsigned block_h = surface.out.blockHeight;
    const unsigned block_d = surface.out.blockSlices == 0 ? 1u : surface.out.blockSlices;
    const unsigned blocks_x = surface.out.pitch / block_w;
    const unsigned block_index = (y / block_h) * blocks_x + x / block_w;
    const unsigned slice_id = equation.thick != 0 ? z / block_d : z;
    const unsigned long long slice_bytes =
        equation.thick != 0 ? surface.out.sliceSize * block_d : surface.out.sliceSize;
    return slice_bytes * slice_id + (static_cast<unsigned long long>(block_index) << equation.bits) +
           evaluate(equation, x, y, z, sample);
}

static bool fill_from_table(const Library& library, Equation* equation, unsigned swizzle, unsigned dimension,
                            unsigned element, unsigned samples) {
    const unsigned bpp = 8u << element;
    const unsigned width = 512;
    const unsigned height = 512;
    const unsigned depth = dimension == 3 ? 64u : 4u;
    const bool thick = dimension == 3 && swizzle != 24 && swizzle != 27;
    const Surface surface = query_surface(library.handle, swizzle, dimension, bpp, width, height, depth, 1, samples);
    if (!surface.ok || surface.out.blockWidth == 0 || surface.out.blockHeight == 0) return false;
    const unsigned block_bytes =
        surface.out.blockWidth * surface.out.blockHeight * surface.out.blockSlices * (bpp / 8) * samples;
    unsigned block_log2 = 0;
    while ((1u << block_log2) < block_bytes && block_log2 < 16) ++block_log2;
    if ((1u << block_log2) != block_bytes) return false;
    equation->swizzle = swizzle;
    equation->element = element;
    equation->samples = samples;
    equation->thick = thick ? 1u : 0u;
    equation->bits = block_log2;
    std::memset(equation->mask, 0, sizeof(equation->mask));
    unsigned long long base = 0;
    if (!query_address(library.handle, swizzle, dimension, bpp, width, height, depth, 1, samples, 0, 0, 0, 0, 0, &base) ||
        base != 0)
        return false;
    const unsigned block_mask = (1u << equation->bits) - 1u;
    const unsigned extent[4] = {surface.out.blockWidth, surface.out.blockHeight, depth, samples};
    for (unsigned channel = 0; channel < 4; ++channel) {
        for (unsigned bit = 0; bit < 16 && (1u << bit) < extent[channel]; ++bit) {
            unsigned coord[4] = {};
            coord[channel] = 1u << bit;
            if (coord[0] >= width || coord[1] >= height || coord[2] >= depth || coord[3] >= samples) continue;
            unsigned long long at = 0;
            if (!query_address(library.handle, swizzle, dimension, bpp, width, height, depth, 1, samples, 0, coord[0],
                               coord[1], coord[2], coord[3], &at))
                return false;
            const unsigned low = static_cast<unsigned>(base ^ at) & block_mask;
            for (unsigned addr_bit = 0; addr_bit < equation->bits; ++addr_bit) {
                if ((low & (1u << addr_bit)) != 0)
                    equation->mask[channel][addr_bit] =
                        static_cast<std::uint16_t>(equation->mask[channel][addr_bit] | (1u << bit));
            }
        }
    }
    const unsigned checks[][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {3, 5, 0, 0}, {7, 2, 1, samples > 1 ? 1u : 0u},
                                  {128, 0, 0, 0}, {0, 128, 0, 0}, {128, 64, 0, 0}, {0, 0, 1, 0}};
    for (const auto& check : checks) {
        if (check[0] >= width || check[1] >= height || check[2] >= depth || check[3] >= samples) continue;
        if (check[0] >= surface.out.blockWidth || check[1] >= surface.out.blockHeight) continue;
        unsigned long long at = 0;
        if (!query_address(library.handle, swizzle, dimension, bpp, width, height, depth, 1, samples, 0, check[0],
                           check[1], check[2], check[3], &at))
            return false;
        if (composed(*equation, surface, check[0], check[1], check[2], check[3]) != at) return false;
    }
    return true;
}

static void print_equation(const Equation& equation, unsigned pipes, unsigned packers, unsigned dimension) {
    std::printf("EQ %u %u %u %u %u %u %u %u\n", pipes, packers, equation.swizzle, equation.element, equation.samples,
                equation.thick, dimension == 3 ? 1u : 0u, equation.bits);
    const char* names = "XYZS";
    for (unsigned channel = 0; channel < 4; ++channel) {
        std::printf("%c", names[channel]);
        for (unsigned bit = 0; bit < 16; ++bit) std::printf(" %u", equation.mask[channel][bit]);
        std::printf("\n");
    }
}

static void print_case(ADDR_HANDLE library, unsigned pipes, unsigned packers, unsigned fragments, unsigned swizzle,
                       unsigned dimension, unsigned element, unsigned samples, unsigned width, unsigned height,
                       unsigned depth, unsigned mips) {
    const unsigned bpp = 8u << element;
    const Surface surface = query_surface(library, swizzle, dimension, bpp, width, height, depth, mips, samples);
    if (!surface.ok) {
        std::printf("MISS %u %u %u %u %u %u %u %u %u %u %u\n", pipes, packers, fragments, swizzle, dimension, element,
                    samples, width, height, depth, mips);
        return;
    }
    std::printf("SIZE %u %u %u %u %u %u %u %u %u %u %u %llu %llu %u %u %u %u %u %u %u\n", pipes, packers, fragments,
                swizzle, dimension, element, samples, width, height, depth, mips,
                static_cast<unsigned long long>(surface.out.surfSize),
                static_cast<unsigned long long>(surface.out.sliceSize), surface.out.pitch, surface.out.height,
                surface.out.blockWidth, surface.out.blockHeight, surface.out.blockSlices, surface.out.baseAlign,
                surface.out.firstMipIdInTail);
    for (unsigned mip = 0; mip < mips; ++mip) {
        const ADDR2_MIP_INFO& level = surface.mips[mip];
        std::printf("MIP %u %u %u %u %u %u %u %u %u %u %u %u %llu %u %u %u %u\n", pipes, packers, fragments, swizzle,
                    dimension, element, samples, width, height, depth, mips, mip,
                    static_cast<unsigned long long>(level.macroBlockOffset), level.mipTailOffset, level.mipTailCoordX,
                    level.mipTailCoordY, level.mipTailOffset != 0 ? 1u : 0u);
    }
    const unsigned xs[] = {0, 1, 3, 7};
    const unsigned ys[] = {0, 1, 5, 2};
    for (unsigned i = 0; i < 4; ++i) {
        if (xs[i] >= width || ys[i] >= height) continue;
        const unsigned sample = samples > 1 && i == 3 ? samples - 1 : 0;
        unsigned long long at = 0;
        if (!query_address(library, swizzle, dimension, bpp, width, height, depth, mips, samples, 0, xs[i], ys[i], 0,
                           sample, &at))
            continue;
        std::printf("ADDR %u %u %u %u %u %u %u %u %u %u %u 0 %u %u 0 %u %llu\n", pipes, packers, fragments, swizzle,
                    dimension, element, samples, width, height, depth, mips, xs[i], ys[i], sample, at);
    }
    if (mips > 1) {
        unsigned long long at = 0;
        if (query_address(library, swizzle, dimension, bpp, width, height, depth, mips, samples, mips - 1, 0, 0, 0, 0,
                          &at))
            std::printf("ADDR %u %u %u %u %u %u %u %u %u %u %u %u 0 0 0 0 %llu\n", pipes, packers, fragments, swizzle,
                        dimension, element, samples, width, height, depth, mips, mips - 1, at);
    }
    if (dimension == 3 && depth > 1) {
        unsigned long long at = 0;
        if (query_address(library, swizzle, dimension, bpp, width, height, depth, mips, samples, 0, 1, 1, 1, 0, &at))
            std::printf("ADDR %u %u %u %u %u %u %u %u %u %u %u 0 1 1 1 0 %llu\n", pipes, packers, fragments, swizzle,
                        dimension, element, samples, width, height, depth, mips, at);
    }
}

int main() {
    const unsigned modes_2d[] = {17, 18, 21, 22, 24, 25, 26, 27};
    const unsigned modes_3d[] = {17, 21, 24, 25, 26, 27};
    const unsigned sample_list[] = {1, 2, 4, 8};
    unsigned count = 0;
    const unsigned configs[][2] = {{4, 3}, {3, 2}, {2, 1}, {4, 2}, {5, 3}, {1, 0}};
    for (const auto& config : configs) {
        const unsigned pipes = config[0];
        const unsigned packers = config[1];
        {
            std::fprintf(stderr, "open %u %u\n", pipes, packers);
            std::fflush(stderr);
            Library library = open_library(pipes, 3, packers);
            Library other = open_library(pipes, 1, packers);
            if (library.handle == nullptr || other.handle == nullptr) return 1;
            unsigned long long left = 0;
            unsigned long long right = 0;
            if (!query_address(library.handle, 25, 2, 32, 64, 64, 1, 1, 1, 0, 3, 5, 0, 0, &left) ||
                !query_address(other.handle, 25, 2, 32, 64, 64, 1, 1, 1, 0, 3, 5, 0, 0, &right) || left != right)
                return 1;
            AddrDestroy(other.handle);
            for (unsigned swizzle : modes_2d) {
                for (unsigned element = 0; element < 5; ++element) {
                    const unsigned sample_count = swizzle == 24 || swizzle == 27 ? 4u : 1u;
                    for (unsigned index = 0; index < sample_count; ++index) {
                        Equation equation{};
                        if (!fill_from_table(library, &equation, swizzle, 2, element, sample_list[index])) continue;
                        print_equation(equation, pipes, packers, 2);
                        ++count;
                    }
                }
            }
            for (unsigned swizzle : modes_3d) {
                for (unsigned element = 0; element < 5; ++element) {
                    Equation equation{};
                    if (!fill_from_table(library, &equation, swizzle, 3, element, 1)) continue;
                    print_equation(equation, pipes, packers, 3);
                    ++count;
                }
            }
            AddrDestroy(library.handle);
            std::fprintf(stderr, "config %u %u equations %u\n", pipes, packers, count);
        }
    }
    const unsigned spots[][3] = {{4, 3, 3}, {3, 2, 2}, {2, 1, 1}, {4, 2, 3}, {5, 3, 2}, {1, 0, 0}};
    for (const auto& spot : spots) {
        Library library = open_library(spot[0], spot[2], spot[1]);
        if (library.handle == nullptr) return 1;
        for (unsigned swizzle : modes_2d) {
            print_case(library.handle, spot[0], spot[1], spot[2], swizzle, 2, 2, 1, 64, 48, 1, 1);
            print_case(library.handle, spot[0], spot[1], spot[2], swizzle, 2, 0, 1, 40, 24, 1, 5);
            print_case(library.handle, spot[0], spot[1], spot[2], swizzle, 2, 4, 1, 96, 20, 1, 1);
        }
        print_case(library.handle, spot[0], spot[1], spot[2], 24, 2, 2, 2, 32, 32, 1, 1);
        print_case(library.handle, spot[0], spot[1], spot[2], 24, 2, 1, 4, 48, 16, 1, 1);
        print_case(library.handle, spot[0], spot[1], spot[2], 24, 2, 0, 8, 16, 16, 1, 1);
        print_case(library.handle, spot[0], spot[1], spot[2], 27, 2, 2, 2, 32, 16, 1, 1);
        print_case(library.handle, spot[0], spot[1], spot[2], 27, 2, 3, 4, 20, 12, 1, 1);
        print_case(library.handle, spot[0], spot[1], spot[2], 27, 2, 4, 8, 8, 8, 1, 1);
        for (unsigned swizzle : modes_3d)
            print_case(library.handle, spot[0], spot[1], spot[2], swizzle, 3, 2, 1, 32, 24, 8, 4);
        print_case(library.handle, spot[0], spot[1], spot[2], 24, 1, 2, 1, 32, 1, 1, 1);
        AddrDestroy(library.handle);
        std::fprintf(stderr, "spots %u %u\n", spot[0], spot[1]);
    }
    std::fprintf(stderr, "equations %u\n", count);
    return 0;
}
