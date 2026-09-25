"use client";

import Link from "next/link";
import { useCallback, useEffect, useState } from "react";
import { api, apiUpload, fmtTimeLeft, friendlyError, reloadUser, timeAgo, useSession } from "@/lib/api";
import { track } from "@/lib/track";
import { AuthGate } from "@/components/AuthGate";
import { AuthorAvatar, PresenceDot } from "@/components/forum/Bits";

type License = {
  status?: string;
  plan?: string;
  expiresAt?: string | null;
  lifetime?: boolean;
  remainingSeconds?: number | null;
};
type Hwid = { bound?: boolean; hint?: string | null };
type LaunchInfo = {
  product?: { name?: string; status?: string; latestVersion?: string };
  client?: { version?: string };
  launch?: { canInject?: boolean; reasons?: string[] };
};
type SessionRow = {
  id: string;
  createdAt: string;
  expiresAt: string;
  userAgent: string | null;
  ip: string | null;
};

function AccountView() {
  const { user } = useSession();
  const [tab, setTab] = useState<"overview" | "redeem" | "profile" | "security" | "dayz">(
    "overview"
  );
  const [license, setLicense] = useState<License | null>(null);
  const [hwid, setHwid] = useState<Hwid | null>(null);
  const [launch, setLaunch] = useState<LaunchInfo | null>(null);
  const [sessions, setSessions] = useState<SessionRow[]>([]);
  const [loadErr, setLoadErr] = useState("");

  const [email, setEmail] = useState("");
  const [curPass, setCurPass] = useState("");
  const [newPass, setNewPass] = useState("");
  const [code, setCode] = useState("");
  const [profileMsg, setProfileMsg] = useState("");
  const [profileOk, setProfileOk] = useState(false);
  const [redeemMsg, setRedeemMsg] = useState("");
  const [redeemOk, setRedeemOk] = useState(false);
  const [verifyMsg, setVerifyMsg] = useState("");
  const [hwidPass, setHwidPass] = useState("");
  const [hwidMsg, setHwidMsg] = useState("");
  const [hwidOk, setHwidOk] = useState(false);

  const load = useCallback(async () => {
    try {
      const me = await api<{ user: { email: string }; license: License; hwid: Hwid }>("/v1/auth/me");
      setEmail(me.user.email);
      setHwid(me.hwid);
      const lic = await api<{ license: License }>("/v1/licenses/me").catch(() => ({
        license: me.license,
      }));
      setLicense(lic.license ?? me.license);
      await reloadUser();
      setLaunch(await api<LaunchInfo>("/v1/products/dayz/launch-info").catch(() => ({})));
      setSessions((await api<{ sessions: SessionRow[] }>("/v1/auth/sessions")).sessions);
    } catch (e) {
      setLoadErr(friendlyError(e));
    }
  }, []);

  useEffect(() => {
    // Client-side fetch on mount: the account view is user-specific and must
    // never be prerendered or cached.
    // eslint-disable-next-line react-hooks/set-state-in-effect
    void load();
  }, [load]);

  useEffect(() => {
    if (typeof window === "undefined") return;
    const params = new URLSearchParams(window.location.search);
    const token = params.get("hwid_reset");
    if (!token) return;
    void (async () => {
      setHwidMsg("");
      setHwidOk(false);
      try {
        await api("/v1/hwid/reset/confirm", {
          method: "POST",
          body: JSON.stringify({ token }),
        });
        setHwidOk(true);
        setHwidMsg("Hardware ID cleared. Sign in from the launcher on your new PC to rebind.");
        window.history.replaceState({}, "", "/account");
        await load();
      } catch (e) {
        setHwidMsg(friendlyError(e));
      }
    })();
  }, [load]);

  return (
    <>
      <header className="page-head">
        <h1>Account</h1>
        <p>License, redeem, profile, and security — one place.</p>
      </header>

      {loadErr ? <p className="error">{loadErr}</p> : null}

      <div className="tabs" role="tablist">
        {(
          [
            ["overview", "Overview"],
            ["redeem", "Redeem"],
            ["profile", "Profile"],
            ["security", "Security"],
            ["dayz", "DayZ"],
          ] as const
        ).map(([id, label]) => (
          <button
            key={id}
            type="button"
            role="tab"
            className={`tab${tab === id ? " is-active" : ""}`}
            aria-selected={tab === id}
            onClick={() => setTab(id)}
          >
            {label}
          </button>
        ))}
      </div>

      {user && user.emailVerified === false ? (
        <div className="callout">
          <div>
            <b>Confirm your email.</b>
            <p className="muted small">
              Posting on the forum stays locked until {email} is verified.
            </p>
          </div>
          <button
            className="btn btn-ghost"
            onClick={async () => {
              setVerifyMsg("");
              try {
                await api("/v1/auth/verify/send", { method: "POST", body: JSON.stringify({}) });
                setVerifyMsg("Confirmation link sent.");
              } catch (e) {
                setVerifyMsg(friendlyError(e));
              }
            }}
          >
            Resend link
          </button>
        </div>
      ) : null}
      {verifyMsg ? (
        <p className={verifyMsg.toLowerCase().includes("sent") ? "ok" : "error"}>{verifyMsg}</p>
      ) : null}

      {tab === "overview" ? (
      <div className="block">
        <h2>Overview</h2>
        <dl className="dl">
          <dt>Username</dt>
          <dd>{user?.username ?? "—"}</dd>
          <dt>Email</dt>
          <dd>
            {email || "—"}{" "}
            {user?.emailVerified ? (
              <span className="tag tag-ok">verified</span>
            ) : (
              <span className="tag tag-warn">unverified</span>
            )}
          </dd>
          <dt>Role</dt>
          <dd>{user?.role ?? "—"}</dd>
          <dt>License</dt>
          <dd>{license?.status ?? "—"}</dd>
          <dt>Plan</dt>
          <dd>{license?.plan ?? "—"}</dd>
          <dt>Expires</dt>
          <dd>{license?.lifetime ? "lifetime" : license?.expiresAt || "—"}</dd>
          <dt>Time left</dt>
          <dd>{fmtTimeLeft(license)}</dd>
          <dt>HWID</dt>
          <dd>{hwid?.bound ? hwid.hint || "bound" : "not bound — use the launcher"}</dd>
          <dt>Forum posts</dt>
          <dd>{user?.forumPostCount ?? 0}</dd>
        </dl>
        {hwid?.bound ? (
          <div className="form-panel" style={{ marginTop: 16 }}>
            <h3 style={{ margin: "0 0 8px" }}>Reset hardware ID</h3>
            <p className="muted small">
              Requires your password and an email confirmation link. 72-hour cooldown between resets.
            </p>
            <div className="field">
              <label htmlFor="hwidPass">Current password</label>
              <input
                id="hwidPass"
                type="password"
                autoComplete="current-password"
                value={hwidPass}
                onChange={(e) => setHwidPass(e.target.value)}
              />
            </div>
            <button
              className="btn btn-ghost"
              type="button"
              disabled={!hwidPass}
              onClick={async () => {
                setHwidMsg("");
                setHwidOk(false);
                try {
                  await api("/v1/hwid/reset/request", {
                    method: "POST",
                    body: JSON.stringify({ password: hwidPass }),
                  });
                  setHwidOk(true);
                  setHwidMsg("Check your email for the confirmation link.");
                  setHwidPass("");
                } catch (e) {
                  setHwidMsg(friendlyError(e));
                }
              }}
            >
              Email reset link
            </button>
            {hwidMsg ? <p className={hwidOk ? "ok" : "error"}>{hwidMsg}</p> : null}
          </div>
        ) : hwidMsg ? (
          <p className={hwidOk ? "ok" : "error"} style={{ marginTop: 12 }}>
            {hwidMsg}
          </p>
        ) : null}
        <p className="muted small" style={{ marginTop: 12 }}>
          Billing or other issues? <Link href="/support">Open a support ticket</Link>.
        </p>
      </div>
      ) : null}

      {tab === "profile" ? <ForumProfileBlock /> : null}

      {tab === "redeem" ? (
      <div className="block">
        <h2>Redeem code</h2>
        <div className="form-panel">
          <div className="field">
            <label htmlFor="code">Code</label>
            <input
              id="code"
              value={code}
              onChange={(e) => setCode(e.target.value)}
              placeholder="OAK-XXXX-XXXX-XXXX-XXXX"
            />
          </div>
          <button
            className="btn btn-primary"
            type="button"
            disabled={!code.trim()}
            onClick={async () => {
              try {
                const r = await api<{ creditedDays: number; license: License }>(
                  "/v1/licenses/redeem",
                  { method: "POST", body: JSON.stringify({ code: code.trim() }) }
                );
                setCode("");
                setRedeemOk(true);
                setRedeemMsg(
                  `Added ${r.creditedDays} day(s). Time left: ${fmtTimeLeft(r.license)}`
                );
                track("web.redeem_ok", { path: "/account" });
                void load();
              } catch (e) {
                setRedeemOk(false);
                setRedeemMsg(friendlyError(e));
                track("web.redeem_fail", { path: "/account" });
              }
            }}
          >
            Redeem
          </button>
          {redeemMsg ? <p className={redeemOk ? "ok" : "error"}>{redeemMsg}</p> : null}
        </div>
      </div>
      ) : null}

      {tab === "security" ? (
      <>
      <div className="block">
        <h2>Profile & password</h2>
        <div className="form-panel">
          <div className="field">
            <label htmlFor="acc-email">Email</label>
            <input
              id="acc-email"
              type="email"
              value={email}
              onChange={(e) => setEmail(e.target.value)}
            />
            <span className="muted small">
              Changing your email requires your current password and re-verification.
            </span>
          </div>
          <div className="field">
            <label htmlFor="acc-cur">Current password</label>
            <input
              id="acc-cur"
              type="password"
              autoComplete="current-password"
              value={curPass}
              onChange={(e) => setCurPass(e.target.value)}
            />
          </div>
          <div className="field">
            <label htmlFor="acc-new">New password (optional)</label>
            <input
              id="acc-new"
              type="password"
              autoComplete="new-password"
              value={newPass}
              onChange={(e) => setNewPass(e.target.value)}
            />
          </div>
          <button
            className="btn btn-primary"
            type="button"
            onClick={async () => {
              setProfileMsg("");
              const payload: Record<string, string> = { email: email.trim() };
              if (curPass) payload.currentPassword = curPass;
              if (newPass) {
                payload.currentPassword = curPass;
                payload.newPassword = newPass;
              }
              try {
                await api("/v1/auth/profile", { method: "PATCH", body: JSON.stringify(payload) });
                setProfileOk(true);
                setProfileMsg("Saved.");
                setCurPass("");
                setNewPass("");
                void load();
              } catch (e) {
                setProfileOk(false);
                setProfileMsg(friendlyError(e));
              }
            }}
          >
            Save changes
          </button>
          {profileMsg ? <p className={profileOk ? "ok" : "error"}>{profileMsg}</p> : null}
        </div>
      </div>

      <div className="block">
        <h2>Active sessions</h2>
        {sessions.length === 0 ? (
          <p className="muted">No other sessions.</p>
        ) : (
          <ul className="session-list">
            {sessions.map((s) => (
              <li key={s.id}>
                <span className="mono small">{s.userAgent?.slice(0, 60) || "unknown device"}</span>
                <span className="muted small">
                  {s.ip ?? "—"} · started {timeAgo(s.createdAt)}
                </span>
                <button
                  className="link-btn danger"
                  onClick={async () => {
                    try {
                      await api(`/v1/auth/sessions/${s.id}`, { method: "DELETE" });
                      void load();
                    } catch {
                      /* the list refresh will show the truth */
                    }
                  }}
                >
                  Revoke
                </button>
              </li>
            ))}
          </ul>
        )}
        <button
          className="btn btn-ghost"
          style={{ marginTop: 12 }}
          onClick={async () => {
            await api("/v1/auth/logout-all", { method: "POST", body: JSON.stringify({}) }).catch(
              () => {}
            );
            window.location.href = "/login";
          }}
        >
          Sign out everywhere
        </button>
      </div>
      </>
      ) : null}

      {tab === "dayz" ? (
      <div className="block">
        <h2>DayZ entitlement</h2>
        <dl className="dl">
          <dt>Product</dt>
          <dd>
            {launch?.product?.name ?? "—"} · {launch?.product?.status ?? "—"}
          </dd>
          <dt>Client</dt>
          <dd>{launch?.client?.version || launch?.product?.latestVersion || "—"}</dd>
          <dt>Can inject</dt>
          <dd>{launch?.launch?.canInject ? "yes" : "no"}</dd>
          <dt>Reasons</dt>
          <dd>{launch?.launch?.reasons?.join(", ") || "—"}</dd>
        </dl>
      </div>
      ) : null}
    </>
  );
}

