"""Browser OTA flow with mocked API; signature security is tested on hardware."""
import json
from pathlib import Path
from urllib.parse import urlsplit
from playwright.sync_api import sync_playwright, expect

WEB = Path(__file__).resolve().parents[1] / "main/web"
SETTINGS = dict(hostname="dongle", dhcp=True, ip="192.0.2.10", mask="255.255.255.0",
                gateway="192.0.2.1", dns="192.0.2.1", vlan=0, baud=115200, data_bits=8,
                parity="none", stop_bits=1, smtp=False, smtp_host="", smtp_port=25,
                mail_from="", mail_to="", web_tls=True, web_port=443,
                ssh=True, ssh_port=22, telnet=False, telnet_port=23)


def scenario(browser, outcome):
    accepted = False
    health_reads = 0
    uploads = []

    def route(route):
        nonlocal accepted, health_reads
        path = urlsplit(route.request.url).path
        if not path.startswith("/api/"):
            name = "index.html" if path == "/" else path[1:]
            mime = {"index.html": "text/html", "app.js": "application/javascript", "style.css": "text/css"}
            route.fulfill(body=(WEB / name).read_bytes(), content_type=mime[name])
            return
        code = 200
        result = {}
        if path == "/api/login":
            result = {"token": "t" * 64}
        elif path == "/api/settings":
            result = SETTINGS
        elif path == "/api/status":
            result = dict(SETTINGS, boot_id="before", version="main-1234567-dirty", resources=0)
        elif path == "/api/health":
            if accepted:
                health_reads += 1
            result = dict(boot_id="after" if accepted else "before", version="main-1234567-dirty",
                          image_id="same-image", ota_state="factory", ota_partition="factory")
            if accepted and outcome == "success":
                result.update(ota_state="pending" if health_reads < 2 else "valid", ota_partition="ota_0")
        elif path == "/api/firmware":
            assert route.request.headers["authorization"] == "Bearer " + "t" * 64
            uploads.append(route.request.post_data_buffer)
            assert len(uploads[-1]) == 8192
            if outcome == "rejected":
                code, result = 400, {"message": "Образ повреждён или не подписан ключом проекта"}
            else:
                accepted = True
                code, result = 202, {"image_id": "same-image", "ota_partition": "ota_0"}
        route.fulfill(status=code, body=json.dumps(result), content_type="application/json")

    page = browser.new_page()
    page.route("**/*", route)
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.goto("https://dongle.test/")
    page.locator('#login [name="name"]').fill("admin")
    page.locator('#login [name="password"]').fill("mock-password")
    page.locator('#login-submit').click()
    page.get_by_role("link", name="Система", exact=True).click()
    file = page.locator("#firmware-file")
    button = page.locator("#firmware-form button")
    file.set_input_files({"name": "bad.bin", "mimeType": "application/octet-stream", "buffer": b"bad"})
    button.click()
    expect(page.locator("#message")).to_contain_text("подписанный образ")
    assert not uploads
    file.set_input_files({"name": "eth2serial.bin", "mimeType": "application/octet-stream", "buffer": b"\0" * 8192})
    page.once("dialog", lambda dialog: dialog.dismiss())
    button.click()
    assert not uploads
    page.once("dialog", lambda dialog: dialog.accept())
    button.click()
    if outcome == "rejected":
        expect(page.locator("#message")).to_contain_text("не подписан ключом проекта")
        expect(button).to_be_enabled()
        expect(page.locator("#main")).to_be_visible()
    else:
        expect(button).to_be_disabled()
        expect(page.locator("#firmware-progress")).to_contain_text("Ожидание перезагрузки")
        expected = "установлено и проверено" if outcome == "success" else "вернулось к другой прошивке"
        expect(page.locator("#message")).to_contain_text(expected, timeout=20000)
        expect(page.locator("#login")).to_be_visible()
    assert len(uploads) == 1 and not errors, errors
    page.close()


with sync_playwright() as playwright:
    browser = playwright.chromium.launch()
    for outcome in ("rejected", "rollback", "success"):
        scenario(browser, outcome)
    browser.close()
print("OTA UI passed: file limit, confirmation, rejection, rollback of identical image, pending then valid")
