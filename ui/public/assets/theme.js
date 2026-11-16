// Apply the saved preference before the stylesheet paints. No external requests.
try {
  const theme = localStorage.getItem('apex-theme') === 'light' ? 'light' : 'dark';
  document.documentElement.dataset.theme = theme;
  document.querySelector('meta[name="theme-color"]')?.setAttribute('content', theme === 'light' ? '#f2eee4' : '#0b1015');
} catch { document.documentElement.dataset.theme = 'dark'; }
