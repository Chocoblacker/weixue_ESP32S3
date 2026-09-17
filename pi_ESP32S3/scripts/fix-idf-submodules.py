#!/usr/bin/env python3
"""
补齐 ESP-IDF 的 git submodule。

背景：本机 git-over-https 不稳，所以 ESP-IDF 是用 codeload tarball 装的。
GitHub 的 tarball **不包含 submodule 内容**，导致 mbedtls / lwip / esp_wifi/lib /
esp_phy/lib / esp_coex/lib / heap/tlsf 等 23 个目录全空 —— 任何工程都编译不了。

本脚本绕开 git submodule 机制：
  1. 读 $IDF_PATH/.gitmodules 拿到 子模块路径 -> 仓库 URL
  2. 用 GitHub API 查该路径在指定 ref 上记录的 gitlink SHA（即精确提交）
  3. 用 codeload 按 SHA 下 tar.gz（这条通道在本机实测 ~7MB/s）
  4. 解压进对应目录（strip-components=1）

用法:
    python3 scripts/fix-idf-submodules.py                 # ref 默认 v5.5.5
    python3 scripts/fix-idf-submodules.py --ref v6.0.2
    python3 scripts/fix-idf-submodules.py --check         # 只检查，不下载
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

IDF_PATH = Path(os.environ.get("IDF_PATH", Path.home() / "esp" / "esp-idf"))
API = "https://api.github.com"
CODELOAD = "https://codeload.github.com"
PARENT_REPO = "espressif/esp-idf"


def http_get_json(url: str, retries: int = 3) -> dict:
    last = None
    for i in range(retries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "fix-idf-submodules"})
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.load(r)
        except Exception as e:  # noqa: BLE001
            last = e
            time.sleep(1.5 * (i + 1))
    raise RuntimeError(f"GET {url} failed: {last}")


def parse_gitmodules(path: Path) -> list[tuple[str, str]]:
    """返回 [(submodule_path, owner/repo), ...]"""
    out: list[tuple[str, str]] = []
    cur_path = cur_url = None
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if line.startswith("path"):
            cur_path = line.split("=", 1)[1].strip()
        elif line.startswith("url"):
            cur_url = line.split("=", 1)[1].strip()
        if cur_path and cur_url:
            url = cur_url
            # 相对 URL 形如 ../../espressif/mbedtls.git -> espressif/mbedtls
            if url.startswith("../../"):
                url = url[len("../../"):]
            elif url.startswith("../"):
                url = url[len("../"):]
            url = url.removesuffix(".git")
            out.append((cur_path, url))
            cur_path = cur_url = None
    return out


def submodule_shas(ref: str, paths: list[str]) -> dict[str, str]:
    """优先用一次递归 tree 查询拿到全部 gitlink SHA；被截断则退化为逐路径查询。"""
    shas: dict[str, str] = {}
    try:
        tree = http_get_json(f"{API}/repos/{PARENT_REPO}/git/trees/{ref}?recursive=1")
        if not tree.get("truncated"):
            for ent in tree.get("tree", []):
                if ent.get("type") == "commit" and ent.get("path") in paths:
                    shas[ent["path"]] = ent["sha"]
            if len(shas) == len(paths):
                print(f"  (递归 tree 一次拿全 {len(shas)} 个 SHA)")
                return shas
            print(f"  (递归 tree 只拿到 {len(shas)}/{len(paths)}，剩余逐个查)")
    except Exception as e:  # noqa: BLE001
        print(f"  (递归 tree 失败: {e}，逐个查)")

    for p in paths:
        if p in shas:
            continue
        try:
            d = http_get_json(f"{API}/repos/{PARENT_REPO}/contents/{p}?ref={ref}")
            if d.get("type") == "submodule":
                shas[p] = d["sha"]
        except urllib.error.HTTPError as e:
            if e.code == 403:
                print("  !! GitHub API 限额用尽（未认证 60 次/小时），稍后重试")
                break
            print(f"  !! {p}: HTTP {e.code}")
    return shas


def is_empty(p: Path) -> bool:
    return (not p.exists()) or (not any(p.iterdir()))


def download_and_extract(repo: str, sha: str, dest: Path) -> None:
    url = f"{CODELOAD}/{repo}/tar.gz/{sha}"
    with tempfile.NamedTemporaryFile(suffix=".tar.gz", delete=False) as tmp:
        tmp_path = Path(tmp.name)
    try:
        last = None
        for attempt in range(4):
            try:
                req = urllib.request.Request(url, headers={"User-Agent": "fix-idf-submodules"})
                with urllib.request.urlopen(req, timeout=120) as r, open(tmp_path, "wb") as f:
                    shutil.copyfileobj(r, f, length=1 << 20)
                break
            except Exception as e:  # noqa: BLE001
                last = e
                time.sleep(2 * (attempt + 1))
        else:
            raise RuntimeError(f"下载失败 {url}: {last}")

        if dest.exists():
            shutil.rmtree(dest)
        dest.mkdir(parents=True)
        # 用系统 tar 而不是 python tarfile：
        # nimble 里有个符号链接故意指向子模块外（../syscfg/syscfg.h，IDF 构建时才生成），
        # python 的 data 过滤器会拒绝并中断解压，留下半个目录。
        subprocess.run(
            ["tar", "--strip-components=1", "-xzf", str(tmp_path), "-C", str(dest)],
            check=True,
        )
    finally:
        tmp_path.unlink(missing_ok=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", default="v5.5.5", help="ESP-IDF 版本 tag，默认与 install-idf.sh 一致")
    ap.add_argument("--idf-path", default=str(IDF_PATH))
    ap.add_argument("--check", action="store_true", help="只报告缺失，不下载")
    ap.add_argument("--only", action="append", default=[], metavar="PATH",
                    help="只处理指定子模块路径（可重复）")
    ap.add_argument("--force", action="store_true",
                    help="已存在的子模块也重新拉取（修复半途中断的目录）")
    args = ap.parse_args()

    idf = Path(args.idf_path)
    gm = idf / ".gitmodules"
    if not gm.exists():
        print(f"找不到 {gm}", file=sys.stderr)
        return 1

    subs = parse_gitmodules(gm)
    if args.only:
        want = set(args.only)
        subs = [(p, r) for p, r in subs if p in want]
        if not subs:
            print(f"--only 没匹配到任何子模块: {args.only}", file=sys.stderr)
            return 1
    if args.force:
        missing = list(subs)
    else:
        missing = [(p, r) for p, r in subs if is_empty(idf / p)]
    print(f"ESP-IDF: {idf}")
    print(f"submodule 共 {len(subs)} 个，缺失 {len(missing)} 个")
    if not missing:
        print("✅ 全部就位，无需处理")
        return 0
    if args.check:
        for p, _ in missing:
            print(f"  ❌ {p}")
        return 0

    print(f"\n查询 {args.ref} 上记录的精确提交 SHA ...")
    shas = submodule_shas(args.ref, [p for p, _ in subs])

    ok = fail = 0
    for path, repo in missing:
        sha = shas.get(path)
        if not sha:
            print(f"  ⚠️  {path}: 拿不到 SHA，跳过")
            fail += 1
            continue
        dest = idf / path
        print(f"  -> {path}  {repo}@{sha[:10]}")
        try:
            download_and_extract(repo, sha, dest)
            ok += 1
        except Exception as e:  # noqa: BLE001
            print(f"     ❌ {e}")
            fail += 1

    print(f"\n完成：成功 {ok}，失败 {fail}")
    still = [(p, r) for p, r in subs if is_empty(idf / p)]
    if still:
        print("仍缺失：")
        for p, _ in still:
            print(f"  ❌ {p}")
        return 1
    print("✅ 所有 submodule 目录均已填充")
    return 0


if __name__ == "__main__":
    sys.exit(main())
