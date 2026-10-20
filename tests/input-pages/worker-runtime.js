self.onmessage = async event => {
  const message = event.data;
  if (message.operation === 'compute') {
    const bytes = new Uint8Array(message.buffer);
    for (let index = 0; index < bytes.length; ++index) bytes[index] ^= 0x5a;
    postMessage({operation: 'compute', sum: message.left + message.right,
      isolated: typeof document === 'undefined', buffer: bytes.buffer}, [bytes.buffer]);
  } else if (message.operation === 'fetch') {
    const response = await fetch('worker-payload.txt');
    postMessage({operation: 'fetch', text: await response.text()});
  } else if (message.operation === 'timer') {
    setTimeout(() => postMessage({operation: 'timer'}), 25);
  } else if (message.operation === 'import') {
    importScripts('worker-import.js');
    postMessage({operation:'import', value:self.importedAnswer});
  } else if (message.operation === 'nested') {
    const child = new Worker('worker-runtime.js');
    child.onmessage = event => { child.terminate(); postMessage({operation:'nested', value:event.data.operation}); };
    child.onerror = event => { child.terminate(); postMessage({operation:'error', value:event.message}); };
    child.postMessage({operation:'timer'});
  } else if (message.operation === 'close') {
    postMessage({operation: 'closing'});
    close();
  }
};
