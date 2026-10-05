# custom_styles: default plus an account that's been branded: a logo, and custom CSS that redefines
# every custom property in app/assets/stylesheets/colors.css, for light and dark.
based_on "default"

account = Account.first

at NOW - 10.days
account.logo.attach(file("black_hole.jpg", "image/jpeg"))
account.logo.analyze
account.logo_variant(:large)
account.logo_variant(:small)

at NOW - 9.days
account.update!(custom_styles: <<~CSS)
  :root {
    --lch-black: 22% 0.03 265;
    --lch-white: 98% 0.01 85;
    --lch-gray: 93% 0.02 85;
    --lch-gray-dark: 88% 0.025 85;
    --lch-gray-darker: 70% 0.03 85;
    --lch-blue: 50% 0.2 300;
    --lch-blue-light: 94% 0.04 300;
    --lch-blue-dark: 78% 0.09 300;
    --lch-orange: 72% 0.18 70;
    --lch-red: 55% 0.22 20;
    --lch-green: 60% 0.2 160;
    --lch-always-black: 10% 0.02 265;

    --color-negative: oklch(var(--lch-red));
    --color-positive: oklch(var(--lch-green));
    --color-bg: oklch(var(--lch-white));
    --color-message-bg: oklch(var(--lch-gray));
    --color-text: oklch(var(--lch-black));
    --color-text-reversed: oklch(var(--lch-white));
    --color-link: oklch(var(--lch-blue));
    --color-border: oklch(var(--lch-gray-dark));
    --color-border-dark: oklch(var(--lch-gray-darker));
    --color-border-darker: oklch(var(--lch-black) / 0.5);
    --color-selected: oklch(var(--lch-blue-light));
    --color-selected-dark: oklch(var(--lch-blue-dark));
    --color-alert: oklch(var(--lch-orange));
  }

  @media (prefers-color-scheme: dark) {
    :root {
      --lch-black: 96% 0.01 85;
      --lch-white: 18% 0.03 265;
      --lch-gray: 26% 0.03 265;
      --lch-gray-dark: 32% 0.03 265;
      --lch-gray-darker: 46% 0.03 265;
      --lch-blue: 74% 0.15 300;
      --lch-blue-light: 30% 0.06 300;
      --lch-blue-dark: 44% 0.08 300;
      --lch-orange: 78% 0.16 70;
      --lch-red: 72% 0.18 20;
      --lch-green: 76% 0.19 160;
      --lch-always-black: 5% 0.01 265;
    }
  }
CSS
