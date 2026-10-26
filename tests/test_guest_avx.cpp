#include "guest_cpu.hpp"

#include <array>
#include <cstdint>
#include <iostream>

using namespace someboxone;

static bool run_one(GuestCpu& cpu, GuestMemory& mem, std::uint64_t address,
                    const std::initializer_list<std::uint8_t>& bytes) {
    const auto count = bytes.size();
    (void)count;
    mem.write_bytes(address, bytes.begin(), bytes.size());
    cpu.rip = address;
    cpu.step();
    return cpu.stop == GuestCpu::Stop::None;
}

int main() {
    GuestMemory mem(1U << 20);
    if (!mem.map(0x1000, 0x1000, "AVX regression code")) return 1;

    // Minecraft's exact failing instruction: C5 FB 12 DB = VMOVDDUP xmm3,xmm3.
    GuestCpu movddup128(mem);
    movddup128.xmm[3].q[0] = 0x1122334455667788ULL;
    movddup128.xmm[3].q[1] = 0xAABBCCDDEEFF0011ULL;
    if (!run_one(movddup128, mem, 0x1000, {0xC5,0xFB,0x12,0xDB}) ||
        movddup128.xmm[3].q[0] != 0x1122334455667788ULL ||
        movddup128.xmm[3].q[1] != 0x1122334455667788ULL ||
        movddup128.ymm_hi[3].q[0] != 0 || movddup128.ymm_hi[3].q[1] != 0) {
        std::cerr << "VMOVDDUP VEX.128 semantics failed\n";
        return 1;
    }

    // VEX.256 duplicates the low qword in each 128-bit lane independently.
    GuestCpu movddup256(mem);
    movddup256.xmm[3].q[0] = 0x0102030405060708ULL;
    movddup256.xmm[3].q[1] = 0x1112131415161718ULL;
    movddup256.ymm_hi[3].q[0] = 0x2122232425262728ULL;
    movddup256.ymm_hi[3].q[1] = 0x3132333435363738ULL;
    if (!run_one(movddup256, mem, 0x1010, {0xC5,0xFF,0x12,0xDB}) ||
        movddup256.xmm[3].q[0] != 0x0102030405060708ULL ||
        movddup256.xmm[3].q[1] != 0x0102030405060708ULL ||
        movddup256.ymm_hi[3].q[0] != 0x2122232425262728ULL ||
        movddup256.ymm_hi[3].q[1] != 0x2122232425262728ULL) {
        std::cerr << "VMOVDDUP VEX.256 semantics failed\n";
        return 1;
    }

    // VMOVSLDUP xmm3,xmm3 duplicates dwords 0/2; VEX.128 clears YMM upper half.
    GuestCpu movsldup(mem);
    movsldup.xmm[3].d[0] = 10; movsldup.xmm[3].d[1] = 11;
    movsldup.xmm[3].d[2] = 12; movsldup.xmm[3].d[3] = 13;
    movsldup.ymm_hi[3].q[0] = 0xFFFFFFFFFFFFFFFFULL;
    if (!run_one(movsldup, mem, 0x1020, {0xC5,0xFA,0x12,0xDB}) ||
        movsldup.xmm[3].d[0] != 10 || movsldup.xmm[3].d[1] != 10 ||
        movsldup.xmm[3].d[2] != 12 || movsldup.xmm[3].d[3] != 12 ||
        movsldup.ymm_hi[3].q[0] != 0) {
        std::cerr << "VMOVSLDUP VEX.128 semantics failed\n";
        return 1;
    }

    // VMOVSHDUP ymm3,ymm3 duplicates dwords 1/3 in each 128-bit lane.
    GuestCpu movshdup(mem);
    movshdup.xmm[3].d[0] = 10; movshdup.xmm[3].d[1] = 11;
    movshdup.xmm[3].d[2] = 12; movshdup.xmm[3].d[3] = 13;
    movshdup.ymm_hi[3].d[0] = 20; movshdup.ymm_hi[3].d[1] = 21;
    movshdup.ymm_hi[3].d[2] = 22; movshdup.ymm_hi[3].d[3] = 23;
    if (!run_one(movshdup, mem, 0x1030, {0xC5,0xFE,0x16,0xDB}) ||
        movshdup.xmm[3].d[0] != 11 || movshdup.xmm[3].d[1] != 11 ||
        movshdup.xmm[3].d[2] != 13 || movshdup.xmm[3].d[3] != 13 ||
        movshdup.ymm_hi[3].d[0] != 21 || movshdup.ymm_hi[3].d[1] != 21 ||
        movshdup.ymm_hi[3].d[2] != 23 || movshdup.ymm_hi[3].d[3] != 23) {
        std::cerr << "VMOVSHDUP VEX.256 semantics failed\n";
        return 1;
    }

    // Peggle 2 stops at C4 E3 F9 16 49 28 01: VPEXTRQ [rcx+0x28], xmm1, 1.
    GuestCpu vpextrq(mem);
    vpextrq.r[1] = 0x1800;
    vpextrq.xmm[1].q[0] = 0x0123456789ABCDEFULL;
    vpextrq.xmm[1].q[1] = 0xFEDCBA9876543210ULL;
    if (!run_one(vpextrq, mem, 0x1040, {0xC4,0xE3,0xF9,0x16,0x49,0x28,0x01}) ||
        mem.read<std::uint64_t>(0x1828) != 0xFEDCBA9876543210ULL) {
        std::cerr << "VPEXTRQ register-to-memory lane extraction failed\n";
        return 1;
    }

    GuestCpu vpextrd(mem);
    vpextrd.r[1] = 0x1800;
    vpextrd.xmm[1].d[0] = 0x11223344U; vpextrd.xmm[1].d[1] = 0x55667788U;
    vpextrd.xmm[1].d[2] = 0x99AABBCCU; vpextrd.xmm[1].d[3] = 0xDDEEFF00U;
    if (!run_one(vpextrd, mem, 0x1050, {0xC4,0xE3,0x79,0x16,0x49,0x28,0x02}) ||
        mem.read<std::uint32_t>(0x1828) != 0x99AABBCCU) {
        std::cerr << "VPEXTRD register-to-memory lane extraction failed\n";
        return 1;
    }

    std::cout << "Guest AVX move/duplicate and extraction regressions passed\n";
    return 0;
}
