"""Status Sphere Editor Server – Flask backend for the display layout editor.

Provides:
  - Static file serving for the HTML editor
  - CORS proxy for the JSON status endpoint
  - Serial port enumeration
  - Build & flash via idf.py or esptool with SSE progress streaming
  - Config push to the ESP setup portal
"""

import json
import os
import queue
import subprocess
import sys
import threading
import time
from pathlib import Path

import requests
import serial.tools.list_ports
from flask import Flask, Response, jsonify, request, send_from_directory

app = Flask(__name__, static_folder=None)

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
SIMULATOR_DIR = PROJECT_ROOT / "tools" / "simulator"
BUILD_DIR = PROJECT_ROOT / "build"

flash_log_queues: dict[str, queue.Queue] = {}
flash_processes: dict[str, subprocess.Popen] = {}


# ── Static files (editor UI) ──

@app.route("/", methods=["GET", "HEAD", "POST"])
def index():
    # Manche Browser/IDE-Tools senden POST auf /. Editor trotzdem ausliefern.
    return send_from_directory(str(SIMULATOR_DIR), "index.html")


@app.route("/favicon.ico")
def favicon():
    return "", 204


@app.route("/static/<path:filename>")
def static_files(filename):
    return send_from_directory(str(SIMULATOR_DIR), filename)


# ── Data proxy (solves CORS) ──

@app.route("/api/data")
def proxy_data():
    url = request.args.get("url", "").strip()
    if not url:
        return jsonify({"error": "url parameter required"}), 400
    try:
        r = requests.get(url, timeout=10)
        resp = Response(r.content, status=r.status_code, content_type=r.headers.get("Content-Type", "application/json"))
        resp.headers["Access-Control-Allow-Origin"] = "*"
        return resp
    except requests.RequestException as e:
        return jsonify({"error": str(e)}), 502


# ── Serial port listing ──

@app.route("/api/ports")
def list_ports():
    ports = []
    for p in serial.tools.list_ports.comports():
        ports.append({
            "device": p.device,
            "description": p.description,
            "hwid": p.hwid,
        })
    return jsonify(ports)


# ── Layout save ──

@app.route("/api/save-layout", methods=["POST"])
def save_layout():
    data = request.get_json(silent=True) or {}
    layout_path = PROJECT_ROOT / "main" / "resources" / "layout.json"
    layout_path.parent.mkdir(parents=True, exist_ok=True)
    with open(layout_path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)

    # Force rebuild of embedded layout by removing ALL cached artifacts
    deleted = []
    if BUILD_DIR.exists():
        for stale in BUILD_DIR.rglob("*layout*json*"):
            try:
                stale.unlink()
                deleted.append(str(stale))
            except OSError:
                pass

    return jsonify({"ok": True, "path": str(layout_path), "deleted_cache": deleted})


# ── Build ──

@app.route("/api/build", methods=["POST"])
def start_build():
    job_id = f"build_{int(time.time()*1000)}"
    q = queue.Queue()
    flash_log_queues[job_id] = q

    def run_build():
        try:
            proc = _run_idf_command(["build"], q, "build")
            flash_processes[job_id] = proc
            for line in proc.stdout:
                q.put(line)
            proc.wait()
            q.put(f"[build] Exit code: {proc.returncode}\n")
        except Exception as e:
            q.put(f"[build] Error: {e}\n")
        finally:
            q.put(None)

    threading.Thread(target=run_build, daemon=True).start()
    return jsonify({"job_id": job_id})


# ── Flash ──

