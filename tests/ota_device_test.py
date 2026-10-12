"""Hardware OTA test: writes the inactive slot; --rollback-test also updates/reboots.

Never run without a device backup. No serial data or SD files are touched.
"""
import argparse
import json
from pathlib import Path
import socket
import ssl
import time
from urllib.parse import urlsplit
import requests
import urllib3

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--url", required=True)
parser.add_argument("--password-file", type=Path, required=True)
parser.add_argument("--image", type=Path, default=Path("build/eth2serial.bin"))
parser.add_argument("--foreign-image", type=Path, required=True)
parser.add_argument("--rollback-test", action="store_true")
parser.add_argument("--browser-install", action="store_true", help="Install the signed image through real Chromium")
args = parser.parse_args()
base = args.url.rstrip("/")
session = requests.Session()
session.trust_env = False
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


def api(path, data=None):
    r = session.request("GET" if data is None else "POST", base + "/api/" + path,
                        json=data, verify=False, timeout=15)
    r.raise_for_status()
    return r.json()


def login():
    # A browser OTA reboot can leave this separate API session with a stale
    # keep-alive connection. Drop it before authenticating to the new boot.
    session.close()
    session.headers.pop("Authorization", None)
    result = api("login", dict(name="admin", password=args.password_file.read_text().rstrip("\r\n")))
    session.headers["Authorization"] = "Bearer " + result["token"]


def wait_boot(old):
    deadline = time.monotonic() + 80
    while time.monotonic() < deadline:
        try:
            health = api("health")
            if health["boot_id"] != old:
                return health
        except requests.RequestException:
            pass
        time.sleep(1)
    raise AssertionError("Device did not return after reboot")


login()
before = api("health")
settings, users = api("settings"), api("users")
assert api("status")["resources"] == 0, "Device busy"
image = args.image.read_bytes()


def upload(data, status):
    r = session.post(base + "/api/firmware", data=data,
                     headers={"Content-Type": "application/octet-stream"}, verify=False, timeout=190)
    assert r.status_code == status, (r.status_code, r.text[:200])
    return r.json()


unauthorized = requests.Session()
unauthorized.trust_env = False
r = unauthorized.post(base + "/api/firmware", data=b"x" * 8192, verify=False, timeout=10)
assert r.status_code == 401
print("Unauthenticated upload rejected", flush=True)
tampered = bytearray(image)
tampered[2048] ^= 1
cases = {"unsigned": image[:-4096], "tampered": bytes(tampered),
         "foreign signature": args.foreign_image.read_bytes(),
         "extra data": image + b"\xff" * 4096, "wrong format": b"x" * 8192}
for name, data in cases.items():
    upload(data, 400)
    assert api("health")["boot_id"] == before["boot_id"]
    assert api("status")["resources"] == 0
    print("Rejected:", name, flush=True)

# Explicitly interrupt an upload after a valid app header.
url = urlsplit(base)
raw = socket.create_connection((url.hostname, url.port or (443 if url.scheme == "https" else 80)), timeout=10)
if url.scheme == "https":
    raw = ssl._create_unverified_context().wrap_socket(raw, server_hostname=url.hostname)
with raw:
    headers = (f"POST /api/firmware HTTP/1.1\r\nHost: {url.netloc}\r\n"
               f"Authorization: {session.headers['Authorization']}\r\n"
               f"Content-Type: application/octet-stream\r\nContent-Length: {len(image)}\r\n\r\n")
    raw.sendall(headers.encode() + image[:1024])
time.sleep(1)
assert api("health")["boot_id"] == before["boot_id"]
assert api("status")["resources"] == 0
print("Interrupted upload preserved current firmware and released resources", flush=True)

if args.rollback_test:
    accepted = upload(image, 202)
    pending = wait_boot(before["boot_id"])
    assert pending["ota_state"] == "pending"
    assert pending["ota_partition"] == accepted["ota_partition"]
    login()
    api("reboot", {"confirm": "REBOOT"})
    rolled_back = wait_boot(pending["boot_id"])
    assert rolled_back["ota_partition"] == before["ota_partition"]
    assert rolled_back["image_id"] == before["image_id"]
    print("Bootloader rollback after restart of unconfirmed image passed", flush=True)
    login()

assert api("settings") == settings and api("users") == users
api("logout", {})
print("Hardware negative OTA checks passed; settings and users preserved", flush=True)

if args.browser_install:
    from playwright.sync_api import sync_playwright, expect, Error
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(channel="chromium")
        page = browser.new_page()
        errors = []
        page.on("pageerror", lambda e: errors.append(str(e)))
        try:
            page.goto(base, timeout=20000)
        except Error as error:
            assert "ERR_CERT_AUTHORITY_INVALID" in str(error)
            page.locator("#details-button").click()
            page.locator("#proceed-link").click()
        expect(page.locator("#login-submit")).to_be_enabled(timeout=20000)
        page.locator('#login [name="name"]').fill("admin")
        page.locator('#login [name="password"]').fill(args.password_file.read_text().rstrip("\r\n"))
        page.locator("#login-submit").click()
        page.get_by_role("link", name="Система", exact=True).click()
        page.locator("#firmware-file").set_input_files(args.image.resolve())
        page.once("dialog", lambda dialog: dialog.accept())
        page.locator("#firmware-form button").click()
        expect(page.locator("#message")).to_contain_text("установлено и проверено", timeout=240000)
        expect(page.locator("#login")).to_be_visible()
        assert not errors, errors
        browser.close()
    login()
    health = api("health")
    assert health["ota_state"] == "valid"
    assert api("settings") == settings and api("users") == users
    api("logout", {})
    print("Real Chromium OTA installation confirmed, settings and users preserved", flush=True)
