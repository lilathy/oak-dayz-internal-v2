const Oak = (() => {
  const TOKEN_KEY = "oak_site_token";

  function getToken() {
    return localStorage.getItem(TOKEN_KEY) || "";
  }

  function setToken(token) {
    if (token) localStorage.setItem(TOKEN_KEY, token);
    else localStorage.removeItem(TOKEN_KEY);
  }

  function clearSession() {
    setToken("");
    localStorage.removeItem("oak_site_user");
  }

  function saveUser(user) {
    if (user) localStorage.setItem("oak_site_user", JSON.stringify(user));
    else localStorage.removeItem("oak_site_user");
  }

  function getUser() {
    try {
      return JSON.parse(localStorage.getItem("oak_site_user") || "null");
    } catch {
      return null;
    }
  }

  async function api(path, opts = {}) {
    const headers = { "Content-Type": "application/json", ...(opts.headers || {}) };
    const token = getToken();
    if (token) headers.Authorization = "Bearer " + token;
    const res = await fetch(path, { ...opts, headers });
    const text = await res.text();
    let body = null;
    try {
      body = text ? JSON.parse(text) : null;
    } catch {
      body = { error: text || "bad_response" };
    }
    if (!res.ok) {
      const err = new Error(body?.error || res.statusText || "request_failed");
      err.status = res.status;
      err.body = body;
      throw err;
    }
    return body;
  }

  function requireAuthPage() {
    if (!getToken()) {
      location.href = "/login.html?next=" + encodeURIComponent(location.pathname);
      return false;
    }
    return true;
  }

  function fmtTimeLeft(lic) {
    if (!lic) return "—";
    if (lic.lifetime) return "lifetime";
    if (lic.status !== "active") return "—";
    const s = lic.remainingSeconds ?? 0;
    const d = Math.floor(s / 86400);
    const h = Math.floor((s % 86400) / 3600);
    return d + "d " + h + "h";
  }

  function renderNav(active) {
    const user = getUser();
    const authed = !!getToken();
    const el = document.getElementById("nav");
    if (!el) return;

    const item = (href, label, key) =>
      `<a class="bare${active === key ? " is-active" : ""}" href="${href}">${label}</a>`;

    el.className = "topnav";
    el.innerHTML = `
      <a class="brand bare" href="/">OAK<span>DayZ</span></a>
      <div class="nav-links">
        ${item("/", "Home", "home")}
        ${item("/plans.html", "Plans", "plans")}
        ${item("/download.html", "Download", "download")}
        ${item("/faq.html", "FAQ", "faq")}
        ${item("/support.html", "Support", "support")}
        ${item("/changelog.html", "Changelog", "changelog")}
        ${
          authed
            ? item("/account.html", "Account", "account") +
              `<a class="bare" href="#" id="navLogout">Logout</a>`
            : item("/login.html", "Login", "login") + item("/register.html", "Register", "register")
        }
      </div>
    `;
    const logout = document.getElementById("navLogout");
    if (logout) {
      logout.onclick = (e) => {
        e.preventDefault();
        clearSession();
        location.href = "/";
      };
    }
  }

  function fonts() {
    if (document.getElementById("oak-fonts")) return;
    const link = document.createElement("link");
    link.id = "oak-fonts";
    link.rel = "stylesheet";
    link.href =
      "https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&family=Syne:wght@500;600;700&display=swap";
    document.head.appendChild(link);
  }

  function foot(bootstrap) {
    let el = document.getElementById("siteFoot");
    if (!el) {
      el = document.createElement("footer");
      el.id = "siteFoot";
      el.className = "site-foot";
      const shell = document.querySelector(".shell") || document.body;
      shell.appendChild(el);
    }
    const discord = bootstrap?.discordUrl
      ? `<a class="bare" href="${bootstrap.discordUrl}" target="_blank" rel="noopener">Discord</a> `
      : "";
    el.innerHTML = `<span>Oak · localhost</span><span>${discord}<a class="bare" href="/faq.html">FAQ</a> <a class="bare" href="/tos.html">Terms</a> <a class="bare" href="/support.html">Support</a> <a class="bare" href="/admin/">Admin</a></span>`;
  }

  function trackEvent(event, path) {
    api("/v1/site/event", {
      method: "POST",
      body: JSON.stringify({ event, path }),
    }).catch(() => {});
  }

  async function applyBootstrap() {
    try {
      const b = await api("/v1/site/bootstrap");
      window.__oakBootstrap = b;
      let banner = document.getElementById("oakBanner");
      if (!banner) {
        banner = document.createElement("div");
        banner.id = "oakBanner";
        const shell = document.querySelector(".shell");
        const nav = document.getElementById("nav");
        if (shell && nav) shell.insertBefore(banner, nav.nextSibling);
        else if (shell) shell.insertBefore(banner, shell.firstChild);
        else document.body.prepend(banner);
      }
      const parts = [];
      if (b.maintenance) parts.push("Maintenance mode — inject disabled.");
      if (b.announcement) parts.push(b.announcement);
      if (parts.length) {
        banner.className = "oak-banner" + (b.maintenance ? " is-maint" : "");
        banner.textContent = parts.join(" · ");
        banner.style.display = "block";
      } else {
        banner.style.display = "none";
      }
      if (b.analyticsScript && !document.getElementById("oak-ext-analytics")) {
        const wrap = document.createElement("div");
        wrap.id = "oak-ext-analytics";
        wrap.innerHTML = b.analyticsScript;
        document.body.appendChild(wrap);
        wrap.querySelectorAll("script").forEach((old) => {
          const s = document.createElement("script");
          if (old.src) s.src = old.src;
          else s.textContent = old.textContent || "";
          document.body.appendChild(s);
        });
      }
      foot(b);
      trackEvent("page_view", location.pathname);
      return b;
    } catch {
      foot(null);
      return null;
    }
  }

  function friendlyError(err) {
    const code = err?.message || "error";
    const details = err?.body?.details;
    const map = {
      invalid_credentials: "Wrong username or password.",
      already_exists: "Email or username already taken.",
      invalid_body: "Check your input (password min 10 chars, username 3+ letters/numbers/_).",
      invalid_code: "Invalid redeem code.",
      code_already_used: "That code was already used.",
      code_revoked: "That code was revoked.",
      license_banned: "License banned.",
      banned: "This account is banned.",
      wrong_password: "Current password is wrong.",
      email_taken: "Email already in use.",
      invalid_or_expired_token: "Reset link is invalid or expired.",
      ticket_closed: "This ticket is closed.",
      forbidden: "Not allowed.",
      rate_limited: "Too many attempts — wait and try again.",
      not_found: "Not found.",
      admin_required: "Admin only.",
    };
    if (code === "invalid_body" && details?.fieldErrors) {
      const parts = [];
      for (const [k, v] of Object.entries(details.fieldErrors)) {
        if (Array.isArray(v) && v.length) parts.push(k + ": " + v[0]);
      }
      if (parts.length) return parts.join(" · ");
    }
    return map[code] || code;
  }

  return {
    getToken,
    setToken,
    clearSession,
    saveUser,
    getUser,
    api,
    requireAuthPage,
    fmtTimeLeft,
    renderNav,
    fonts,
    foot,
    friendlyError,
    applyBootstrap,
    trackEvent,
  };
})();

Oak.fonts();
