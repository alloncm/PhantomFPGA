# Contributing to PhantomFPGA

First off - thank you for considering contributing! Whether you're fixing a typo, improving documentation, or adding features, every bit helps make this training platform better.

And yes, we accept dad jokes in code comments. Quality control is minimal on those.

## Table of contents

- [Code of conduct](#code-of-conduct)
- [Ways to contribute](#ways-to-contribute)
- [Getting started](#getting-started)
- [Development setup](#development-setup)
- [Making changes](#making-changes)
- [Submitting changes](#submitting-changes)
- [Style guide](#style-guide)
- [Questions?](#questions)

## Code of conduct

Be nice. We're all here to learn and help others learn. No gatekeeping, no "well actually" energy, no making people feel bad for not knowing things. Everyone was a beginner once, even Chuck Norris (okay, maybe not Chuck Norris).

Specifically:
- Be welcoming to newcomers
- Be respectful of differing viewpoints
- Accept constructive criticism gracefully
- Focus on what's best for the learning community

## Ways to contribute

### For everyone

- Found something broken? Report a bug: open an issue!
- Have an idea? We'd love to hear it - suggest improvements.
- Typos, unclear explanations, missing examples - all fair game for improving the documentation.
- Did the training help you? What was confusing? What clicked? Share your experiences, here and in social and professional media. The more you talk about this repo, the more people we can help!

### For developers

- Check the issue tracker for things labeled `bug` or `good first issue`, help us nock down the bugs.
- You are welcome to add stuff like new device capabilities, better testing, improved tooling.
- The skeleton code may need better TODOs, clearer hints, more helpful error messages. Just remember - we're here to teach and guide, not to disclose.
- Help with tests. More tests = more confidence = happier developers.

### For trainers/educators

- Help improve the learning flow: Is the progression logical? What's missing?
- Thinking of additional exercises? Suggest more practice problems, challenges, "extra credit" tasks.
- Translating the docs may be helpful. Help making it accessible to more people.

## Getting started

### Find something to work on

1. Check the issues. Look for labels like:
   - `good first issue` - Great starting points
   - `help wanted` - We especially need help here
   - `documentation` - No code changes needed
   - `enhancement` - New features or improvements
2. Scratch your own itch. Did something annoy you while using the project? Fix it!
3. Comment on the issue before starting work to avoid duplicate effort.

### What makes a good contribution

- One logical change per PR. Don't mix unrelated changes.
- Did you actually try it? Does it work? Don't push untested work.
- If you add/change features, update the docs.
- Don't submit half-finished work (WIP PRs are okay if labeled).

## Development setup

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

### Running tests

```bash
# Unit tests (QEMU device)
cd tests/unit
./run-qtest.sh

# Integration tests (needs running VM)
cd tests/integration
./run_all.sh
```

## Making changes

### Create a branch

```bash
git checkout -b my-feature-branch
# or
git checkout -b fix/issue-42
```

Branch naming suggestions:
- `feature/cool-new-thing`
- `fix/broken-build`
- `docs/clarify-setup`
- `test/more-coverage`

### Make your changes

- Follow the existing code style (see [Style Guide](#style-guide))
- Test your changes
- Update documentation if needed
- Add yourself to CONTRIBUTORS.md if you want (optional)

### Commit messages

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

## Submitting changes (common stuff for most open source projects)

### Before you submit

1. Test it. Actually run the code. Boot the VM. Try to break it.
2. If you changed behavior, update the documentation.
3. Rebase on latest main.
4. Look at the diff again. Anything embarrassing? Fix it.

### Open a pull request

1. Push your branch to your fork
2. Open a PR against `walruscraft/PhantomFPGA:main`
3. Fill out the PR template (if there is one)
4. Describe what you changed and why
5. Reference any related issues

### What happens next

- A maintainer will review your PR
- They might ask for changes - that's normal and not a criticism
- Once approved, it gets merged
- You get a virtual high-five and our eternal gratitude

### PR review checklist (for reviewers)

- [ ] Does the code work?
- [ ] Is it tested?
- [ ] Is it documented?
- [ ] Does it follow the style guide?
- [ ] Is it focused (one logical change)?
- [ ] Are commit messages clear?
- [ ] Any security concerns?

## Style guide

### C code (QEMU device, driver)

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

### Shell scripts

- Use `#!/bin/bash` (we need bash features)
- Use `set -e` for error handling
- Quote your variables: `"${VAR}"` not `$VAR`
- Add helpful comments for non-obvious stuff

### Documentation

- Use simple, clear English
- Include examples with expected output
- Keep the tone friendly and approachable
- Dad jokes are encouraged
- No non-ASCII characters (keeps things simple)

### Naming things

- Be descriptive but not verbose
- Use snake_case for C functions and variables
- Use UPPER_CASE for C macros and constants

## Questions?

- Open an issue for project-related questions
- Start a discussion for broader topics
- Check existing issues before creating new ones

## Recognition

Contributors are awesome. We keep a list in CONTRIBUTORS.md (add yourself if you want).

Significant contributors may be invited to become maintainers. This comes with great power, great responsibility, and absolutely no compensation. Just like open source intended.

---

Thanks again for contributing. Every improvement, no matter how small, helps someone learn kernel development. And that's pretty cool.

*"In open source, we don't have bugs. We have 'undocumented features' and 'learning opportunities'."*
*- Every maintainer ever*
