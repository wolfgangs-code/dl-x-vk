#!/usr/bin/env python3
import os
import sys
import time
import glob
import select
import subprocess
import signal

# Ensure single sudo prompt: re-exec with sudo if not root
if os.geteuid() != 0:
    print("[*] Root privileges required for systemd and DRM control.")
    print("[*] Requesting sudo once upfront...")
    try:
        os.execvp("sudo", ["sudo", sys.executable] + sys.argv)
    except Exception as e:
        print(f"[!] Failed to escalate via sudo: {e}")
        sys.exit(1)

def is_dl_connected():
    for dev in glob.glob("/sys/bus/usb/devices/*"):
        v_path = os.path.join(dev, "idVendor")
        p_path = os.path.join(dev, "idProduct")
        if os.path.exists(v_path) and os.path.exists(p_path):
            try:
                with open(v_path) as vf, open(p_path) as pf:
                    if vf.read().strip() == "17e9" and pf.read().strip() == "6015":
                        return dev
            except Exception:
                pass
    return None

def wait_for_user_replug(reason):
    print("\n" + "=" * 76)
    print(f" >>> [ACTION REQUIRED] {reason}")
    print(" >>> 1. UNPLUG the DisplayLink USB-C connector from your laptop.")
    print(" >>> 2. Wait 2 seconds, then PLUG IT BACK IN.")
    print(" >>> (Or press [ENTER] at any time once reconnected)")
    print("=" * 76)

    # First wait for disconnect (if connected)
    was_connected = is_dl_connected() is not None
    if was_connected:
        print("[*] Waiting for USB disconnect...", end="", flush=True)
        while is_dl_connected() is not None:
            r, _, _ = select.select([sys.stdin], [], [], 0.3)
            if r:
                sys.stdin.readline()
                print(" [User confirmed]")
                return
        print(" [Disconnected!]")

    # Now wait for reconnect
    print("[*] Waiting for USB reconnect...", end="", flush=True)
    while is_dl_connected() is None:
        r, _, _ = select.select([sys.stdin], [], [], 0.3)
        if r:
            sys.stdin.readline()
            print(" [User confirmed]")
            return
    print(" [Reconnected!]")
    print("[*] Allowing USB and DRM displays 4 seconds to settle...")
    time.sleep(4.0)

def find_pid(name_pattern):
    try:
        out = subprocess.check_output(["pgrep", "-f", name_pattern], stderr=subprocess.DEVNULL).decode().strip()
        lines = out.split()
        if lines:
            return int(lines[0])
    except Exception:
        pass
    return None

def get_proc_stats(pid):
    try:
        with open(f"/proc/{pid}/stat", "r") as f:
            fields = f.read().split()
            utime = int(fields[13])
            stime = int(fields[14])
        rss = 0
        threads = 0
        with open(f"/proc/{pid}/status", "r") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    rss = int(line.split()[1])
                elif line.startswith("Threads:"):
                    threads = int(line.split()[1])
        return utime + stime, rss, threads
    except Exception:
        return 0, 0, 0

def get_cpu_total():
    with open("/proc/stat", "r") as f:
        fields = [int(x) for x in f.readline().split()[1:]]
        idle = fields[3] + fields[4]
        return sum(fields), idle

def get_gpu_percent():
    try:
        with open("/sys/class/drm/card1/device/gpu_busy_percent", "r") as f:
            return int(f.read().strip())
    except Exception:
        return -1

def sample_metrics(pid, label, duration_sec=15):
    num_cpus = os.cpu_count() or 1
    samples = []
    print(f"\n[*] Sampling '{label}' (PID {pid}) for {duration_sec} seconds...")
    print(f"[*] (Test in progress: video playing on primary + wallpaper animated on secondary)")
    
    t0, idle0 = get_cpu_total()
    p0, _, _ = get_proc_stats(pid)

    for i in range(duration_sec):
        time.sleep(1.0)
        t1, idle1 = get_cpu_total()
        p1, rss, th = get_proc_stats(pid)
        gpu = get_gpu_percent()

        dt = t1 - t0
        dp = p1 - p0
        d_idle = idle1 - idle0

        proc_cpu = (dp / dt) * 100.0 * num_cpus if dt > 0 else 0.0
        sys_cpu = ((dt - d_idle) / dt) * 100.0 if dt > 0 else 0.0
        samples.append({
            "proc_cpu": proc_cpu,
            "sys_cpu": sys_cpu,
            "rss_mb": rss / 1024.0,
            "gpu": gpu,
            "threads": th
        })
        print(f"  [{i+1:2d}/{duration_sec}] Process CPU: {proc_cpu:6.1f}% | Sys CPU: {sys_cpu:5.1f}% | RSS: {rss/1024.0:5.1f} MB | GPU: {gpu:2d}% | Threads: {th}")
        t0, idle0 = t1, idle1
        p0 = p1

    avg_proc = sum(s["proc_cpu"] for s in samples) / len(samples)
    peak_proc = max(s["proc_cpu"] for s in samples)
    avg_sys = sum(s["sys_cpu"] for s in samples) / len(samples)
    avg_rss = sum(s["rss_mb"] for s in samples) / len(samples)
    avg_gpu = sum(s["gpu"] for s in samples) / len(samples)
    avg_th = sum(s["threads"] for s in samples) / len(samples)

    return {
        "avg_proc_cpu": avg_proc,
        "peak_proc_cpu": peak_proc,
        "avg_sys_cpu": avg_sys,
        "avg_rss_mb": avg_rss,
        "avg_gpu": avg_gpu,
        "avg_threads": avg_th
    }

