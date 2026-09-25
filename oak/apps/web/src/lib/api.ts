"use client";

import { useSyncExternalStore } from "react";

const API_URL = (process.env.NEXT_PUBLIC_API_URL ?? "http://localhost:8787").replace(/\/$/, "");

export type OakUser = {
  id: string;
  email: string;
  username: string;
  role: string;
  banned?: boolean;
  banReason?: string | null;
  emailVerified?: boolean;
  forumPostCount?: number;
  forumMutedUntil?: string | null;
  createdAt?: string;
  bio?: string;
  avatarUrl?: string | null;
  presence?: {
    status: "online" | "away" | "offline";
    preference?: string;
    lastSeenAt?: string | null;
  };
};

export class ApiError extends Error {
  status: number;
  body: Record<string, unknown>;
  constructor(message: string, status: number, body: Record<string, unknown>) {
    super(message);
    this.status = status;
    this.body = body;
  }
}

/**
 * Session state.
 *
 * The access token lives in memory only and the refresh token is an httpOnly
 * cookie the API sets, so a script injected into the page cannot read either
 * one or keep a stolen session alive past a reload.
 */
let accessToken: string | null = null;
let user: OakUser | null = null;
let ready = false;

type Snapshot = { user: OakUser | null; authed: boolean; ready: boolean };
let snapshot: Snapshot = { user: null, authed: false, ready: false };
const subscribers = new Set<() => void>();

function publish() {
  snapshot = { user, authed: !!accessToken, ready };
  for (const fn of subscribers) fn();
}

function subscribe(fn: () => void) {
  subscribers.add(fn);
  return () => subscribers.delete(fn);
}

const serverSnapshot: Snapshot = { user: null, authed: false, ready: false };

export function useSession(): Snapshot {
  return useSyncExternalStore(subscribe, () => snapshot, () => serverSnapshot);
}

export function currentUser(): OakUser | null {
  return user;
}

function setSession(token: string | null, nextUser: OakUser | null) {
  accessToken = token;
  if (nextUser !== undefined) user = nextUser;
  ready = true;
  publish();
}

// ------------------------------------------------------------------ fetch

type Options = RequestInit & { skipAuthRetry?: boolean };

async function request<T>(path: string, opts: Options = {}): Promise<T> {
  const isForm = typeof FormData !== "undefined" && opts.body instanceof FormData;
  const headers: Record<string, string> = {
    "X-Oak-Client": "web",
    ...(opts.body && !isForm ? { "Content-Type": "application/json" } : {}),
    ...(opts.headers as Record<string, string> | undefined),
  };
  if (accessToken) headers.Authorization = `Bearer ${accessToken}`;

  const res = await fetch(`${API_URL}${path}`, {
    ...opts,
    headers,
    credentials: "include",
  });

  const text = await res.text();
  let body: Record<string, unknown> = {};
  try {
    body = text ? JSON.parse(text) : {};
  } catch {
    body = { error: text || "bad_response" };
  }

  if (!res.ok) {
    throw new ApiError(String(body.error || res.statusText || "request_failed"), res.status, body);
  }
  return body as T;
}

export async function apiUpload<T = unknown>(path: string, form: FormData): Promise<T> {
  return api<T>(path, { method: "POST", body: form });
}

export async function api<T = unknown>(path: string, opts: Options = {}): Promise<T> {
  try {
    return await request<T>(path, opts);
  } catch (err) {
    const retriable =
      err instanceof ApiError &&
      err.status === 401 &&
      !opts.skipAuthRetry &&
      !path.startsWith("/v1/auth/refresh") &&
      !path.startsWith("/v1/auth/login") &&
      !path.startsWith("/v1/auth/register");
    if (!retriable) throw err;

    // The access token expires every 15 minutes; renew once and replay.
    const renewed = await refreshSession();
    if (!renewed) throw err;
    return request<T>(path, { ...opts, skipAuthRetry: true });
  }
}

// ---------------------------------------------------------------- session

type AuthResponse = { user: OakUser; accessToken: string; expiresIn: number };

let refreshInFlight: Promise<boolean> | null = null;

export function refreshSession(): Promise<boolean> {
  // One refresh at a time, otherwise parallel 401s would race and rotate the
  // refresh token out from under each other.
  refreshInFlight ??= (async () => {
    try {
      const data = await request<AuthResponse>("/v1/auth/refresh", {
        method: "POST",
        body: JSON.stringify({}),
      });
      setSession(data.accessToken, data.user ?? user);
      return true;
    } catch {
      setSession(null, null);
      return false;
    } finally {
      refreshInFlight = null;
    }
  })();
  return refreshInFlight;
}

let booted = false;

/** Restores the session from the refresh cookie on first paint. */
export async function bootstrapSession(): Promise<void> {
  if (booted) return;
  booted = true;
  await refreshSession();
  ready = true;
  publish();
}

export async function login(
  loginId: string,
  password: string,
  captchaToken?: string
): Promise<OakUser> {
  const data = await request<AuthResponse>("/v1/auth/login", {
    method: "POST",
    body: JSON.stringify({ login: loginId, password, captchaToken }),
  });
  setSession(data.accessToken, data.user);
  return data.user;
}

