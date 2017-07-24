'use strict';

const { URL } = require('url');
const CJSmodule = require('module');
const resolve = process.binding('module_wrap').resolve;
const { Error, JSON: { stringify: JSONStringify } } = global;

module.exports = (target, base) => {
  target = `${target}`;
  if (base === undefined) {
    // We cannot search without a base.
    throw new Error('module not found');
  }
  base = `${base}`;
  try {
    return resolve(target, base);
  } catch (e) {
    e.stack; // cause V8 to generate stack before rethrow
    let error = e;
    try {
      const questionedBase = new URL(base);
      const tmpMod = new CJSmodule(questionedBase.pathname, null);
      tmpMod.paths = CJSmodule._nodeModulePaths(
        new URL('./', questionedBase).pathname);
      const found = CJSmodule._resolveFilename(target, tmpMod);
      error = new Error(
        `${JSONStringify(target)} not found by import in ` +
        `${JSONStringify(base)}, but would be found by require at ` +
        JSONStringify(found)
      );
    } catch (problemChecking) {
      // ignore
    }
    throw error;
  }
};
