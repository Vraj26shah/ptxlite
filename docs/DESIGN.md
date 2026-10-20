# ptxlite design notes

Each section explains a decision and the alternative that was rejected.

## 1. IR that stays close to PTX

`Instruction = opcode + types + modifier bitmask + guard + operands`. Registers
are referenced by name, and labels are pseudo-instructions in the stream.

*Why:* every pass can print its output as valid PTX (`--emit=opt-ptx`), which
makes debugging easy. After register allocation, the printed program is still
valid PTX that ptxlite can re-parse, and a test checks this.

*Trade-off:* string-keyed registers are slower than dense integer IDs. For
kernels of this size that does not matter, and readability wins. A production
compiler would intern names into integer IDs.

## 2. Separate parser and verifier

The parser only checks syntax. The verifier checks meaning: sizes,
declarations, ISA rules. This keeps both simple and lets the verifier run
**again after optimization**, like LLVM's `-verify-each`. An optimizer bug then
shows up as a clear internal error instead of a corrupt object file.

Rules worth knowing (all from the PTX ISA):

* Register size must equal the instruction type size. A `.b32` register works
  with `.u32`, `.s32` and `.f32`, but not with `.u64`.
* Integer `mul` and `mad` must say `.lo`, `.hi` or `.wide`, because the full
  product is twice as wide as the operands.
* `mul.wide.s32` reads 32-bit sources and writes a **64-bit** destination.
* Shift amounts are always 32-bit, whatever the data type.
* Float immediates use `0fXXXXXXXX` (f32 bits) or `0dXXXXXXXXXXXXXXXX` (f64 bits).

## 3. Liveness and predication

Liveness is a backward "may" dataflow problem solved by iterating to a fixed
point over the CFG:

```
liveOut(B) = ∪ liveIn(S) for all successors S
liveIn(B)  = use(B) ∪ (liveOut(B) − def(B))
```

**Predication subtlety.** In `@%p0 mov.u32 %r0, 2;` the write only happens if
`%p0` is true. If it is false, the *old* `%r0` survives. A guarded definition
therefore must **not** remove `%r0` from the live set. If it did, DCE would
delete the earlier `mov.u32 %r0, 1`, and the register allocator could reuse
`%r0`'s register. Both are silent miscompiles. The test
`liveness_guarded_definition_does_not_kill` and the differential fuzzer both
catch this mistake.

## 4. Constant folding: block-local, integers only

* **Block-local** because, across blocks, a register can carry different values
  on different paths. Handling that correctly needs SSA plus SCCP. Block-local
  is simpler and cannot be wrong.
* **Integers only.** Folding float math on the host CPU can differ from the
  GPU in rounding, flush-to-zero of denormals, and NaN payloads. A compiler must
  never change results, so float arithmetic is left alone.
* **Exact PTX semantics** live in one place (`evalIntOp`), which both the
  folder and the simulator use. Details that differ from C++:
  * Shifts by ≥ the bit width are *clamped* in PTX. `shl` gives 0, and signed
    `shr` gives the sign fill. In C++ this is undefined behaviour.
  * `shr.s32` is arithmetic. `shr.u32` and `shr.b32` are logical.
  * Results wrap to the type width (`normalizeInt`).
  * Division by zero is never folded. The instruction is left as is.

## 5. Linear-scan register allocation

Graph colouring (Chaitin and Briggs) gives better results but is
O(n²) in the interference graph. Linear scan (Poletto and Sarkar, 1999) is
O(n log n) and popular in JITs, and GPU compilers also care about compile time.

For each virtual register, the allocator computes **one interval** from the
first to the last instruction where it is defined, used or live. This hull is
conservative but safe: a value live around a loop back edge is live in every
block of the loop, so its hull covers the whole loop.

Registers are allocated **per class** (pred, 16, 32, 64-bit). 64-bit values
count double in the reported total, because the hardware uses register pairs.

**Why register count matters on GPUs:** registers are split among all threads
on an SM. More registers per thread means fewer resident warps (lower
occupancy), so there is less latency hiding.

**Not implemented:** spilling. With a budget (`--max-regs`), the allocator
reports an error instead of spilling to `.local` memory.

## 6. Object file format

* Fixed-size 76-byte instructions. This is simple to encode and decode, and it
  gives random access by index (branch targets are instruction indices).
* All integers are little-endian and written **byte by byte**. The output is
  identical on any host, and there are no struct-padding or alignment problems.
* On-disk enums (`ObjOperandKind`) are separate from in-memory enums, so the
  IR can change without breaking old files. The header has a format version.
* A string table de-duplicates names. Each function has a line table that maps
  every instruction to its PTX source line. This is the role DWARF
  `.debug_line` plays in a real cubin.
* **Reading is defensive.** Every count is checked against the bytes remaining
  *before* allocating memory. A corrupt `numInsts = 0xFFFFFFFF` is rejected
  instead of trying to allocate 300 GB.

## 7. Testing approach

| Test kind | Catches |
|---|---|
| Unit tests per stage | Regressions in a single component |
| Allocator invariant check | Two live values sharing a physical register |
| Differential testing (O0 vs O1 + RA, in the simulator) | Any optimization or allocation that changes behaviour |
| Random program generator (300 programs) | Edge cases nobody thought to write tests for |
| Corrupt object files | Crashes and huge allocations in the reader |
| ASan + UBSan build | Memory errors and C++ undefined behaviour |

The sanitizer build found a real bug during development: a dangling reference
in the test harness's `CHECK_EQ` macro.
