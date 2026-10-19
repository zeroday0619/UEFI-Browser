"""Verify local child-frame origins, sandbox flags, inherited CSP and detach cleanup."""
def run(key, wait_marker, type_address):
    key('ctrl', 'l'); type_address('http://10.0.2.2:8765/frame-policy.html'); key('ret')
    wait_marker('FRAME_POLICY PASS')
    key('ctrl', 'l'); type_address('about:blank'); key('ret')
    print('PASS: child frame origins, sandbox, inherited CSP and detach cleanup')
