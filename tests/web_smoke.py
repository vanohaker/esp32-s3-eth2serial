"""Browser smoke test with mocked API. This does not exercise ESP-IDF handlers."""
import json
import secrets
from pathlib import Path
from urllib.parse import urlsplit

from playwright.sync_api import sync_playwright, expect

WEB = Path(__file__).resolve().parents[1] / "main" / "web"
settings = dict(hostname="serial-dongle", dhcp=True, ip="192.168.1.50",
                mask="255.255.255.0", gateway="192.168.1.1", dns="192.168.1.1",
                vlan=0, baud=115200, data_bits=8, parity="none", stop_bits=1,
                smtp=False, smtp_host="", smtp_port=25, mail_from="", mail_to="",
                web_tls=True, web_port=443, ssh=True, ssh_port=22, telnet=False, telnet_port=23)
status = dict(version="main-1234567-dirty", boot_id="boot-before", hostname="serial-dongle", ip="192.0.2.10",
              network=True, microSD=True, resources=0, ssh=True, ssh_port=22,
              telnet=False, telnet_port=23, web_tls=True, web_port=443, uptime=30,
              free_heap=500000, transfer="Idle", sent=0, size=0, storage="Mounted")
calls = []
health_reads = 0
reboot_requested = False
pending_script = []
password = secrets.token_hex(12)
token = secrets.token_hex(32)


def route_request(route):
    global health_reads, reboot_requested
    path = urlsplit(route.request.url).path
    if not path.startswith("/api/"):
        name = "index.html" if path == "/" else path[1:]
        assert name in {"index.html", "app.js", "style.css"}, name
        mime = {"index.html": "text/html", "app.js": "application/javascript", "style.css": "text/css"}
        if name == "app.js":
            pending_script.append(route)
            return
        route.fulfill(body=(WEB / name).read_bytes(), content_type=mime[name])
        return
    data = route.request.post_data_json if route.request.method == "POST" else None
    calls.append((path, data))
    result = {"message": "OK"}
    if path == "/api/login":
        assert data == {"name": "admin", "password": password}
        result = {"token": token}
    elif path == "/api/health":
        health_reads += 1
        result = {"version": status["version"], "boot_id": "boot-after" if reboot_requested and health_reads >= 4 else "boot-before"}
    else:
        assert route.request.headers.get("authorization") == "Bearer " + token
        if path == "/api/settings" and data is None:
            result = settings
        elif path == "/api/status":
            result = status
        elif path == "/api/users" and data is None:
            result = [{"name": "admin", "enabled": True, "public_key": ""}, {"name": "operator", "enabled": False, "public_key": ""}, {"name": "<img src=x onerror=alert(1)>", "enabled": True, "public_key": ""}]
        elif path == "/api/files":
            result = [{"name": "firmware.bin", "size": 1024}]
        elif path == "/api/reboot":
            reboot_requested = True
    route.fulfill(body=json.dumps(result), content_type="application/json")


