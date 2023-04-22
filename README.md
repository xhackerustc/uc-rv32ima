# uc-rv32ima
Run linux on various MCUs with the help of RISC-V emulator. This project uses [CNLohr's mini-rv32ima](https://github.com/cnlohr/mini-rv32ima) RISC-V emulator core to run Linux on various MCUs such as ESP32C3, RP2040, STM32F103 etc. Although, only ESP32C3 is tested now. In theory, ESP series SoCs are supported, but except ESP32C3 I don't have other SoCs' dev boards, so I can't test them.

## How it works
It uses one 8MB SPI PSRAM chip as the system memory. On startup, it initializes the PSRAM, and load linux kernel Image(an initramfs is embedded which is used as rootfs) and device tree binary from flash to PSRAM, then start the booting.

- To improve the performance, a simple cache is implemented, it turns out we acchieved 95.1% cache hit during linux booting:
    - 4KB cache
    - two way set associative
    - 64B cacheline

## Difference from [tvlad1234's pico-rv32ima](https://github.com/tvlad1234/pico-rv32ima)
- esp32c3 VS rp2040, although rp2040 will be supported too in uc-rv32ima
- only one 8MB SPI PSRAM is needed
- a simple cache mechanism is implemented, thus much better performance
- no need sdcard

## Requirements
- one ESP32C3 development board
- one 8 megabyte (64Mbit) SPI PSRAM chip (I used PSRAM64H) and solder it on a SOP8 adapter board, then properly connect the adapter board with MCU dev board with dupont line.

## How to use
- Ensure the SPI PSRAM chip pins are properly connected with your MCU board. E.g the PSRAM is connected with below GPIOs of ESP32C3, and don't forget VCC and GND pins:
    - MOSI: GPIO7
    - MISO: GPIO2
    - CS: GPIO10
    - SCLK: GPIO6

- build uc-rv32ima with esp idf env

- write imgs to the board's flash as following with esptool then reset the board
    - 0x10000 build/uc-rv32ima.bin
    - 0x8000 build/partition_table/partition-table.bin
    - 0x200000 main/Image
    - 0x3ff000 main/uc.dtb

- In no less than 1 sec, Linux kernel messages starts printing on the USB CDC console. The boot process from pressing reset button to linux shell takes about 1 minute and 20 seconds.


https://user-images.githubusercontent.com/113400028/233765806-63810c28-1fe9-45fc-b7a3-8c76106ba86c.mp4

