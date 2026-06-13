# Frontend i18n rules

## Purpose

Prevent mixed-language UI text in `web_root`.

## Hard Rules

- All user-visible text added in JavaScript must go through the `I18N` dictionary and `t()` / `tf()`.
- Every new `I18N.en` key must have a matching `I18N.zh` key in the same change.
- Do not rely on English fallback for Chinese UI.
- Do not hardcode English labels in dynamic HTML templates.
- After changing `web_root/app.js`, run:

```powershell
node --check web_root\app.js
```

- For any new button, label, placeholder, alert, confirm text, modal title, status text, or empty state, verify the Chinese and English strings both exist before finishing.
