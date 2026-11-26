# Contributing

This repo is where the Manalens Unreal SDK is developed. Open issues and pull requests here.

## Test

You need Unreal Engine 5 on macOS or Linux.

```bash
UE_ROOT="/Users/Shared/Epic Games/UE_5.8" Tests/run-tests.sh
```

The script builds the plugin, copies it into `HostProject/Plugins`, and runs `Automation RunTests Manalens` headless. The fixture tests compare the request bodies with the Unity SDK fixtures. The script reads them from `../unity/Tests~/fixtures`, so clone [manalens-unity](https://github.com/manalens/manalens-unity) next to this repo as `unity` (the Manalens monorepo has this layout in `.sdks/`). Set `UPDATE_FIXTURES=1` to write the request bodies to `Tests/fixtures`, and review the diff.

GitHub-hosted runners have no Unreal Engine. CI (`.github/workflows/ci.yml`) only checks `Manalens/Manalens.uplugin`, the version, and that no build output is committed. Run `Tests/run-tests.sh` before you merge a change to the plugin source.

## Release

1. In a pull request, set `VersionName` (and raise `Version` by 1) in `Manalens/Manalens.uplugin`, and add a `## X.Y.Z` section at the top of `CHANGELOG.md`. CI checks that `VersionName` and the heading are the same.
2. Run `Tests/run-tests.sh` on that commit. Merge it to `main`.
3. Tag that commit and push the tag:

   ```bash
   git tag vX.Y.Z && git push origin vX.Y.Z
   ```

The release workflow (`.github/workflows/release.yml`) checks the tag, runs the CI checks, builds the plugin source zip, `SHA256SUMS`, and the provenance attestation, and makes the GitHub release.

## The delivery contract

The SDK sends app events to the Manalens collector (`POST /v0/events`). The contract (the request shape, the retry and batch defaults, and the shared fixture scenario) is `sdks/SPEC.md` in the private Manalens monorepo, next to the collector. The monorepo pins a commit of this repo in `sdks.lock.json`. Its tests check the fixtures and the source of the pinned commit against the collector schema, and run the SDK end to end against a local collector and Tinybird.

So a change that also needs a collector change takes two pull requests:

1. This repo first. Merge it to `main`.
2. Then the monorepo: bump the pin (`pnpm sdks:bump <engine>`) together with the collector change.

A change to the request body changes the fixtures. Write them again (see Test), review the diff, and commit them with the change.

## Commits

This repo is public, and its history shows each commit's author and committer.

- Use your GitHub noreply email (`<id>+<user>@users.noreply.github.com`) for both. Set it with `git config user.email` in this clone.
- Do not add AI attribution lines (`Co-Authored-By: Claude ...`, `Claude-Session: ...`, or "Generated with Claude Code") to commit messages or pull request descriptions.
- Merge after CI passes. Use a fast-forward push of the reviewed branch (`git push origin <branch>:main`), or a squash merge on GitHub. Both keep noreply emails in the history.
