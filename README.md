# ptxlite: a miniature PTX assembler

`ptxlite` is a small, readable compiler for a subset of NVIDIA's
[PTX ISA](https://docs.nvidia.com/cuda/parallel-thread-execution/). It
follows the same pipeline as `ptxas`. It parses PTX, checks it, optimizes it,
allocates registers, and writes a binary object file with debug line info.
`ptxlite-objdump` disassembles that object, the way `cuobjdump` does.

It does **not** generate real GPU machine code (SASS). The goal is to show
each compiler stage clearly and correctly in about 4,000 lines of C++17, tests included.

```
 kernel.ptx
     │
     ▼
 ┌────────┐  tokens  ┌────────┐   IR    ┌──────────┐   IR    ┌───────────────┐
 │ Lexer  ├─────────►│ Parser ├────────►│ Verifier ├────────►│  Optimizer    │
 └────────┘          └────────┘         └──────────┘         │ const-fold,DCE│
                                                             └───────┬───────┘
                                                                     │ IR (re-verified)
 kernel.pobj   ┌──────────────┐  ┌─────────────────┐  ┌──────────────▼──────┐
 ◄─────────────┤ Object writer│◄─┤ Linear-scan RA  │◄─┤ CFG + liveness      │
               └──────────────┘  └─────────────────┘  └─────────────────────┘
```

## Features

| Stage | What it does |
|---|---|
| **Lexer** | Hand-written scanner. Handles PTX number forms (`0f3F800000`, `0d…`, hex) and dotted mnemonics. Tracks line and column. |
| **Parser** | Recursive descent. Handles `.entry`, `.param`, `.reg %r<N>`, labels, predicate guards (`@!%p1`) and memory operands (`[%rd1+8]`, `[param]`). Recovers after errors. |
| **Verifier** | Checks the ISA rules. Operand sizes must match the type, integer `mul` needs `.lo/.hi/.wide`, `.wide` widens the destination, addresses must be 64-bit, and labels and registers must be declared. Reports **every** error with `file:line:col`. |
| **CFG and liveness** | Builds basic blocks and runs iterative backward dataflow. A predicated definition does *not* kill liveness. |
| **Constant folding** | Block-local propagation and folding with exact PTX integer semantics (wraparound, arithmetic vs logical shift, shift clamping, `mul.hi`, `mul.wide`). Float math is deliberately not folded. |
| **Dead code elimination** | Liveness-based. Runs to a fixed point. |
| **Register allocation** | Linear scan per register class (`pred`, 16, 32, 64-bit). Reports `used N registers` like `ptxas -v`. Supports a `--max-regs` budget. |
| **Object file** | Versioned, little-endian `.pobj` format with a string table and a per-instruction line table. The reader validates every count and offset. |
| **Simulator** | Single-thread interpreter used for **differential testing**. The program must behave the same at `-O0` and at `-O1` with register allocation. |

## Build and test

Requires CMake ≥ 3.16 and a C++17 compiler (GCC or Clang). There are no other dependencies.

```bash
cmake -S . -B build
cmake --build build -j
cd build && ctest --output-on-failure
```

Developer build with AddressSanitizer and UBSan:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DPTXLITE_SANITIZE=ON
cmake --build build-asan -j && ./build-asan/ptxlite_tests
```

## Usage

```bash
./build/ptxlite examples/vec_add.ptx -v                 # -> examples/vec_add.pobj
./build/ptxlite-objdump examples/vec_add.pobj           # disassemble
./build/ptxlite examples/const_fold.ptx --emit=opt-ptx  # see the optimizer's output
./build/ptxlite examples/row_sum.ptx --emit=ptx         # see register-allocated PTX
./build/ptxlite examples/errors.ptx                     # see diagnostics
```

Resource usage, in the same spirit as `ptxas -v`:

```
ptxlite info    : Compiling function 'vec_add' for 'sm_80'
ptxlite info    : Function properties for vec_add: 22 instructions (22 before -O1), used 15 registers, 1 predicates (19 virtual registers)
```

The optimizer at work on `examples/const_fold.ptx` (15 instructions become 8):

```
mov.u32    %r1, 16;                    mov.u32 %r6, %tid.x;
mov.u32    %r2, 4;                     add.u32 %r7, %r6, 280;
mul.lo.u32 %r3, %r1, %r2;      ==>     ...
add.u32    %r4, %r3, 6;
shl.b32    %r5, %r4, 2;
mov.u32    %r6, %tid.x;
add.u32    %r7, %r6, %r5;
sub.u32    %r8, %r1, %r2;   // dead
xor.b32    %r9, %r8, 255;   // dead
```

Diagnostics:

```
examples/errors.ptx:16:5: error: integer 'mul.s32' must specify .lo, .hi or .wide
examples/errors.ptx:18:30: error: use of undeclared register '%r9'
examples/errors.ptx:19:30: error: address register '%r3' must be 64-bit (.address_size 64)
```

Disassembly. Labels are resolved to instruction indices, and each instruction keeps its source line:

```
.visible .entry vec_add(.param .u64 a, .param .u64 b, .param .u64 c, .param .u32 n)
// registers: %P<1> %H<0> %R<5> %RD<5>, 22 instructions
  /*0007*/  mad.lo.s32 %R4, %R1, %R2, %R3;                // line 30
  /*0008*/  setp.ge.s32 %P0, %R4, %R0;                    // line 31
  /*0009*/  @%P0 bra L_0021;                              // line 32
```

## Testing strategy

* **Unit tests** cover every stage (30 tests, `tests/tests.cpp`).
* **Allocator invariant check.** At every program point, no two live values share a physical register.
* **Differential testing.** Each example, plus 300 randomly generated integer programs, runs in the simulator twice: once unoptimized with virtual registers, and once optimized with register allocation. Memory must match byte for byte. Introducing a liveness bug on purpose makes this test fail.
* **Robustness.** Corrupted and truncated object files are rejected without crashing. The suite runs clean under ASan and UBSan.
* **Round trip.** Printed PTX re-parses, and object files re-encode byte-identically.

## Supported PTX subset

`mov add sub mul mad fma div rem and or xor not shl shr neg abs min max setp selp ld st cvt cvta bra ret exit bar`,
with types `.pred .b8–.b64 .u8–.u64 .s8–.s64 .f32 .f64`, state spaces
`.param .global .shared .local .const`, and special registers `%tid %ntid %ctaid %nctaid %laneid %warpid`.

## Limitations and next steps

* Register spilling to `.local` memory is not implemented. The allocator reports an error when it goes over budget.
* Constant propagation is block-local. Sparse conditional constant propagation (SCCP) on SSA form would go further.
* There is no instruction scheduling and no SASS encoding.
* The simulator runs one thread at a time, so `bar.sync` is a no-op.

See [`docs/DESIGN.md`](docs/DESIGN.md) for the design decisions behind each stage.

## Project layout

```
src/        compiler library (Lexer, Parser, Verifier, CFG, passes, RegAlloc, ObjectFile, Interpreter)
tools/      ptxlite (driver) and ptxlite-objdump
tests/      unit + differential tests (tiny built-in test harness)
examples/   sample kernels: vec_add, saxpy, row_sum (loop), const_fold, errors
docs/       design notes
```

## License

MIT