@app.route("/api/flash", methods=["POST"])
def start_flash():
    data = request.get_json(silent=True) or {}
    port = data.get("port", "").strip()
    if not port:
        return jsonify({"error": "port required"}), 400

    job_id = f"flash_{int(time.time()*1000)}"
    q = queue.Queue()
    flash_log_queues[job_id] = q

    def run_flash():
        try:
            q.put(f"[flash] Port: {port}\n")
            proc = _run_idf_command(["flash", "-p", port], q, "flash")
            flash_processes[job_id] = proc
            for line in proc.stdout:
                q.put(line)
            proc.wait()
            q.put(f"[flash] Exit code: {proc.returncode}\n")
        except FileNotFoundError:
            q.put("[flash] idf.py not found, trying esptool fallback...\n")
            _flash_esptool(port, q)
        except Exception as e:
            q.put(f"[flash] Error: {e}\n")
        finally:
            q.put(None)

    threading.Thread(target=run_flash, daemon=True).start()
    return jsonify({"job_id": job_id})


@app.route("/api/flash/status/<job_id>")
def flash_status(job_id):
    q = flash_log_queues.get(job_id)
    if not q:
        return jsonify({"error": "unknown job_id"}), 404

    def generate():
        while True:
            try:
                line = q.get(timeout=60)
            except queue.Empty:
                yield "data: [timeout]\n\n"
                continue
            if line is None:
                yield "data: [DONE]\n\n"
                flash_log_queues.pop(job_id, None)
                flash_processes.pop(job_id, None)
                break
            yield f"data: {line.rstrip()}\n\n"

    return Response(generate(), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


# ── Config push to ESP ──

@app.route("/api/push-config", methods=["POST"])
def push_config():
    data = request.get_json(silent=True) or {}
    esp_ip = data.get("esp_ip", "").strip()
    config = data.get("config", {})

    if not esp_ip:
        return jsonify({"error": "esp_ip required"}), 400

    url = f"http://{esp_ip}/api/config"
    try:
        r = requests.post(url, json=config, timeout=10)
        return jsonify({"status": r.status_code, "response": r.text})
    except requests.RequestException as e:
        return jsonify({"error": str(e)}), 502


@app.route("/api/push-display-settings", methods=["POST"])
def push_display_settings():
    """Push display settings (brightness, contrast, invert, screen off) to ESP."""
    data = request.get_json(silent=True) or {}
    esp_ip = data.get("esp_ip", "").strip()
    settings = data.get("settings", {})

    if not esp_ip:
        return jsonify({"error": "esp_ip required"}), 400

    url = f"http://{esp_ip}/api/display-settings"
    try:
        r = requests.post(url, json=settings, timeout=10)
        return jsonify({"status": r.status_code, "response": r.text})
    except requests.RequestException as e:
        return jsonify({"error": str(e)}), 502


@app.route("/api/push-arc-colors", methods=["POST"])
def push_arc_colors():
    """Push arc color scheme to the ESP setup portal."""
    data = request.get_json(silent=True) or {}
    esp_ip = data.get("esp_ip", "").strip()
    colors = data.get("colors", {})

    if not esp_ip:
        return jsonify({"error": "esp_ip required"}), 400

    url = f"http://{esp_ip}/api/arc-colors"
    try:
        r = requests.post(url, json=colors, timeout=10)
        return jsonify({"status": r.status_code, "response": r.text})
    except requests.RequestException as e:
        return jsonify({"error": str(e)}), 502


# ── ESP-IDF discovery ──

_IDF_NOT_SCANNED = object()
_idf_install_cache = _IDF_NOT_SCANNED


def _read_idf_from_env_json(path: Path):
    """Parse Espressif idf-env.json or eim_idf.json (installer)."""
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError):
        return None

    selected = data.get("idfSelectedId") or data.get("selected_idf_version")
    installed = data.get("idfInstalled") or data.get("installed") or []

    def _extract_path(entry):
        if isinstance(entry, dict):
            return entry.get("path") or entry.get("idfPath") or entry.get("idf_path")
        if isinstance(entry, str):
            return entry
        return None

    # idfInstalled can be a list (EIM v2) or a dict (older format)
    if isinstance(installed, list):
        # EIM v2 format: list of objects with "id" and "path"
        if selected:
            for entry in installed:
                if isinstance(entry, dict) and entry.get("id") == selected:
                    p = _extract_path(entry)
                    if p:
                        return str(Path(p))
        for entry in installed:
            p = _extract_path(entry)
            if p:
                return str(Path(p))
    elif isinstance(installed, dict):
        if selected and selected in installed:
            p = _extract_path(installed[selected])
            if p:
                return str(Path(p))
        for entry in installed.values():
            p = _extract_path(entry)
            if p:
                return str(Path(p))

    return None


