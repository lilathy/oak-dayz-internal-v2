import "./ui-lab.css";

const SWATCHES = [
  { name: "Background", token: "--bg", hex: "#09090b" },
  { name: "Surface", token: "--surface", hex: "#131316" },
  { name: "Surface 2", token: "--surface-2", hex: "#1a1a1f" },
  { name: "Border", token: "--border", hex: "#27272e" },
  { name: "Text", token: "--text", hex: "#f4f4f5" },
  { name: "Muted", token: "--muted", hex: "#a1a1aa" },
  { name: "Accent", token: "--accent", hex: "#fafafa" },
  { name: "Success", token: "--ok", hex: "#86efac" },
  { name: "Warning", token: "--warn", hex: "#fcd34d" },
  { name: "Error", token: "--bad", hex: "#fca5a5" },
];

export default function UiLabPage() {
  return (
    <div className="ui-lab">
      <div className="ui-lab__wrap">
        <header className="ui-lab__top">
          <div>
            <h1 className="ui-lab__title">Oak UI Lab</h1>
            <p className="ui-lab__sub">
              Tweak colors and spacing in <code>ui-lab.css</code>. Not linked in nav — for design only.
            </p>
          </div>
          <span className="ui-lab__badge">Draft · not live</span>
        </header>

        <section className="ui-lab__section">
          <p className="ui-lab__label">Colors</p>
          <div className="ui-lab__grid ui-lab__grid--4">
            {SWATCHES.map((s) => (
              <div key={s.token} className="ui-lab__swatch">
                <div className="ui-lab__swatch-color" style={{ background: s.hex }} />
                <div className="ui-lab__swatch-meta">
                  <strong>{s.name}</strong>
                  {s.token}
                  <br />
                  {s.hex}
                </div>
              </div>
            ))}
          </div>
        </section>

        <section className="ui-lab__section">
          <p className="ui-lab__label">Typography</p>
          <div className="ui-lab__type-sample">
            <h1>Oak</h1>
            <h2>Simple, dark, clean</h2>
            <p>
              Body text stays quiet. Solid surfaces, rounded corners. No clutter.
            </p>
            <span className="ui-lab__dim-text">Build 1.4.12 · sha256 a3f9…</span>
          </div>
        </section>

        <section className="ui-lab__section">
          <p className="ui-lab__label">Buttons</p>
          <div className="ui-lab__row">
            <button type="button" className="ui-lab__btn ui-lab__btn--primary">
              Primary
            </button>
            <button type="button" className="ui-lab__btn ui-lab__btn--ghost">
              Ghost
            </button>
            <button type="button" className="ui-lab__btn ui-lab__btn--soft">
              Soft
            </button>
            <button type="button" className="ui-lab__btn ui-lab__btn--primary" disabled>
              Disabled
            </button>
          </div>
        </section>

        <section className="ui-lab__section">
          <p className="ui-lab__label">Inputs</p>
          <div className="ui-lab__grid ui-lab__grid--2">
            <div className="ui-lab__field">
              <label htmlFor="lab-email">Email</label>
              <input id="lab-email" className="ui-lab__input" placeholder="you@example.com" />
            </div>
            <div className="ui-lab__field">
              <label htmlFor="lab-pass">Password</label>
              <input id="lab-pass" className="ui-lab__input" type="password" placeholder="••••••••" />
            </div>
          </div>
          <div className="ui-lab__check" style={{ marginTop: 14 }}>
            <input id="lab-remember" type="checkbox" defaultChecked />
            <label htmlFor="lab-remember">Stay signed in on this PC</label>
          </div>
        </section>

        <section className="ui-lab__section">
          <p className="ui-lab__label">Cards</p>
          <div className="ui-lab__grid ui-lab__grid--2">
            <div className="ui-lab__card">
              <h3>License active</h3>
              <p>30 days remaining · DayZ</p>
            </div>
            <div className="ui-lab__card">
              <h3>Build 1.4.12</h3>
              <p>Published · ready to launch</p>
            </div>
          </div>
        </section>

        <section className="ui-lab__section">
          <p className="ui-lab__label">Status</p>
          <div className="ui-lab__row">
            <span className="ui-lab__pill ui-lab__pill--ok">Online</span>
            <span className="ui-lab__pill ui-lab__pill--warn">Maintenance</span>
            <span className="ui-lab__pill ui-lab__pill--bad">Offline</span>
            <span className="ui-lab__pill ui-lab__pill--dim">Pending</span>
          </div>
        </section>

        <section className="ui-lab__section">
          <p className="ui-lab__label">Notices</p>
          <div className="ui-lab__grid ui-lab__grid--2">
            <div className="ui-lab__notice">Neutral notice — session expires in 12 minutes.</div>
            <div className="ui-lab__notice ui-lab__notice--warn">
              Warning — launcher update required before inject.
            </div>
          </div>
        </section>

        <section className="ui-lab__section">
          <p className="ui-lab__label">Launcher preview</p>
          <div className="ui-lab__launcher">
            <aside className="ui-lab__launcher-side">
              <div className="ui-lab__launcher-brand">Oak</div>
              <div className="ui-lab__launcher-user">username</div>
              <nav className="ui-lab__launcher-nav" aria-label="Products">
                <button type="button" className="is-active">
                  DayZ
                </button>
                <button type="button">Rust</button>
                <button type="button">EFT</button>
              </nav>
            </aside>
            <div className="ui-lab__launcher-main">
              <div className="ui-lab__launcher-stats">
                <div className="ui-lab__stat">
                  <div className="k">License</div>
                  <div className="v" style={{ color: "var(--ok)" }}>
                    Active
                  </div>
                </div>
                <div className="ui-lab__stat">
                  <div className="k">Game</div>
                  <div className="v">Closed</div>
                </div>
                <div className="ui-lab__stat">
                  <div className="k">Build</div>
                  <div className="v">1.4.12</div>
                </div>
              </div>
              <div className="ui-lab__notice">Ready — hit Launch to start DayZ.</div>
              <div className="ui-lab__launcher-foot">
                <button type="button" className="ui-lab__btn ui-lab__btn--primary">
                  Launch
                </button>
              </div>
            </div>
          </div>
        </section>

        <section className="ui-lab__section">
          <p className="ui-lab__label">In-game menu preview</p>
          <div className="ui-lab__menu">
            <div className="ui-lab__menu-nav">
              <div className="brand">OAK</div>
              <div className="item is-active">Visuals</div>
              <div className="item">Combat</div>
              <div className="item">World</div>
              <div className="item">Binds</div>
            </div>
            <div className="ui-lab__menu-panel">
              <h4>ESP</h4>
              <div className="ui-lab__menu-row">
                <span>Players</span>
                <span className="ui-lab__toggle" />
              </div>
              <div className="ui-lab__menu-row">
                <span>Loot</span>
                <span className="ui-lab__toggle is-off" />
              </div>
              <div className="ui-lab__menu-row">
                <span>Distance</span>
                <span className="ui-lab__dim-text">500m</span>
              </div>
            </div>
          </div>
        </section>
      </div>
    </div>
  );
}
