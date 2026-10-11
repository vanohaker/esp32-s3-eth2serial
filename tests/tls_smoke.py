"""Read-only hardware check of the factory TLS certificate and browser warning.

Uses Chromium's real certificate verifier, then explicitly proceeds through the
device's self-signed certificate warning. Does not install trust or send UART data.
"""
import argparse
import ipaddress
import socket
import ssl

from playwright.sync_api import Error, expect, sync_playwright


def check(host, port, hostname):
    # Capture the public certificate, then trust only it in a separate test
    # context to verify its signature, dates, server purpose and DNS identity.
    context = ssl._create_unverified_context()
    with socket.create_connection((host, port), timeout=10) as raw:
        with context.wrap_socket(raw, server_hostname=host) as tls:
            pem = ssl.DER_cert_to_PEM_cert(tls.getpeercert(binary_form=True))
    trusted = ssl.create_default_context(cadata=pem)
    trusted.hostname_checks_common_name = False
    with socket.create_connection((host, port), timeout=10) as raw:
        with trusted.wrap_socket(raw, server_hostname=hostname) as tls:
            assert ("DNS", hostname) in tls.getpeercert()["subjectAltName"]

    with sync_playwright() as playwright:
        # Full Chromium shows the same warning UI as the normal desktop browser.
        browser = playwright.chromium.launch(channel="chromium")
        page = browser.new_page()  # Deliberately no ignore_https_errors.
        errors = []
        page.on("pageerror", lambda error: errors.append(str(error)))
        try:
            page.goto(f"https://{host}:{port}/", timeout=20000)
        except Error as error:
            assert "net::ERR_CERT_AUTHORITY_INVALID" in str(error), str(error)
        else:
            raise AssertionError("Expected an untrusted self-signed certificate warning")
        expect(page.locator("#error-code")).to_contain_text("ERR_CERT_AUTHORITY_INVALID")
        page.locator("#details-button").click()
        expect(page.locator("#proceed-link")).to_be_visible()
        page.locator("#proceed-link").click()
        expect(page.locator("#login")).to_be_visible(timeout=20000)
        health = page.evaluate("async () => (await fetch('/api/health')).json()")
        assert health.get("boot_id"), health
        assert not errors, errors
        browser.close()
    print("TLS passed: SAN/server validation; Chromium warning is overridable; UI and API reachable")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, type=ipaddress.IPv4Address)
    parser.add_argument("--port", type=int, default=443)
    parser.add_argument("--hostname", required=True)
    args = parser.parse_args()
    check(str(args.host), args.port, args.hostname)
