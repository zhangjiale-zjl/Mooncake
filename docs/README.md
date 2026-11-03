# Mooncake documents

## Build the docs

- Make sure in `docs` directory

```bash
cd docs
```

- Install the dependencies:

```bash
pip install -r requirements-docs.txt
```

- Clean the previous build (optional but recommended):

```bash
make clean
```

- Generate the HTML documentation:

```bash
make html
```

Be sure to set `locale` right before building.

## Open the docs with your browser

- Serve the documentation locally:

```bash
python -m http.server -d build/html/
```

This will start a local server at http://localhost:8000. You can now open your browser and view the documentation.

If port 8000 is already in use, you can specify a different port, for example:

```bash
python -m http.server 3000 -d build/html/
```

## NDS feature and validation branches

`feat/tent-nds` contains the NDS feature code and required transport integration.
`chore/tent-nds-validation` adds three commits: tests and validation tools
(standalone benchmark and experimental vLLM adapter), CI, and documentation with
Pages deployment. Keep these commits based on the current feature commit when
updating the feature.

A push to the validation branch runs the CPU ON/OFF, style and documentation
checks. The workflow verifies that its production code is identical to the
feature branch, then attaches `NDS required checks` to that exact feature commit.
The feature PR keeps the existing required check without carrying CI files.

## Publish to GitHub Pages

The personal fork documentation is available at
<https://zhangjiale-zjl.github.io/Mooncake/>. Its publishing branch is selected by
the repository Actions variable `DOCS_PUBLISH_BRANCH`, currently
`chore/tent-nds-validation`. Pushes that change documentation or `.github/workflows/deploy.yml`
on that branch build and publish the site. The workflow can also be run manually
with that branch selected.

In repository **Settings → Pages**, the build source is **GitHub Actions**.
The `github-pages` environment allows deployments from `chore/tent-nds-validation` and
`main`; the workflow publishes only the branch selected by `DOCS_PUBLISH_BRANCH`.
To publish `main`, ensure it contains the Pages workflow and documentation, set
the variable to `main`, then run **Deploy documentation to GitHub Pages** on
`main`. These settings apply to `zhangjiale-zjl/Mooncake`.

The workflow sets `MOONCAKE_DOCS_REPOSITORY`, `MOONCAKE_DOCS_BRANCH`, and
`MOONCAKE_DOCS_BASE_URL` so source links, edit links, and canonical URLs refer to
the published fork and branch. The same variables can be set for a local build.
Existing issue and PR references retain their original repository targets.

## PyPI downloads page

`source/community/pypi-downloads.md` uses a same-origin snapshot in
`source/_static/pypi-downloads.json`; it does not depend on an external app.
The Pages workflow refreshes this snapshot during deployment. Scheduled daily
deployment runs when `main` is the selected publishing branch.
Successful endpoint responses are reused for 24 hours, and the workflow cache
keeps the last available responses when PyPIStats is unavailable. Collection
age and partial data are visible on the page. The committed snapshot also
supports offline documentation builds.

To refresh manually and run the offline checks, from `docs/`:

```bash
python scripts/update_pypi_downloads.py
python -m unittest discover -s tests -p 'test_pypi_downloads.py'
node --test tests/pypi-downloads.test.cjs
```

Avoid repeatedly fetching the same PyPIStats endpoints within a day.
