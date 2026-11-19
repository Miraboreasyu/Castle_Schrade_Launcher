"""Automated end-to-end MHW startup benchmark & verification harness.

Assumes the Windows desktop is unlocked and Steam is running. Launches MHW via
Steam, watches nativePC\\plugins\\auto_quest_start.log for the deterministic
post-init cue emitted by auto_quest_start.dll (`startup-bench-done:`), verifies
stability, optionally captures a post-fade-in screenshot, stops the game, and
prints per-phase timings (median/min/max across runs) plus optional CSV export.

Examples:
  # Benchmark 3 runs in current INI mode with screenshots + CSV:
  python tests/bench_startup.py --runs 3 --screenshot --csv bench_results.csv

  # Compare Quest Mode vs Hub Mode (automatically restores auto_quest_start.ini):
  python tests/bench_startup.py --mode quest --runs 3 --tag quest_v13
  python tests/bench_startup.py --mode hub   --runs 3 --tag hub_v13

  # Parse an existing auto_quest_start.log offline without launching MHW:
  python tests/bench_startup.py --parse-log "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Monster Hunter World\\nativePC\\plugins\\auto_quest_start.log" --last 5
"""
from __future__ import annotations

import argparse
import csv
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

DEFAULT_GAME_DIR = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Monster Hunter World")
TS_RE = re.compile(r"^\[(\d+) ms\] (.*)$")
PROFILE_AT_RE = re.compile(r"^resource-profile: at=(\d+) (.*)$")


def find_pids(image_name: str) -> list[int]:
    out = subprocess.check_output(
        ["tasklist", "/FI", f"IMAGENAME eq {image_name}", "/FO", "CSV", "/NH"],
        text=True,
        errors="replace",
    )
    pids: list[int] = []
    for line in out.strip().splitlines():
        if image_name.lower() in line.lower():
            parts = [p.strip('"') for p in line.split('","')]
            if len(parts) >= 2 and parts[1].isdigit():
                pids.append(int(parts[1]))
    return pids


def is_desktop_locked() -> bool:
    return bool(find_pids("LogonUI.exe"))


def read_graphics_dx12(game_dir: Path) -> str:
    ini = game_dir / "graphics_option.ini"
    if not ini.exists():
        return "unknown"
    for line in ini.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("DirectX12Enable="):
            return line.split("=", 1)[1].strip()
    return "unknown"


def read_fallback_marker(game_dir: Path) -> str:
    fb = game_dir / "rendering_api_fallback.txt"
    if not fb.exists():
        return "<missing>"
    return fb.read_text(encoding="utf-8", errors="replace").strip()


def take_screenshot(out_png: Path) -> bool:
    ps = (
        "Add-Type -AssemblyName System.Windows.Forms,System.Drawing; "
        "$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds; "
        "$bmp = New-Object System.Drawing.Bitmap($b.Width, $b.Height); "
        "$g = [System.Drawing.Graphics]::FromImage($bmp); "
        "$g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size); "
        f"$bmp.Save('{str(out_png)}', [System.Drawing.Imaging.ImageFormat]::Png); "
        "$g.Dispose(); $bmp.Dispose();"
    )
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-Command", ps],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
            timeout=15,
        )
        if r.returncode != 0 or not out_png.exists():
            print("[WARN] screenshot unavailable (exclusive HDR fullscreen blocks GDI CopyFromScreen)")
            return False
        return True
    except Exception as exc:
        print(f"[WARN] screenshot failed: {exc}")
        return False


