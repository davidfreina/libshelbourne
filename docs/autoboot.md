# Auto-boot: Running Your App on Startup

You can configure the device to start your application automatically on
boot, replacing the stock Bose software entirely. This uses the device's
built-in process supervisor (shepherdd) with a custom configuration.

No stock Bose process will run. Your app has exclusive access to the
audio hardware, display, keypad, and LED. A factory reset (hold Preset 1
+ Volume-down during boot) reverts to stock behavior.

## How It Works

The init script (`/etc/init.d/SoundTouch`) checks for the directory
`/mnt/nv/shepherd/` at boot. If it exists, shepherdd reads its XML
configuration from there instead of `/opt/Bose/etc/`. This lets you
control which processes start without modifying the root filesystem.

`/mnt/nv/` is writable NAND flash that persists across reboots. It is
cleared by a factory reset.

## Setup

### 1. Build your app

```
make
```

### 2. Copy the binary to NAND

The binary must live on `/mnt/nv/` (NAND flash), not `/media/sda1/`
(USB). The USB drive may not be mounted when shepherdd starts.

```
scp build/demo root@<device-ip>:/mnt/nv/demo
ssh root@<device-ip> chmod +x /mnt/nv/demo
```

### 3. Create the shepherd override

```
ssh root@<device-ip> 'mkdir -p /mnt/nv/shepherd && cat > /mnt/nv/shepherd/Shepherd-spotty.xml' <<'XML'
<ShepherdConfig>
  <daemon name="/mnt/nv/demo"/>
</ShepherdConfig>
XML
```

The daemon `name` must be an absolute path — shepherdd uses `execv`,
not `execvp`, so it will not search `PATH`.

Do not set `recovery="reboot"`. If your app crashes, shepherdd will
restart it. With `recovery="reboot"`, a crash causes a reboot loop that
is only recoverable via factory reset.

### 4. Reboot

```
ssh root@<device-ip> reboot
```

The device will start your app instead of the Bose software. SSH
remains available.

## Reverting to Stock

Remove the override directory, then reboot:

```
ssh root@<device-ip> 'rm -rf /mnt/nv/shepherd && sync'
ssh root@<device-ip> reboot
```

Or factory reset (hold Preset 1 + Volume-down during boot). Note:
factory reset clears all of `/mnt/nv/`, including your binary and SSH
keys. You will need to re-enable SSH via USB stick.

## Notes

- Your app is a child of shepherdd. If shepherdd is killed, your app
  receives SIGTERM. If your app needs to manage stock processes (e.g.,
  for testing), freeze shepherdd with SIGSTOP, don't kill it.

- `shelbourne_stop_stock_processes()` is safe to call even when no stock
  processes are running — it's a no-op.

- `shelbourne_stock_processes_active()` returns 0 under the override
  since no stock audio processes are started.

- If you need to pass arguments to your app, add `<arg>` elements:

  ```xml
  <ShepherdConfig>
    <daemon name="/mnt/nv/myapp">
      <arg>--volume</arg>
      <arg>30</arg>
    </daemon>
  </ShepherdConfig>
  ```

- You can run multiple daemons. shepherdd manages them all:

  ```xml
  <ShepherdConfig>
    <daemon name="/mnt/nv/myapp"/>
    <daemon name="/mnt/nv/my-webserver"/>
  </ShepherdConfig>
  ```
