"""Package a clean, tested Windows x64 release."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parent.parent


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--test-report", required=True, type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts" / "packages")
    args = parser.parse_args()
    version = args.version.removeprefix("v")
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise SystemExit("Version must have three numeric components")
    if git("status", "--porcelain"):
        raise SystemExit("Commit release sources before packaging")
    commit = git("rev-parse", "HEAD")
    resource = (ROOT / "AudioPlaybackConnector.rc").read_text(encoding="utf-16")
    expected = version.replace(".", ",") + ",0"
    if "FILEVERSION " + expected not in resource or "PRODUCTVERSION " + expected not in resource:
        raise SystemExit("Resource version mismatch")
    report = args.test_report.read_text(encoding="utf-8-sig")
    passes = [line for line in report.splitlines() if line.startswith("PASS:")]
    if len(passes) < 8 or "FAIL:" in report:
        raise SystemExit("A passing complete regression report is required")
    binary = args.binary.resolve()
    data = binary.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[:2] != b"MZ" or data[pe:pe + 4] != b"PE\0\0" or struct.unpack_from("<H", data, pe + 4)[0] != 0x8664:
        raise SystemExit("Expected Windows x64 executable")
    output = args.output.resolve() / ("v" + version)
    output.mkdir(parents=True, exist_ok=True)
    executable = output / "AudioPlaybackConnector64.exe"
    shutil.copy2(binary, executable)
    shutil.copy2(ROOT / "LICENSE", output / "LICENSE")
    readme = f"""AudioPlaybackConnector v{version} - Windows x64 便携版
源代码提交：{commit}
项目地址：https://github.com/NeverBetterEnough/AudioPlaybackConnector

使用方法
1. 解压到固定、可写的目录；更新前先退出旧版托盘程序。
2. 运行 AudioPlaybackConnector64.exe，无需安装，中文资源已内置。
3. 在 Windows 蓝牙设置完成手机配对，右键托盘图标进入“设置”。
4. 可选择自动连接的手机、开机启动或暂时关闭手机音频连接。
5. 在手机播放音乐，声音使用 Windows 当前默认输出设备。

本次改进
- 使用持续运行的音频后台线程，保持会话通知有效。
- 只有实际收到手机声音信号才完成初始化；接口成功但无声时继续轻量激活。
- 支持手机自动重连后延迟播放，实际有声后停止激活重试。
- 初始化不额外断开蓝牙，保留现有音量和静音状态。
- 合并重连期间的重复连接请求，取消失效的旧任务。
- 保留诊断日志、显式检测修复和开机启动设置。

说明
- 适用于 Windows 10 2004 及以上版本的 64 位系统，需要可用的蓝牙适配器。
- 设置存放在程序旁 AudioPlaybackConnector.json，日志位于 logs 文件夹。
- 真正断开蓝牙可能让手机播放器暂停，重新连接后请恢复播放。
- 移动程序目录后，请在新位置重新设置开机启动。
- 启用静音或把音量设为零时，程序尊重这些设置。
- FILE-SHA256SUMS.txt 用于校验压缩包内文件。
"""
    (output / "README.zh-CN.txt").write_bytes(b"\xef\xbb\xbf" + readme.replace("\n", "\r\n").encode("utf-8"))
    metadata = {
        "version": version, "tag": "v" + version, "source_commit": commit,
        "source_url": "https://github.com/NeverBetterEnough/AudioPlaybackConnector/commit/" + commit,
        "configuration": "Release", "architecture": "x64", "toolset": "MSVC v143",
        "packaged_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "executable": executable.name, "executable_sha256": sha256(executable),
        "regression_checks_passed": len(passes),
    }
    (output / "build-info.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    files = [executable.name, "LICENSE", "README.zh-CN.txt", "build-info.json"]
    (output / "FILE-SHA256SUMS.txt").write_text(
        "".join(sha256(output / name) + "  " + name + "\n" for name in files), encoding="ascii")
    files.append("FILE-SHA256SUMS.txt")
    archive = output / ("AudioPlaybackConnector-v" + version + "-win-x64.zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in files:
            z.write(output / name, name)
    with zipfile.ZipFile(archive) as z:
        if z.testzip() is not None or sorted(z.namelist()) != sorted(files):
            raise SystemExit("Archive integrity/content validation failed")
        if hashlib.sha256(z.read(executable.name)).hexdigest() != sha256(executable):
            raise SystemExit("Archived executable mismatch")
        if json.loads(z.read("build-info.json"))["source_commit"] != commit:
            raise SystemExit("Archive source identity mismatch")
    (output / "SHA256SUMS.txt").write_text(
        "".join(sha256(path) + "  " + path.name + "\n" for path in [executable, archive]), encoding="ascii")
    print(json.dumps({"version": version, "commit": commit, "exe": str(executable), "zip": str(archive),
                      "sha256_file": str(output / "SHA256SUMS.txt"), "exe_sha256": sha256(executable),
                      "zip_sha256": sha256(archive), "zip_bytes": archive.stat().st_size,
                      "archive_verified": True}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
