import test from 'node:test';
import assert from 'node:assert/strict';
import { parseTag } from '../ci/release.mjs';

test('tag maps to Windows version components', () => {
  assert.deepEqual(parseTag('v0.1.0'), { tag: 'v0.1.0', version: '0.1.0', parts: ['0', '1', '0'] });
  assert.equal(parseTag('v1.2.3').version, '1.2.3');
  assert.equal(parseTag('v65535.65535.65535').parts.length, 3);
});

test('invalid or overflowing tags cannot generate a release', () => {
  for (const tag of [undefined, '', '0.1.0', 'v01.2.3', 'v65536.0.0', 'v1.2.3-beta',
    'v1.2.3\n', 'v1.2.3&echo bad', 'v1.2', 'v-1.2.3']) assert.throws(() => parseTag(tag));
});
