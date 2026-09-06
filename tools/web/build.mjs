#!/usr/bin/env node
/* Build the board's web files.
 *
 *   src/web/*   commented sources, the files a human edits
 *   data/*      minified, plus a pre-gzipped copy of each - what the boards actually serve
 *
 * data/ IS COMMITTED and must be, because setupAssist.cpp checkDataFiles() re-downloads the web
 * files straight from this repo's data/ directory on raw.githubusercontent.com whenever they are
 * missing from a card, which is what happens after a CFG_VER bump wipes DATA_DIR. Both forms are
 * written: firmware from before the gzip change asks for the plain names, so removing them would
 * leave an un-upgraded board with no web UI and only the built-in OTA page to recover through.
 *
 * Usage:
 *   node tools/web/build.mjs            write data/
 *   node tools/web/build.mjs --check    build in memory, exit 1 if data/ is stale. Run this
 *                                       before committing a web change or uploading one
 *
 * The minifier settings below are deliberately conservative, each against a hazard this project
 * has already measured. Read the table in tools/web/README.md before loosening any of them.
 */
import { readFileSync, writeFileSync, existsSync, mkdirSync } from 'node:fs';
import { gzipSync } from 'node:zlib';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { minify as minifyJs } from 'terser';
import { minify as minifyHtml } from 'html-minifier-terser';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const SRC = join(ROOT, 'src', 'web');
const OUT = join(ROOT, 'data');
const CHECK = process.argv.includes('--check');

const BANNER = 'generated from src/web/%s by tools/web/build.mjs - DO NOT EDIT, your change will be overwritten';

/* Terser: rename locals, leave everything reachable from the other file alone.
 * - toplevel off (the default, stated here so nobody turns it on): the page calls sendControl,
 *   show, hide, isOn, $, $$ and a dozen more that are defined in common.js, and common.js calls
 *   processStatus, customInit and processBuffer back. Both files are plain top-level scripts, so
 *   renaming or dropping a top-level name breaks the pair.
 * - mangle.properties stays off (the default): handlers[type] is looked up by a string off the
 *   websocket, every /status key reaches an element by id, and processStatus compares key strings.
 */
const TERSER = {
  compress: { toplevel: false, passes: 2 },
  mangle: { toplevel: false },
  format: { comments: /^!/ }
};

/* html-minifier-terser. Ids, classes and attribute values are never touched by it, which matters
 * here: updateStatus() finds an element by id equal to a status key, the click dispatch routes on
 * e.id and e.classList.value, and showView() prefix-matches <option> TEXT against a size name.
 * minifyCSS is pinned to clean-css level 1 - level 2 merges and reorders rules, and this stylesheet
 * has live cascade-order dependencies (the max-width: 30rem block has to stay after the phone one).
 */
const HTML = {
  collapseWhitespace: true,
  conservativeCollapse: false,
  removeComments: true,
  removeRedundantAttributes: false,
  removeAttributeQuotes: false,
  removeEmptyAttributes: false,
  sortAttributes: false,
  sortClassName: false,
  minifyJS: TERSER,
  minifyCSS: { level: 1 }
};

async function buildJs(name) {
  const src = readFileSync(join(SRC, name), 'utf8');
  const res = await minifyJs(src, TERSER);
  if (res.error) throw res.error;
  return '/*! ' + BANNER.replace('%s', name) + ' */\n' + res.code;
}

async function buildHtml(name) {
  const src = readFileSync(join(SRC, name), 'utf8');
  const out = await minifyHtml(src, HTML);
  // the banner goes AFTER the doctype: anything before it puts the browser into quirks mode
  const m = out.match(/^<!doctype html>/i);
  if (!m) throw new Error(name + ': expected a doctype as the first thing in the file');
  return m[0] + '<!-- ' + BANNER.replace('%s', name) + ' -->' + out.slice(m[0].length);
}

function fmt(n) {
  return n < 1024 ? n + ' B' : (n / 1024).toFixed(1) + ' KB';
}

const built = [];
for (const name of ['MJPEG2SD.htm', 'common.js']) {
  const text = name.endsWith('.htm') ? await buildHtml(name) : await buildJs(name);
  const plain = Buffer.from(text, 'utf8');
  // level 9, and mtime 0 so the same input always produces the same bytes - a gzip header
  // carrying the build time would make every rebuild look like a change to --check and to git
  const gz = gzipSync(plain, { level: 9, mtime: 0 });
  built.push({ name, plain, gz, raw: readFileSync(join(SRC, name)).length });
}

if (CHECK) {
  let stale = 0;
  for (const b of built) {
    for (const [file, buf] of [[b.name, b.plain], [b.name + '.gz', b.gz]]) {
      const path = join(OUT, file);
      if (!existsSync(path) || !readFileSync(path).equals(buf)) {
        console.error('STALE: data/' + file + ' does not match a fresh build of src/web/' + b.name);
        stale++;
      }
    }
  }
  if (stale) {
    console.error('\nRun: node tools/web/build.mjs');
    process.exit(1);
  }
  console.log('data/ is up to date with src/web/');
  process.exit(0);
}

mkdirSync(OUT, { recursive: true });
for (const b of built) {
  writeFileSync(join(OUT, b.name), b.plain);
  writeFileSync(join(OUT, b.name + '.gz'), b.gz);
  console.log(
    b.name.padEnd(14) + fmt(b.raw).padStart(9) + '  ->  ' + fmt(b.plain.length).padStart(9) +
    ' minified  ->  ' + fmt(b.gz.length).padStart(9) + ' gzipped' +
    '   (' + Math.round(100 * b.gz.length / b.raw) + '% of source)'
  );
}
