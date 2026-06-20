# Qiming Hardware Acceleration Architecture

## FPGA Feasibility Analysis

### Architecture Decomposition

The Qiming simulator has two distinct parts.

| Location | Modules |
|---|---|
| CPU (must stay) | PEG parser, Elaboration, SDF back-annotation, Test harness |
| Accelerable | Event queue, Signal propagation (4-value), Expression evaluation, Sensitivity matching, Waveform recording |

### Feasibility Assessment

| Dimension | Verdict |
|---|---|
| Scale | RV32I ~200 signals; mid-range FPGA handles ~50K. Million-gate ASIC exceeds BRAM |
| Performance | 10-100x speedup for wide designs with long simulations |
| Complexity | Very high -- tree expression eval, event sorting, dual semantics in hardware |
| Debug | Hardware bugs reproduce slowly; no breakpoints/waveforms |
| Maintainability | Each extension requires changes on both CPU and FPGA |

### Bottlenecks Unsuitable for FPGA

| Module | Problem | Code Share |
|---|---|---|
| PEG Parser | Recursive descent depth thousands; FPGA cannot implement | ~20% |
| Elaboration | Dynamic allocation, module lookup, generate expansion | ~15% |
| Expression Eval | UIR tree structure, recursive traversal; needs flattening to DFG | ~30% |
| Event Queue | Ordered insert (time+delta); O(log n) heap, needs sorting network in HW | ~10% |

Only ~25% of code paths suit FPGA acceleration.

### Recommended Approach (Priority Order)

| Priority | Method | Rationale |
|---|---|---|
| 1 | GPU (CUDA/OpenCL) | Signal propagation suits SIMD; 3-6 months vs 1-2 years FPGA |
| 2 | Multi-threaded CPU | Existing partition infrastructure; zero hardware cost |
| 3 | FPGA Coprocessor (HLS) | Accelerate only eval_assign + signal propagation; use Vivado HLS |

### FPGA Architecture (if required)

| Component | Implementation |
|---|---|
| Signal Store | BRAM, 8-bit per signal, double-buffered current/next |
| Eval Pipeline | DSP+LUT, 4-stage: Fetch->Decode->ALU->Writeback |
| Sensitivity Matcher | CAM, O(1) parallel lookup |
| Event Scheduler | Hardware heap / sorting network, 1-2 cycles per enqueue |

---

## CPU + GPU + ASIC Heterogeneous Architecture

This is the architecture behind **Cadence Palladium / Synopsys Zebu / Mentor Veloce**.

### Layer Division

| Layer | Share | Responsibilities |
|---|---|---|
| CPU (Host) | ~30% | Parse/compile, Partitioning, Testbench, Waveform display, SDF/SPEF |
| GPU | ~40% | Combinational eval (SIMD), Bit-level parallelism, Waveform dump (24GB+ VRAM), Delay model lookup |
| ASIC | ~30% | Event scheduler (O(1) enqueue), Crossbar interconnect, Timing engine, Delta-cycle FSM |

### ASIC: Emulation Processor (EP)

Each EP contains:
- Instruction Cache (32KB)
- Signal Register File (256 x 8-bit)
- 4-way parallel execution (ALU, Boolean, Shift, Compare)
- Crossbar switch (NxN, full-duplex, single-cycle)
- Hardware event heap (time+delta 2D sort)

**EP Instruction Set:**

| Instr | Format | Description |
|---|---|---|
| EVAL | EVAL op, dst, srcA, srcB | Combinational evaluation |
| SCHED | SCHED time, delta, sig | Schedule event |
| XFER | XFER src_ep, dst_ep, sig | Inter-EP signal transfer |
| BRANCH | BRANCH cond, target | Conditional branch |
| TIMING | TIMING sig, rise, fall | Timing annotation |

### Compilation Flow

1. CPU parses Verilog/VHDL -> UIR -> flattened netlist
2. Graph partitioning (Metis/hMetis) maps design to EPs, minimizing cross-partition signals
3. Code generation: UIR -> EP microcode, GPU -> CUDA kernels
4. Runtime loop:

```
while (!done) {
    GPU: eval_combinational();
    ASIC: step_delta();
    ASIC->GPU: sync_signals();
    if (checkpoint)
        GPU->CPU: dump_waves();
}
```

### Performance Estimates

| Metric | CPU Only | +GPU | +GPU+ASIC |
|---|---|---|---|
| Events/s | ~10M | ~500M | ~10B |
| Gate capacity | ~1M | ~100M | ~2B |
| Latency/cycle | ~100ns | ~1us | ~10ns |
| Power | 300W | 500W | 800W |
| Dev cost | $0 | $500K | $5-10M |

### Commercial Comparison

| Feature | Palladium Z1 | This Design |
|---|---|---|
| Processor | Custom ASIC (XP) | CPU+GPU+ASIC |
| Capacity | 2B gates | ~2B gates (scalable) |
| Compile time | Hours | Hours |
| Debug | Full visibility | GPU VRAM = full waveform |
| Power | ~20KW (full rack) | ~800W (single node) |
| Cost | $1M+ | $5-10M (tapeout), lower at scale |

### Implementation Roadmap

**Phase 1 (Year 1): CPU + GPU Prototype**
- CUDA-accelerated eval_assigns and signal propagation
- Verify architectural correctness
- Target: 100M gates, 100M events/s

**Phase 2 (Year 2): ASIC Design + Tapeout**
- EP micro-architecture RTL design
- Crossbar network, Event scheduler ASIC
- TSMC 7nm tapeout (Mask: $3-5M)
- Target: 1B gates, 1B events/s

**Phase 3 (Year 3): Multi-chip Scaling**
- Inter-chip interconnect (SerDes)
- Distributed event scheduling
- Target: 10B gates (16-chip array)

### Key Risks

| Risk | Level | Mitigation |
|---|---|---|
| Tapeout cost | Critical: $5-10M | GPU prototype validates business model first |
| Compiler complexity | Critical | Hire EDA compiler experts |
| Timing accuracy | Medium | ASIC deterministic execution |
| Ecosystem | Medium | Reuse Qiming parser |
| Competition | Medium | Target AI chip customers (new market) |

### Conclusion

| Aspect | Assessment |
|---|---|
| Feasibility | Architecture is sound, technical path clear |
| Cost | Tapeout $5-10M, total 3yr ~$20-30M |
| Market | AI inference chip companies, domestic substitution (sanctioned EDA customers) |
| Timing | Now -- domestic EDA boom + AI chip explosion |

**Key insight**: Qiming parser + UIR + event-driven engine is the correct foundation.
GPU+ASIC upgrades it from software simulator to hardware emulation accelerator,
competing with Palladium at ~1/10 the cost.

---
*Author: Xiao, Shaoning | Date: 2026-06-20*
