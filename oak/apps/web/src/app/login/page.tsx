"use client";

import Link from "next/link";
import { useRouter, useSearchParams } from "next/navigation";
import { Suspense, useEffect, useState } from "react";
import { friendlyError, login as doLogin, useSession } from "@/lib/api";
import { track } from "@/lib/track";
import { Captcha, captchaConfigured } from "@/components/Captcha";

function LoginForm() {
  const router = useRouter();
  const params = useSearchParams();
  const { authed, ready } = useSession();
  const [loginId, setLoginId] = useState("");
  const [password, setPassword] = useState("");
  const [captchaToken, setCaptchaToken] = useState("");
  const [err, setErr] = useState("");
  const [busy, setBusy] = useState(false);

  // Only allow same-origin relative paths — blocks //evil.com open redirects.
  const rawNext = params.get("next") || "/account";
  const next =
    rawNext.startsWith("/") && !rawNext.startsWith("//") && !rawNext.includes("\\")
      ? rawNext
      : "/account";

  useEffect(() => {
    if (ready && authed) router.replace(next);
  }, [ready, authed, next, router]);

  async function submit() {
    if (busy) return;
    setErr("");
    setBusy(true);
    try {
      await doLogin(loginId.trim(), password, captchaToken || undefined);
      track("web.login_ok", { path: "/login" });
      router.push(next);
    } catch (e) {
      track("web.login_fail", { path: "/login" });
      setErr(friendlyError(e));
      setBusy(false);
    }
  }

  return (
    <>
      <header className="page-head">
        <h1>Login</h1>
        <p>Sign in to manage license time, redeem keys and post on the forum.</p>
      </header>
      <div className="form-panel">
        <div className="field">
          <label htmlFor="login">Username / email</label>
          <input
            id="login"
            autoComplete="username"
            value={loginId}
            onChange={(e) => setLoginId(e.target.value)}
          />
        </div>
        <div className="field">
          <label htmlFor="password">Password</label>
          <input
            id="password"
            type="password"
            autoComplete="current-password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            onKeyDown={(e) => e.key === "Enter" && submit()}
          />
        </div>
        <Captcha onToken={setCaptchaToken} />
        <div className="form-actions">
          <button
            className="btn btn-primary"
            type="button"
            disabled={busy || !loginId || !password || (captchaConfigured && !captchaToken)}
            onClick={submit}
          >
            {busy ? "Signing in…" : "Sign in"}
          </button>
        </div>
        {err ? <p className="error">{err}</p> : null}
        <p className="form-foot">
          No account? <Link href="/register">Register</Link> ·{" "}
          <Link href="/forgot-password">Forgot password</Link>
        </p>
      </div>
    </>
  );
}

export default function LoginPage() {
  return (
    <Suspense>
      <LoginForm />
    </Suspense>
  );
}