export default function AccountPage() {
  return (
    <AuthGate>
      <AccountView />
    </AuthGate>
  );
}

function ForumProfileBlock() {
  const { user } = useSession();
  const [bio, setBio] = useState("");
  const [presence, setPresence] = useState("online");
  const [msg, setMsg] = useState("");
  const [ok, setOk] = useState(false);
  const [avatarUrl, setAvatarUrl] = useState<string | null>(null);

  useEffect(() => {
    setBio(user?.bio ?? "");
    setPresence(user?.presence?.preference ?? "online");
    setAvatarUrl(user?.avatarUrl ?? null);
  }, [user]);

  return (
    <div className="block">
      <h2>Forum profile</h2>
      <p className="muted small">
        Avatar, bio, and online status appear on your posts and member page. Posting still requires an
        active subscription.
      </p>
      <div className="form-panel" style={{ display: "grid", gap: 14 }}>
        <div style={{ display: "flex", gap: 16, alignItems: "center" }}>
          <AuthorAvatar
            author={{
              id: user?.id ?? "",
              username: user?.username ?? "?",
              role: user?.role ?? "user",
              createdAt: user?.createdAt ?? "",
              postCount: user?.forumPostCount ?? 0,
              avatarUrl,
              presence: user?.presence,
            }}
            size={64}
          />
          <div>
            <PresenceDot status={user?.presence?.status} />{" "}
            <span className="muted small">{user?.presence?.status ?? "offline"}</span>
            <div style={{ marginTop: 8, display: "flex", gap: 8, flexWrap: "wrap" }}>
              <label className="btn btn-ghost" style={{ cursor: "pointer" }}>
                Upload avatar
                <input
                  type="file"
                  accept="image/png,image/jpeg,image/webp"
                  hidden
                  onChange={async (e) => {
                    const file = e.target.files?.[0];
                    e.target.value = "";
                    if (!file) return;
                    setMsg("");
                    setOk(false);
                    try {
                      const fd = new FormData();
                      fd.append("avatar", file);
                      const res = await apiUpload<{ member: { avatarUrl?: string | null } }>(
                        "/v1/forum/profile/avatar",
                        fd
                      );
                      setAvatarUrl(res.member.avatarUrl ?? null);
                      await reloadUser();
                      setOk(true);
                      setMsg("Avatar updated.");
                    } catch (err) {
                      setMsg(friendlyError(err));
                    }
                  }}
                />
              </label>
              <button
                type="button"
                className="btn btn-ghost"
                onClick={async () => {
                  setMsg("");
                  setOk(false);
                  try {
                    await api("/v1/forum/profile/avatar", { method: "DELETE" });
                    setAvatarUrl(null);
                    await reloadUser();
                    setOk(true);
                    setMsg("Avatar removed.");
                  } catch (err) {
                    setMsg(friendlyError(err));
                  }
                }}
              >
                Remove
              </button>
            </div>
            <p className="muted small">PNG / JPEG / WebP · max 256 KB · max 512×512</p>
          </div>
        </div>

        <div className="field">
          <label htmlFor="bio">Bio</label>
          <textarea
            id="bio"
            rows={3}
            maxLength={280}
            value={bio}
            onChange={(e) => setBio(e.target.value)}
            placeholder="Short plain-text bio (280 chars). No HTML."
          />
          <span className="muted small">{bio.length}/280</span>
        </div>

        <div className="field">
          <label htmlFor="presence">Status</label>
          <select id="presence" value={presence} onChange={(e) => setPresence(e.target.value)}>
            <option value="online">Online (auto)</option>
            <option value="away">Away</option>
            <option value="offline">Appear offline</option>
            <option value="invisible">Invisible</option>
          </select>
        </div>

        <button
          type="button"
          className="btn"
          onClick={async () => {
            setMsg("");
            setOk(false);
            try {
              await api("/v1/forum/profile", {
                method: "PATCH",
                body: JSON.stringify({ bio, presence }),
              });
              await reloadUser();
              setOk(true);
              setMsg("Profile saved.");
            } catch (err) {
              setMsg(friendlyError(err));
            }
          }}
        >
          Save profile
        </button>
        {msg ? <p className={ok ? "ok" : "error"}>{msg}</p> : null}
      </div>
    </div>
  );
}
