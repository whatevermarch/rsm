# Reflective Shadow Map
(Very) Naive implementation of [Reflective Shadow Map](https://dl.acm.org/doi/10.1145/1053427.1053460) without Screen-space interpolation or any optimization. This demo is based on [Cauldron Framework](https://github.com/GPUOpen-LibrariesAndSDKs/Cauldron), and it only exploits Vulkan backend.

![RSM](https://github.com/whatevermarch/rsm/raw/main/screenshot.png)

## Prerequisites
The following packages need to be installed in order to run this demo.
- CMake 3.16
- Visual Studio 2017
- Windows 10 SDK 10.0.18362.0 (for DirectXMath)
- Vulkan SDK 1.2.131.2

## Build Instruction
- Create blank directory on the root directory of this repository. Let's say `build/` for example.
- Change directory to `build/`, and run `cmake ..`.
- Now you will see a Visual Studio solution file. Open it and build.
- The execution file will be in `bin/` at the root directory. (Or you can also execute it via Visual Studio Local Debugger.)

### References
Carsten Dachsbacher and Marc Stamminger. 2005. Reflective shadow maps. In Proceedings of the 2005 symposium on Interactive 3D graphics and games (I3D '05). Association for Computing Machinery, New York, NY, USA, 203–231. DOI:https://doi.org/10.1145/1053427.1053460