def _idf_roots_to_scan():
    """Yield candidate ESP-IDF root directories."""
    seen = set()

    def add(p: Path):
        p = p.resolve()
        if p in seen or not p.is_dir():
            return
        seen.add(p)
        yield p

    env_idf = os.environ.get("IDF_PATH", "").strip()
    if env_idf:
        yield from add(Path(env_idf))

    # Manual override has highest priority after IDF_PATH env
    override = Path(__file__).parent / "idf_path.txt"
    if override.is_file():
        line = override.read_text(encoding="utf-8").strip()
        if line:
            yield from add(Path(line))

    userprofile = Path(os.environ.get("USERPROFILE", ""))

    # EIM / installer JSON files
    for env_json in (
        Path("C:/Espressif/tools/eim_idf.json"),
        userprofile / ".espressif" / "idf-env.json",
        Path("C:/Espressif/idf-env.json"),
        Path(os.environ.get("LOCALAPPDATA", "")) / "Espressif" / "idf-env.json",
    ):
        if env_json.is_file():
            idf_path = _read_idf_from_env_json(env_json)
            if idf_path:
                yield from add(Path(idf_path))

    search_bases = [
        Path("C:/esp"),
        Path("C:/Espressif/frameworks"),
        Path("C:/Espressif"),
        userprofile / "esp",
        userprofile / "Desktop" / "esp",
    ]
    for base in search_bases:
        if not base.is_dir():
            continue
        if (base / "tools" / "idf.py").is_file():
            yield from add(base)
        for child in sorted(base.iterdir(), reverse=True):
            if not child.is_dir():
                continue
            # Direct match: esp-idf, esp-idf-v6.0.1, etc.
            if child.name.startswith("esp-idf"):
                yield from add(child)
            # Version subdirectory: C:\esp\v6.0.1\esp-idf
            esp_idf_in_version = child / "esp-idf"
            if esp_idf_in_version.is_dir():
                yield from add(esp_idf_in_version)


def discover_idf():
    """Return dict with idf_path, idf_py, export_bat or None."""
    global _idf_install_cache
    if _idf_install_cache is not _IDF_NOT_SCANNED:
        return _idf_install_cache

    for root in _idf_roots_to_scan():
        root = Path(root)
        idf_py = root / "tools" / "idf.py"
        if not idf_py.is_file():
            continue
        export_bat = root / "export.bat"
        export_ps1 = root / "export.ps1"
        _idf_install_cache = {
            "idf_path": str(root),
            "idf_py": str(idf_py),
            "export_bat": str(export_bat) if export_bat.is_file() else None,
            "export_ps1": str(export_ps1) if export_ps1.is_file() else None,
        }
        return _idf_install_cache

    _idf_install_cache = None
    return None


@app.route("/api/idf-status")
def idf_status():
    install = discover_idf()
    return jsonify({
        "found": install is not None,
        "idf_path": install["idf_path"] if install else None,
        "export_bat": install.get("export_bat") if install else None,
        "idf_path_env": os.environ.get("IDF_PATH"),
    })


def _read_eim_idf_json():
    """Read the EIM installer config to get python path and tools path."""
    for candidate in (
        Path("C:/Espressif/tools/eim_idf.json"),
        Path(os.environ.get("LOCALAPPDATA", "")) / "Espressif" / "eim_idf.json",
    ):
        if not candidate.is_file():
            continue
        try:
            with open(candidate, encoding="utf-8") as f:
                data = json.load(f)
            selected = data.get("idfSelectedId")
            installed = data.get("idfInstalled", [])
            if isinstance(installed, list):
                for entry in installed:
                    if isinstance(entry, dict):
                        if selected and entry.get("id") != selected:
                            continue
                        return entry
        except (OSError, json.JSONDecodeError):
            continue
    return None


