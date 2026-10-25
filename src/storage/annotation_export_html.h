#pragma once

namespace kobalt::annotation_export {
inline constexpr char style[] = R"EXPORT(
:root { color-scheme: light; --ink: #242830; --muted: #626975; --rule: #dfe3e9; --accent: #365ba5; }
* { box-sizing: border-box; }
[hidden] { display: none !important; }
html { scroll-padding-top: 130px; }
body { margin: 0; background: #f5f6f8; color: var(--ink); font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; line-height: 1.5; -webkit-text-size-adjust: 100%; }
main { max-width: 1000px; margin: 28px auto; padding: 36px 48px 24px; background: #fff; border: 1px solid var(--rule); border-radius: 4px; }
.eyebrow { margin: 0 0 12px; color: var(--accent); font-size: 10px; font-weight: 650; letter-spacing: .12em; text-transform: uppercase; }
h1 { margin: 0; font-family: Georgia, "Times New Roman", serif; font-size: 40px; font-weight: 400; line-height: 1.12; letter-spacing: -.035em; overflow-wrap: anywhere; }
.author { margin: 10px 0; font-size: 16px; color: #4d535e; overflow-wrap: anywhere; }
.summary { margin: 8px 0; color: var(--muted); font-size: 12px; }
.search-toolbar { position: sticky; top: 0; z-index: 2; display: flex; align-items: center; gap: 10px; flex-wrap: wrap; margin-top: 18px; padding: 12px 0; background: #fff; border-bottom: 1px solid var(--rule); }
.search-field { flex: 1; min-width: 180px; }
.sort-field { width: 145px; }
.markup-option { flex-basis: 100%; }
.markup-option label { display: inline-flex; align-items: center; gap: 7px; margin: 0; color: var(--muted); font-size: 12px; font-weight: 400; cursor: pointer; }
.markup-option input { width: 16px; height: 16px; margin: 0; padding: 0; accent-color: var(--accent); }
.entry-chapter { display: none; margin: 0 0 6px; color: var(--muted); font-size: 11px; overflow-wrap: anywhere; }
.chronological .entry-chapter { display: block; }
label { display: block; font-size: 11px; font-weight: 600; margin-bottom: 4px; }
input, select { font: inherit; font-size: 14px; width: 100%; height: 36px; border: 1px solid #abb3c0; border-radius: 4px; padding: 6px 10px; color: var(--ink); background: #fff; }
:focus-visible { outline: 2px solid var(--accent); outline-offset: 3px; }
button { height: 36px; align-self: flex-end; padding: 6px 12px; border: 1px solid #bbc2cd; border-radius: 4px; background: #fff; color: var(--ink); font: inherit; font-size: 12px; cursor: pointer; }
button:disabled { color: #737984; cursor: default; }
#search-status { align-self: flex-end; line-height: 36px; color: var(--muted); font-size: 11px; min-width: 132px; }
nav { margin: 12px 0 22px; color: var(--muted); font-size: 12px; }
summary { cursor: pointer; width: fit-content; }
nav ol { list-style: none; columns: 3; column-gap: 24px; margin: 12px 0 0; padding: 0; }
nav li { break-inside: avoid; margin: 0 0 6px; }
a { color: var(--accent); text-underline-offset: 3px; }
nav a { font-size: 11px; overflow-wrap: anywhere; }
section.chapter { margin-top: 22px; }
h2 { margin: 0; padding: 7px 10px; border-radius: 3px; background: #f1f4f8; font-size: 13px; font-weight: 600; line-height: 1.5; break-after: avoid; overflow-wrap: anywhere; }
.chapter-count { margin-left: 6px; color: var(--muted); font-weight: 400; font-size: 11px; }
article { display: grid; grid-template-columns: 60px minmax(0, 1fr); gap: 10px; padding: 12px 0; border-bottom: 1px solid var(--rule); }
.number { display: inline-flex; align-self: start; width: fit-content; padding: 3px 8px; border: 1px solid #dce3ed; border-radius: 6px; background: #f0f3f8; color: #4f6178; font-size: 11px; font-weight: 600; font-variant-numeric: tabular-nums; text-decoration: none; white-space: nowrap; }
.number:hover { border-color: var(--accent); color: var(--accent); }
blockquote { margin: 0; font-family: "Kobalt Libron", Georgia, "Times New Roman", serif; font-size: 17px; line-height: 1.5; overflow-wrap: anywhere; }
blockquote p { margin: 0; white-space: pre-line; }
blockquote p + p { margin-top: 10px; }
.passage-mark { -webkit-box-decoration-break: clone; box-decoration-break: clone; -webkit-print-color-adjust: exact; print-color-adjust: exact; }
blockquote[data-style="shaded"] .passage-mark { background: var(--mark); border-radius: 2px; padding: .025em .12em; }
blockquote[data-style="underline"] .passage-mark, blockquote[data-style="dotted"] .passage-mark { text-decoration-line: underline; text-decoration-color: var(--mark-ink); text-decoration-thickness: .065em; text-underline-offset: .14em; text-decoration-skip-ink: none; }
blockquote[data-style="dotted"] .passage-mark { text-decoration-style: dotted; }
main[data-markup="off"] .passage-mark { background: none; text-decoration: none; }
.note { margin-top: 10px; padding-left: 10px; border-left: 2px solid #b9c9e8; font-size: 13px; line-height: 1.5; overflow-wrap: anywhere; }
.note h3 { display: inline; margin: 0 6px 0 0; color: #4e5c75; font-size: 11px; font-weight: 650; }
.note p { display: inline; margin: 0; white-space: pre-line; }
.note p + p { display: block; margin-top: 8px; }
.annotation-meta { margin: 7px 0 0; color: var(--muted); font-size: 10px; }
#no-results { padding: 24px 0; font-size: 14px; }
footer { display: flex; justify-content: space-between; gap: 12px; flex-wrap: wrap; margin-top: 24px; color: var(--muted); font-size: 10px; }
footer a { color: inherit; }
@media (max-width: 640px) {
  html { scroll-padding-top: 220px; }
  body { background: #fff; }
  main { margin: 0; padding: 24px 18px 20px; border: 0; border-radius: 0; }
  h1 { font-size: 34px; }
  nav ol { columns: 1; }
  .search-toolbar { gap: 8px; }
  #search-status { min-width: auto; font-size: 10px; flex-basis: 100%; line-height: 18px; }
  .search-field { min-width: 100%; }
  .sort-field { flex: 1; }
  article { display: block; }
  .number { display: inline-flex; margin-bottom: 7px; }
  blockquote { font-size: 16px; }
}
@media print {
  @page { margin: 16mm; }
  body { background: #fff; }
  main { max-width: none; margin: 0; padding: 0; border: 0; }
  h1 { font-size: 28pt; }
  .search-toolbar, nav, footer a, #no-results { display: none !important; }
  article { display: grid; break-inside: avoid; }
  blockquote { font-size: 10.5pt; }
  .note { font-size: 9pt; }
  h2 { font-size: 10pt; }
  .annotation-meta { font-size: 8pt; }
}
)EXPORT";

// A classic inline script keeps local-file exports independent of network and storage access.
inline constexpr char search[] = R"EXPORT((() => {
  'use strict';
  const formatter = new Intl.DateTimeFormat(undefined, {
    year: 'numeric', month: 'short', day: 'numeric',
    hour: '2-digit', minute: '2-digit', second: '2-digit', timeZoneName: 'short'
  });
  for (const time of document.querySelectorAll('time[data-local-time]')) {
    const instant = new Date(time.getAttribute('datetime'));
    if (Number.isFinite(instant.getTime())) time.textContent = formatter.format(instant);
  }
  const input = document.getElementById('query');
  const markup = document.getElementById('show-markup');
  const root = document.getElementById('top');
  const kind = 'annotations';
  if (markup) {
    markup.checked = true;
    root.dataset.markup = 'on';
    markup.addEventListener('change', () => { root.dataset.markup = markup.checked ? 'on' : 'off'; });
  }
  const clear = document.getElementById('clear-search');
  const status = document.getElementById('search-status');
  const empty = document.getElementById('no-results');
  const contents = document.getElementById('chapter-summary');
  const navigation = document.getElementById('chapter-navigation');
  const sort = document.getElementById('sort-order');
  sort.value = 'position';
  const chronological = document.getElementById('chronological');
  const normalize = value => value.normalize('NFD').replace(/[\u0300-\u036f]/g, '').toLowerCase().replace(/\s+/g, ' ').trim();
  let position = 0;
  const chapters = Array.from(document.querySelectorAll('section.chapter')).map(section => ({
    section,
    link: document.querySelector('nav a[href="#' + section.id + '"]'),
    rows: Array.from(section.querySelectorAll('article')).map(article => ({
      article,
      position: position++,
      created: Date.parse(article.dataset.created),
      text: normalize(section.querySelector('h2').textContent + ' ' + article.textContent)
    }))
  }));
  const rows = chapters.flatMap(chapter => chapter.rows);
  function update() {
    const terms = normalize(input.value).split(' ').filter(Boolean);
    let count = 0;
    let matchingChapters = 0;
    for (const chapter of chapters) {
      let matches = 0;
      for (const row of chapter.rows) {
        const visible = terms.every(term => row.text.includes(term));
        row.article.hidden = !visible;
        if (visible) matches++;
      }
      chapter.section.hidden = sort.value !== 'position' || matches === 0;
      chapter.link.parentElement.hidden = matches === 0;
      count += matches;
      if (matches) matchingChapters++;
    }
    navigation.hidden = sort.value !== 'position';
    chronological.hidden = sort.value === 'position';
    status.textContent = terms.length ? count + ' of ' + rows.length + ' ' + kind : rows.length + ' ' + kind;
    contents.textContent = terms.length ? 'Browse ' + matchingChapters + ' matching chapters' : 'Browse ' + chapters.length + ' chapters';
    empty.hidden = count !== 0;
    clear.disabled = input.value.length === 0;
  }
  sort.addEventListener('change', () => {
    if (sort.value === 'position') {
      for (const chapter of chapters)
        for (const row of chapter.rows) chapter.section.appendChild(row.article);
    } else {
      const ordered = rows.slice().sort((a, b) => {
        if (Number.isFinite(a.created) !== Number.isFinite(b.created)) return Number.isFinite(a.created) ? -1 : 1;
        const difference = sort.value === 'newest' ? b.created - a.created : a.created - b.created;
        return difference || a.position - b.position;
      });
      for (const row of ordered) chronological.appendChild(row.article);
    }
    update();
  });
  input.addEventListener('input', update);
  clear.addEventListener('click', () => { input.value = ''; update(); input.focus(); });
  input.addEventListener('keydown', event => { if (event.key === 'Escape') { event.preventDefault(); input.value = ''; update(); } });
  let printQuery = null;
  window.addEventListener('beforeprint', () => {
    printQuery = input.value;
    input.value = '';
    update();
  });
  window.addEventListener('afterprint', () => {
    if (printQuery !== null) { input.value = printQuery; printQuery = null; update(); }
  });
  document.getElementById('search-toolbar').hidden = false;
  update();
})();)EXPORT";
}
