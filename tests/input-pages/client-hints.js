(async () => {
  try {
    function check(condition, message) {
      if (!condition) throw new Error(message);
    }
    if (!isSecureContext) {
      check(navigator.userAgentData === undefined, 'Insecure API exposure');
      for (const headers of [documentHints, resourceHints])
        check(Object.values(headers).every(values => values.length === 0), 'Insecure header disclosure');
      console.log('CLIENT_HINTS_TEST HTTP PASS');
      return;
    }
    if (hintsPolicy) {
      check(Object.values(resourceHints).every(values => values.length === 0), 'Permissions Policy ignored');
      console.log('CLIENT_HINTS_TEST POLICY PASS');
      return;
    }
    const data = navigator.userAgentData;
    check(data.platform === 'UEFI' && data.mobile === false, 'Platform or mobile mismatch');
    check(data.brands.some(value => value.brand === 'UEFIBrowser' && value.version === '0'), 'Browser brand missing');
    const serialized = data.brands.map(value => JSON.stringify(value.brand) + ';v=' + JSON.stringify(value.version)).join(', ');
    for (const headers of [documentHints, resourceHints]) {
      check(headers['sec-ch-ua'].length === 1 && headers['sec-ch-ua'][0] === serialized, 'Brand header mismatch');
      check(headers['sec-ch-ua-platform'].length === 1 && headers['sec-ch-ua-platform'][0] === '"UEFI"', 'Platform header mismatch');
      check(headers['sec-ch-ua-mobile'].length === 1 && headers['sec-ch-ua-mobile'][0] === '?0', 'Mobile header mismatch');
    }
    const high = await data.getHighEntropyValues(['architecture', 'bitness', 'platformVersion', 'fullVersionList', 'model', 'wow64']);
    check(high.architecture === 'x86' && high.bitness === '64', 'Architecture mismatch');
    check(high.platformVersion === '' && high.model === '' && high.wow64 === false, 'Invented device information');
    check(high.fullVersionList.some(value => value.brand === 'UEFIBrowser' && value.version === '0.3'), 'Full browser version missing');
    console.log('CLIENT_HINTS_TEST HTTPS PASS ' + JSON.stringify({low: data.toJSON(), high}));
  } catch (error) {
    console.log('CLIENT_HINTS_TEST FAIL ' + error.message);
  }
})();