def _build_idf_env_win32(install):
    """Build environment dict with all IDF tools on PATH (like the PowerShell profile does)."""
    env = os.environ.copy()
    idf_path = install["idf_path"]
    env["IDF_PATH"] = idf_path

    tools_path = "C:\\Espressif\\tools"
    eim_entry = _read_eim_idf_json()
    if eim_entry:
        tools_path = eim_entry.get("idfToolsPath", tools_path)
        python = eim_entry.get("python")
        if python:
            install["python"] = python

    env["IDF_TOOLS_PATH"] = tools_path

    # ESP_IDF_VERSION is required by idf_component_manager
    if "ESP_IDF_VERSION" not in env:
        version_cmake = Path(idf_path) / "tools" / "cmake" / "version.cmake"
        idf_ver = "6.0"
        if version_cmake.is_file():
            major = minor = None
            for line in version_cmake.read_text(encoding="utf-8").splitlines():
                if "IDF_VERSION_MAJOR" in line:
                    m = __import__("re").search(r"\d+", line)
                    if m:
                        major = m.group()
                elif "IDF_VERSION_MINOR" in line:
                    m = __import__("re").search(r"\d+", line)
                    if m:
                        minor = m.group()
            if major and minor:
                idf_ver = f"{major}.{minor}"
        env["ESP_IDF_VERSION"] = idf_ver

    python_venv = Path(tools_path) / "python"
    venv_scripts = None
    for vdir in sorted(python_venv.glob("v*"), reverse=True):
        candidate = vdir / "venv" / "Scripts"
        if candidate.is_dir():
            venv_scripts = str(candidate)
            env["IDF_PYTHON_ENV_PATH"] = str(vdir / "venv")
            if "python" not in install or not install["python"]:
                install["python"] = str(candidate / "python.exe")
            break

    idf_tools_base = Path(tools_path)
    extra_paths = []

    tool_bin_patterns = [
        "ccache/*/ccache-*",
        "cmake/*/bin",
        "dfu-util/*/dfu-util-*",
        "esp-clang/*/esp-clang/bin",
        "esp-rom-elfs/*/",
        "esp32ulp-elf/*/esp32ulp-elf/bin",
        "esp32ulp-elf/*/esp32ulp-elf/esp32ulp-elf/bin",
        "idf-exe/*/",
        "ninja/*/",
        "openocd-esp32/*/openocd-esp32/bin",
        "riscv32-esp-elf-gdb/*/riscv32-esp-elf-gdb/bin",
        "riscv32-esp-elf/*/riscv32-esp-elf/bin",
        "riscv32-esp-elf/*/riscv32-esp-elf/riscv32-esp-elf/bin",
        "xtensa-esp-elf-gdb/*/xtensa-esp-elf-gdb/bin",
        "xtensa-esp-elf/*/xtensa-esp-elf/bin",
        "xtensa-esp-elf/*/xtensa-esp-elf/xtensa-esp-elf/bin",
    ]
    for pattern in tool_bin_patterns:
        for match in idf_tools_base.glob(pattern):
            if match.is_dir():
                extra_paths.append(str(match))

    if venv_scripts:
        extra_paths.append(venv_scripts)

    if extra_paths:
        env["PATH"] = ";".join(extra_paths) + ";" + env.get("PATH", "")

    return env


