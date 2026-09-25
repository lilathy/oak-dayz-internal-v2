"use client";

import Link from "next/link";
import { usePathname, useRouter } from "next/navigation";
import { useEffect, useState } from "react";
import { api, bootstrapSession, logout, useSession } from "@/lib/api";

type Bootstrap = {
  announcement?: string;
  maintenance?: boolean;
  discordUrl?: string;
  analytics?: { src: string; domain: string } | null;
};

const PRIMARY = [
  { href: "/plans", label: "Plans" },
  { href: "/download", label: "Download" },
  { href: "/forum", label: "Forum" },
];

const HELP = [
  { href: "/faq", label: "FAQ" },
  { href: "/changelog", label: "Changelog" },
  { href: "/support", label: "Support" },
];

export function SiteChrome({ children }: { children: React.ReactNode }) {
  const pathname = usePathname();
  const router = useRouter();
  const { user, authed, ready } = useSession();
  const [boot, setBoot] = useState<Bootstrap | null>(null);
  const [menuOpen, setMenuOpen] = useState(false);

  useEffect(() => {
    void bootstrapSession();
    api<Bootstrap>("/v1/site/bootstrap")
      .then(setBoot)
      .catch(() => setBoot({}));
  }, []);

  useEffect(() => {
    const src = boot?.analytics?.src;
    if (!src || document.getElementById("oak-analytics")) return;
    const s = document.createElement("script");
    s.id = "oak-analytics";
    s.src = src;
    s.defer = true;
    if (boot?.analytics?.domain) s.setAttribute("data-domain", boot.analytics.domain);
    document.head.appendChild(s);
  }, [boot]);

  useEffect(() => {
    api("/v1/site/event", {
      method: "POST",
      body: JSON.stringify({ event: "page_view", path: pathname }),
    }).catch(() => {});
  }, [pathname]);

  useEffect(() => {
    setMenuOpen(false);
  }, [pathname]);

  const isActive = (href: string) =>
    href === "/" ? pathname === "/" : pathname.startsWith(href);

  const helpActive = HELP.some((h) => isActive(h.href));

  if (pathname.startsWith("/ui-lab")) {
    return <>{children}</>;
  }

  return (
    <div className="shell">
      <nav className="topnav" aria-label="Primary">
        <Link href="/" className="brand">
          OAK<span>DayZ</span>
        </Link>

        <button
          type="button"
          className="nav-toggle"
          aria-expanded={menuOpen}
          aria-label="Toggle navigation"
          onClick={() => setMenuOpen((v) => !v)}
        >
          Menu
        </button>

        <div className={`nav-links${menuOpen ? " is-open" : ""}`}>
          {PRIMARY.map((item) => (
            <Link
              key={item.href}
              href={item.href}
              className={`nav-link${isActive(item.href) ? " is-active" : ""}`}
            >
              {item.label}
            </Link>
          ))}

          <details className="nav-group">
            <summary className={helpActive ? "is-active" : undefined}>Help</summary>
            <div className="nav-flyout">
              {HELP.map((item) => (
                <Link key={item.href} href={item.href}>
                  {item.label}
                </Link>
              ))}
            </div>
          </details>

          {!ready ? null : authed ? (
            <>
              <Link
                href="/account"
                className={`nav-link${isActive("/account") ? " is-active" : ""}`}
              >
                {user?.username ?? "Account"}
              </Link>
              <button
                type="button"
                className="nav-link"
                onClick={async () => {
                  await logout();
                  router.push("/");
                }}
              >
                Log out
              </button>
            </>
          ) : (
            <>
              <Link href="/login" className={`nav-link${isActive("/login") ? " is-active" : ""}`}>
                Log in
              </Link>
              <Link href="/register" className="nav-link nav-cta">
                Get started
              </Link>
            </>
          )}
        </div>
      </nav>

      {boot?.maintenance ? (
        <div className="oak-banner is-maint">Maintenance mode — injection is disabled.</div>
      ) : null}
      {boot?.announcement ? <div className="oak-banner">{boot.announcement}</div> : null}
      {authed && user && user.emailVerified === false ? (
        <div className="oak-banner">
          Your email is not verified yet. <Link href="/account">Verify from Account</Link> to unlock
          forum posting.
        </div>
      ) : null}

      <main>{children}</main>

      <footer className="site-foot">
        <span className="foot-brand">Oak · DayZ</span>
        <span className="foot-links">
          {boot?.discordUrl ? (
            <a href={boot.discordUrl} target="_blank" rel="noopener noreferrer">
              Discord
            </a>
          ) : null}
          <Link href="/faq">FAQ</Link>
          <Link href="/changelog">Changelog</Link>
          <Link href="/support">Support</Link>
          <Link href="/tos">Terms</Link>
        </span>
      </footer>
    </div>
  );
}
