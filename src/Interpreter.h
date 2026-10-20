// Interpreter.h - a tiny single-thread PTX simulator.
//
// Purpose: *testing the compiler*. We run a kernel unoptimized (-O0, virtual
// registers) and fully compiled (-O1 + register allocation, physical
// registers) on the same inputs and require identical memory afterwards.
// If an optimization or the register allocator changed program behaviour,
// this "differential test" catches it.
#pragma once

#include "IR.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ptxl {

class Memory {
public:
    void store(uint64_t addr, uint64_t value, unsigned bytes);
    uint64_t load(uint64_t addr, unsigned bytes) const;  // unwritten bytes read as 0
    bool operator==(const Memory& other) const { return bytes_ == other.bytes_; }
    bool operator!=(const Memory& other) const { return !(*this == other); }
    std::string dump() const;

private:
    std::map<uint64_t, uint8_t> bytes_;
};

struct ExecConfig {
    std::array<uint32_t, 3> tid{0, 0, 0};
    std::array<uint32_t, 3> ntid{1, 1, 1};
    std::array<uint32_t, 3> ctaid{0, 0, 0};
    std::array<uint32_t, 3> nctaid{1, 1, 1};
    uint64_t maxSteps = 1000000;  // guards against infinite loops
};

struct ExecResult {
    bool ok = true;
    std::string error;
    uint64_t steps = 0;
};

// params[i] holds the value of kernel parameter i (e.g. a device pointer).
ExecResult execute(const Function& fn, const std::vector<uint64_t>& params, Memory& mem,
                   const ExecConfig& cfg = {});

}  // namespace ptxl
