"""Render a release card; publish only with --publish and a webhook secret."""

import argparse
from datetime import datetime
import io
import json
import os
from pathlib import Path
import re
import sys
import urllib.error
import urllib.request
import uuid

from PIL import Image, ImageDraw, ImageFont

SITE = "https://hakune.blog/haku-control.html"
FALLBACK = "The latest Windows build is now available."


def plain(text):
    text = re.sub(r"!?\[([^\]]+)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"https?://\S+", "", text)
    text = re.sub(r"\s+by @\S+.*$", "", text)
    return " ".join(text.replace("`", "").replace("**", "").replace("__", "").split())


def highlights(body):
    result = []
    for line in (body or "").splitlines():
        if re.match(r"\s*#{1,6}\s", line) or not line.strip():
            continue
        line = plain(re.sub(r"^\s*(?:[-*+]\s+|\d+[.)]\s+)", "", line))
        if not line or re.match(r"(?i)(full changelog|new contributors|what.s changed)\s*:?$", line):
            continue
        result.append(line)
        if len(result) == 3:
            break
    return result or [FALLBACK]


def font(size, bold=False):
    names = (["/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "C:/Windows/Fonts/arialbd.ttf"]
             if bold else ["/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "C:/Windows/Fonts/arial.ttf"])
    for name in names:
        if Path(name).exists():
            return ImageFont.truetype(name, size)
    raise RuntimeError("Install DejaVu Sans fonts to render news cards")


def fit(text, face, width):
    if face.getlength(text) <= width:
        return text
    low, high = 0, len(text)
    while low < high:
        middle = (low + high + 1) // 2
        if face.getlength(text[:middle] + "…") <= width:
            low = middle
        else:
            high = middle - 1
    return text[:low].rstrip() + "…"


def wrapped(text, face, width):
    lines = [""]
    for word in text.split():
        candidate = (lines[-1] + " " + word).strip()
        if face.getlength(candidate) <= width:
            lines[-1] = candidate
        elif len(lines) == 1:
            lines.append(word)
        else:
            lines[-1] = fit(lines[-1] + " " + word + "…", face, width)
            break
    return [fit(line, face, width) for line in lines]


def render(release, output):
    image = Image.new("RGB", (1200, 800), "#101014")
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((28, 28, 1171, 771), radius=28, outline="#33313e", width=2)
    draw.rounded_rectangle((64, 64, 70, 125), radius=3, fill="#bca7ff")
    draw.text((90, 66), "haku control", font=font(44, True), fill="#f4f1fa")
    draw.text((90, 120), "ONE LIGHT SCENE. EVERY DEVICE.", font=font(16), fill="#9b96aa")
    draw.rounded_rectangle((924, 72, 1136, 116), radius=22, fill="#292238")
    draw.text((948, 83), "RELEASE NOTES", font=font(18, True), fill="#cbb8ff")

    tag = str(release["tag_name"])
    draw.text((64, 180), fit(tag, font(110, True), 1070), font=font(110, True), fill="#f4f1fa")
    date = datetime.fromisoformat(release["published_at"].replace("Z", "+00:00"))
    draw.text((68, 318), date.strftime("%d %B %Y").upper() + "  /  WINDOWS", font=font(20), fill="#bca7ff")
    draw.line((64, 373, 1136, 373), fill="#33313e", width=2)

    items = highlights(release.get("body"))
    for i, item in enumerate(items):
        y = 410 + i * 83
        draw.text((68, y + 2), f"0{i + 1}", font=font(18), fill="#bca7ff")
        for j, line in enumerate(wrapped(item, font(26), 989)):
            draw.text((122, y + j * 32), line, font=font(26), fill="#ded9e8")

    draw.line((64, 688, 1136, 688), fill="#33313e", width=2)
    draw.text((66, 718), "GET THE UPDATE", font=font(20, True), fill="#cbb8ff")
    draw.text((660, 719), "hakune.blog/haku-control.html", font=font(22), fill="#ded9e8")
    image.save(output, format="PNG")
    return items


def publish(release, image_path, items):
    webhook = os.environ.get("DISCORD_NEWS_WEBHOOK_URL", "")
    if not re.fullmatch(r"https://discord\.com/api/webhooks/\d+/[\w-]+", webhook):
        raise ValueError("DISCORD_NEWS_WEBHOOK_URL must be a Discord incoming webhook URL")
    payload = {
        "username": "Haku Control News",
        "allowed_mentions": {"parse": []},
        "embeds": [{
            "title": fit(str(release["tag_name"]), font(24), 600) + " is out",
            "url": SITE,
            "description": "[Download for Windows · Setup guide](" + SITE + ")",
            "color": 0xA78BFA,
            "image": {"url": "attachment://haku-control-news.png"},
        }],
        "attachments": [{"id": 0, "filename": "haku-control-news.png",
                         "description": (str(release["tag_name"]) + ": " + " / ".join(items))[:1024]}],
    }
    boundary = uuid.uuid4().hex
    body = io.BytesIO()
    body.write((f'--{boundary}\r\nContent-Disposition: form-data; name="payload_json"\r\n'
                'Content-Type: application/json\r\n\r\n').encode())
    body.write(json.dumps(payload).encode())
    body.write((f'\r\n--{boundary}\r\nContent-Disposition: form-data; name="files[0]"; '
                'filename="haku-control-news.png"\r\nContent-Type: image/png\r\n\r\n').encode())
    body.write(Path(image_path).read_bytes())
    body.write(f"\r\n--{boundary}--\r\n".encode())
    request = urllib.request.Request(webhook + "?wait=true", data=body.getvalue(), headers={
        "Content-Type": "multipart/form-data; boundary=" + boundary,
        "User-Agent": "HakuControlNews/1.0",
    })
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            message = json.load(response)
    except urllib.error.HTTPError as error:
        raise RuntimeError(f"Discord rejected the card (HTTP {error.code}); inspect channel permissions") from None
    except urllib.error.URLError:
        raise RuntimeError("Discord could not be reached; check news before retrying to avoid duplicates") from None
    print("News card published; message " + message["id"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release_json", type=Path)
    parser.add_argument("--output", type=Path, default=Path("haku-control-news.png"))
    parser.add_argument("--publish", action="store_true")
    args = parser.parse_args()
    release = json.loads(args.release_json.read_text(encoding="utf-8"))
    if release.get("draft") or release.get("prerelease"):
        raise ValueError("Only published stable releases can be announced")
    if not release.get("tag_name") or not release.get("published_at"):
        raise ValueError("Release tag and publication date are required")
    items = render(release, args.output)
    if args.publish:
        publish(release, args.output, items)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, RuntimeError) as error:
        sys.exit(str(error))
