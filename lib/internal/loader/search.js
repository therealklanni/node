'use strict';

const { URL } = require('url');
const CJSmodule = require('module');
const resolve = process.binding('module_wrap').resolve;
const {Error, JSON: {stringify: JSONStringify}} = global;

module.exports = (target, base) => {
  target = `${target}`
  base = `${base}`;
  try {
    return resolve(target, base);
  } catch (e) {
    e.stack; // cause v8 to generate stack before rethrow
    try {
      const questionedBase = new URL(base);
      const tmpMod = new CJSmodule(questionedBase.pathname, null);
      tmpMod.paths = CJSmodule._nodeModulePaths(
        new URL('./', questionedBase).pathname);
      const found = CJSmodule._resolveFilename(target, tmpMod);
      e = new Error(`${JSONStringify(target)} not found by import in ${JSONStringify(base)}, but would be found by require at ${JSONStringify(found)}`);
    } catch (problemChecking) {//ignore
      problemChecking.stack // cause v8 to generate stack before rethrow
    }
    throw e;
  }
}
