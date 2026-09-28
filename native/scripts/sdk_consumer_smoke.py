#!/usr/bin/env python3
"""从预编译 SDK 外部使用 find_package、链接并运行最小消费者。"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path


def run(command: list[str], *, cwd: Path, env: dict[str, str]) -> None:
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True,
                            text=True, timeout=180)
    if result.returncode:
        raise RuntimeError(
            f"命令失败 ({result.returncode}): {' '.join(command)}\n"
            f"{result.stdout}\n{result.stderr}"
        )


def main() -> int:
    if len(sys.argv) != 2:
        print("用法: sdk_consumer_smoke.py <sdk-dist>", file=sys.stderr)
        return 2
    sdk = Path(sys.argv[1]).resolve()
    config = sdk / "lib" / "cmake" / "ToposCodec" / "ToposCodecConfig.cmake"
    if not config.is_file():
        print(f"SDK 缺少 CMake package config: {config}", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="topos-sdk-consumer-") as temp:
        root = Path(temp)
        (root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.16)\n"
            "project(topos_sdk_consumer C)\n"
            "find_package(ToposCodec 0.1 CONFIG REQUIRED)\n"
            "add_executable(sdk_consumer main.c)\n"
            "target_link_libraries(sdk_consumer PRIVATE ToposCodec::topos_codec)\n",
            encoding="utf-8",
        )
        (root / "main.c").write_text(
            "#include <topos_codec.h>\n"
            "#include <topos_image.h>\n"
            "int main(void) {\n"
            "  if (tc_abi_version() != TOPOS_CODEC_ABI_VERSION) return 1;\n"
            "  return tc_image_status_message(0) == 0 ? 2 : 0;\n"
            "}\n",
            encoding="utf-8",
        )
        env = os.environ.copy()
        env["PATH"] = str(sdk / "lib") + os.pathsep + env.get("PATH", "")
        run(["cmake", "-S", str(root), "-B", str(root / "build"),
             f"-DCMAKE_PREFIX_PATH={sdk}"], cwd=root, env=env)
        run(["cmake", "--build", str(root / "build"), "--config", "Release"],
            cwd=root, env=env)
        executable = root / "build" / ("Release" if os.name == "nt" else "")
        executable /= "sdk_consumer.exe" if os.name == "nt" else "sdk_consumer"
        run([str(executable)], cwd=root, env=env)
    print("SDK CMake 消费者: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
