"use client";

import Link from "next/link";
import { useRouter } from "next/navigation";
import { useEffect, useState } from "react";
import { friendlyError, register, useSession } from "@/lib/api";
import { track } from "@/lib/track";
import { Captcha, captchaConfigured } from "@/components/Captcha";

/** Mirrors the server-side rules so people get feedback before submitting. */
function passwordIssue(password: string, email: string, username: string): string | null {
  if (password.length < 10) return "Password must be at least 10 characters.";
  const p = password.toLowerCase();
  if (username.length >= 3 && p.includes(username.toLowerCase()))
    return "Password must not contain your username.";
  const local = email.split("@")[0]?.toLowerCase() ?? "";
  if (local.length >= 4 && p.includes(local)) return "Password must not contain your email name.";
  if (/^(.)\1+$/.test(p) || /^0?123456789/.test(p)) return "Pick something less predictable.";
  return null;
}

function strengthLabel(password: string): { label: string; level: number } {
  let score = 0;
  if (password.length >= 10) score++;
  if (password.length >= 14) score++;
  if (/[a-z]/.test(password) && /[A-Z]/.test(password)) score++;
  if (/\d/.test(password)) score++;
  if (/[^a-zA-Z0-9]/.test(password)) score++;
  const labels = ["very weak", "weak", "okay", "good", "strong", "strong"];
  return { label: labels[score]!, level: score };
}

export default function RegisterPage() {
  const router = useRouter();
  const { authed, ready } = useSession();
  const [email, setEmail] = useState("");
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [captchaToken, setCaptchaToken] = useState("");
  const [accepted, setAccepted] = useState(false);
  const [err, setErr] = useState("");
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    if (ready && authed) router.replace("/account");
  }, [ready, authed, router]);

  const usernameOk = /^[a-zA-Z0-9_]{3,32}$/.test(username);
  const emailOk = /^[^@\s]+@[^@\s]+\.[^@\s]+$/.test(email);
  const pwIssue = password ? passwordIssue(password, email, username) : null;
  const strength = strengthLabel(password);
  const canSubmit =
    emailOk && usernameOk && !pwIssue && accepted && (!captchaConfigured || !!captchaToken);

  async function submit() {
    if (busy || !canSubmit) return;
    setErr("");
    setBusy(true);
    track("web.signup_start", { path: "/register" });
    try {
      await register({
        email: email.trim(),
        username: username.trim(),
        password,
        captchaToken: captchaToken || undefined,
      });
      track("web.signup_ok", { path: "/register" });
      router.push("/account?welcome=1");
    } catch (e) {
      track("web.signup_fail", { path: "/register" });
      setErr(friendlyError(e));
      setBusy(false);
    }
  }

  return (
    <>
      <header className="page-head">
        <h1>Register</h1>
        <p>Create an account, then buy a plan or redeem a key.</p>
      </header>
      <div className="form-panel">
        <div className="field">
          <label htmlFor="email">Email</label>
          <input
            id="email"
            type="email"
            autoComplete="email"
            value={email}
            onChange={(e) => setEmail(e.target.value)}
          />
          {email && !emailOk ? <span className="error small">Enter a valid email.</span> : null}
          <span className="muted small">We send a confirmation link here.</span>
        </div>
        <div className="field">
          <label htmlFor="username">Username</label>
          <input
            id="username"
            autoComplete="username"
            value={username}
            onChange={(e) => setUsername(e.target.value)}
          />
          {username && !usernameOk ? (
            <span className="error small">3–32 characters, letters, numbers and underscore.</span>
          ) : null}
        </div>
        <div className="field">
          <label htmlFor="password">Password</label>
          <input
            id="password"
            type="password"
            autoComplete="new-password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            onKeyDown={(e) => e.key === "Enter" && submit()}
          />
          {password ? (
            <span className={pwIssue ? "error small" : "muted small"}>
              {pwIssue ?? `Strength: ${strength.label}`}
            </span>
          ) : (
            <span className="muted small">At least 10 characters.</span>
          )}
        </div>

        <label className="check-row">
          <input
            type="checkbox"
            checked={accepted}
            onChange={(e) => setAccepted(e.target.checked)}
          />
          <span>
            I agree to the <Link href="/tos">terms of service</Link>.
          </span>
        </label>

        <Captcha onToken={setCaptchaToken} />

        <div className="form-actions">
          <button
            className="btn btn-primary"
            type="button"
            disabled={busy || !canSubmit}
            onClick={submit}
          >
            {busy ? "Creating…" : "Create account"}
          </button>
        </div>
        {err ? <p className="error">{err}</p> : null}
        <p className="form-foot">
          Already have an account? <Link href="/login">Login</Link>
        </p>
      </div>
    </>
  );
}