def _run_idf_command(args, q, label):
    """Run idf.py with ESP-IDF environment (export.bat on Windows)."""
    install = discover_idf()
    project = str(PROJECT_ROOT)
    idf_args = " ".join(["idf.py", *args])

    if install is None:
        # Last resort: idf.py already on PATH (ESP-IDF shell was used to start server)
        for probe in (["idf.py", *args], [sys.executable, "-m", "idf", *args]):
            try:
                subprocess.run(probe, capture_output=True, check=True, cwd=project, timeout=15)
                install = {"idf_path": os.environ.get("IDF_PATH", ""), "via_path": True}
                cmd = list(probe)
                q.put(f"[{label}] Using PATH: {' '.join(cmd)}\n")
                proc = subprocess.Popen(
                    cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    cwd=project, text=True, bufsize=1,
                )
                return proc
            except (FileNotFoundError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
                continue
        raise FileNotFoundError(
            "ESP-IDF nicht gefunden. Starte den Editor über tools\\start_editor.bat "
            "oder lege den Pfad in tools\\editor\\idf_path.txt ab (eine Zeile, z.B. "
            "C:\\Espressif\\frameworks\\esp-idf-v6.0.1)."
        )

    q.put(f"[{label}] IDF_PATH: {install['idf_path']}\n")
    q.put(f"[{label}] Working directory: {project}\n")

    if sys.platform == "win32":
        env = _build_idf_env_win32(install)
        python_exe = install.get("python") or env.get("_IDF_PYTHON") or sys.executable
        cmd = [python_exe, install["idf_py"], *args]
        q.put(f"[{label}] python: {python_exe}\n")
        q.put(f"[{label}] command: {' '.join(cmd)}\n")
        return subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            cwd=project,
            text=True,
            bufsize=1,
            env=env,
        )

    # Linux / macOS
    export_sh = Path(install["idf_path"]) / "export.sh"
    if export_sh.is_file():
        script = f'. "{export_sh}" && cd "{project}" && {idf_args}'
        q.put(f"[{label}] bash: {script}\n")
        return subprocess.Popen(
            ["bash", "-lc", script],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            cwd=project,
            text=True,
            bufsize=1,
        )

    env = os.environ.copy()
    env["IDF_PATH"] = install["idf_path"]
    cmd = [sys.executable, install["idf_py"], *args]
    q.put(f"[{label}] {' '.join(cmd)}\n")
    return subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        cwd=project,
        text=True,
        bufsize=1,
        env=env,
    )


def _flash_esptool(port, q):
    """Fallback: flash using esptool directly from build artifacts."""
    flasher_args_path = BUILD_DIR / "flasher_args.json"
    if not flasher_args_path.exists():
        q.put(f"[flash] {flasher_args_path} not found. Run 'idf.py build' first.\n")
        return

    try:
        with open(flasher_args_path) as f:
            args = json.load(f)
    except Exception as e:
        q.put(f"[flash] Failed to parse flasher_args.json: {e}\n")
        return

    flash_files = args.get("flash_files", {})
    extra_args = args.get("extra_esptool_args", {})
    chip = extra_args.get("chip", "esp32s3")
    flash_mode = args.get("flash_settings", {}).get("flash_mode", "dio")
    flash_freq = args.get("flash_settings", {}).get("flash_freq", "80m")
    flash_size = args.get("flash_settings", {}).get("flash_size", "16MB")

    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", chip,
        "--port", port,
        "--baud", "460800",
        "write_flash",
        "--flash_mode", flash_mode,
        "--flash_freq", flash_freq,
        "--flash_size", flash_size,
    ]
    for offset, filepath in flash_files.items():
        full_path = str(BUILD_DIR / filepath)
        cmd.extend([offset, full_path])

    q.put(f"[flash] esptool command: {' '.join(cmd)}\n")

    try:
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            cwd=str(PROJECT_ROOT), text=True, bufsize=1,
        )
        for line in proc.stdout:
            q.put(line)
        proc.wait()
        q.put(f"[flash] Exit code: {proc.returncode}\n")
    except Exception as e:
        q.put(f"[flash] esptool error: {e}\n")


if __name__ == "__main__":
    import webbrowser

    print("Status Sphere Editor Server")
    print(f"Project root: {PROJECT_ROOT}")
    print("Editor UI:    http://127.0.0.1:5000")
    print()
    # use_reloader=False vermeidet Doppelstart und POST-Probes vom Reloader
    threading.Timer(1.0, lambda: webbrowser.open("http://127.0.0.1:5000")).start()
    app.run(host="0.0.0.0", port=5000, debug=False, use_reloader=False, threaded=True)
