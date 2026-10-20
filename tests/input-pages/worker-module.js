import {answer} from './worker-module-value.js';
self.onmessage = () => postMessage(answer);
