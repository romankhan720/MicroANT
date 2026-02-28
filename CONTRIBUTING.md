# Contributing to MicroANT

Thanks for your interest in contributing!

## How to contribute

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/my-change`)
3. Make your changes
4. Test that the project builds: `make clean && make`
5. If possible, test with QEMU: `make run`
6. Commit your changes with a clear message
7. Push to your fork and open a Pull Request

## Ideas for contributions

- Support for additional ANT+ device profiles (speed/cadence, power meter)
- Multiple heart rate sensor pairing
- Display improvements (graphs, history, animations)
- USB hub support in the xHCI driver
- Port to x86_64 (long mode)

## Code style

- C99 (gnu99 with GCC extensions)
- English comments — didactic style, explain the hardware
- Keep it bare-metal: no standard library, no dynamic allocation after boot
