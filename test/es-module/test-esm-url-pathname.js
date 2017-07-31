'use strict';
const common = require('../common');
const assert= require('assert');
const child_process = require('child_process');

const pathname = `${common.fixturesDir}/es-module-url/pathname.mjs`;
child_process.spawn(process.execPath, [
  pathname
]).on('exit', (code) => {
  assert.equal(code, 0);
}).stderr.pipe(process.stderr)
