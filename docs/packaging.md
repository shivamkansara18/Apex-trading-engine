# Package and hand off a verified candidate

The application is version 0.5.0. Local implementation and verification are
complete; publishing remains separate. [Changelog](../CHANGELOG.md) describes the
candidate. See [release evaluation](release.md) for the build and walkthrough.

## Produce a package

First obtain a successful full verification with clean-source checks:

```sh
python3 scripts/verify_release.py --clean-source
```

Use the evidence directory printed by that command:

```sh
python3 scripts/package_release.py tmp/release-TIMESTAMP-PID \
  --output tmp/releases/apex-0.5.0-candidate.zip
```

The output must be new; neither the ZIP nor its companion checksum is overwritten.
The packager requires the full matrix, clean-source build/UI checks, successful
steps and a source ZIP and evidence file hashes matching the manifest. Missing samples, changed source,
quick/failed/incomplete verification and mismatched versions reject packaging.
It packages the verified snapshot, not subsequent working-tree edits. Rerun full
verification to include later source changes in a new snapshot.

The ZIP includes:

- `START_HERE.md` with prerequisites, build/run/demo instructions and scope.
- `source.zip` containing the exact verified source snapshot.
- `evidence/verification.json`, all step logs, workload input, raw benchmark
  samples, per-run results and machine/compiler metadata.
- `SHA256SUMS` covering the included files. A companion `.zip.sha256` covers the
  outer package. Checksums detect changes; they are not publisher signatures.

Generated source/build/dependency trees and compiled binaries are excluded. This
is a portable source/evidence bundle, not a prebuilt application or installer.
The source now also includes compact historical paired results, raw-file hashes
and the exact measured source ZIP under `results/summary`; the full historical
455 MiB raw capture remains separate. New full verification runs include three
portable evidence-guard checks in both source trees.

Pinned GoogleTest cache reuse in source verification is recorded in the manifest.
The package can be generated without Git history or a remote.

## Verify a received package

From the directory containing both outer files:

```sh
shasum -a 256 -c apex-0.5.0-candidate.zip.sha256
unzip apex-0.5.0-candidate.zip -d apex-candidate
cd apex-candidate
shasum -a 256 -c SHA256SUMS
```

Follow `START_HERE.md` and the docs inside `source.zip`. The source has already
been rebuilt and tested locally from an extracted archive. That check is distinct
from hosted clean-clone CI or independent review on another machine.

## Test the packaging tool

```sh
python3 -m unittest discover -s tests -p package_release_test.py -v
```

Five tests cover checksums and overwrite rejection, incomplete/failed verification,
source/version tampering, invalid log paths and missing raw samples. The full
verification runner and Debug CI configuration also run these packaging tests.

## Publication status

No remote is configured and no initial commit/tag exists. A GitHub destination is
needed before preparing a push. Review the source and proposed publication first,
then run hosted CI and review the cream theme in a normal browser before tagging.
The local tooling does not choose an account, create a remote or publish files.
The original implementation-guide PDF is not included in the curated source ZIP.
