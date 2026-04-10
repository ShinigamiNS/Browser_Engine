import re, sys
# Fetch and extract inline scripts from google.com
try:
    import urllib.request
    html = urllib.request.urlopen("https://www.google.com").read().decode('utf-8', errors='replace')
    scripts = re.findall(r'<script[^>]*>(.*?)</script>', html, re.DOTALL)
    for i, s in enumerate(scripts):
        s = s.strip()
        if s and 'src=' not in s[:50]:
            print(f"--- Script {i} ({len(s)} bytes) ---")
            print(s[:200])
            print()
except Exception as e:
    print(f"Error: {e}")
