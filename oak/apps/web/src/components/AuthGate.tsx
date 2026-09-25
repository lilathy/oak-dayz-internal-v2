"use client";

import { usePathname, useRouter } from "next/navigation";
import { useEffect } from "react";
import { useSession } from "@/lib/api";

/** Renders children only for signed-in visitors; everyone else is sent to login. */
export function AuthGate({ children }: { children: React.ReactNode }) {
  const { authed, ready } = useSession();
  const router = useRouter();
  const pathname = usePathname();

  useEffect(() => {
    if (ready && !authed) router.replace(`/login?next=${encodeURIComponent(pathname)}`);
  }, [ready, authed, pathname, router]);

  if (!ready) return <p className="muted" style={{ padding: "48px 0" }}>Loading…</p>;
  if (!authed) return null;
  return <>{children}</>;
}
