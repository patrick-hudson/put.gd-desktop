#!/usr/bin/env python3
"""Runs inside the Playwright container started by check.py (#1872).

Finishes a Nextcloud Login Flow v2 sign-in in a real headless Chromium, the way a user would
in their browser, and takes screenshots of public file and share links. The job is JSON in
argv[1], a private file (it holds login and share links); passwords are read from mounted files,
never from the job.
Nothing secret is printed: each step prints its name and result only.
"""
import json
import sys

from playwright.sync_api import sync_playwright


def password(path):
    return open(path).read().strip()


def login_flow(context, job):
    """Open the login URL, sign in, grant access. True once Nextcloud says the account is connected."""
    page = context.new_page()
    page.goto(job['url'], wait_until='domcontentloaded')
    for _ in range(12):
        page.wait_for_load_state('networkidle')
        text = page.inner_text('body')
        if 'Account connected' in text or 'account connected' in text.lower():
            page.screenshot(path=job['shot'])
            return True
        if page.locator('input[name="password"]').count() and page.locator('input[name="password"]').first.is_visible():
            page.fill('input[name="user"]', job['user'])
            page.fill('input[name="password"]', password(job['password_file']))
            page.locator('button[type="submit"], input[type="submit"]').first.click()
            continue
        grant = page.get_by_role('button', name='Grant access')
        if grant.count():
            grant.first.click()
            continue
        for label in ('Log in', 'Connect to your account'):
            link = page.get_by_role('link', name=label).or_(page.get_by_role('button', name=label))
            if link.count() and link.first.is_visible():
                link.first.click()
                break
        else:
            page.wait_for_timeout(1000)
    page.screenshot(path=job['shot'])
    return False


def shot(context, job):
    page = context.new_page()
    reply = page.goto(job['url'], wait_until='networkidle')
    page.screenshot(path=job['shot'])
    return reply.status if reply else 0


def main():
    with open(sys.argv[1]) as file:
        jobs = json.load(file)
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch()
        context = browser.new_context(ignore_https_errors=True, viewport={'width': 1000, 'height': 700})
        for job in jobs:
            try:
                result = {'login_flow': login_flow, 'shot': shot}[job['kind']](context, job)
            except Exception as error:
                result = 'error %s' % type(error).__name__
            print('%s %s %s' % (job['kind'], job.get('name', ''), result), flush=True)
        browser.close()


if __name__ == '__main__':
    main()
