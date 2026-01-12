# D3D12 Backend Stress Test

Stress test for validating the new Sokol D3D12 backend against the D3D11 baseline.

## Overview

Tests different rendering techniques to validate D3D12 backend performance:
- **Individual Draws:** Per-draw uniform updates
- **Batched Uniforms:** Storage buffer technique with indexed access
- **GPU Instancing:** Standard modern technique for repeated geometry

Includes predefined test scenarios simulating game workloads from current AAA to extreme stress cases.

## Building

### Prerequisites
- Visual Studio 2026 (or compatible)
- CMake 3.15+
- Windows SDK 10.0.26100.0+

### Build Commands

**Build D3D12 backend:**
```bash
cd stress_test
cmake -B build/win_d3d12 -DSOKOL_BACKEND=SOKOL_D3D12
cmake --build build/win_d3d12 --config Release
```

**Build D3D11 backend:**
```bash
cd stress_test
cmake -B build/win_d3d11 -DSOKOL_BACKEND=SOKOL_D3D11
cmake --build build/win_d3d11 --config Release
```

Executables are output to:
- `build/win_d3d12/Release/stress-test-d3d12.exe`
- `build/win_d3d11/Release/stress-test-d3d11.exe`

## Usage

### Command-Line Parameters

```
stress-test-<backend>.exe [options]

Options:
  -draws <count>            Number of individual draw calls
  -batched-draws <count>    Number of batched draws using storage buffers
  -instanced-draws <count>  Number of instances in ONE instanced draw call
  -passes <count>           Number of render passes (default: 1)
  -buffer-updates <count>   Number of dynamic buffer updates per frame
  -texture-updates <count>  Number of texture updates per frame
  -compute <count>          Number of compute dispatches (D3D12 only)
  -duration <seconds>       Test duration in seconds (default: infinite)
```

### Rendering Techniques

**1. Individual Draws**
- Uses `-draws` parameter
- Per-draw uniform updates (64-byte MVP matrix + 16-byte color)

**2. Batched Uniforms**
- Uses `-batched-draws` parameter
- Upload all transform data to storage buffer once per frame
- Per-draw uniform is small (16-byte index only)
- Shader indexes into storage buffer: `transforms[draw_id]`

**3. GPU Instancing**
- Uses `-instanced-draws` parameter
- One draw call for many instances
- Per-instance data in vertex attributes

## Tiered Test Suites

### Tier 1: High-End Current Gen

**Purpose:** Simulates modern AAA games at maximum settings

**Workload:**
- 5,500 unique draws (buildings, NPCs, vehicles, terrain)
- 70,000 instances (vegetation, particles, crowds)
- 6 render passes (shadow cascades, main, reflections, post-process)
- 950 buffer updates (animation)
- 75 texture updates (streaming)
- 18 compute dispatches

**Commands:**

```bash
# D3D12 Modern (Batched + Instanced)
stress-test-d3d12.exe -batched-draws 5500 -instanced-draws 70000 -passes 6 -buffer-updates 950 -texture-updates 75 -compute 18 -duration 10

# D3D12 Legacy (Individual + Instanced)
stress-test-d3d12.exe -draws 5500 -instanced-draws 70000 -passes 6 -buffer-updates 950 -texture-updates 75 -compute 18 -duration 10

# D3D11
stress-test-d3d11.exe -draws 5500 -instanced-draws 70000 -passes 6 -buffer-updates 950 -texture-updates 75 -duration 10
```

---

### Tier 2: Extreme Next-Gen

**Purpose:** Simulates next-generation AAA requirements

**Workload:**
- 12,500 unique draws (Nanite-like meshes, complex terrain)
- 145,000 instances (ultra-dense vegetation, particles)
- 8 render passes (shadow cascades, Lumen GI, reflections)
- 1,500 buffer updates (complex animation, physics)
- 120 texture updates (aggressive streaming)
- 28 compute dispatches

**Commands:**

```bash
# D3D12 Modern
stress-test-d3d12.exe -batched-draws 12500 -instanced-draws 145000 -passes 8 -buffer-updates 1500 -texture-updates 120 -compute 28 -duration 10

# D3D12 Legacy
stress-test-d3d12.exe -draws 12500 -instanced-draws 145000 -passes 8 -buffer-updates 1500 -texture-updates 120 -compute 28 -duration 10

# D3D11
stress-test-d3d11.exe -draws 12500 -instanced-draws 145000 -passes 8 -buffer-updates 1500 -texture-updates 120 -duration 10
```

---

### Tier 3: Maximum Stress Test (Break Point Testing)

**Purpose:** Find absolute limits of each API

**Workload:**
- 20,000 unique draws (extreme geometry)
- 200,000 instances (absolute maximum)
- 10 render passes
- 2,000 buffer updates
- 150 texture updates
- 40 compute dispatches

**Commands:**

```bash
# D3D12 Modern
stress-test-d3d12.exe -batched-draws 20000 -instanced-draws 200000 -passes 10 -buffer-updates 2000 -texture-updates 150 -compute 40 -duration 10

# D3D12 Legacy
stress-test-d3d12.exe -draws 20000 -instanced-draws 200000 -passes 10 -buffer-updates 2000 -texture-updates 150 -compute 40 -duration 10

# D3D11
stress-test-d3d11.exe -draws 20000 -instanced-draws 200000 -passes 10 -buffer-updates 2000 -texture-updates 150 -duration 10
```