def stop_mhw() -> None:
    if not find_pids("MonsterHunterWorld.exe"):
        return
    # Terminate directly with /F so MHW never opens the in-game "quit without saving?" prompt.
    subprocess.run(
        ["taskkill", "/F", "/IM", "MonsterHunterWorld.exe"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    for _ in range(40):
        if not find_pids("MonsterHunterWorld.exe"):
            return
        time.sleep(0.1)


def parse_run_log(text: str) -> dict[str, Any]:
    """Parse one run's log lines into structured milestone timestamps and segment deltas."""
    events: list[tuple[int, str]] = []
    for raw in text.splitlines():
        m = TS_RE.match(raw.strip())
        if not m:
            continue
        tick = int(m.group(1))
        msg = m.group(2)
        pm = PROFILE_AT_RE.match(msg)
        if pm:
            events.append((int(pm.group(1)), pm.group(2)))
        else:
            events.append((tick, msg))

    def first_ts(substr: str) -> int | None:
        for t, msg in events:
            if substr in msg:
                return t
        return None

    def first_regex(pattern: str) -> re.Match[str] | None:
        rx = re.compile(pattern)
        for _, msg in events:
            m = rx.search(msg)
            if m:
                return m
        return None

    t_ready = first_ts("ready")
    t_logo_prep = first_ts("phase=logo-prepare state=0")
    t_logo5 = first_ts("boot-trace: aLogo state=0x5")
    t_logo16 = first_ts("boot-trace: aLogo state=0x16")
    t_tprep0 = first_ts("phase=title-prepare state=0")
    t_t0 = first_ts("boot-trace: aTitle state=0x0")
    t_t4_1 = first_ts("boot-trace: aTitle state=0x4 menu=1")
    t_t4_2 = first_ts("boot-trace: aTitle state=0x4 menu=2")
    t_create = first_ts("native hub create")
    t_entry = first_ts("-entry state reached")
    t_playable0 = first_ts("phase=playable-prepare state=0")
    t_village0 = first_ts("phase=village-prepare state=0")
    t_stage0 = first_ts("phase=stage-prepare state=0")
    t_stage_init = first_ts("stage-init:")
    t_load_close = first_ts("stage-ready: loading-screen-closed")
    t_fade_done = first_ts("stage-ready: fade-in-complete")

    def delta(a: int | None, b: int | None) -> int | None:
        return (b - a) if (a is not None and b is not None) else None

    m_cfg = first_regex(r"config: enabled=1 title_screen_only=(\d+) title_only_hub=(\d+) .* quest_id=(\d+)")
    m_save = first_regex(r"save-parallel: decrypt-ms=([0-9.]+) unpack-ms=([0-9.]+) ok=(\d+)/3")
    m_ctor_font = first_regex(r"font-startup: site=ctor language=(\d+) refresh-ms=([0-9.]+)")
    m_dx11_sh = first_regex(r"dx11-shader: range=\d+\.\.\d+ compiled=(\d+) threads=(\d+) ms=([0-9.]+)")
    m_stage = first_regex(r"stage-init: stageId=(0x[0-9A-Fa-f]+) isVillage=(\d+) questId=(\d+) mode=(\w+)")
    m_sync = first_regex(r"stage-sync-wait: observed aStage\+0xE54=([0-9.]+) s")
    m_load = first_regex(r"loading-screen-closed dt_from_init=(\d+) ms")
    m_fade = first_regex(r"fade-in-complete dt_from_init=(\d+) ms")

    mode = "unknown"
    if m_stage:
        mode = m_stage.group(4)
    elif m_cfg:
        mode = "title-only" if m_cfg.group(1) == "1" else ("hub" if m_cfg.group(2) == "1" else "quest")

    return {
        "mode": mode,
        "stage_id": m_stage.group(1) if m_stage else "",
        "preLogo_ms": delta(t_ready, t_logo_prep),
        "logoPrep_ms": delta(t_logo_prep, t_logo5),
        "logo5_ms": delta(t_logo5, t_logo16),
        "font2tprep_ms": delta(t_logo16, t_tprep0),
        "tprep_ms": delta(t_tprep0, t_t0),
        "title4_ms": delta(t_t4_1, t_t4_2),
        "create_wait_ms": delta(t_create, t_entry),
        "ready_to_entry_ms": delta(t_ready, t_entry),
        "fade_0x11_ms": delta(t_entry, t_playable0),
        "playable_ms": delta(t_playable0, t_village0 or t_stage0),
        "village_ms": delta(t_village0, t_stage0) if (t_village0 and t_stage0) else None,
        "stage_ms": delta(t_stage0, t_stage_init),
        "ready_to_stage_init_ms": delta(t_ready, t_stage_init),
        "init_to_load_close_ms": int(m_load.group(1)) if m_load else delta(t_stage_init, t_load_close),
        "init_to_fade_done_ms": int(m_fade.group(1)) if m_fade else delta(t_stage_init, t_fade_done),
        "save_decrypt_ms": float(m_save.group(1)) if m_save else None,
        "save_unpack_ms": float(m_save.group(2)) if m_save else None,
        "ctor_font_ms": float(m_ctor_font.group(2)) if m_ctor_font else None,
        "dx11_shader_ms": float(m_dx11_sh.group(3)) if m_dx11_sh else None,
        "dx11_pso_skipped": 1 if first_ts("dx11-pso-skip:") is not None else 0,
        "stage_sync_wait_s": float(m_sync.group(1)) if m_sync else None,
        "content_op0_skipped": 1 if first_ts("skipped redundant 11MB op0 re-save") is not None else 0,
        "dup_font_skipped": 1 if first_ts("skipped-duplicate-refresh=1") is not None else 0,
        "silent_op1": 1 if first_ts("save-silent: enabled") is not None else 0,
        "resident_prefetch": 1 if first_ts("resident-prefetch submitted") is not None else 0,
        "playable_prefetch": 1 if first_ts("playable-prefetch submitted") is not None else 0,
        "dx12_fallback_warn": 1 if first_ts("dx12-fallback-detected") is not None else 0,
        "warnings": [msg for _, msg in events if any(w in msg for w in ("WARN", "mismatch", "failed", "timed out"))],
    }


def split_log_into_runs(log_text: str) -> list[str]:
    runs: list[str] = []
    cur: list[str] = []
    for line in log_text.splitlines():
        if "config: enabled=" in line and cur:
            runs.append("\n".join(cur))
            cur = []
        cur.append(line)
    if cur:
        runs.append("\n".join(cur))
    return runs


def set_ini_mode(ini_path: Path, mode: str) -> None:
    if mode == "keep":
        return
    val = "1" if mode == "hub" else "0"
    lines = ini_path.read_text(encoding="utf-8", errors="replace").splitlines(keepends=True)
    out = []
    replaced = False
    for line in lines:
        if re.match(r"^\s*title_only_hub\s*=", line):
            out.append(f"title_only_hub={val}\n")
            replaced = True
        else:
            out.append(line)
    if not replaced:
        out.append(f"\ntitle_only_hub={val}\n")
    ini_path.write_text("".join(out), encoding="utf-8")


def set_graphics_api(game_dir: Path, api: str) -> None:
    if api == "keep":
        return
    ini = game_dir / "graphics_option.ini"
    val = "On" if api == "dx12" else "Off"
    lines = ini.read_text(encoding="utf-8", errors="replace").splitlines(keepends=True)
    out = []
    replaced = False
    for line in lines:
        if re.match(r"^\s*DirectX12Enable\s*=", line):
            out.append(f"DirectX12Enable={val}\n")
            replaced = True
        else:
            out.append(line)
    if not replaced:
        out.append(f"\nDirectX12Enable={val}\n")
    ini.write_text("".join(out), encoding="utf-8")


def run_once(
    game_dir: Path,
    run_idx: int,
    tag: str,
    cue: str,
    settle: float,
    timeout: float,
    screenshot_dir: Path | None,
) -> dict[str, Any]:
    log_path = game_dir / r"nativePC\plugins\auto_quest_start.log"
    fb_before = read_fallback_marker(game_dir)
    dx12_cfg = read_graphics_dx12(game_dir)
    start_size = log_path.stat().st_size if log_path.exists() else 0

    print(
        f"[{tag} #{run_idx}] Launching MHW (DX12={dx12_cfg}, fallback={fb_before}, cue='{cue}')...",
        flush=True,
    )
    t_launch = time.perf_counter()
    os.startfile("steam://rungameid/582010")

    proc_seen = False
    t_proc: float | None = None
    t_ready_wall: float | None = None
    t_cue_wall: float | None = None
    crashed = False
    new_text = ""

    while True:
        elapsed = time.perf_counter() - t_launch
        pids = find_pids("MonsterHunterWorld.exe")
        if pids and not proc_seen:
            proc_seen = True
            t_proc = elapsed
            print(f"[{tag} #{run_idx}] PID={pids[0]} spawned at +{t_proc:.2f}s", flush=True)
        if proc_seen and not pids:
            crashed = True
            print(f"[{tag} #{run_idx}] [ERROR] MHW exited unexpectedly at +{elapsed:.2f}s", flush=True)
            break

        if log_path.exists() and log_path.stat().st_size > start_size:
            with open(log_path, "rb") as f:
                f.seek(start_size)
                new_text = f.read().decode("utf-8", errors="replace")
            if t_ready_wall is None and "] ready" in new_text:
                t_ready_wall = elapsed
            if cue in new_text:
                t_cue_wall = elapsed
                print(
                    f"[{tag} #{run_idx}] Cue '{cue}' hit at +{t_cue_wall:.2f}s; settling {settle:.1f}s...",
                    flush=True,
                )
                break

        if elapsed >= timeout:
            print(f"[{tag} #{run_idx}] [TIMEOUT] Exceeded {timeout:.1f}s without cue '{cue}'", flush=True)
            break
        time.sleep(0.1)

    if t_cue_wall is not None and not crashed:
        time.sleep(settle)
        if not find_pids("MonsterHunterWorld.exe"):
            crashed = True
            print(f"[{tag} #{run_idx}] [ERROR] MHW crashed during {settle:.1f}s post-cue settle!", flush=True)
        elif screenshot_dir is not None:
            screenshot_dir.mkdir(parents=True, exist_ok=True)
            png = screenshot_dir / f"{tag}_run{run_idx}.png"
            if take_screenshot(png):
                print(f"[{tag} #{run_idx}] Screenshot saved: {png}", flush=True)

    stop_mhw()
    time.sleep(0.3)
    if log_path.exists() and log_path.stat().st_size > start_size:
        with open(log_path, "rb") as f:
            f.seek(start_size)
            new_text = f.read().decode("utf-8", errors="replace")

    fb_after = read_fallback_marker(game_dir)
    row = parse_run_log(new_text)
    row["run"] = run_idx
    row["tag"] = tag
    row["ok"] = int(t_cue_wall is not None and not crashed)
    row["dx12_ini"] = dx12_cfg
    row["fallback_before"] = fb_before
    row["fallback_after"] = fb_after
    row["wall_to_proc_ms"] = round(t_proc * 1000) if t_proc is not None else None
    row["wall_to_ready_ms"] = round(t_ready_wall * 1000) if t_ready_wall is not None else None
    row["wall_to_cue_ms"] = round(t_cue_wall * 1000) if t_cue_wall is not None else None
    if fb_after == "1" and fb_before != "1":
        print(f"[{tag} #{run_idx}] [WARN] rendering_api_fallback.txt flipped to 1!", flush=True)
    return row


SUMMARY_KEYS = [
    ("preLogo_ms", "preLogo"),
    ("logoPrep_ms", "logoPrep"),
    ("logo5_ms", "logo5"),
    ("font2tprep_ms", "font2tprep"),
    ("tprep_ms", "tprep"),
    ("title4_ms", "title4"),
    ("create_wait_ms", "createWait"),
    ("ready_to_entry_ms", "ready->0x11"),
    ("fade_0x11_ms", "0x11->play"),
    ("playable_ms", "playable"),
    ("village_ms", "village"),
    ("stage_ms", "stagePrep"),
    ("ready_to_stage_init_ms", "ready->init"),
    ("init_to_fade_done_ms", "init->vis"),
    ("wall_to_cue_ms", "wall->cue"),
    ("save_decrypt_ms", "saveDec"),
    ("save_unpack_ms", "saveUnp"),
    ("ctor_font_ms", "ctorFont"),
    ("dx11_shader_ms", "dx11Shader"),
]


def print_table(rows: list[dict[str, Any]]) -> None:
    if not rows:
        print("No runs to display.")
        return
    print("\n=== STARTUP BENCHMARK SUMMARY (ms) ===")
    header = f"{'Metric':14s} | " + " | ".join(f"Run{i+1:>2d}" for i in range(len(rows)))
    if len(rows) > 1:
        header += " | Median |    Min |    Max"
    print(header)
    print("-" * len(header))
    for key, label in SUMMARY_KEYS:
        vals = [r.get(key) for r in rows]
        if all(v is None for v in vals):
            continue
        cells = [f"{v:5.0f}" if isinstance(v, (int, float)) else "    -" for v in vals]
        line = f"{label:14s} | " + " | ".join(cells)
        nums = [float(v) for v in vals if isinstance(v, (int, float))]
        if len(rows) > 1:
            if nums:
                line += f" | {statistics.median(nums):6.1f} | {min(nums):6.1f} | {max(nums):6.1f}"
            else:
                line += " |      - |      - |      -"
        print(line)

    print("\n=== OPTIMIZATION ACTIVATION FLAGS ===")
    for i, r in enumerate(rows, 1):
        print(
            f"  Run {i}: mode={r.get('mode')} stage={r.get('stage_id') or '-'} "
            f"silentOp1={r['silent_op1']} op0Skip={r['content_op0_skipped']} "
            f"dupFontSkip={r['dup_font_skipped']} resPrefetch={r['resident_prefetch']} "
            f"playPrefetch={r['playable_prefetch']} dx11PsoSkip={r.get('dx11_pso_skipped', 0)} "
            f"syncWait={r.get('stage_sync_wait_s')}s "
            f"warns={len(r.get('warnings') or [])}"
        )
        for w in (r.get("warnings") or [])[:5]:
            print(f"    [WARN] {w}")


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        return
    cols = [k for k in rows[0].keys() if k != "warnings"]
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k) for k in cols})
    print(f"\nCSV exported: {path}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Automated MHW startup benchmark & verifier")
    ap.add_argument("--game-dir", type=Path, default=DEFAULT_GAME_DIR)
    ap.add_argument("--mode", choices=["keep", "hub", "quest"], default="keep")
    ap.add_argument("--api", choices=["keep", "dx12", "dx11"], default="keep")
    ap.add_argument("--deploy", type=Path, default=None, help="Optional DLL to deploy into nativePC\\plugins before running")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--cooldown", type=float, default=5.0, help="Seconds between runs")
    ap.add_argument("--cue", default="startup-bench-done:", help="Log substring cue to stop waiting")
    ap.add_argument("--settle", type=float, default=1.0, help="Seconds after cue to verify stability")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--tag", default="bench")
    ap.add_argument("--screenshot", action="store_true")
    ap.add_argument("--screenshot-dir", type=Path, default=Path("bench_shots"))
    ap.add_argument("--csv", type=Path, default=None)
    ap.add_argument("--allow-locked", action="store_true")
    ap.add_argument("--parse-log", type=Path, default=None, help="Offline mode: parse existing auto_quest_start.log")
    ap.add_argument("--last", type=int, default=5, help="With --parse-log, show last N runs")
    args = ap.parse_args()

    if args.parse_log is not None:
        text = args.parse_log.read_text(encoding="utf-8", errors="replace")
        chunks = split_log_into_runs(text)[-args.last:]
        rows = [parse_run_log(c) for c in chunks]
        print_table(rows)
        if args.csv:
            write_csv(args.csv, rows)
        return 0

    if not args.allow_locked and is_desktop_locked():
        print("[ERROR] Windows desktop is locked (LogonUI.exe active). Unlock first or pass --allow-locked.")
        return 2
    if find_pids("MonsterHunterWorld.exe"):
        print("[ERROR] MonsterHunterWorld.exe is already running.")
        return 2
    if not find_pids("steam.exe"):
        print("[ERROR] steam.exe is not running.")
        return 2

    plugins_dir = args.game_dir / r"nativePC\plugins"
    ini_path = plugins_dir / "auto_quest_start.ini"
    dll_path = plugins_dir / "auto_quest_start.dll"
    gfx_path = args.game_dir / "graphics_option.ini"
    ini_backup = ini_path.read_bytes() if (args.mode != "keep" and ini_path.exists()) else None
    dll_backup = dll_path.read_bytes() if (args.deploy is not None and dll_path.exists()) else None
    gfx_backup = gfx_path.read_bytes() if (args.api != "keep" and gfx_path.exists()) else None

    rows: list[dict[str, Any]] = []
    try:
        if args.deploy is not None:
            shutil.copy2(args.deploy, dll_path)
            print(f"[deploy] Installed {args.deploy} -> {dll_path}")
        if args.mode != "keep":
            set_ini_mode(ini_path, args.mode)
            print(f"[mode] Temporarily set title_only_hub={'1' if args.mode == 'hub' else '0'}")
        if args.api != "keep":
            set_graphics_api(args.game_dir, args.api)
            print(f"[api] Temporarily set DirectX12Enable={'On' if args.api == 'dx12' else 'Off'}")

        shot_dir = args.screenshot_dir if args.screenshot else None
        for i in range(1, args.runs + 1):
            row = run_once(args.game_dir, i, args.tag, args.cue, args.settle, args.timeout, shot_dir)
            rows.append(row)
            if i < args.runs:
                time.sleep(args.cooldown)
    finally:
        if ini_backup is not None:
            ini_path.write_bytes(ini_backup)
            print("[restore] Restored original auto_quest_start.ini")
        if dll_backup is not None:
            dll_path.write_bytes(dll_backup)
            print("[restore] Restored previous auto_quest_start.dll")
        if gfx_backup is not None:
            gfx_path.write_bytes(gfx_backup)
            print("[restore] Restored original graphics_option.ini")

    print_table(rows)
    if args.csv:
        write_csv(args.csv, rows)
    return 0 if all(r.get("ok") == 1 for r in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
