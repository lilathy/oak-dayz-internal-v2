"use client";

import { useEffect, useRef } from "react";

const SITE_KEY = process.env.NEXT_PUBLIC_TURNSTILE_SITE_KEY ?? "";

declare global {
  interface Window {
    turnstile?: {
      render: (
        el: HTMLElement,
        opts: { sitekey: string; theme?: string; callback: (token: string) => void; "expired-callback"?: () => void }
      ) => string;
      remove: (id: string) => void;
    };
  }
}

export const captchaConfigured = !!SITE_KEY;

/**
 * Cloudflare Turnstile. Renders nothing when no site key is configured, so the
 * forms stay usable before the operator plugs in keys.
 */
export function Captcha({ onToken }: { onToken: (token: string) => void }) {
  const ref = useRef<HTMLDivElement>(null);
  const widgetId = useRef<string | null>(null);

  useEffect(() => {
    if (!SITE_KEY || !ref.current) return;
    const el = ref.current;

    const render = () => {
      if (!window.turnstile || widgetId.current || !el) return;
      widgetId.current = window.turnstile.render(el, {
        sitekey: SITE_KEY,
        theme: "dark",
        callback: onToken,
        "expired-callback": () => onToken(""),
      });
    };

    if (window.turnstile) {
      render();
    } else if (!document.getElementById("cf-turnstile-script")) {
      const s = document.createElement("script");
      s.id = "cf-turnstile-script";
      s.src = "https://challenges.cloudflare.com/turnstile/v0/api.js?render=explicit";
      s.async = true;
      s.onload = render;
      document.head.appendChild(s);
    } else {
      const timer = window.setInterval(() => {
        if (window.turnstile) {
          window.clearInterval(timer);
          render();
        }
      }, 200);
      return () => window.clearInterval(timer);
    }

    return () => {
      if (widgetId.current && window.turnstile) {
        window.turnstile.remove(widgetId.current);
        widgetId.current = null;
      }
    };
  }, [onToken]);

  if (!SITE_KEY) return null;
  return <div ref={ref} className="field" />;
}
