import nodemailer from "nodemailer";
import { config } from "./config.js";

export async function sendMail(opts: { to: string; subject: string; text: string; html?: string }) {
  if (!config.smtpHost) {
    console.log("[oak-mail] SMTP not configured — message dumped:");
    console.log(`  to: ${opts.to}`);
    console.log(`  subject: ${opts.subject}`);
    console.log(`  text:\n${opts.text}`);
    return { queued: false, logged: true };
  }
  const transporter = nodemailer.createTransport({
    host: config.smtpHost,
    port: config.smtpPort,
    secure: config.smtpSecure,
    auth: config.smtpUser ? { user: config.smtpUser, pass: config.smtpPass } : undefined,
  });
  await transporter.sendMail({
    from: config.smtpFrom,
    to: opts.to,
    subject: opts.subject,
    text: opts.text,
    html: opts.html ?? opts.text.replace(/\n/g, "<br>"),
  });
  return { queued: true, logged: false };
}