export async function register(input: {
  email: string;
  username: string;
  password: string;
  captchaToken?: string;
}): Promise<OakUser> {
  const data = await request<AuthResponse>("/v1/auth/register", {
    method: "POST",
    body: JSON.stringify(input),
  });
  setSession(data.accessToken, data.user);
  return data.user;
}

export async function logout(): Promise<void> {
  try {
    await api("/v1/auth/logout", { method: "POST", body: JSON.stringify({}) });
  } catch {
    // Signing out locally matters more than the server round trip succeeding.
  }
  setSession(null, null);
}

/** Re-reads the profile, e.g. after verifying an email or redeeming a code. */
export async function reloadUser(): Promise<OakUser | null> {
  try {
    const data = await api<{ user: OakUser }>("/v1/auth/me");
    setSession(accessToken, data.user);
    return data.user;
  } catch {
    return null;
  }
}

// ----------------------------------------------------------------- errors

const ERRORS: Record<string, string> = {
  invalid_credentials: "Wrong username or password.",
  already_exists: "Email or username already taken.",
  username_reserved: "That username is reserved. Pick another one.",
  weak_password: "Pick a stronger password — it must not contain your name or email.",
  captcha_failed: "Captcha check failed. Reload the page and try again.",
  invalid_body: "Check your input (password min 10 chars, username 3+ letters/numbers/_).",
  invalid_code: "Invalid redeem code.",
  code_already_used: "Code already used.",
  code_revoked: "Code revoked.",
  license_banned: "License banned.",
  wrong_password: "Current password is wrong.",
  email_taken: "Email already in use.",
  invalid_or_expired_token: "That link is invalid or expired.",
  ticket_closed: "This ticket is closed.",
  rate_limited: "Too many attempts — wait and try again.",
  email_unverified: "Verify your email address before posting.",
  subscription_required: "An active Oak subscription is required to post on the forum.",
  account_too_new: "Your account is too new to post. Try again in a few minutes.",
  posting_too_fast: "You are posting too quickly. Wait a moment.",
  thread_limit_reached: "You have hit the hourly thread limit.",
  post_limit_reached: "You have hit the hourly reply limit.",
  links_not_allowed_yet: "New accounts cannot post links yet. Take part in a few threads first.",
  too_many_links: "Too many links in one post.",
  duplicate_post: "You just posted that. Say something new instead.",
  forum_muted: "You are muted on the forum.",
  thread_locked: "This thread is locked.",
  category_locked: "You cannot post in this section.",
  edit_window_closed: "The edit window for this post has passed.",
  not_your_post: "That is not your post.",
  cannot_react_own: "You cannot like your own post.",
  query_too_short: "Search needs at least 3 characters.",
  not_found: "Not found.",
  banned: "This account is banned.",
  bio_too_long: "Bio is too long (280 characters max).",
  bio_forbidden_chars: "Bio cannot include HTML characters.",
  bio_spammy: "That bio looks like spam.",
  bio_rate_limited: "You are updating your bio too often.",
  invalid_bio: "Invalid bio.",
  invalid_presence: "Invalid presence status.",
  presence_rate_limited: "You are changing presence too often.",
  avatar_too_large: "Avatar must be 256 KB or smaller.",
  avatar_too_large_dim: "Avatar must be 512×512 or smaller.",
  avatar_unsupported_type: "Avatar must be PNG, JPEG, or WebP.",
  avatar_invalid: "That image could not be read.",
  avatar_missing: "Choose an image to upload.",
  avatar_rate_limited: "You can change your avatar a few times per hour.",
  avatar_upload_failed: "Avatar upload failed.",
};
export function friendlyError(err: unknown): string {
  if (!(err instanceof ApiError)) return err instanceof Error ? err.message : "Something went wrong.";
  const code = err.message;
  if (code === "banned" && err.body?.reason) {
    return `This account is banned. Reason: ${String(err.body.reason)}`;
  }
  if (code === "account_too_new" && typeof err.body?.retryAfterSec === "number") {
    return `Your account is too new to post. Try again in ${Math.ceil(
      Number(err.body.retryAfterSec) / 60
    )} minute(s).`;
  }
  if (code === "posting_too_fast" && typeof err.body?.retryAfterSec === "number") {
    return `Slow down — try again in ${err.body.retryAfterSec}s.`;
  }
  return ERRORS[code] || code;
}

// ---------------------------------------------------------------- format

export function fmtTimeLeft(lic: {
  status?: string;
  lifetime?: boolean;
  remainingSeconds?: number | null;
} | null): string {
  if (!lic) return "—";
  if (lic.lifetime) return "lifetime";
  if (lic.status !== "active") return "—";
  const s = lic.remainingSeconds ?? 0;
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  return `${d}d ${h}h`;
}

export function timeAgo(iso: string | null | undefined): string {
  if (!iso) return "—";
  const diff = Date.now() - new Date(iso).getTime();
  if (Number.isNaN(diff)) return "—";
  const mins = Math.floor(diff / 60000);
  if (mins < 1) return "just now";
  if (mins < 60) return `${mins}m ago`;
  const hours = Math.floor(mins / 60);
  if (hours < 24) return `${hours}h ago`;
  const days = Math.floor(hours / 24);
  if (days < 30) return `${days}d ago`;
  return new Date(iso).toLocaleDateString();
}

export { API_URL };