with sync_playwright() as playwright:
    browser = playwright.chromium.launch()
    page = browser.new_page(viewport={"width": 1024, "height": 850})
    errors = []
    page.on("pageerror", lambda error: errors.append(str(error)))
    page.route("**/*", route_request)
    page.goto("https://dongle.test/", wait_until="commit")
    expect(page.locator("#login-submit")).to_be_disabled()
    # A visible form must not submit credentials before deferred JS is loaded.
    page.locator('#login [name="name"]').fill("admin")
    page.locator('#login [name="password"]').fill(password)
    page.locator('#login [name="password"]').press("Enter")
    assert page.url == "https://dongle.test/"
    assert not any(path == "/api/login" for path, _ in calls)
    assert pending_script
    pending_script.pop().fulfill(body=(WEB / "app.js").read_bytes(), content_type="application/javascript")
    expect(page.locator("#login-submit")).to_be_enabled()
    expect(page.locator("#build-version")).to_have_text(status["version"])
    assert page.locator("nav button").count() == 0
    page.locator('#login [name="name"]').fill("admin")
    page.locator('#login [name="password"]').fill(password)
    page.get_by_role("button", name="Войти", exact=True).click()
    expect(page.locator("#main")).to_be_visible()
    expect(page.locator("#status-data")).to_contain_text("192.0.2.10")
    page.screenshot(path="/tmp/knife-web-desktop.png")
    page.get_by_role("link", name="Сеть", exact=True).click()
    page.locator('[name="vlan"]').fill("120")
    page.locator("#network-form button").click()
    expect(page.locator("#message")).to_contain_text("Сохранено")
    saved = [data for path, data in calls if path == "/api/settings" and data]
    assert saved[-1]["vlan"] == 120 and saved[-1]["dhcp"] is True
    page.get_by_role("link", name="Пользователи", exact=True).click()
    expect(page.locator("#user-list")).to_contain_text("<img src=x onerror=alert(1)>")
    assert page.locator("#user-list img").count() == 0
    admin = page.locator("#user-list tr").filter(has=page.get_by_role("cell", name="admin", exact=True))
    assert admin.get_by_role("button", name="Удалить", exact=True).count() == 0
    admin.get_by_role("button", name="Изменить").click()
    expect(page.locator('#user-form [name="name"]')).to_have_attribute("readonly", "")
    expect(page.locator('#user-form [name="enabled"]')).to_be_disabled()
    expect(page.locator('#user-form [name="enabled"]')).to_be_checked()
    page.locator("#user-list tr").filter(has=page.get_by_role("cell", name="operator", exact=True)).get_by_role("button", name="Изменить").click()
    expect(page.locator('#user-form [name="name"]')).to_have_attribute("readonly", "")
    expect(page.locator('#user-form [name="enabled"]')).to_be_enabled()
    page.locator('#user-form [name="enabled"]').check()
    page.locator("#user-form button").click()
    expect(page.locator("#login")).to_be_visible()
    assert ("/api/users", {"operation": "update", "name": "operator", "password": "", "enabled": True, "public_key": ""}) in calls
    page.locator('#login [name="password"]').fill(password)
    page.get_by_role("button", name="Войти", exact=True).click()
    page.get_by_role("link", name="Пользователи", exact=True).click()
    page.get_by_role("link", name="Добавить пользователя").click()
    expect(page.locator('#user-form [name="name"]')).to_be_editable()
    page.locator('#user-form [name="name"]').fill("new-admin")
    page.locator('#user-form [name="password"]').fill(password)
    page.locator("#user-form button").click()
    expect(page.locator("#login")).to_be_visible()
    assert any(path == "/api/users" and data and data.get("operation") == "create" and data["name"] == "new-admin" for path, data in calls)
    page.locator('#login [name="password"]').fill(password)
    page.get_by_role("button", name="Войти", exact=True).click()
    page.get_by_role("link", name="Файлы", exact=True).click()
    expect(page.locator("#file-list")).to_contain_text("firmware.bin")
    page.get_by_role("button", name="Выбрать", exact=True).click()
    expect(page.locator('#transfer-form [name="name"]')).to_have_value("firmware.bin")
    page.once("dialog", lambda dialog: dialog.dismiss())
    page.locator("#format").click()
    assert not any(path == "/api/format" for path, _ in calls)
    page.once("dialog", lambda dialog: dialog.accept("FORMAT"))
    page.locator("#format").click()
    expect(page.locator("#message")).to_contain_text("Форматирование запущено")
    assert ("/api/format", {"confirm": "FORMAT"}) in calls
    page.set_viewport_size({"width": 390, "height": 844})
    page.screenshot(path="/tmp/knife-web-mobile.png", full_page=True)
    assert page.evaluate("document.documentElement.scrollWidth <= innerWidth")
    page.get_by_role("link", name="Система", exact=True).click()
    assert page.locator('#tls-form, #tls-cert, #tls-key').count() == 0
    expect(page.locator('#system')).to_contain_text('встроенный самоподписанный сертификат')
    page.locator('[name="web_tls"]').select_option("false")
    expect(page.locator('[name="web_port"]')).to_have_value("80")
    page.locator("#system-form button").click()
    expect(page.locator("#web-address")).to_have_attribute("href", "http://dongle.test/")
    assert any(path == "/api/settings" and data and data.get("web_tls") is False and data["web_port"] == 80 for path, data in calls)
    page.locator('[name="web_port"]').fill("8080")
    page.locator('[name="web_tls"]').select_option("true")
    expect(page.locator('[name="web_port"]')).to_have_value("8080")
    page.locator('[name="web_port"]').fill("443")
    page.locator("#system-form button").click()
    expect(page.locator("#reconnect")).to_be_hidden()
    page.once("dialog", lambda dialog: dialog.accept())
    page.locator("#reboot").click()
    expect(page.locator("#message")).to_contain_text("Ожидание ответа")
    expect(page.locator("#message")).to_contain_text("Устройство перезагрузилось", timeout=20000)
    expect(page.locator("#login")).to_be_visible()
    assert health_reads >= 4 and not errors, errors
    # A protocol change cannot be polled through the old origin. Show the new
    # address without claiming that the reboot has already completed.
    page.locator('#login [name="password"]').fill(password)
    page.get_by_role("button", name="Войти", exact=True).click()
    page.get_by_role("link", name="Система", exact=True).click()
    page.locator('[name="web_tls"]').select_option("false")
    page.locator("#system-form button").click()
    expect(page.locator("#web-address")).to_have_attribute("href", "http://dongle.test/")
    page.once("dialog", lambda dialog: dialog.accept())
    page.locator("#reboot").click()
    expect(page.locator("#message")).to_contain_text("Команда перезагрузки принята")
    expect(page.locator("#web-address")).to_be_visible()
    assert not errors, errors
    browser.close()
print("Web smoke passed: version, navigation, immutable usernames, admin controls, HTTP/HTTPS, forms, escaping, formatting, mobile layout, reboot")
