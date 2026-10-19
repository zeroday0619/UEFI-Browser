(async () => {
  try {
    const check = (condition, text) => { if (!condition) throw new Error(text); };
    const data = navigator.userAgentData;
    const platform = expectedChrome ? 'Linux' : 'UEFI';
    const brand = expectedChrome ? 'Google Chrome' : 'UEFIBrowser';
    check(data.platform === platform && data.brands.some(value => value.brand === brand), 'Metadata mismatch');
    check(navigator.userAgent.includes('UEFIBrowser') !== expectedChrome, 'UA product mismatch');
    check(navigator.userAgent.includes(expectedChrome ? 'X11; Linux x86_64' : 'UEFI; x86_64'), 'UA platform mismatch');
    const brands = data.brands.map(value => JSON.stringify(value.brand) + ';v=' + JSON.stringify(value.version)).join(', ');
    for (const headers of [documentIdentity, resourceIdentity]) {
      for (const name of Object.keys(headers)) check(headers[name].length === 1, 'Duplicate/missing ' + name);
      check(headers['User-Agent'][0] === navigator.userAgent, 'Wire UA mismatch');
      check(headers['Sec-CH-UA'][0] === brands, 'Wire brands mismatch');
      check(headers['Sec-CH-UA-Platform'][0] === JSON.stringify(platform), 'Wire platform mismatch');
      check(headers['Sec-CH-UA-Mobile'][0] === '?0', 'Wire mobile mismatch');
    }
    const high = await data.getHighEntropyValues(['fullVersionList', 'architecture', 'bitness']);
    check(high.fullVersionList.some(value => value.brand === brand), 'Full brand mismatch');
    const permissions = await Promise.race([
      Promise.all(['geolocation', 'notifications', 'camera', 'microphone', 'clipboard-read']
        .map(name => navigator.permissions.query({name}))),
      new Promise((resolve, reject) => setTimeout(() => reject(new Error('Permission timeout')), 5000))
    ]);
    check(permissions.every(result => result.state === 'denied'), 'Unsupported permission granted');
    let rejected = false;
    try { await navigator.permissions.query({name: 'not-a-permission'}); }
    catch (error) { rejected = error instanceof TypeError; }
    check(rejected, 'Invalid permission descriptor accepted');
    console.log('PERMISSIONS PASS ' + location.hostname);
    check(navigator.cookieEnabled === false, 'Cookie capability mismatch');
    document.cookie = 'fixture=1; SameSite=Lax';
    check(document.cookie === '', 'Unsupported cookie state retained');
    check(await document.hasStorageAccess() === false, 'Unsupported storage access granted');
    console.log('COOKIE_DISABLED PASS ' + location.hostname);
    console.log('CHROME_IDENTITY PASS ' + location.hostname + ' ' + JSON.stringify(data));
  } catch (error) { console.log('CHROME_IDENTITY FAIL ' + error.message); }
})();
