## Summary

<!-- What does this PR change, and why? -->

## Related issue

<!-- e.g. Closes #123 -->

## Testing

<!-- How did you verify the change? -->

- [ ] Ran the native test harness (`python3 tests/run_tests.py`)
- [ ] Walked the relevant items in [`tests/SMOKE_CHECKLIST.md`](../tests/SMOKE_CHECKLIST.md) (for changes that affect runtime UX)
- [ ] Built the core locally (Windows DLL via `scripts/build-windows.ps1`, if applicable)

## Checklist

- [ ] Code follows the C99 / 4-space style in [`CONTRIBUTING.md`](../CONTRIBUTING.md)
- [ ] Changes respect the 320x240 RGB565 display constraint
- [ ] New UI elements handle both responsive layout and non-responsive fallback placement
- [ ] Documentation (README / core options) updated if behavior changed
