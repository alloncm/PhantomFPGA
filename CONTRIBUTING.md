# Contributing to PhantomFPGA

First off - thank you for considering contributing! Whether you're fixing a typo, improving documentation, or adding features, every bit helps make this training platform better.

And yes, we accept dad jokes in code comments. Quality control is minimal on those.

## Table of Contents

- [Code of Conduct](#code-of-conduct)
- [Ways to Contribute](#ways-to-contribute)
- [Getting Started](#getting-started)
- [Development Setup](#development-setup)
- [Making Changes](#making-changes)
- [Submitting Changes](#submitting-changes)
- [Style Guide](#style-guide)
- [Questions?](#questions)

## Code of Conduct

Be nice. We're all here to learn and help others learn. No gatekeeping, no "well actually" energy, no making people feel bad for not knowing things. Everyone was a beginner once, even Chuck Norris (okay, maybe not Chuck Norris).

Specifically:
- Be welcoming to newcomers
- Be respectful of differing viewpoints
- Accept constructive criticism gracefully
- Focus on what's best for the learning community

## Ways to Contribute

### For Everyone

- **Report bugs**: Found something broken? Open an issue!
- **Suggest improvements**: Have an idea? We'd love to hear it.
- **Improve documentation**: Typos, unclear explanations, missing examples - all fair game.
- **Share your experience**: Did the training help you? What was confusing? What clicked?

### For Developers

- **Fix bugs**: Check the issue tracker for things labeled `bug` or `good first issue`.
- **Add features**: New device capabilities, better testing, improved tooling.
- **Improve the skeleton code**: Better TODOs, clearer hints, more helpful error messages.
- **Write tests**: More tests = more confidence = happier developers.

### For Trainers/Educators

- **Improve the learning flow**: Is the progression logical? What's missing?
- **Add exercises**: More practice problems, challenges, "extra credit" tasks.
- **Translate documentation**: Make it accessible to more people.

## Getting Started

### Find Something to Work On

1. **Check the issues**: Look for labels like:
   - `good first issue` - Great starting points
   - `help wanted` - We especially need help here
   - `documentation` - No code changes needed
   - `enhancement` - New features or improvements

2. **Or scratch your own itch**: Did something annoy you while using the project? Fix it!

3. **Comment on the issue** before starting work to avoid duplicate effort.

### What Makes a Good Contribution

- **Focused**: One logical change per PR. Don't mix unrelated changes.
- **Tested**: Did you actually try it? Does it work?
- **Documented**: If you add features, update the docs.
- **Complete**: Don't submit half-finished work (WIP PRs are okay if labeled).

## Development Setup

You'll need the full dev environment. Follow the main README's prerequisites, then:

```bash
# Clone your fork
git clone https://github.com/YOUR_USERNAME/PhantomFPGA.git
cd PhantomFPGA

# Add upstream remote
git remote add upstream https://github.com/walruscraft/PhantomFPGA.git

# Build everything (yeah, it takes a while)
cd platform/qemu && ./setup.sh && make build && cd ../..
cd platform/buildroot && make && cd ../..

# Verify it works
./platform/run_qemu.sh --headless &
# (wait for boot, then kill it)
```

### Running Tests

```bash
# Unit tests (QEMU device)
cd tests/unit
./run-qtest.sh

# Integration tests (needs running VM)
cd tests/integration
./run_all.sh
```

## Making Changes

### Create a Branch

```bash
git checkout -b my-feature-branch
# or
git checkout -b fix/issue-42
```

Branch naming suggestions:
- `feature/cool-new-thing`
- `fix/broken-widget`
- `docs/clarify-setup`
- `test/more-coverage`

### Make Your Changes

- Follow the existing code style (see [Style Guide](#style-guide))
- Test your changes
- Update documentation if needed
- Add yourself to CONTRIBUTORS.md if you want (optional)

### Commit Messages

Write clear, descriptive commit messages:

```
Good:
  "Fix DMA buffer alignment for aarch64"
  "Add timeout handling to frame wait loop"
  "Clarify MSI-X setup in driver guide"

Bad:
  "fix stuff"
  "wip"
  "asdfasdf"  (we've all done it, but please don't submit it)
```

Format:
```
Short summary (50 chars or less)

Longer description if needed. Wrap at 72 characters.
Explain what and why, not how (the code shows how).

Fixes #123  (if applicable)
```

## Submitting Changes

### Before You Submit

1. **Test your changes**: Actually run the code. Boot the VM. Try to break it.
2. **Update docs**: If you changed behavior, update the documentation.
3. **Check for conflicts**: Rebase on latest main if needed.
4. **Review your own PR**: Look at the diff. Anything embarrassing? Fix it.

### Open a Pull Request

1. Push your branch to your fork
2. Open a PR against `walruscraft/PhantomFPGA:main`
3. Fill out the PR template (if there is one)
4. Describe what you changed and why
5. Reference any related issues

### What Happens Next

- A maintainer will review your PR
- They might ask for changes - that's normal and not a criticism
- Once approved, it gets merged
- You get a virtual high-five and our eternal gratitude

### PR Review Checklist (for reviewers)

- [ ] Does the code work?
- [ ] Is it tested?
- [ ] Is it documented?
- [ ] Does it follow the style guide?
- [ ] Is it focused (one logical change)?
- [ ] Are commit messages clear?
- [ ] Any security concerns?

## Style Guide

### C Code (QEMU Device, Driver)

- Follow Linux kernel style for driver code
- Follow QEMU style for QEMU device code
- Use tabs for indentation (kernel) or spaces (QEMU)
- Keep lines under 80 characters when reasonable
- Comments should explain "why", not "what"

```c
// Good comment:
/* Delay IRQ to simulate real hardware latency */

// Bad comment:
/* Add 1 to x */
x = x + 1;
```

### Shell Scripts

- Use `#!/bin/bash` (we need bash features)
- Use `set -e` for error handling
- Quote your variables: `"${VAR}"` not `$VAR`
- Add helpful comments for non-obvious stuff

### Documentation

- Use simple, clear English
- Include examples with expected output
- Keep the tone friendly and approachable
- Dad jokes and mild sarcasm are encouraged
- No non-ASCII characters (keeps things simple)

### Naming Things

- Be descriptive but not verbose
- Use snake_case for C functions and variables
- Use UPPER_CASE for C macros and constants
- Use kebab-case for shell script files

## Questions?

- **Open an issue** for project-related questions
- **Start a discussion** for broader topics
- **Check existing issues** before creating new ones

## Recognition

Contributors are awesome. We keep a list in CONTRIBUTORS.md (add yourself if you want).

Significant contributors may be invited to become maintainers. This comes with great power, great responsibility, and absolutely no compensation. Just like open source intended.

---

Thanks again for contributing. Every improvement, no matter how small, helps someone learn kernel development. And that's pretty cool.

*"In open source, we don't have bugs. We have 'undocumented features' and 'learning opportunities'."*
*- Every maintainer ever*