---

### Tier 4: Draw-Heavy Workload

**Purpose:** Test workloads with many unique draws and minimal instancing

**Best For:** Strategy games, city builders, simulation games

**Workload:**
- 10,000 unique draws (all different objects)
- 5,000 instances (minimal instancing)
- 4 render passes
- 800 buffer updates
- 60 texture updates
- 12 compute dispatches

**Commands:**

```bash
# D3D12 Modern (Batched)
stress-test-d3d12.exe -batched-draws 10000 -instanced-draws 5000 -passes 4 -buffer-updates 800 -texture-updates 60 -compute 12 -duration 10

# D3D12 Legacy (Individual)
stress-test-d3d12.exe -draws 10000 -instanced-draws 5000 -passes 4 -buffer-updates 800 -texture-updates 60 -compute 12 -duration 10

# D3D11
stress-test-d3d11.exe -draws 10000 -instanced-draws 5000 -passes 4 -buffer-updates 800 -texture-updates 60 -duration 10
```

---

### Tier 5: Instance-Heavy Workload

**Purpose:** Test workloads where instancing dominates

**Best For:** Nature scenes, space games, crowd simulators

**Workload:**
- 500 unique draws (minimal geometry)
- 150,000 instances (extreme instancing)
- 3 render passes
- 100 buffer updates
- 20 texture updates
- 8 compute dispatches

**Commands:**

```bash
# D3D12 Modern
stress-test-d3d12.exe -batched-draws 500 -instanced-draws 150000 -passes 3 -buffer-updates 100 -texture-updates 20 -compute 8 -duration 10

# D3D12 Legacy
stress-test-d3d12.exe -draws 500 -instanced-draws 150000 -passes 3 -buffer-updates 100 -texture-updates 20 -compute 8 -duration 10

# D3D11
stress-test-d3d11.exe -draws 500 -instanced-draws 150000 -passes 3 -buffer-updates 100 -texture-updates 20 -duration 10
```

---

## Test Suite Summary

| Tier | Draws | Instances | Best For |
|------|-------|-----------|----------|
| **Tier 1: High-End Current Gen** | 5,500 | 70,000 | Modern AAA games |
| **Tier 2: Extreme Next-Gen** | 12,500 | 145,000 | Future AAA games |
| **Tier 3: Maximum Stress** | 20,000 | 200,000 | Absolute limits |
| **Tier 4: Draw-Heavy** | 10,000 | 5,000 | Strategy/simulation |
| **Tier 5: Instance-Heavy** | 500 | 150,000 | Nature scenes |

## Running Tests

### For Best Results
1. Close all background applications
2. **Log out of Windows** to minimize system overhead
3. Run from command line
4. Use `-duration 10` for 10-second tests

### Example Test Run

```bash
# Navigate to build directory
cd build/win_d3d12/Release

# Run Tier 1 test (recommended starting point)
stress-test-d3d12.exe -batched-draws 5500 -instanced-draws 70000 -passes 6 -buffer-updates 950 -texture-updates 75 -compute 18 -duration 10
```

### Reading Results

Tests output:
- **FPS:** Average frames per second over test duration
- **Frame Time:** Milliseconds per frame
- **Draw Calls:** Number of draw calls submitted
- **State Changes:** Pipeline and binding changes
- **Buffer/Texture Updates:** Dynamic resource updates

D3D12 tests also show timing breakdown:
- **commit:** Time to commit command list
- **present:** Time to present frame
- **wait:** Time waiting for GPU

## Technical Details

### Storage Buffer Batching Implementation

**Shader (Vertex):**
```hlsl
StructuredBuffer<TransformData> transforms : register(t0);
cbuffer draw_params : register(b0) {
    uint draw_id;
};

vs_out main(vs_in inp) {
    TransformData t = transforms[draw_id];
    outp.pos = mul(t.mvp, inp.pos);
    outp.color = t.tint;
    return outp;
}
```

**CPU-side:**
```c
// Upload ALL transforms once per frame
for (int i = 0; i < num_draws; i++) {
    transform_data[i].mvp = calculate_transform(objects[i]);
    transform_data[i].tint = objects[i].color;
}
sg_update_buffer(transform_buffer, transform_data, size);

// Draw with tiny uniform updates (just index)
for (int i = 0; i < num_draws; i++) {
    uint32_t draw_id = i;
    sg_apply_uniforms(0, &draw_id);  // Only 16 bytes!
    sg_draw(0, 6, 1);
}
```

### D3D12 vs D3D11 Overhead Characteristics

**D3D12:**
- Higher fixed overhead per frame (command list commit, present, GPU wait)
- Lower per-draw overhead (explicit command list recording)
- Overhead amortized over many draws

**D3D11:**
- Lower fixed overhead per frame
- Higher per-draw overhead (driver validation)
- Immediate mode execution

## License

This benchmark uses the Sokol library which is licensed under the zlib license.

## Credits

Built using:
- [Sokol](https://github.com/floooh/sokol) - Simple STB-style cross-platform libraries for C and C++
- Direct3D 12 / Direct3D 11 - Microsoft graphics APIs
