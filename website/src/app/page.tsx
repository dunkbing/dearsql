import Image from "next/image";

function Header() {
  return (
    <header className="fixed top-0 left-0 right-0 z-50 bg-base/80 backdrop-blur-sm border-b border-surface1">
      <nav className="max-w-5xl mx-auto px-6 h-14 flex items-center justify-between">
        <a href="/" className="flex items-center gap-2 font-semibold text-lg">
          <Image
            src="/icon.webp"
            alt="Dear SQL"
            width={28}
            height={28}
            className="rounded-md"
          />
          Dear SQL
        </a>
        <div className="flex items-center gap-6 text-sm">
          <a href="#features" className="text-subtext0 hover:text-text">
            Features
          </a>
        </div>
      </nav>
    </header>
  );
}

function Hero() {
  return (
    <section className="pt-32 pb-16 px-6">
      <div className="max-w-5xl mx-auto text-center">
        <div className="flex justify-center mb-8">
          <Image
            src="/icon.webp"
            alt="Dear SQL"
            width={96}
            height={96}
            className="rounded-2xl"
          />
        </div>
        <h1 className="text-4xl md:text-5xl font-semibold tracking-tight mb-6 text-text">
          Simple, powerful database manager
        </h1>
        <p className="text-xl text-subtext0 max-w-2xl mx-auto mb-10">
          A native desktop SQL client for SQLite, PostgreSQL, MySQL, and Redis.
          Fast, intuitive, and built for developers who value simplicity.
        </p>
        <div className="inline-flex items-center gap-2 bg-surface0 text-subtext0 px-6 py-3 rounded-lg text-lg">
          <span className="relative flex h-2 w-2">
            <span className="animate-ping absolute inline-flex h-full w-full rounded-full bg-blue opacity-75"></span>
            <span className="relative inline-flex rounded-full h-2 w-2 bg-blue"></span>
          </span>
          Coming soon for macOS
        </div>
      </div>
    </section>
  );
}

function Screenshot() {
  return (
    <section className="pb-20 px-6">
      <div className="max-w-5xl mx-auto">
        <div className="bg-surface0 rounded-xl p-2 shadow-2xl shadow-black/50">
          <Image
            src="/sc.png"
            alt="Dear SQL application screenshot"
            width={1920}
            height={1080}
            className="rounded-lg w-full h-auto"
            priority
          />
        </div>
      </div>
    </section>
  );
}

function Features() {
  const features = [
    {
      title: "Multiple database support",
      description:
        "Connect to SQLite, PostgreSQL, MySQL, and Redis from a single app. Switch between databases effortlessly.",
    },
    {
      title: "Native performance",
      description:
        "Built with C++ and Metal rendering on macOS. No Electron, no bloat. Just fast, responsive UI.",
    },
    {
      title: "Smart SQL editor",
      description:
        "Syntax highlighting, autocomplete, and query history. Write SQL faster with less friction.",
    },
    {
      title: "Visual table editing",
      description:
        "Browse, filter, and edit table data inline. Changes generate proper SQL statements.",
    },
    {
      title: "ER diagrams",
      description:
        "Visualize table relationships with auto-generated entity-relationship diagrams.",
    },
    {
      title: "Secure connections",
      description:
        "Credentials encrypted locally. Support for SSH tunnels and SSL connections.",
    },
  ];

  return (
    <section id="features" className="py-20 px-6 bg-mantle">
      <div className="max-w-5xl mx-auto">
        <h2 className="text-3xl font-semibold text-center mb-4 text-text">
          Best at the basics
        </h2>
        <p className="text-subtext0 text-center mb-12 max-w-2xl mx-auto">
          Dear SQL focuses on what matters: connecting to databases, writing
          queries, and managing data. No unnecessary complexity.
        </p>
        <div className="grid md:grid-cols-2 lg:grid-cols-3 gap-6">
          {features.map((feature) => (
            <div
              key={feature.title}
              className="bg-surface0 p-6 rounded-xl border border-surface1"
            >
              <h3 className="font-semibold text-lg mb-2 text-text">
                {feature.title}
              </h3>
              <p className="text-subtext0 text-sm leading-relaxed">
                {feature.description}
              </p>
            </div>
          ))}
        </div>
      </div>
    </section>
  );
}

function Databases() {
  const databases = [
    { name: "SQLite", description: "Local file-based databases" },
    { name: "PostgreSQL", description: "Full schema and pool support" },
    { name: "MySQL", description: "Complete MySQL compatibility" },
    { name: "Redis", description: "Key-value browsing and editing" },
  ];

  return (
    <section className="py-20 px-6">
      <div className="max-w-5xl mx-auto">
        <h2 className="text-3xl font-semibold text-center mb-12 text-text">
          All your databases, one app
        </h2>
        <div className="grid sm:grid-cols-2 lg:grid-cols-4 gap-6">
          {databases.map((db) => (
            <div
              key={db.name}
              className="text-center p-6 border border-surface1 rounded-xl bg-surface0"
            >
              <div className="w-16 h-16 bg-surface1 rounded-full mx-auto mb-4 flex items-center justify-center">
                <span className="text-2xl font-semibold text-blue">
                  {db.name[0]}
                </span>
              </div>
              <h3 className="font-semibold mb-1 text-text">{db.name}</h3>
              <p className="text-subtext0 text-sm">{db.description}</p>
            </div>
          ))}
        </div>
      </div>
    </section>
  );
}

function ComingSoon() {
  return (
    <section id="download" className="py-20 px-6 bg-crust">
      <div className="max-w-5xl mx-auto text-center">
        <h2 className="text-3xl font-semibold mb-4 text-text">Coming Soon</h2>
        <p className="text-subtext0 mb-8 max-w-xl mx-auto">
          Dear SQL is currently in development. Sign up to get notified when
          it's ready.
        </p>
        <div className="max-w-md mx-auto">
          <iframe
            src="https://tally.so/embed/eqMqGo?alignLeft=1&hideTitle=1&transparentBackground=1&dynamicHeight=1"
            width="100%"
            height="200"
            frameBorder="0"
            title="Waitlist signup form"
          />
        </div>
        <p className="text-overlay1 text-sm mt-6">
          Available for macOS. Windows and Linux planned.
        </p>
      </div>
    </section>
  );
}

function Footer() {
  return (
    <footer className="py-8 px-6 border-t border-surface1">
      <div className="max-w-5xl mx-auto flex flex-col sm:flex-row items-center justify-between gap-4">
        <p className="text-overlay1 text-sm">
          &copy; {new Date().getFullYear()} Dear SQL.
        </p>
        <div className="flex gap-6 text-sm">
          <a
            href="https://x.com/dunkbingg"
            target="_blank"
            className="text-overlay1 hover:text-text"
          >
            X
          </a>
        </div>
      </div>
    </footer>
  );
}

export default function Home() {
  return (
    <div className="min-h-screen bg-base font-sans">
      <Header />
      <main>
        <Hero />
        <Screenshot />
        <Features />
        <Databases />
        <ComingSoon />
      </main>
      <Footer />
    </div>
  );
}
