// The documentation site: light/dark switch, site nav open on wide screens, copy buttons, and the
// search panel's visibility. Search itself is MkDocs' own (search/main.js, lunr in a worker).
(function () {
  var root = document.documentElement;

  function dark() {
    var t = root.getAttribute('data-theme');
    return t ? t === 'dark' : window.matchMedia('(prefers-color-scheme: dark)').matches;
  }
  var toggle = document.getElementById('theme-toggle');
  if (toggle) toggle.addEventListener('click', function () {
    var next = dark() ? 'light' : 'dark';
    root.setAttribute('data-theme', next);
    try { localStorage.setItem('docs.theme', next); } catch (e) {}
  });

  // The nav is a <details> so it folds on phones; on wider screens it is always open.
  var details = document.getElementById('nav-details');
  var wide = window.matchMedia('(min-width: 801px)');
  function syncNav() { if (details) details.open = wide.matches || details.hasAttribute('data-user-open'); }
  if (details) details.addEventListener('toggle', function () {
    if (!wide.matches) details.toggleAttribute('data-user-open', details.open);
  });
  syncNav();
  if (wide.addEventListener) wide.addEventListener('change', syncNav);

  // Copy buttons on code blocks.
  document.querySelectorAll('.page .highlight').forEach(function (block) {
    var pre = block.querySelector('pre');
    if (!pre || !navigator.clipboard) return;
    var button = document.createElement('button');
    button.type = 'button';
    button.className = 'copy';
    button.textContent = 'copy';
    button.addEventListener('click', function () {
      navigator.clipboard.writeText(pre.innerText.replace(/\n$/, '')).then(function () {
        button.textContent = 'copied';
        setTimeout(function () { button.textContent = 'copy'; }, 1500);
      });
    });
    block.appendChild(button);
  });

  // Show search results only while there is a query.
  var input = document.getElementById('mkdocs-search-query');
  var results = document.getElementById('mkdocs-search-results');
  if (input && results) {
    var update = function () { results.hidden = input.value.trim().length < 2; };
    input.addEventListener('input', update);
    input.addEventListener('focus', update);
    input.addEventListener('keydown', function (e) {
      if (e.key === 'Escape') { input.value = ''; update(); input.blur(); }
    });
    document.addEventListener('click', function (e) {
      if (!e.target.closest('.search')) results.hidden = true;
    });
    document.addEventListener('keydown', function (e) {
      if (e.key === '/' && document.activeElement === document.body) { e.preventDefault(); input.focus(); }
    });
  }
})();
