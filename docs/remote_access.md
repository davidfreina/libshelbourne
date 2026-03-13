# Remote Access

How to get SSH access to a SoundTouch 20 running stock firmware.

## Prerequisites

- A SoundTouch 20 (device ID `0x0923`, "spotty" variant)
- A USB flash drive formatted as FAT32
- The device connected to your local network via Wi-Fi or Ethernet

## Method 1: USB Trigger (Simplest)

The stock firmware checks for a special file on USB drives plugged into
the rear micro-USB service port.

1. **Prepare the USB drive**: Create an empty file named `remote_services`
   in the root directory of a FAT32-formatted USB stick:

   ```
   touch /Volumes/USB/remote_services
   ```

2. **Insert the drive** into the rear **SERVICE** micro-USB port (not the
   front USB-A port).

3. The device's udev mount script detects the file and starts SSH and
   Telnet daemons automatically.

4. **Connect**:

   ```
   ssh root@<device-ip>
   ```

   No password is needed (root has an empty password).

5. **Find the device IP**: Check your router's DHCP lease table, or use
   the SoundTouch app which shows the IP in device settings.

## Making It Persistent

By default, SSH is only enabled while the USB stick is inserted. After a
reboot without the stick, SSH will not start.

To make it permanent (survives reboot):

```
ssh root@<device-ip> "touch /mnt/nv/remote_services"
```

`/mnt/nv` is persistent NAND storage that survives reboots. The init
scripts check for `/mnt/nv/remote_services` on boot.

## Factory Reset Warning

Factory reset (hold Preset 1 + Volume Down during boot) erases `/mnt/nv`,
which removes the persistent `remote_services` file. After a factory
reset, you need to re-insert the USB stick to re-enable SSH.

## Alternative: USB Gadget Network

When a micro-USB cable connects the service port to a PC:

- The device creates a virtual Ethernet interface over USB
- Device IP: `203.0.113.1`
- Host IP: `203.0.113.2` (assigned via DHCP)
- SSH works over this interface once remote services are enabled

## Alternative: Serial Console

The rear 3.5mm SERVICE jack provides a serial console:

- Baud rate: 115200, 8N1
- Pinout: Tip=TX, Ring=RX, Sleeve=GND
- By default, this provides a Bose CLI (not a shell)
- To get a root shell via serial, create `/mnt/nv/local_services`

## Security Notes

- All processes run as root
- SSH host keys are shared across all units of this model
- The firewall only blocks external subnets; local network access is open
- Firmware updates are not cryptographically signed
