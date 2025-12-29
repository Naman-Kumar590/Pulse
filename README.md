# Smart Governor Daemon 🔧

A small Linux daemon written in C that monitors CPU utilization and thermal zones and automatically switches CPU frequency governors between a performance and a powersave governor. It supports a simple Zenity GUI for configuring thresholds, daemonizes to run in the background, logs to syslog, and emits a shutdown summary of time spent in each mode.

---

## Features ✅

- Automatic switching between **performance** and **powersave** governors based on CPU usage thresholds
- Temperature-based forced powersave to protect hardware
- GUI configuration via `zenity` (optional) at startup
- System daemonization and PID locking (`/var/run/smart-governor.pid`)
- Syslog logging, including a shutdown summary with time spent in each governor

---

## Requirements

- Linux with cpufreq support (accessible under `/sys/devices/system/cpu/*/cpufreq/scaling_governor`)
- Access to thermal zones under `/sys/class/thermal/thermal_zone*/temp` (optional but recommended)
- `gcc` to build
- Root privileges to change CPU governors (run with `sudo` or install as a system daemon)
- `zenity` (optional) to use the startup GUI

---

## Build

From the directory containing `smart-governor.c`:

```bash
gcc -O2 -Wall -o smart_governor smart-governor.c
```

Move the binary to a system location (optional):

```bash
sudo mv smart_governor /usr/local/sbin/
```

---

## Configuration

By default the app loads `/etc/smart-governor.conf` if present. The format is simple `key = value` lines. Example:

```
poll_interval_secs = 2
cpu_high_threshold = 60.0
cpu_low_threshold = 25.0
temp_high_threshold_c = 85
performance_governor = performance
powersave_governor = schedutil
```

If the config file is not found, built-in defaults are used.

---

## Usage

- Interactive (shows Zenity GUI to change thresholds):

```bash
sudo ./smart_governor
```

- Non-interactive/daemon mode: the program will fork and run in background after start. Check syslog for messages (e.g., `sudo journalctl -f` or `tail -f /var/log/syslog`).

Notes:
- The program will refuse to run if not started as root (it must write to cpufreq files).
- On shutdown (SIGINT/SIGTERM) it logs a summary of time spent in powersave vs performance.

---

## Example systemd service

Create `/etc/systemd/system/smart-governor.service` with:

```ini
[Unit]
Description=Smart CPU Governor Daemon
After=network.target

[Service]
Type=forking
ExecStart=/usr/local/sbin/smart_governor
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Then enable & start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now smart-governor.service
```

---

## Troubleshooting

- If you see write permission errors when changing governors, ensure the binary is run as `root` and that cpufreq is enabled on your platform.
- If no thermal zones are found, `get_max_temp()` will return `-1` and temperature-based forcing will be disabled.
- If the Zenity dialog fails (e.g., on headless servers), run without X/Zenity installed; defaults or `/etc/smart-governor.conf` will be used.

---

## Security and Safety Notes ⚠️

- This project requires root privileges to modify CPU governor settings — review the code and test cautiously.
- Behavior depends on kernel interfaces (`/sys`); different distributions/hardware might expose governors or thermal sensors with different names.

---

## Contributing & License

Contributions welcome. Add issues/pull requests to improve robustness, add tests, or package the daemon for your distribution. Add a `LICENSE` file to specify project license (e.g., MIT).

---

If you'd like, I can also add a `systemd` unit file in the repository or a small packaging script to install/uninstall the binary. 🔧