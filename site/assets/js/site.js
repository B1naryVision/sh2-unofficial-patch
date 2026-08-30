/* ==================================================================
   Stronghold 2 Unofficial Patch — page script.
   No framework, no build step. Reads data/patches.js and renders.
   ================================================================== */
(function () {
  "use strict";

  var $  = function (s, r) { return (r || document).querySelector(s); };
  var $$ = function (s, r) { return Array.prototype.slice.call((r || document).querySelectorAll(s)); };

  function esc(s) {
    return String(s == null ? "" : s)
      .replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;").replace(/'/g, "&#39;");
  }

  /* Turn the few things players type as keys into real keycaps.
     One pass, longest alternative first, so a replacement is never
     rescanned and keycaps can't end up nested inside each other.
     Runs on already-escaped text, which contains no tags of its own. */
  var KEYCAP = /\b(Ctrl\+Shift\+O|Win\+R|Mouse4|Shift|Ctrl|Alt|Esc|Enter|[HJ])\b/g;

  function keycaps(text) {
    return text.replace(/`/g, "<kbd>`</kbd>")
               .replace(KEYCAP, function (m) { return "<kbd>" + m + "</kbd>"; });
  }

  var ICON = {
    download: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>',
    play:     '<svg viewBox="0 0 24 24" fill="currentColor"><path d="M8 5v14l11-7z"/></svg>',
    close:    '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>',
    shield:   '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"><path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/></svg>',
    file:     '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>',
    save:     '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/><polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/></svg>',
    users:    '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"><path d="M17 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2"/><circle cx="9" cy="7" r="4"/><path d="M23 21v-2a4 4 0 0 0-3-3.87"/><path d="M16 3.13a4 4 0 0 1 0 7.75"/></svg>',
    camera:   '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"><rect x="3" y="7" width="18" height="13" rx="2"/><circle cx="12" cy="13.5" r="3.5"/><path d="M8 7l1.5-3h5L16 7"/></svg>',
    grip:     '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><polyline points="9 6 4 12 9 18"/><polyline points="15 6 20 12 15 18"/></svg>',
    crest:    '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"><path d="M12 2l8 3v6.5C20 17 12 22 12 22S4 17 4 11.5V5z"/><path d="M12 6v10M8.5 9.5h7"/></svg>',
    sun:      '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><circle cx="12" cy="12" r="4.2"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/></svg>',
    moon:     '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"/></svg>'
  };

  var MP = {
    solo:    { cls:"mp--solo",    label:"Others don't need it" },
    partial: { cls:"mp--partial", label:"Better if both have it" },
    host:    { cls:"mp--host",    label:"Only the host needs it" },
    all:     { cls:"mp--all",     label:"Everyone needs it" }
  };

  /* ---------------------------------------------------------------
     Theme
     --------------------------------------------------------------- */
  function initTheme() {
    var root = document.documentElement;
    var btn  = $("#themeBtn");
    var saved;

    try { saved = localStorage.getItem("sh2-theme"); } catch (e) { saved = null; }

    if (saved) { root.setAttribute("data-theme", saved); }

    function paint() {
      var night = root.getAttribute("data-theme") !== "parchment";
      btn.innerHTML = night ? ICON.sun : ICON.moon;
      btn.setAttribute("aria-label", night ? "Switch to the light theme" : "Switch to the dark theme");
    }

    btn.addEventListener("click", function () {
      var next = root.getAttribute("data-theme") === "parchment" ? "night" : "parchment";
      root.setAttribute("data-theme", next);

      try { localStorage.setItem("sh2-theme", next); } catch (e) { /* private mode */ }

      paint();
    });

    paint();
  }

  /* ---------------------------------------------------------------
     Media — youtube / image / before-after / placeholder
     --------------------------------------------------------------- */
  var isFile = location.protocol === "file:";

  function mediaHTML(m, id) {
    if (!m) {
      return '<div class="media media--empty"><div class="ph">' + ICON.camera +
             '<span>Screenshot on the way</span></div></div>';
    }

    if (m.type === "youtube") {
      if (isFile) {
        return '<div class="media"><div class="media__fallback">' +
               '<p style="margin:0;color:var(--muted)">Videos need the page to be served, not opened from disk.</p>' +
               '<a class="btn btn--sm" target="_blank" rel="noopener" href="https://www.youtube.com/watch?v=' +
               esc(m.id) + '">' + ICON.play + 'Open on YouTube</a></div></div>' + cap(m);
      }

      return '<div class="media"><iframe src="https://www.youtube-nocookie.com/embed/' + esc(m.id) +
             '" title="' + esc(m.cap || "Video") + '" allow="accelerometer; autoplay; clipboard-write; ' +
             'encrypted-media; picture-in-picture" allowfullscreen loading="lazy"></iframe></div>' + cap(m);
    }

    if (m.type === "playlist") {
      if (isFile) {
        return '<div class="media"><div class="media__fallback">' +
               '<p style="margin:0;color:var(--muted)">Videos need the page to be served, not opened from disk.</p>' +
               '<a class="btn btn--sm" target="_blank" rel="noopener" href="' + esc(SITE.playlist) +
               '">' + ICON.play + 'Open on YouTube</a></div></div>';
      }

      return '<div class="media"><iframe src="https://www.youtube-nocookie.com/embed/videoseries?list=' +
             esc(m.id) + '" title="Patch release walkthroughs" allow="accelerometer; autoplay; ' +
             'clipboard-write; encrypted-media; picture-in-picture" allowfullscreen ' +
             'loading="lazy"></iframe></div>';
    }

    if (m.type === "image") {
      return '<div class="media"><img src="' + esc(m.src) + '" alt="' + esc(m.cap || "") +
             '" loading="lazy"></div>' + cap(m);
    }

    if (m.type === "ba") {
      var uid = "ba-" + id;
      return '<div class="ba" id="' + uid + '" style="--pos:50%">' +
             '<img src="' + esc(m.before) + '" alt="Before">' +
             '<img class="ba__after" src="' + esc(m.after) + '" alt="After">' +
             '<span class="ba__lbl ba__lbl--b">' + esc(m.beforeLabel || "Before") + '</span>' +
             '<span class="ba__lbl ba__lbl--a">' + esc(m.afterLabel || "After") + '</span>' +
             '<div class="ba__handle"><span class="ba__grip">' + ICON.grip + '</span></div>' +
             '<input class="ba__range" type="range" min="0" max="100" value="50" ' +
             'aria-label="Compare before and after"></div>' +
             '<p class="ba__hint">' + esc(m.cap || "Drag the handle to compare") + "</p>";
    }

    return "";
  }

  function cap(m) {
    return m.cap ? '<p class="media__cap">' + esc(m.cap) + "</p>" : "";
  }

  function wireSliders(root) {
    $$(".ba", root).forEach(function (ba) {
      var range = $(".ba__range", ba);

      function set() { ba.style.setProperty("--pos", range.value + "%"); }

      range.addEventListener("input", set);
      set();
    });
  }

  /* ---------------------------------------------------------------
     Feature cards
     --------------------------------------------------------------- */
  function mpBadge(p) {
    var m = MP[p.mp] || MP.solo;
    return '<span class="mp ' + m.cls + '">' + ICON.users + esc(m.label) + "</span>";
  }

  function cardHTML(p) {
    var thumb = "";

    if (p.media && p.media.type === "youtube") {
      thumb = '<div class="card-fx__thumb"><img src="https://i.ytimg.com/vi/' + esc(p.media.id) +
              '/hqdefault.jpg" alt="" loading="lazy"><span class="play">' + ICON.play + "</span></div>";
    } else if (p.media && p.media.type === "image") {
      thumb = '<div class="card-fx__thumb"><img src="' + esc(p.media.src) + '" alt="" loading="lazy"></div>';
    } else if (p.media && p.media.type === "ba") {
      thumb = '<div class="card-fx__thumb"><img src="' + esc(p.media.after) + '" alt="" loading="lazy"></div>';
    }

    var ver = p.version === "next" ? "Next update" : "v" + p.version;

    return '<button class="card-fx" type="button" data-id="' + esc(p.id) + '" data-cat="' + esc(p.cat) + '">' +
      '<span class="card-fx__meta">' +
        '<span class="tag tag--' + esc(p.kind) + '">' + esc(p.kind) + "</span>" +
        (p.optIn ? '<span class="optin">Optional</span>' : "") +
        '<span class="card-fx__ver">' + esc(ver) + "</span>" +
      "</span>" +
      thumb +
      '<span class="card-fx__body">' +
        "<h3>" + esc(p.title) + "</h3>" +
        '<span class="card-fx__was"><span class="lbl">Was</span><span>' + esc(p.was) + "</span></span>" +
        '<span class="card-fx__now"><span class="lbl">Now</span><span>' + esc(p.now) + "</span></span>" +
      "</span>" +
      '<span class="card-fx__foot">' + mpBadge(p) +
        '<span class="card-fx__more">Read more &rarr;</span>' +
      "</span>" +
    "</button>";
  }

  function renderCards() {
    var grid    = $("#cards");
    var filters = $("#filters");
    var count   = $("#cardCount");
    var active  = "all";

    filters.innerHTML = CATEGORIES.map(function (c) {
      var n = c.id === "all" ? PATCHES.length
                             : PATCHES.filter(function (p) { return p.cat === c.id; }).length;
      return '<button class="chip' + (c.id === "all" ? " is-on" : "") + '" type="button" data-cat="' +
             esc(c.id) + '">' + esc(c.label) + '<span class="chip__n">' + n + "</span></button>";
    }).join("") + '<span class="filters__count" id="cardCount"></span>';

    count = $("#cardCount");

    function paint() {
      var list = active === "all" ? PATCHES
                                  : PATCHES.filter(function (p) { return p.cat === active; });

      grid.innerHTML = list.map(cardHTML).join("");
      count.textContent = list.length + " of " + PATCHES.length + " shown";
    }

    filters.addEventListener("click", function (e) {
      var chip = e.target.closest(".chip");

      if (!chip) { return; }

      active = chip.dataset.cat;
      $$(".chip", filters).forEach(function (c) { c.classList.toggle("is-on", c === chip); });
      paint();
    });

    grid.addEventListener("click", function (e) {
      var card = e.target.closest(".card-fx");

      if (!card) { return; }

      openModal(card.dataset.id);
    });

    paint();
  }

  /* ---------------------------------------------------------------
     Detail modal
     --------------------------------------------------------------- */
  function openModal(id) {
    var p = PATCHES.filter(function (x) { return x.id === id; })[0];

    if (!p) { return; }

    var dlg  = $("#detail");
    var ver  = p.version === "next" ? "Next update" : "Version " + p.version;

    $("#detailTitle").textContent = p.title;
    $("#detailMeta").innerHTML =
      '<span class="tag tag--' + esc(p.kind) + '">' + esc(p.kind) + "</span>" +
      mpBadge(p) +
      (p.optIn ? '<span class="optin">Optional &mdash; off until you switch it on</span>' : "") +
      '<span class="tag">' + esc(ver) + "</span>";

    $("#detailBody").innerHTML =
      mediaHTML(p.media, p.id) +
      "<h4>What the problem was</h4><p>" + esc(p.was) + "</p>" +
      "<h4>What happens now</h4><p class=\"now\">" + esc(p.now) + "</p>" +
      (p.how ? '<div class="modal__how"><h4>How to use it</h4><p>' +
               keycaps(esc(p.how)) + "</p></div>" : "");

    wireSliders($("#detailBody"));

    if (history.replaceState) { history.replaceState(null, "", "#" + p.id); }

    if (typeof dlg.showModal === "function") { dlg.showModal(); }
    else { dlg.setAttribute("open", ""); }
  }

  function initModal() {
    var dlg = $("#detail");

    $("#detailClose").addEventListener("click", function () { dlg.close(); });

    dlg.addEventListener("click", function (e) {
      if (e.target === dlg) { dlg.close(); }
    });

    dlg.addEventListener("close", function () {
      $("#detailBody").innerHTML = "";   /* stops any playing video */

      if (history.replaceState) { history.replaceState(null, "", location.pathname + location.search); }
    });
  }

  /* ---------------------------------------------------------------
     Settings table, FAQ, changelog
     --------------------------------------------------------------- */
  function renderSettings() {
    $("#setBody").innerHTML = SETTINGS.map(function (g) {
      return '<tr class="set__group"><td colspan="3">' + esc(g.group) + "</td></tr>" +
        g.rows.map(function (r) {
          return '<tr><td class="k"><code>' + esc(r[0]) + "</code></td>" +
                 '<td class="v">' + esc(r[1]) + "</td>" +
                 "<td>" + esc(r[2]) + "</td></tr>";
        }).join("");
    }).join("");
  }

  function renderFaq() {
    $("#faqList").innerHTML = FAQ.map(function (f) {
      return "<details><summary>" + esc(f.q) + "</summary>" +
             '<div class="faq__a">' + f.a.join("") + "</div></details>";
    }).join("");
  }

  function renderLog() {
    $("#log").innerHTML = HISTORY.map(function (h, i) {
      var name = h.v === "next" ? "Next update" : "Version " + h.v;
      return "<details" + (i === 0 ? " open" : "") + "><summary>" +
        '<span class="n">' + esc(name) + "</span>" +
        (h.date ? "<span>" + esc(h.date) + "</span>" : "") +
        '<span class="c">' + h.items.length + " change" + (h.items.length === 1 ? "" : "s") + "</span>" +
        "</summary><ul>" +
        h.items.map(function (t) { return "<li>" + esc(t) + "</li>"; }).join("") +
        "</ul></details>";
    }).join("");
  }

  /* ---------------------------------------------------------------
     Live version from the GitHub release, with a static fallback
     --------------------------------------------------------------- */
  function loadVersion() {
    $$("[data-ver]").forEach(function (el) { el.textContent = SITE.fallbackVer; });

    if (isFile || !window.fetch) { return; }

    fetch("https://api.github.com/repos/" + SITE.repo + "/releases/latest", {
      headers: { Accept: "application/vnd.github+json" }
    }).then(function (r) {
      return r.ok ? r.json() : null;
    }).then(function (j) {
      if (!j || !j.tag_name) { return; }

      $$("[data-ver]").forEach(function (el) { el.textContent = j.tag_name; });

      var when = $("#relDate");

      if (when && j.published_at) {
        when.textContent = new Date(j.published_at).toLocaleDateString(undefined, {
          year: "numeric", month: "long", day: "numeric"
        });
        when.parentNode.hidden = false;
      }
    }).catch(function () { /* offline, rate-limited: the fallback stands */ });
  }

  /* ---------------------------------------------------------------
     Small things
     --------------------------------------------------------------- */
  function initCopy() {
    $$(".copy-btn").forEach(function (btn) {
      btn.addEventListener("click", function () {
        var text = btn.dataset.copy;
        var done = function () {
          var old = btn.textContent;
          btn.textContent = "Copied";
          btn.classList.add("is-done");
          setTimeout(function () { btn.textContent = old; btn.classList.remove("is-done"); }, 1600);
        };

        if (navigator.clipboard) { navigator.clipboard.writeText(text).then(done, function () {}); }
      });
    });
  }

  function initHero() {
    if (!SITE.heroImage) { return; }

    var slot = $("#heroArt");
    slot.innerHTML = '<div class="poster">' +
      '<span class="corner corner--tl"></span><span class="corner corner--tr"></span>' +
      '<span class="corner corner--bl"></span><span class="corner corner--br"></span>' +
      '<div class="poster__img"><img src="' + esc(SITE.heroImage) + '" alt=""></div>' +
      '<div class="poster__cap"><span class="txt">' + esc(SITE.heroCaption) + "</span></div></div>";
  }

  function initPlaylist() {
    if (!SITE.playlistId) { return; }   /* no playlist: the section stays hidden */

    $("#playlistEmbed").innerHTML = mediaHTML({ type:"playlist", id:SITE.playlistId }, "pl");
    $("#playlistBtn").href = SITE.playlist;
    $("#videos").hidden = false;
  }

  function initInstallVideo() {
    if (!SITE.installVideo) { return; }

    $("#installVideo").innerHTML = mediaHTML(
      { type:"youtube", id:SITE.installVideo, cap:"The whole install, start to finish" }, "install");
  }

  function initDeepLink() {
    var id = location.hash.replace("#", "");

    if (id && PATCHES.some(function (p) { return p.id === id; })) { openModal(id); }
  }

  function fillStats() {
    var crashes = PATCHES.filter(function (p) { return p.cat === "crashes"; }).length;
    var solo    = PATCHES.filter(function (p) { return p.mp === "solo"; }).length;

    $("#statTotal").textContent   = PATCHES.length;
    $("#statCrashes").textContent = crashes;
    $("#statSolo").textContent    = solo + "/" + PATCHES.length;
  }

  document.addEventListener("DOMContentLoaded", function () {
    initTheme();
    renderCards();
    initModal();
    renderSettings();
    renderFaq();
    renderLog();
    fillStats();
    loadVersion();
    initCopy();
    initHero();
    initPlaylist();
    initInstallVideo();
    initDeepLink();
    $("#year").textContent = new Date().getFullYear();
  });
})();
