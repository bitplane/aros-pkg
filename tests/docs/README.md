# Documentation examples

`fixture.key` signs the example packages the documentation's commands install
and nothing else: it is here, public, so that the outputs shown in the guides
are the same on every run. Never use it for a real package.

`fixtures.sh` builds what the examples need; `../docs-examples.sh` runs every
command the documentation shows and checks its exit code.
