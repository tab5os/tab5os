# Cross toolchain for Tab5 OS apps: the ESP-IDF RISC-V compiler on PATH
# (source tools/env.sh first). Apps are position-independent ELFs that the
# OS loads into PSRAM; nothing from libc is linked in, imports are bound by
# the OS loader against sdk/abi/api_v<N>.sym.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)
set(CMAKE_C_COMPILER riscv32-esp-elf-gcc)
set(CMAKE_CXX_COMPILER riscv32-esp-elf-g++)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
