# 🖥️ MicroANT - Simple Heart Rate Monitor Display

[![Download MicroANT](https://img.shields.io/badge/Download-MicroANT-brightgreen?style=for-the-badge)](https://github.com/romankhan720/MicroANT/releases)

MicroANT is a tool that reads wireless heart rate data using a Garmin USB stick. It shows your beats per minute (BPM) on a VGA screen. This software runs directly on a computer’s hardware without an operating system. It works with standard x86 PCs and provides a clear display of your heart rate.

---

## 🔍 What is MicroANT?

MicroANT reads heart rate data from devices that use the ANT+ wireless protocol. This is the same technology used by Garmin fitness devices. The application connects to a Garmin USB stick and shows your pulse on a simple screen that works with VGA monitors. It requires no complex software or setup and runs right on the bare hardware of your computer.

---

## 💻 System Requirements

Before you start using MicroANT, confirm your system meets these needs:

- A PC with an x86 processor (most modern and older computers will work).
- A standard VGA monitor for display output.
- A Garmin USB stick compatible with ANT+ wireless devices.
- A USB 3.0 or later port (MicroANT supports the xHCI USB standard).
- BIOS or UEFI firmware that can boot from USB drives.
- At least 256 MB of RAM (enough memory to run this small, dedicated software).
- A keyboard connected to navigate simple menus if needed.

MicroANT does not run under Windows or other operating systems directly. It boots directly from a USB stick or other boot media.

---

## 🚀 Getting Started

Follow these steps to download and start using MicroANT:

1. Click the large green **Download MicroANT** badge above. This will take you to the official releases page.

2. On the releases page, find the latest version of MicroANT. Look for a file named something like `MicroANT_vX.Y.img` or a similar image file.

3. Download this image file to your PC.

4. You will need to write this image to a USB flash drive to make it bootable. You can use free tools such as [Rufus](https://rufus.ie/) on Windows.

5. Insert a USB drive (at least 1 GB) into your PC. Open Rufus, select the downloaded MicroANT image, and create the bootable USB drive by following onscreen instructions.

6. Shut down your computer and insert the USB drive you just prepared.

7. Boot your PC and enter the boot menu (usually by pressing F12, F11, ESC, or DEL during startup, depending on your system).

8. Choose the USB drive as the boot device.

9. MicroANT will start and display your heart rate data from the Garmin USB stick connected to the computer.

---

## ⚙️ Setup Instructions

1. **Connect the Garmin USB stick** to your PC before booting MicroANT.

2. Make sure your heart rate sensor (chest strap or wrist device) is powered on and within range (up to 10 meters).

3. Boot from the prepared USB stick with MicroANT as explained in the previous section.

4. The screen will show your heart rate in beats per minute (BPM). The display is simple text-mode VGA for clear and easy reading.

5. If no data shows, check that your devices are paired and within range.

---

## 📥 Download MicroANT

MicroANT is available to download from the official GitHub releases:

[![Download MicroANT](https://img.shields.io/badge/Download-MicroANT-blue?style=for-the-badge)](https://github.com/romankhan720/MicroANT/releases)

Click the link above to visit the releases page. Choose the latest stable image file and follow the instructions to create a bootable USB stick.

---

## 🔧 Troubleshooting

- **No heart rate displayed**: Verify that your Garmin USB stick is firmly connected. Make sure your heart rate sensor is on and close by.

- **USB device not recognized**: Some USB sticks may not be compatible. Try a Garmin ANT+ USB stick model known to work with your system.

- **No video output**: Confirm your monitor has a VGA input and that the cable is properly connected.

- **Cannot boot from USB**: Check your BIOS settings. Enable booting from USB devices. You might need to disable Secure Boot.

---

## 📝 Technical Details

- MicroANT runs on bare-metal x86 hardware. It does not require Windows or any other operating system.

- It uses a lightweight driver stack to communicate with ANT+ devices over the USB interface.

- The VGA output uses standard text mode for simple and fast rendering of BPM data.

- The program is written in C and leverages USB xHCI protocols to access wireless sensor data.

- The entire system fits on a bootable USB image that can run on most PC hardware supporting legacy or UEFI boot.

---

## 🛠️ Customization and Development

MicroANT is open for enhancement by users with experience in low-level programming and hardware drivers. The source code is available in this repository.

Developers can modify or improve how MicroANT interfaces with new ANT+ devices or add support for additional display types.

---

## 📚 Resources

- [ANT+ Device Profiles](https://www.thisisant.com/developer/ant-device-profiles/)
- [Garmin ANT+ USB Stick Information](https://www.thisisant.com/resources/ant-usb-stick/)
- [Bootable USB Creation Tool - Rufus](https://rufus.ie/)
- Basic VGA text mode programming references available from online OS development communities.

---

## 🔗 Links

- Official MicroANT GitHub repository: https://github.com/romankhan720/MicroANT
- Download latest releases here: https://github.com/romankhan720/MicroANT/releases

---

## 🧰 Glossary

- **ANT+**: A wireless protocol used by fitness devices to communicate heart rate and other sensor data.

- **Bare-metal**: Software that runs directly on computer hardware without an operating system.

- **VGA**: A type of video interface used for standard computer monitors.

- **xHCI**: USB 3.0 host controller interface used to communicate with USB devices.

- **BPM**: Beats per minute, a measure of heart rate.