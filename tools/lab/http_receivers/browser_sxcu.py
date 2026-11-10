#!/usr/bin/env python3
"""Runs inside the Playwright container started by receivers.py.

Finishes each receiver's first-run setup through its own web interface and downloads the
ShareX custom uploader file (.sxcu) that interface offers, exactly as a user would get it.
Passwords are read from, and the .sxcu files written to, the private state folder mounted
at /state. Nothing secret is printed: the summary shows field names and lengths only.
"""
import json
import os
import re
import sys
import urllib.error
import urllib.request

from playwright.sync_api import sync_playwright

STATE = '/state'


def secret(name):
    return open(os.path.join(STATE, name)).read().strip()


def save(download, name):
    path = os.path.join(STATE, 'sxcu', name + '.sxcu')
    download.save_as(path)
    os.chmod(path, 0o600)
    return path


def post(url, body):
    request = urllib.request.Request(url, data=json.dumps(body).encode(), headers={'Content-Type': 'application/json'}, method='POST')
    try:
        with urllib.request.urlopen(request, timeout=20) as reply:
            return reply.status
    except urllib.error.HTTPError as error:
        return error.code


def zipline(context, base):
    # The first-run wizard posts here; later runs find the admin already there.
    post(base + '/api/setup', {'username': 'putgdlab', 'password': secret('zipline.adminpw')})
    page = context.new_page()
    page.goto(base + '/auth/login', wait_until='networkidle')
    page.fill('input[name="username"], input[placeholder*="sername" i]', 'putgdlab')
    page.fill('input[type="password"]', secret('zipline.adminpw'))
    page.keyboard.press('Enter')
    page.wait_for_url('**/dashboard**', timeout=30000)
    # Settings → Uploaders & exports → Generate Uploaders → ShareX → Download.
    page.goto(base + '/dashboard/settings?tab=tools', wait_until='networkidle')
    page.get_by_role('button', name='ShareX').first.click()
    with page.expect_download() as waiting:
        page.locator('[role="dialog"]').first.get_by_role('button', name='Download').click()
    return save(waiting.value, 'zipline')


def chibisafe(context, base):
    page = context.new_page()

    def login(password):
        # Signed out first: an open session would skip the form and prove nothing.
        context.clear_cookies()
        page.goto(base + '/login', wait_until='load')
        page.evaluate('() => { localStorage.clear(); sessionStorage.clear(); }')
        page.goto(base + '/login', wait_until='load')
        page.wait_for_selector('input[name="username"]', timeout=20000)
        page.fill('input[name="username"]', 'admin')
        page.fill('input[name="password"]', password)
        page.keyboard.press('Enter')
        try:
            page.wait_for_url('**/dashboard**', timeout=8000)
        except Exception:  # noqa: BLE001 - a refused password stays on the form
            return False
        return True

    password = secret('chibisafe.adminpw')
    if not login(password):
        # A new instance has the documented admin/admin: replace it first.
        if not login('admin'):
            raise RuntimeError('Chibisafe refused both the stored and the first-run password')
        page.goto(base + '/dashboard/account', wait_until='load')
        page.wait_for_timeout(2000)
        page.wait_for_selector('input[type="password"]', timeout=20000)
        fields = page.locator('input[type="password"]')
        fields.nth(0).fill('admin')
        fields.nth(1).fill(password)
        fields.nth(2).fill(password)
        page.get_by_role('button', name='Change password').click()
        page.wait_for_timeout(2000)
        if not login(password):
            raise RuntimeError('Chibisafe did not accept its new password')
    # Links are built from this address; without it files and thumbnails are broken.
    page.goto(base + '/dashboard/admin/settings', wait_until='load')
    page.wait_for_timeout(2500)
    serve = page.locator('input[name="serveUploadsFrom"]').first
    if serve.input_value() != base:
        serve.fill(base)
        page.get_by_role('button', name='Save settings').click()
        page.wait_for_timeout(2000)
    page.goto(base + '/dashboard/account', wait_until='load')
    page.wait_for_timeout(2000)
    page.get_by_role('button', name='Request new API key').click()
    page.wait_for_timeout(1500)
    # Sidebar → Utils → ShareX config.
    page.goto(base + '/dashboard', wait_until='load')
    page.wait_for_timeout(2000)
    with page.expect_download(timeout=30000) as waiting:
        page.get_by_text('ShareX config', exact=True).first.click()
    return save(waiting.value, 'chibisafe')


def xbackbone(context, base):
    page = context.new_page()
    email, password = 'putgdlab@example.test', secret('xbackbone.adminpw')
    page.goto(base + '/', wait_until='networkidle')
    if '/install' in page.url:
        if page.input_value('#storage_path') == '':
            page.fill('#storage_path', '/app/www/public/storage')
        page.fill('#email', email)
        page.fill('#password', password)
        page.click('button[type="submit"]')
        page.wait_for_load_state('networkidle')
    page.goto(base + '/login', wait_until='networkidle')
    if '/login' in page.url:
        page.fill('input[name="username"]', email)
        page.fill('input[name="password"]', password)
        page.click('button[type="submit"]')
        page.wait_for_load_state('networkidle')
    # Profile → Token → Update, then Client Configuration → ShareX.
    page.goto(base + '/profile', wait_until='networkidle')
    if not page.locator('#token').input_value():
        page.locator('#refresh-token, button:has-text("Update")').first.click()
        page.wait_for_timeout(1500)
    with page.expect_download() as waiting:
        page.locator('a[href$="/config/sharex"]').click()
    return save(waiting.value, 'xbackbone')


def describe(path):
    """Field names and value lengths: enough to see what a receiver handed out."""
    def shape(value, credential=False):
        if isinstance(value, dict):
            return {key: shape(child, credential or key in ('Headers', 'Parameters', 'Arguments', 'Data')) for key, child in value.items()}
        text = str(value)
        # A header, parameter or argument is shown only when it is a bare placeholder.
        if credential and text.strip().lower() not in ('{filename}', '{input}', '$filename$', '$input$'):
            return '<%d characters>' % len(text)
        # Elsewhere any long unbroken run is treated as a key.
        return re.sub(r'[A-Za-z0-9_\-]{24,}', lambda run: '<%d characters>' % len(run.group()), text)
    return shape(json.load(open(path)))


def main():
    receivers = json.loads(sys.argv[1])
    os.makedirs(os.path.join(STATE, 'sxcu'), exist_ok=True)
    failed = False
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch()
        for name, base in receivers.items():
            context = browser.new_context(accept_downloads=True, viewport={'width': 1400, 'height': 1100})
            # A refresh that fails must not leave the previous file to be mistaken for a new one.
            previous = os.path.join(STATE, 'sxcu', name + '.sxcu')
            if os.path.exists(previous):
                os.remove(previous)
            try:
                path = {'zipline': zipline, 'chibisafe': chibisafe, 'xbackbone': xbackbone}[name](context, base)
                print(json.dumps({'receiver': name, 'sxcu': os.path.basename(path), 'fields': describe(path)}))
            except Exception as error:  # noqa: BLE001 - reported per receiver, the others still run
                failed = True
                print(json.dumps({'receiver': name, 'error': type(error).__name__ + ': ' + str(error).splitlines()[0][:200]}))
            finally:
                context.close()
        browser.close()
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
