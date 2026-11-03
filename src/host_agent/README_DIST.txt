FPGA Deploy Assistant (onedir)

Double-click FPGA_Deploy_Assistant.exe to start.

Included:
- prebuilt/gcc-arm-none-eabi
- prebuilt/ncnn
- prebuilt/openocd
- src / bsp / tools / firmware
- train / skills / board_profile.json
- models / models_sd / apps (if present at pack time)

New PC:
1. Windows x64. No need to install Python, Vitis gcc, or full fpga_ncnn.
2. Training needs datasets/ (download on first train, or copy from another PC).
3. Before push: configure PC NIC in Settings or chat tool; open board File Transfer.
4. Board sources ship in src/, bsp/ and tools/. Leave the board-project setting empty unless replacing firmware from a Vitis tree.
5. JTAG download uses Vivado xsdb or Vitis xsct when installed, otherwise prebuilt/openocd. Rebuilding firmware still needs Vitis.

Default torch is whatever was installed on the build machine (often CUDA if that env had it).
See BUILD_GUIDE.md for rebuild steps.