def main():
    workspace_dir = "/home/wolfgang/Documents/displaylink-turbo"
    turbo_bin = os.path.join(workspace_dir, "build/dl-x-vk")

    if not os.path.exists(turbo_bin):
        print(f"[!] Cannot find binary {turbo_bin}. Run cmake build first.")
        sys.exit(1)

    print("\n" + "=" * 76)
    print(" DISPLAYLINK DRIVER COMPARISON BENCHMARK (REAL-WORLD STRESS TEST)")
    print("=" * 76)
    print(" Test Setup:")
    print("  - Primary Display:  Landscape 2560x1440 @ 60Hz (Video Playback)")
    print("  - Secondary Display: Portrait 1080x1920 @ 60Hz (Animated Wallpaper)")
    print("  - Sudo Authentication: Done upfront (no more password prompts)")
    print("=" * 76)

    # ---------------------------------------------------------
    # PHASE 1: Vanilla DisplayLinkManager
    # ---------------------------------------------------------
    print("\n[PHASE 1] Preparing Vanilla DisplayLinkManager...")
    subprocess.run(["systemctl", "stop", "displaylink-turbo.service"], stderr=subprocess.DEVNULL)
    subprocess.run(["systemctl", "stop", "displaylink.service"], stderr=subprocess.DEVNULL)
    time.sleep(1.0)

    wait_for_user_replug("Reconnecting dock to reset ASIC firmware for Vanilla DisplayLink...")

    print("[*] Starting vanilla displaylink.service...")
    subprocess.run(["systemctl", "start", "displaylink.service"], check=True)
    time.sleep(3.0)

    vanilla_pid = find_pid("DisplayLinkManager")
    if not vanilla_pid:
        print("[!] Error: DisplayLinkManager failed to start.")
        sys.exit(1)
    print(f"[*] Vanilla DisplayLinkManager running with PID {vanilla_pid}.")

    print("[*] Allowing desktop environment 5 seconds to stabilize modes...")
    time.sleep(5.0)

    vanilla_results = sample_metrics(vanilla_pid, "Vanilla DisplayLinkManager", duration_sec=15)

    # Stop vanilla service
    print("\n[*] Stopping vanilla displaylink.service...")
    subprocess.run(["systemctl", "stop", "displaylink.service"], stderr=subprocess.DEVNULL)
    time.sleep(1.0)

    # ---------------------------------------------------------
    # PHASE 2: DisplayLink Turbo (dl-x-vk)
    # ---------------------------------------------------------
    print("\n[PHASE 2] Preparing DisplayLink Turbo (dl-x-vk)...")
    wait_for_user_replug("Reconnecting dock to reset ASIC firmware for DisplayLink Turbo...")

    print("[*] Starting DisplayLink Turbo via systemd-run...")
    subprocess.run([
        "systemd-run", "--unit=displaylink-turbo",
        turbo_bin, "-logging"
    ], check=True)
    time.sleep(3.0)

    turbo_pid = find_pid("dl-x-vk")
    if not turbo_pid:
        print("[!] Error: DisplayLink Turbo (dl-x-vk) failed to start.")
        sys.exit(1)
    print(f"[*] DisplayLink Turbo running with PID {turbo_pid}.")

    print("[*] Allowing desktop environment 5 seconds to stabilize modes...")
    time.sleep(5.0)

    turbo_results = sample_metrics(turbo_pid, "DisplayLink Turbo (dl-x-vk)", duration_sec=15)

    # ---------------------------------------------------------
    # SUMMARY REPORT
    # ---------------------------------------------------------
    cpu_speedup = vanilla_results["avg_proc_cpu"] / max(0.01, turbo_results["avg_proc_cpu"])
    sys_reduction = vanilla_results["avg_sys_cpu"] - turbo_results["avg_sys_cpu"]
    mem_saved = vanilla_results["avg_rss_mb"] - turbo_results["avg_rss_mb"]

    report = f"""
========================================================================================
                      EMPIRICAL STRESS TEST BENCHMARK RESULTS
========================================================================================
 Metric                        | Vanilla DisplayLinkManager | DisplayLink Turbo (dl-x-vk) | Difference
-------------------------------+----------------------------+-----------------------------+-----------------
 Daemon Avg CPU Usage          | {vanilla_results['avg_proc_cpu']:6.2f}%                    | {turbo_results['avg_proc_cpu']:6.2f}%                     | {cpu_speedup:5.1f}x less CPU (-{vanilla_results['avg_proc_cpu'] - turbo_results['avg_proc_cpu']:.1f}%)
 Daemon Peak CPU Usage         | {vanilla_results['peak_proc_cpu']:6.2f}%                    | {turbo_results['peak_proc_cpu']:6.2f}%                     | -{vanilla_results['peak_proc_cpu'] - turbo_results['peak_proc_cpu']:5.2f}%
 Total System CPU Load         | {vanilla_results['avg_sys_cpu']:6.2f}%                    | {turbo_results['avg_sys_cpu']:6.2f}%                     | -{sys_reduction:5.2f}% system load
 Memory RSS                    | {vanilla_results['avg_rss_mb']:6.1f} MB                 | {turbo_results['avg_rss_mb']:6.1f} MB                  | -{mem_saved:5.1f} MB (-{(mem_saved/vanilla_results['avg_rss_mb'])*100.1:.1f}%)
 Active Thread Count           | {vanilla_results['avg_threads']:6.0f} threads               | {turbo_results['avg_threads']:6.0f} threads                | -{vanilla_results['avg_threads'] - turbo_results['avg_threads']:.0f} threads
 Average GPU Busy Time         | {vanilla_results['avg_gpu']:6.1f}%                    | {turbo_results['avg_gpu']:6.1f}%                     | -{vanilla_results['avg_gpu'] - turbo_results['avg_gpu']:5.1f}% GPU stalls
========================================================================================
"""
    print(report)

    # Save report to markdown file
    md_report = f"""# Stress Test Benchmark Comparison Report

**Test Conditions**:
- Head 0 (Landscape 2560x1440 @ 60Hz): Active video playback
- Head 1 (Portrait 1080x1920 @ 60Hz): Active animated wallpaper
- Hardware: AMD Ryzen 5 PRO 4650U APU (Radeon Vega 6, RADV RENOIR)
- Dock: ThinkPad Hybrid USB-C with USB-A Dock (`17e9:6015`)

| Metric | Vanilla `DisplayLinkManager` | DisplayLink Turbo (`dl-x-vk`) | Performance Gain |
| :--- | :---: | :---: | :---: |
| **Daemon Process CPU Usage** | **{vanilla_results['avg_proc_cpu']:.2f}%** | **{turbo_results['avg_proc_cpu']:.2f}%** | **{cpu_speedup:.1f}x reduction** |
| **Total System CPU Load** | **{vanilla_results['avg_sys_cpu']:.2f}%** | **{turbo_results['avg_sys_cpu']:.2f}%** | **-{sys_reduction:.2f}% system load** |
| **Memory Footprint (RSS)** | **{vanilla_results['avg_rss_mb']:.1f} MB** | **{turbo_results['avg_rss_mb']:.1f} MB** | **-{mem_saved:.1f} MB ({-(mem_saved/vanilla_results['avg_rss_mb'])*100:.1f}%)** |
| **Active OS Thread Count** | **{vanilla_results['avg_threads']:.0f} threads** | **{turbo_results['avg_threads']:.0f} threads** | **-{vanilla_results['avg_threads'] - turbo_results['avg_threads']:.0f} threads** |
| **Average GPU Busy Time** | **{vanilla_results['avg_gpu']:.1f}%** | **{turbo_results['avg_gpu']:.1f}%** | **-{vanilla_results['avg_gpu'] - turbo_results['avg_gpu']:.1f}%** |

*Generated automatically by `compare_benchmark.py`.*
"""
    out_file = os.path.join(workspace_dir, "COMPARISON_REPORT.md")
    with open(out_file, "w") as f:
        f.write(md_report)
    print(f"[*] Report saved to {out_file}")

if __name__ == "__main__":
    main()
