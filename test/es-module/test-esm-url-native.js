'use strict';
const common = require('../common');
const assert= require('assert');
const child_process = require('child_process');

const native = `${common.fixturesDir}/es-module-url/native.mjs`;
child_process.spawn(process.execPath, [
  native
]).on('exit', (code) => {
  assert.equal(code, 1);
});
